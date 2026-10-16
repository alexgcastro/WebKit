/*
 * Copyright (C) 2014, 2025 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ThreadedCompositor.h"

#if USE(COORDINATED_GRAPHICS)
#include "AcceleratedSurface.h"
#include "CoordinatedSceneState.h"
#include "LayerTreeHost.h"
#include "RenderProcessInfo.h"
#include "WebPage.h"
#include "WebProcess.h"
#include <WebCore/CoordinatedPlatformLayer.h>
#include <WebCore/Damage.h>
#include <WebCore/FontCache.h>
#include <WebCore/Page.h>
#include <WebCore/PlatformDisplay.h>
#include <WebCore/Settings.h>
#include <WebCore/SkiaCompositingLayer.h>
#include <WebCore/SkiaDamageRegion.h>
#include <WebCore/TransformationMatrix.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkCanvas.h>
#include <skia/core/SkFont.h>
#include <skia/core/SkFontMgr.h>
#include <skia/core/SkPaint.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#include "EventDispatcher.h"
#include <wtf/Box.h>
#include <wtf/SetForScope.h>
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringToIntegerConversion.h>

#if OS(ANDROID)
#include <WebCore/ThreadedScrollingTree.h>
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <unistd.h>
#endif

#if USE(GLIB_EVENT_LOOP)
#include <wtf/glib/RunLoopSourcePriority.h>
#endif

#if USE(TEXTURE_MAPPER)
#include <WebCore/TextureMapperLayer.h>
#endif

#if USE(LIBEPOXY)
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#else
#include <GLES2/gl2.h>
#endif

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(ThreadedCompositor);

// debug.wpe.scroll.samplepercomposite=0 leaves scroll animations to the display refresh alone.
static bool scrollAnimationsSampledPerComposite()
{
#if OS(ANDROID)
    static const bool sampled = [] {
        char value[PROP_VALUE_MAX] = { };
        return !(__system_property_get("debug.wpe.scroll.samplepercomposite", value) > 0 && !strcmp(value, "0"));
    }();
    return sampled;
#else
    return false;
#endif
}

#if OS(ANDROID)
// Android's performance hints (ADPF): the compositor thread reports how long it worked for each frame
// against the frame period, so the CPU governor raises its clock before that work misses the frame, as
// SurfaceFlinger, HWUI and Chrome do, and warns before a one-off burst (pre-rendered tiles). Without
// them a fling's tile flushes ran on the big core at a third of its top clock and held frames for
// 20-50 ms. debug.wpe.perf.hints=0 disables them. API 33 (spikes: 36), looked up at run time.
// debug.wpe.perf.hints=chrome reports as Chrome does (components/viz/service/display/display.cc and
// display_scheduler.cc): for each frame, the time from the vsync at or before the moment it could first start (asked
// for, and the previous frame done) until it is sent, so waiting behind other work counts, against 3/4 of the frame
// period; and on the first scrolling frame after a pause it reports a frame of 1.5 times the target, as Chrome does
// when a touch starts (boost_manager.cc), to raise the clocks before the work arrives.
struct PerformanceHintFunctions {
    bool chromeLike { false };
    void* (*getManager)() { nullptr };
    void* (*createSession)(void*, const int32_t*, size_t, int64_t) { nullptr };
    int (*updateTargetWorkDuration)(void*, int64_t) { nullptr };
    int (*reportActualWorkDuration)(void*, int64_t) { nullptr };
    void (*closeSession)(void*) { nullptr };
    int (*notifyWorkloadSpike)(void*, bool, bool, const char*) { nullptr };
};

static const PerformanceHintFunctions& performanceHintFunctions()
{
    static const PerformanceHintFunctions functions = [] {
        PerformanceHintFunctions functions;
        char value[PROP_VALUE_MAX] = { };
        if (__system_property_get("debug.wpe.perf.hints", value) > 0) {
            if (!strcmp(value, "0"))
                return functions;
            functions.chromeLike = !strcmp(value, "chrome");
        }
        void* android = dlopen("libandroid.so", RTLD_NOW);
        if (!android)
            return functions;
        functions.getManager = reinterpret_cast<decltype(functions.getManager)>(dlsym(android, "APerformanceHint_getManager"));
        functions.createSession = reinterpret_cast<decltype(functions.createSession)>(dlsym(android, "APerformanceHint_createSession"));
        functions.updateTargetWorkDuration = reinterpret_cast<decltype(functions.updateTargetWorkDuration)>(dlsym(android, "APerformanceHint_updateTargetWorkDuration"));
        functions.reportActualWorkDuration = reinterpret_cast<decltype(functions.reportActualWorkDuration)>(dlsym(android, "APerformanceHint_reportActualWorkDuration"));
        functions.closeSession = reinterpret_cast<decltype(functions.closeSession)>(dlsym(android, "APerformanceHint_closeSession"));
        functions.notifyWorkloadSpike = reinterpret_cast<decltype(functions.notifyWorkloadSpike)>(dlsym(android, "APerformanceHint_notifyWorkloadSpike"));
        if (!functions.getManager || !functions.createSession || !functions.updateTargetWorkDuration || !functions.reportActualWorkDuration || !functions.closeSession)
            functions.createSession = nullptr;
        return functions;
    }();
    return functions;
}

void ThreadedCompositor::reportCompositorWork(Seconds frameWork, std::optional<MonotonicTime> frameVSync)
{
    ASSERT(m_workQueue->runLoop().isCurrent());
    auto& functions = performanceHintFunctions();
    if (!m_performanceHintSessionTried) {
        m_performanceHintSessionTried = true;
        if (functions.createSession) {
            if (auto* manager = functions.getManager()) {
                int32_t threadID = gettid();
                m_performanceHintTargetNanos = 16'666'666;
                m_performanceHintSession = functions.createSession(manager, &threadID, 1, m_performanceHintTargetNanos);
            }
        }
        WPE_TRACE_INSTANT("perf: performance hint session %s, workload spikes %s", m_performanceHintSession ? "created" : "unavailable", functions.notifyWorkloadSpike ? "supported" : "unsupported");
    }
    if (!m_performanceHintSession)
        return;

    if (auto period = ThreadedScrollingTree::displayRefreshPeriod()) {
        int64_t target = (functions.chromeLike ? *period * 0.75 : *period).nanosecondsAs<int64_t>();
        if (target > 0 && target != m_performanceHintTargetNanos) {
            functions.updateTargetWorkDuration(m_performanceHintSession, target);
            m_performanceHintTargetNanos = target;
        }
    }
    auto work = frameWork + m_workSinceLastFrame;
    m_workSinceLastFrame = 0_s;
    if (functions.chromeLike && frameVSync)
        work = MonotonicTime::now() - *frameVSync;
    functions.reportActualWorkDuration(m_performanceHintSession, std::max<int64_t>(work.nanosecondsAs<int64_t>(), 1));
    WPE_TRACE_COUNTER("wpe.perf.reported-work-us", work.microsecondsAs<int64_t>());
}

void ThreadedCompositor::boostPerformanceHintForScrollIfNeeded(OptionSet<CompositionReason> reasons, MonotonicTime frameStart)
{
    if (!reasons.contains(CompositionReason::AsyncScrolling))
        return;
    bool afterPause = !m_lastScrollingFrameTime || frameStart - *m_lastScrollingFrameTime > 100_ms;
    m_lastScrollingFrameTime = frameStart;
    auto& functions = performanceHintFunctions();
    if (afterPause && m_performanceHintSession && functions.chromeLike && m_performanceHintTargetNanos) {
        WPE_TRACE_INSTANT("perf: boost, scrolling after a pause");
        functions.reportActualWorkDuration(m_performanceHintSession, m_performanceHintTargetNanos * 3 / 2);
    }
}

void ThreadedCompositor::notifyCompositorWorkloadSpike(const char* reason)
{
    auto& functions = performanceHintFunctions();
    if (m_performanceHintSession && functions.notifyWorkloadSpike)
        functions.notifyWorkloadSpike(m_performanceHintSession, true, true, reason);
}

void ThreadedCompositor::closePerformanceHintSession()
{
    if (m_performanceHintSession)
        performanceHintFunctions().closeSession(std::exchange(m_performanceHintSession, nullptr));
}
#endif

Ref<ThreadedCompositor> ThreadedCompositor::create(WebPage& webPage, LayerTreeHost& layerTreeHost, CoordinatedSceneState& sceneState)
{
    return adoptRef(*new ThreadedCompositor(webPage, layerTreeHost, sceneState));
}

ThreadedCompositor::ThreadedCompositor(WebPage& webPage, LayerTreeHost& layerTreeHost, CoordinatedSceneState& sceneState)
    : m_workQueue(WorkQueue::create("org.webkit.ThreadedCompositor"_s))
    , m_layerTreeHost(&layerTreeHost)
    , m_pageID(webPage.identifier())
    , m_surface(AcceleratedSurface::create(webPage, [this] { frameComplete(); }, AcceleratedSurface::RenderingPurpose::Composited))
    , m_sceneState(&sceneState)
#if USE(TEXTURE_MAPPER)
    , m_flipY(!m_surface->shouldPaintMirrored())
#endif
    , m_renderTimer(m_workQueue->runLoop(), "ThreadedCompositor::RenderTimer"_s, this, &ThreadedCompositor::renderLayerTree)
{
    ASSERT(RunLoop::isMain());

    // Make sure rendering update is the highest priority task in worker loop.
    m_renderTimer.setPriority(RunLoopSourcePriority::RunLoopTimer - 1);

    lazyInitialize(m_didCompositeRunLoopObserver, makeUnique<RunLoopObserver>(RunLoopObserver::WellKnownOrder::GraphicsCommit, [this] {
        this->didCompositeRunLoopObserverFired();
    }));

    initializeFPSCounter();
#if ENABLE(DAMAGE_TRACKING)
#if USE(TEXTURE_MAPPER)
    m_damage.visualizer = TextureMapperDamageVisualizer::create();
#else
    // The margin TextureMapperDamageVisualizer takes means nothing here, since the overlay fills the
    // damage as a region, but the same variable enables both so it does not depend on the compositor.
    if (const auto* showDamageEnvvar = getenv("WEBKIT_SHOW_DAMAGE")) {
        if (auto value = parseInteger<unsigned>(StringView::fromLatin1(showDamageEnvvar)); value && *value)
            m_damage.showAccumulatedDamageOverlay = true;
    }
#endif
#endif

    updateSceneAttributes(webPage.size(), webPage.deviceScaleFactor());

    m_surface->didCreateCompositingRunLoop(m_workQueue->runLoop());

    m_workQueue->dispatchSync([this] {
        // GLNativeWindowType depends on the EGL implementation: reinterpret_cast works
        // for pointers (only if they are 64-bit wide and not for other cases), and static_cast for
        // numeric types (and when needed they get extended to 64-bit) but not for pointers. Using
        // a plain C cast expression in this one instance works in all cases.
        static_assert(sizeof(GLNativeWindowType) <= sizeof(uint64_t), "GLNativeWindowType must not be longer than 64 bits.");
        auto nativeSurfaceHandle = (GLNativeWindowType)m_surface->window();
        auto context = GLContext::create(PlatformDisplay::sharedDisplay(), nativeSurfaceHandle);
        if (!context || !context->makeContextCurrent()) {
            Locker locker { m_state.lock };
            m_state.state = State::Invalidated;
            return;
        }

        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_maxTextureSize);

#if USE(TEXTURE_MAPPER)
        m_context = WTF::move(context);
        m_textureMapper = TextureMapper::create();
#else
        PlatformDisplay::sharedDisplay().setSkiaGLContextForCurrentThread(WTF::move(context));
        m_threadSafeGrContext = PlatformDisplay::sharedDisplay().skiaGrContext()->threadSafeProxy();
#endif
    });
}

ThreadedCompositor::~ThreadedCompositor() = default;

uint64_t ThreadedCompositor::surfaceID() const
{
    ASSERT(RunLoop::isMain());
    return m_surface->surfaceID();
}

void ThreadedCompositor::invalidate()
{
    ASSERT(RunLoop::isMain());

    {
        Locker locker { m_state.lock };
        stopRenderTimer();
        m_state.didCompositeRenderingUpdateFunction = nullptr;
        m_state.state = State::Invalidated;
    }

    m_didCompositeRunLoopObserver->invalidate();
    m_workQueue->dispatchSync([this] {
#if OS(ANDROID)
        closePerformanceHintSession();
#endif
#if USE(TEXTURE_MAPPER)
        if (!m_context || !m_context->makeContextCurrent())
            return;
#endif

        // Update the scene at this point ensures the layers state are correctly propagated.
        flushCompositingState(CompositionReason::RenderingUpdate);

        m_sceneState->invalidateCommittedLayers();
#if USE(TEXTURE_MAPPER)
        m_textureMapper = nullptr;
#endif
        m_surface->willDestroyGLContext();
#if USE(TEXTURE_MAPPER)
        m_context = nullptr;
#else
        PlatformDisplay::sharedDisplay().setSkiaGLContextForCurrentThread(nullptr);
#endif
    });
    m_sceneState = nullptr;
    m_layerTreeHost = nullptr;
    m_workQueue->dispatchSync([this] {
        m_surface->willDestroyCompositingRunLoop();
    });
    m_surface = nullptr;
}

void ThreadedCompositor::startRenderTimer()
{
    assertIsHeld(m_state.lock);
    ASSERT(!m_state.isRenderTimerActive);
    m_state.isRenderTimerActive = true;
    updateRenderTimer();
}

void ThreadedCompositor::stopRenderTimer()
{
    assertIsHeld(m_state.lock);
    m_state.isRenderTimerActive = false;
    updateRenderTimer();
}

void ThreadedCompositor::updateRenderTimer()
{
    assertIsHeld(m_state.lock);

    // m_renderTimer is bound to the compositor thread's run loop (m_workQueue), so it must be started
    // and stopped there. The render timer's desired state is tracked synchronously under m_state.lock by
    // m_state.isRenderTimerActive and may be changed from the main thread (e.g. from suspend()/resume()/
    // invalidate()), so hop to the compositor thread to bring the timer in line with that state.
    if (!m_workQueue->runLoop().isCurrent()) {
        m_workQueue->dispatch([protectedThis = Ref { *this }] {
            Locker locker { protectedThis->m_state.lock };
            protectedThis->updateRenderTimer();
        });
        return;
    }

    if (m_state.isRenderTimerActive) {
        if (!m_renderTimer.isActive())
            m_renderTimer.startOneShot(0_s);
    } else
        m_renderTimer.stop();
}

bool ThreadedCompositor::isOnlyRenderingUpdatePendingAndWaitingForTiles() const
{
    assertIsHeld(m_state.lock);
    return m_state.reasons.containsOnly({ CompositionReason::RenderingUpdate }) && m_state.isWaitingForTiles;
}

void ThreadedCompositor::suspend()
{
    ASSERT(RunLoop::isMain());
    m_surface->visibilityDidChange(false);

    if (++m_suspendedCount > 1)
        return;

    Locker locker { m_state.lock };
    stopRenderTimer();
}

void ThreadedCompositor::resume()
{
    ASSERT(RunLoop::isMain());
    m_surface->visibilityDidChange(true);

    ASSERT(m_suspendedCount > 0);
    if (--m_suspendedCount > 0)
        return;

    Locker locker { m_state.lock };
    if (m_state.state == State::Scheduled && !isOnlyRenderingUpdatePendingAndWaitingForTiles())
        startRenderTimer();
}

bool ThreadedCompositor::isActive() const
{
    Locker locker { m_state.lock };
    return m_state.state != State::Idle && m_state.state != State::Invalidated;
}

void ThreadedCompositor::backgroundColorDidChange()
{
    ASSERT(RunLoop::isMain());
    m_surface->backgroundColorDidChange();
}

#if PLATFORM(WPE) && ENABLE(WPE_PLATFORM) && (USE(GBM) || OS(ANDROID))
void ThreadedCompositor::preferredBufferFormatsDidChange()
{
    ASSERT(RunLoop::isMain());
    m_surface->preferredBufferFormatsDidChange();
}
#endif

void ThreadedCompositor::pendingTilesDidChange()
{
    Locker locker { m_state.lock };
    if (!m_state.isWaitingForTiles)
        return;

    if (m_sceneState->pendingTiles())
        return;

    m_state.isWaitingForTiles = false;
    scheduleUpdateLocked();
}

void ThreadedCompositor::preRenderedTilesDidChange()
{
    m_workQueue->dispatch([this, protectedThis = Ref { *this }] {
        {
            Locker locker { m_state.lock };
            if (m_state.state == State::Invalidated)
                return;
        }

#if USE(TEXTURE_MAPPER)
        if (!m_context || !m_context->makeContextCurrent())
            return;
#endif

#if OS(ANDROID)
        notifyCompositorWorkloadSpike("pre-rendered tiles");
        auto start = MonotonicTime::now();
#endif
        WPE_TRACE_SCOPE("composite: pre-rendered tiles");
        // Keep processing pending tile updates for at most 2ms while no other composition is requested.
        auto deadline = MonotonicTime::now() + 2_ms;
        Function<bool()> shouldStopProcessingTiles = [&] -> bool {
            if (MonotonicTime::now() >= deadline)
                return true;

            Locker locker { m_state.lock };
            return m_state.isRenderTimerActive;
        };

        bool hasPendingPreRenderedTiles = m_sceneState->processLayersWithPreRenderedTiles(shouldStopProcessingTiles);
        PlatformDisplay::sharedDisplay().skiaGrContext()->flushAndSubmit(GrSyncCpu::kNo);
#if OS(ANDROID)
        m_workSinceLastFrame += MonotonicTime::now() - start;
#endif
        if (hasPendingPreRenderedTiles)
            preRenderedTilesDidChange();
    });
}

void ThreadedCompositor::setSize(const IntSize& size, float deviceScaleFactor)
{
    ASSERT(RunLoop::isMain());
    Locker locker { m_attributes.lock };
    updateSceneAttributes(size, deviceScaleFactor);
}

#if ENABLE(DAMAGE_TRACKING)
void ThreadedCompositor::setDamagePropagationSettings(std::optional<OptionSet<DamagePropagationFlags>> flags, unsigned rectangleThreshold)
{
    m_damage.flags = flags;

    rectangleThreshold = Damage::clampRectangleThreshold(rectangleThreshold);
    if (m_surface) {
        m_surface->setFrameDamageRectangleThreshold(rectangleThreshold);
        m_surface->setDamageUsedForCompositing(damageUsedForCompositing());
    }
}

bool ThreadedCompositor::damageUsedForCompositing() const
{
    // Only the Skia compositor draws just the damage rects. TextureMapper scissors with the damage
    // bounds, which needs neither fine-grained records nor swap-chain accumulation.
#if USE(TEXTURE_MAPPER)
    return false;
#else
    return m_damage.flags && m_damage.flags->contains(DamagePropagationFlags::UseForCompositing);
#endif
}

bool ThreadedCompositor::drawsOverlay() const
{
    // The damage overlay is drawn by paintToSkiaCanvas(), the visualizer by paintToTextureMapper(), so
    // each path only has its own. Both draw over the damage itself, and damaging what they drew would
    // feed their own rects back into what they show next, growing until they cover the surface. So the
    // frame repaints in full instead. The FPS counter has no such loop and damages its own box.
#if USE(TEXTURE_MAPPER)
    return !!m_damage.visualizer;
#else
    return m_damage.showAccumulatedDamageOverlay;
#endif
}

void ThreadedCompositor::enableFrameDamageNotificationForTesting()
{
    m_damage.shouldNotifyFrameDamageForTesting = true;
}
#endif

void ThreadedCompositor::flushCompositingState(const OptionSet<CompositionReason>& reasons)
{
    if (reasons.hasExactlyOneBitSet() && reasons.contains(CompositionReason::Animation))
        return;

#if ASSERT_ENABLED
    {
        Locker locker { m_state.lock };
        ASSERT(!reasons.contains(CompositionReason::RenderingUpdate) || !m_state.isWaitingForTiles);
    }
#endif

    if (m_sceneState->flushCompositingState(reasons))
        preRenderedTilesDidChange();
}

TargetContents ThreadedCompositor::paintToCurrentGLContext(const TransformationMatrix& matrix, const IntSize& size, const OptionSet<CompositionReason>& reasons)
{
#if USE(TEXTURE_MAPPER)
    paintToTextureMapper(matrix, size, reasons);
    return TargetContents::Valid;
#else
    return paintToSkiaCanvas(matrix, size, reasons);
#endif
}

#if USE(TEXTURE_MAPPER)
void ThreadedCompositor::paintToTextureMapper(const TransformationMatrix& matrix, const IntSize& size, const OptionSet<CompositionReason>& reasons)
{
    FloatRect clipRect(FloatPoint { }, size);
    TextureMapperLayer& currentRootLayer = m_sceneState->rootLayer().ensureTarget();
    if (currentRootLayer.transform() != matrix)
        currentRootLayer.setTransform(matrix);

    bool sceneHasRunningAnimations = currentRootLayer.applyAnimationsRecursively(MonotonicTime::now());

    m_textureMapper->beginPainting(m_flipY ? TextureMapper::FlipY::Yes : TextureMapper::FlipY::No);
    m_textureMapper->beginClip(TransformationMatrix(), FloatRoundedRect(clipRect));

#if ENABLE(DAMAGE_TRACKING)
    std::optional<FloatRoundedRect> rectContainingRegionThatActuallyChanged;
    currentRootLayer.prepareForPainting(*m_textureMapper);
    if (m_damage.flags) {
        Damage frameDamage(size, m_damage.flags->contains(DamagePropagationFlags::Unified) ? Damage::Mode::BoundingBox : Damage::Mode::Rectangles);

        WTFBeginSignpost(this, CollectDamage);
        currentRootLayer.collectDamage(*m_textureMapper, frameDamage);
        WTFEndSignpost(this, CollectDamage);

        recordFrameDamage(WTF::move(frameDamage));

        if (m_damage.flags->contains(DamagePropagationFlags::UseForCompositing)) {
            if (drawsOverlay()) {
                // No damage means repaint the whole frame, so skip the scissor below too.
                m_textureMapper->setDamage(std::nullopt);
            } else {
                const auto& damageSinceLastSurfaceUse = m_surface->renderTargetDamage();
                if (damageSinceLastSurfaceUse && !FloatRect(damageSinceLastSurfaceUse->bounds()).contains(clipRect))
                    rectContainingRegionThatActuallyChanged = FloatRoundedRect(damageSinceLastSurfaceUse->bounds());

                m_textureMapper->setDamage(damageSinceLastSurfaceUse);
            }
        }
    }

    if (rectContainingRegionThatActuallyChanged)
        m_textureMapper->beginClip(TransformationMatrix(), *rectContainingRegionThatActuallyChanged);
#endif

    m_surface->clear(reasons);

    WTFBeginSignpost(this, PaintTextureMapperLayerTree);
    currentRootLayer.paint(*m_textureMapper);
    WTFEndSignpost(this, PaintTextureMapperLayerTree);

#if ENABLE(DAMAGE_TRACKING)
    if (rectContainingRegionThatActuallyChanged)
        m_textureMapper->endClip();
#endif

#if ENABLE(DAMAGE_TRACKING)
    if (m_damage.visualizer) {
        m_damage.visualizer->paintDamage(*m_textureMapper, m_surface->frameDamage());
        // When damage visualizer is active, we cannot send the original damage to the platform as in this case
        // the damage rects visualized previous frame may not get erased if platform actually uses damage.
        m_surface->setFrameDamageForPlatformOnly(Damage(size, Damage::Mode::Full));
    }
#endif

    m_textureMapper->endClip();
    m_textureMapper->endPainting();

    if (sceneHasRunningAnimations)
        requestComposition(CompositionReason::Animation);
}

#else

#if ENABLE(DAMAGE_TRACKING)
static void drawDamageOverlay(SkCanvas& canvas, const Damage& damage, const IntSize& size, SkColor color)
{
    SkPaint paint;
    paint.setStyle(SkPaint::kFill_Style);
    paint.setColor(color);

    // Drawn as a region, so the translucent overlay composites once even where the rects touch. No region
    // means the damage covers the whole surface, which the overlay has to show as such.
    if (auto damageRegion = SkiaDamageRegion::create(damage, size))
        damageRegion->fillCanvasInDeviceSpace(canvas, paint);
    else
        canvas.drawPaint(paint);
}
#endif

TargetContents ThreadedCompositor::paintToSkiaCanvas(const TransformationMatrix& matrix, const IntSize& size, const OptionSet<CompositionReason>& reasons)
{
    auto* canvas = m_surface->canvas();
    if (!canvas)
        return TargetContents::Invalid;

    auto& rootLayer = m_sceneState->rootLayer().ensureTarget();
    rootLayer.setTransform(matrix);

    // paint() collects this frame's damage into frameDamage, and limits the draw to what this target must
    // redraw: priorTargetDamage - what changed since the target was last current, read before this frame
    // is recorded - folded with frameDamage. An unset priorTargetDamage repaints the whole target.
    std::optional<Damage> frameDamage;
    std::optional<Damage> priorTargetDamage;
    const std::optional<SkColor> clearColor = m_surface->skiaClearColor(reasons);

#if ENABLE(DAMAGE_TRACKING)
    // Also collect for the accumulated-damage overlay, so it works with the compositing feature off.
    if (m_damage.flags || m_damage.showAccumulatedDamageOverlay)
        frameDamage = Damage(size, m_damage.flags && m_damage.flags->contains(DamagePropagationFlags::Unified) ? Damage::Mode::BoundingBox : Damage::Mode::Rectangles);

    // No layer damages the counter's box, so seed it here, before paint() collects.
    if (frameDamage && m_fpsCounter.drawsFPS)
        frameDamage->add(takeFPSCounterDamage());

    // Read the target's record before this frame is recorded into it.
    if (damageUsedForCompositing() && !drawsOverlay())
        priorTargetDamage = m_surface->renderTargetDamage();
#endif

    canvas->save();
    const bool hasRunningAnimations = rootLayer.paint(*canvas, frameDamage, priorTargetDamage, clearColor);
    canvas->restore();

#if ENABLE(DAMAGE_TRACKING)
    // Record into every target, so each one repaints this frame's damage the next time it is used.
    if (frameDamage)
        recordFrameDamage(WTF::move(*frameDamage));

    if (m_damage.showAccumulatedDamageOverlay) {
        if (const auto& damage = m_surface->renderTargetDamage(); damage && !damage->isEmpty()) {
            drawDamageOverlay(*canvas, *damage, size, SkColorSetARGB(128, 0, 255, 0));

            // Schedule one more composition, so that once damage stops the next frame repaints the
            // overlay away instead of leaving it frozen on an idle page.
            requestComposition(CompositionReason::Animation);
        }
    }

    // An overlay is redrawn every frame and is in no layer's damage, so tell the platform the whole
    // surface changed. Otherwise it may leave the previous frame's overlay on screen.
    if (drawsOverlay())
        m_surface->setFrameDamageForPlatformOnly(Damage(size, Damage::Mode::Full));
#endif

    if (m_fpsCounter.drawsFPS)
        drawFPSCounter(*canvas);

    if (auto* surface = canvas->getSurface()) {
        // Where the frame's GPU work is actually handed to the driver. Split in two because
        // they fail differently: flush() turns the recorded draws into GPU commands, which is
        // where a program compile or a texture upload lands, while submit() only hands the
        // command buffer over, which is where driver queue back-pressure lands.
        auto* grContext = PlatformDisplay::sharedDisplay().skiaGrContext();

        int resourcesBefore = 0;
        size_t bytesBefore = 0;
        const bool tracing = WPE_TRACE_ENABLED();
        if (tracing)
            grContext->getResourceCacheUsage(&resourcesBefore, &bytesBefore);

        {
            WPE_TRACE_SCOPE("composite: flush");
            grContext->flush(surface);
        }
        {
            WPE_TRACE_SCOPE("composite: submit");
            grContext->submit(GrSyncCpu::kNo);
        }

        if (tracing) {
            int resourcesAfter = 0;
            size_t bytesAfter = 0;
            grContext->getResourceCacheUsage(&resourcesAfter, &bytesAfter);
            // A frame that allocates GPU resources is a frame that uploaded new content.
            WPE_TRACE_COUNTER("wpe.skia.gpu-resources-added", resourcesAfter - resourcesBefore);
            WPE_TRACE_COUNTER("wpe.skia.gpu-bytes-added", static_cast<int64_t>(bytesAfter) - static_cast<int64_t>(bytesBefore));
        }
    }

    if (hasRunningAnimations)
        requestComposition(CompositionReason::Animation);

    return TargetContents::Valid;
}
#endif

#if ENABLE(DAMAGE_TRACKING)
void ThreadedCompositor::recordFrameDamage(Damage&& damage)
{
    if (m_damage.shouldNotifyFrameDamageForTesting && m_layerTreeHost)
        m_layerTreeHost->notifyFrameDamageForTesting(damage.regionForTesting());

    // Nothing changed this frame: an empty damage contributes nothing to the targets, since Damage::add()
    // ignores empty damage. Skip it rather than replace the recorded frame damage with an empty one.
    if (damage.isEmpty())
        return;

    m_surface->setFrameDamage(WTF::move(damage));
}
#endif

#if HAVE(SIGNPOSTS)
static String reasonsToString(const OptionSet<CompositionReason>& reasons)
{
    StringBuilder builder;
    for (auto reason : reasons) {
        if (!builder.isEmpty())
            builder.append(", "_s);
        builder.append(enumName(reason));
    }
    return builder.toString();
}
#endif

void ThreadedCompositor::renderLayerTree()
{
    ASSERT(m_sceneState);
    ASSERT(m_workQueue->runLoop().isCurrent());
#if PLATFORM(GTK) || PLATFORM(WPE)
    TraceScope traceScope(RenderLayerTreeStart, RenderLayerTreeEnd);
#endif
#if OS(ANDROID)
    auto frameStart = MonotonicTime::now();
#endif

    if (m_suspendedCount > 0)
        return;

    OptionSet<CompositionReason> reasons;
    bool shouldNotifiyDidComposite = false;
#if OS(ANDROID)
    std::optional<MonotonicTime> frameRequestTime;
#endif
    {
        Locker locker { m_state.lock };

        if (m_state.state == State::Invalidated)
            return;

        // The timer has been stopped.
        if (!m_state.isRenderTimerActive)
            return;

        m_state.isRenderTimerActive = false;
        reasons = std::exchange(m_state.reasons, { });
#if OS(ANDROID)
        frameRequestTime = std::exchange(m_state.requestTime, std::nullopt);
#endif
        if (reasons.contains(CompositionReason::RenderingUpdate)) {
            if (m_state.isWaitingForTiles) {
                reasons.remove(CompositionReason::RenderingUpdate);
                m_state.reasons.add(CompositionReason::RenderingUpdate);
            } else
                shouldNotifiyDidComposite = !!m_state.didCompositeRenderingUpdateFunction;
        }

        ASSERT(m_state.state == State::Scheduled);
        m_state.state = State::InProgress;
    }

#if USE(TEXTURE_MAPPER)
    if (!m_context || !m_context->makeContextCurrent())
        return;
#endif

    // Retrieve the scene attributes in a thread-safe manner.
    IntSize viewportSize;
    float deviceScaleFactor;
    {
        Locker locker { m_attributes.lock };
        viewportSize = m_attributes.viewportSize;
        deviceScaleFactor = m_attributes.deviceScaleFactor;
    }

    if (viewportSize.isEmpty())
        return;

    TransformationMatrix viewportTransform;
    viewportTransform.scale(deviceScaleFactor);

    m_surface->willRenderFrame(viewportSize);

    RunLoop::mainSingleton().dispatch([this, protectedThis = Ref { *this }] {
        if (m_layerTreeHost)
            m_layerTreeHost->willRenderFrame();
    });

#if ENABLE(ASYNC_SCROLLING) && ENABLE(SCROLLING_THREAD)
    if (scrollAnimationsSampledPerComposite())
        WebProcess::singleton().eventDispatcher().serviceScrollAnimationsForComposite(m_pageID);
#endif

#if OS(ANDROID)
    // The vsync this frame belongs to: the one at or before the moment it could first start (asked for, and the
    // previous frame done, one frame being in flight at a time), taken before rendering. SurfaceFlinger takes a frame
    // on the vsync after it is sent; the frame-done message itself can wait behind tile work on this thread, so its
    // arrival would hide that wait.
    std::optional<MonotonicTime> previousFrameDone;
    if (m_lastFrameSentTime) {
        auto vsync = ThreadedScrollingTree::vsyncAtOrBefore(*m_lastFrameSentTime);
        auto period = ThreadedScrollingTree::displayRefreshPeriod();
        if (vsync && period)
            previousFrameDone = *vsync + *period;
    }
    auto frameReady = std::min(std::max(frameRequestTime.value_or(frameStart), previousFrameDone.value_or(MonotonicTime { })), frameStart);
    auto frameVSync = ThreadedScrollingTree::vsyncAtOrBefore(frameReady);
    boostPerformanceHintForScrollIfNeeded(reasons, frameStart);
#endif
    WTFBeginSignpost(this, FlushCompositingState);
    flushCompositingState(reasons);
    WTFEndSignpost(this, FlushCompositingState);

    WTFBeginSignpost(this, PaintToGLContext);
    const auto targetContents = paintToCurrentGLContext(viewportTransform, viewportSize, reasons);
    WTFEndSignpost(this, PaintToGLContext);

    updateFPSCounter();

    if (shouldNotifiyDidComposite)
        m_didCompositeRunLoopObserver->schedule(&RunLoop::mainSingleton());

    WTFEmitSignpost(this, DidRenderFrame, "reasons: %s", reasonsToString(reasons).utf8());

#if USE(TEXTURE_MAPPER)
    if (m_context)
        m_context->swapBuffers();
#else
    PlatformDisplay::sharedDisplay().skiaGLContext()->swapBuffers();
#endif

    m_surface->didRenderFrame(targetContents);
    m_surface->sendFrame();
#if OS(ANDROID)
    reportCompositorWork(MonotonicTime::now() - frameStart, frameVSync);
    m_lastFrameSentTime = MonotonicTime::now();
#endif

    RunLoop::mainSingleton().dispatch([this, protectedThis = Ref { *this }] {
        if (m_layerTreeHost)
            m_layerTreeHost->didRenderFrame();
    });

}

void ThreadedCompositor::requestCompositionForRenderingUpdate(Function<void()>&& didCompositeFunction)
{
    ASSERT(RunLoop::isMain());
    Locker locker { m_state.lock };
    m_state.reasons.add(CompositionReason::RenderingUpdate);
    ASSERT(!m_state.didCompositeRenderingUpdateFunction);
    m_state.didCompositeRenderingUpdateFunction = WTF::move(didCompositeFunction);
    if (m_sceneState->pendingTiles())
        m_state.isWaitingForTiles = true;
    scheduleUpdateLocked();
}

void ThreadedCompositor::requestComposition(CompositionReason reason)
{
    Locker locker { m_state.lock };
    m_state.reasons.add(reason);
    scheduleUpdateLocked();
}

ASCIILiteral ThreadedCompositor::stateToString(ThreadedCompositor::State state)
{
    switch (state) {
    case State::Idle:
        return "Idle"_s;
    case State::Scheduled:
        return "Scheduled"_s;
    case State::InProgress:
        return "InProgress"_s;
    case State::ScheduledWhileInProgress:
        return "ScheduledWhileInProgress"_s;
    case State::Invalidated:
        return "Invalidated"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ThreadedCompositor::scheduleUpdateLocked()
{
    assertIsHeld(m_state.lock);
#if OS(ANDROID)
    if (!m_state.requestTime)
        m_state.requestTime = MonotonicTime::now();
#endif
    WTFEmitSignpost(this, ScheduleComposition, "reasons: %s, state: %s, waiting for tiles: %s, render timer active: %s", reasonsToString(m_state.reasons).utf8(), stateToString(m_state.state).characters(), m_state.isWaitingForTiles ? "yes" : "no", m_state.isRenderTimerActive ? "yes" : "no");

    switch (m_state.state) {
    case State::Idle:
        m_state.state = State::Scheduled;
        if (!m_state.isWaitingForTiles && !m_suspendedCount.load())
            startRenderTimer();
        break;
    case State::Scheduled:
        if (!m_state.isRenderTimerActive && !m_suspendedCount.load())
            startRenderTimer();
        break;
    case State::InProgress:
        m_state.state = State::ScheduledWhileInProgress;
        break;
    case State::ScheduledWhileInProgress:
    case State::Invalidated:
        break;
    }
}

void ThreadedCompositor::frameComplete()
{
    ASSERT(m_workQueue->runLoop().isCurrent());

    Locker locker { m_state.lock };
    WTFEmitSignpost(this, FrameComplete, "reasons: %s, state: %s, waiting for tiles: %s", reasonsToString(m_state.reasons).utf8(), stateToString(m_state.state).characters(), m_state.isWaitingForTiles ? "yes" : "no");

    switch (m_state.state) {
    case State::Idle:
    case State::Scheduled:
    case State::Invalidated:
        break;
    case State::InProgress:
        if (m_state.reasons.contains(CompositionReason::RenderingUpdate) && m_state.isWaitingForTiles)
            m_state.state = State::Scheduled;
        else
            m_state.state = State::Idle;
        break;
    case State::ScheduledWhileInProgress:
        m_state.state = State::Scheduled;
        if (!isOnlyRenderingUpdatePendingAndWaitingForTiles() && !m_suspendedCount.load())
            startRenderTimer();
        break;
    }
}

RunLoop* ThreadedCompositor::runLoop()
{
    return m_surface ? &m_workQueue->runLoop() : nullptr;
}

void ThreadedCompositor::didCompositeRunLoopObserverFired()
{
    m_didCompositeRunLoopObserver->invalidate();
    Function<void()> didCompositeFunction;
    {
        Locker locker { m_state.lock };
        didCompositeFunction = std::exchange(m_state.didCompositeRenderingUpdateFunction, nullptr);
    }
    if (didCompositeFunction)
        didCompositeFunction();
}

void ThreadedCompositor::updateSceneAttributes(const IntSize& size, float deviceScaleFactor)
{
    m_attributes.viewportSize = size;
    m_attributes.deviceScaleFactor = deviceScaleFactor;
    m_attributes.viewportSize.scale(m_attributes.deviceScaleFactor);
}

void ThreadedCompositor::initializeFPSCounter()
{
    // When the envvar is set, the FPS is logged to the console, so it may be necessary to enable the
    // 'LogsPageMessagesToSystemConsole' runtime preference to see it.
    const auto showFPSEnvironment = String::fromLatin1(getenv("WEBKIT_SHOW_FPS"));
    bool ok = false;
    Seconds interval(showFPSEnvironment.toDouble(&ok));
    if (ok && interval) {
        m_fpsCounter.exposesFPS = true;
        m_fpsCounter.calculationInterval = interval;
    }

    // WEBKIT_DRAW_FPS=1 additionally renders the FPS as an on-screen overlay,
    // reusing the calculation interval (which WEBKIT_SHOW_FPS may override).
    if (const auto* drawFPSEnvironment = getenv("WEBKIT_DRAW_FPS")) {
        if (auto enabled = parseInteger<unsigned>(StringView::fromLatin1(drawFPSEnvironment)); enabled && *enabled) {
            m_fpsCounter.exposesFPS = true;
            m_fpsCounter.drawsFPS = true;
        }
    }
}

void ThreadedCompositor::updateFPSCounter()
{
    if (!m_fpsCounter.exposesFPS
#if USE(SYSPROF_CAPTURE)
        && !SysprofAnnotator::singletonIfCreated()
#endif
    )
        return;

    m_fpsCounter.frameCountSinceLastCalculation++;
    const Seconds delta = MonotonicTime::now() - m_fpsCounter.lastCalculationTimestamp;
    if (delta >= m_fpsCounter.calculationInterval) {
        m_fpsCounter.lastFPS = static_cast<int>(std::round(m_fpsCounter.frameCountSinceLastCalculation / delta.seconds()));
        WTFSetCounter(FPS, m_fpsCounter.lastFPS);
        if (m_fpsCounter.exposesFPS)
            m_fpsCounter.fps = m_fpsCounter.frameCountSinceLastCalculation / delta.seconds();
        m_fpsCounter.frameCountSinceLastCalculation = 0;
        m_fpsCounter.lastCalculationTimestamp += delta;
    } else if (m_fpsCounter.exposesFPS)
        m_fpsCounter.fps = std::nullopt;
}

static const SkFont& fpsCounterFont()
{
    static SkFont font = [] {
        constexpr unsigned defaultFontSize = 14;
        unsigned fontSize = defaultFontSize;
        if (const auto* fontSizeEnvvar = getenv("WEBKIT_DRAW_FPS_FONT_SIZE")) {
            if (auto value = parseInteger<unsigned>(StringView::fromLatin1(fontSizeEnvvar)); value && *value)
                fontSize = *value;
        }
        auto typeface = FontCache::forCurrentThread().fontManager().matchFamilyStyle("monospace", SkFontStyle::Bold());
        SkFont f(typeface, fontSize);
        f.setEdging(SkFont::Edging::kAntiAlias);
        f.setSubpixel(true);
        return f;
    }();
    return font;
}

// Scale the box padding with the font size so the overlay stays proportionate at large
// WEBKIT_DRAW_FPS_FONT_SIZE values (~3px at the default size of 14).
static float fpsCounterPadding()
{
    return fpsCounterFont().getSize() * 0.2f;
}

FloatRect ThreadedCompositor::fpsCounterRect() const
{
    return FloatRect(0, 0, m_fpsCounter.backgroundWidth, m_fpsCounter.backgroundHeight);
}

void ThreadedCompositor::updateFPSCounterGeometry()
{
    if (m_fpsCounter.lastFPS == m_fpsCounter.displayedFPS)
        return;

    m_fpsCounter.displayedFPS = m_fpsCounter.lastFPS;
    m_fpsCounter.fpsString = String::number(m_fpsCounter.lastFPS).ascii();

    const auto& font = fpsCounterFont();
    SkRect textBounds;
    font.measureText(m_fpsCounter.fpsString.data(), m_fpsCounter.fpsString.length(), SkTextEncoding::kUTF8, &textBounds);

    const float padding = fpsCounterPadding();
    m_fpsCounter.backgroundWidth = textBounds.width() + padding * 2;
    m_fpsCounter.backgroundHeight = textBounds.height() + padding * 2;
    m_fpsCounter.textBaseline = -textBounds.fTop + padding;
}

#if ENABLE(DAMAGE_TRACKING)
IntRect ThreadedCompositor::takeFPSCounterDamage()
{
    // Nothing to repaint on the frames between counts, and drawFPSCounter() redraws the box regardless.
    if (m_fpsCounter.lastFPS == m_fpsCounter.displayedFPS)
        return { };

    updateFPSCounterGeometry();

    // The box drawn last frame is damaged too, since it shrinks when the count gets shorter.
    auto damage = enclosingIntRect(fpsCounterRect());
    damage.unite(std::exchange(m_fpsCounter.lastDrawnRect, damage));
    return damage;
}
#endif

void ThreadedCompositor::drawFPSCounter(SkCanvas& canvas)
{
    updateFPSCounterGeometry();

    // Drawn in device space at the top-left corner, matching the debug repaint
    // counter style used by SkiaCompositingLayer.
    SkAutoCanvasRestore autoRestore(&canvas, true);
    canvas.resetMatrix();

    SkPaint backgroundPaint;
    backgroundPaint.setColor(SK_ColorBLACK);
    backgroundPaint.setStyle(SkPaint::kFill_Style);
    canvas.drawRect(SkRect(fpsCounterRect()), backgroundPaint);

    SkPaint textPaint;
    textPaint.setColor(SK_ColorWHITE);
    textPaint.setAntiAlias(true);
    canvas.drawString(m_fpsCounter.fpsString.data(), fpsCounterPadding(), m_fpsCounter.textBaseline, fpsCounterFont(), textPaint);
}

void ThreadedCompositor::fillGLInformation(RenderProcessInfo&& info, CompletionHandler<void(RenderProcessInfo&&)>&& completionHandler)
{
    m_workQueue->dispatchSync([protectedThis = Ref { *this }, info = WTF::move(info), completionHandler = WTF::move(completionHandler)]() mutable {
        info.glRenderer = String::fromUTF8(reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
        info.glVendor = String::fromUTF8(reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
        info.glVersion = String::fromUTF8(reinterpret_cast<const char*>(glGetString(GL_VERSION)));
        info.glShadingVersion = String::fromUTF8(reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)));
        info.glExtensions = String::fromUTF8(reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS)));

        auto eglDisplay = eglGetCurrentDisplay();
        info.eglVersion = String::fromUTF8(eglQueryString(eglDisplay, EGL_VERSION));
        info.eglVendor = String::fromUTF8(eglQueryString(eglDisplay, EGL_VENDOR));
        info.eglExtensions = makeString(unsafeSpan(eglQueryString(nullptr, EGL_EXTENSIONS)), ' ', unsafeSpan(eglQueryString(eglDisplay, EGL_EXTENSIONS)));

        RunLoop::mainSingleton().dispatch([info = WTF::move(info), completionHandler = WTF::move(completionHandler)]() mutable {
            completionHandler(WTF::move(info));
        });
    });
}

void ThreadedCompositor::releaseMemory(WTF::Critical critical)
{
    m_workQueue->dispatchSync([protectedThis = Ref { *this }, critical] {
        PlatformDisplay::sharedDisplay().skiaReleaseUnusedResources(critical);
    });
}

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
