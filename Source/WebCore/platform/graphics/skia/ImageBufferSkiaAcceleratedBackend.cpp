/*
 * Copyright (C) 2024, 2025 Igalia S.L.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ImageBufferSkiaAcceleratedBackend.h"

#if USE(SKIA)
#include "FontRenderOptions.h"
#include "GLContext.h"
#include "GLFence.h"
#include "GraphicsContextSkia.h"
#include "IntRect.h"
#include "NativeImage.h"
#include "PixelBuffer.h"
#include "PixelBufferConversion.h"
#include "PlatformDisplay.h"
#include "ProcessCapabilities.h"
#include "SkiaRecordingResult.h"
#include "SkiaUtilities.h"
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkPixmap.h>
#include <skia/gpu/ganesh/GrBackendSurface.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
#include <skia/utils/SkNWayCanvas.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#include <wtf/TZoneMallocInlines.h>

#if USE(COORDINATED_GRAPHICS)
#if USE(TEXTURE_MAPPER)
#include "CoordinatedPlatformLayerBufferNativeImage.h"
#else
#include "CoordinatedPlatformLayerBufferSkiaImage.h"
#include <epoxy/gl.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/android/SkCanvasAndroid.h>
#include <skia/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <skia/gpu/ganesh/gl/GrGLTypes.h>
#include <skia/private/chromium/GrPromiseImageTexture.h>
#include <skia/private/chromium/SkImageChromium.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/ThreadSafeRefCounted.h>
#endif
#include "GraphicsLayerContentsDisplayDelegateCoordinated.h"
#endif

namespace WebCore {

// A canvas proxy that delegates all drawing operations to a single target canvas,
// which can be dynamically switched. This allows GraphicsContextSkia to hold a
// reference to this canvas while the actual target (recording vs surface) changes.
class SkiaSwitchableCanvas final : public SkNWayCanvas {
WTF_MAKE_TZONE_ALLOCATED(SkiaSwitchableCanvas);
public:
    explicit SkiaSwitchableCanvas(const IntSize& size)
        : SkNWayCanvas(size.width(), size.height())
    {
    }

    void switchToCanvas(SkCanvas* canvas)
    {
        SkNWayCanvas::removeAll();
        if (canvas)
            SkNWayCanvas::addCanvas(canvas);
    }
};

#if USE(COORDINATED_GRAPHICS) && !USE(TEXTURE_MAPPER)
class CompositorBufferPool final : public ThreadSafeRefCounted<CompositorBufferPool> {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(CompositorBufferPool);
public:
    static constexpr unsigned maximumBufferCount = 4;

    static Ref<CompositorBufferPool> create(const IntSize& size)
    {
        return adoptRef(*new CompositorBufferPool(size));
    }

    ~CompositorBufferPool()
    {
        auto* glContext = PlatformDisplay::sharedDisplay().skiaGLContext();
        if (!glContext || !glContext->makeContextCurrent())
            return;
        for (auto& buffer : m_buffers) {
            if (buffer.texture)
                glDeleteTextures(1, &buffer.texture);
        }
    }

    const IntSize& size() const { return m_size; }

    struct Lease {
        unsigned index { 0 };
        unsigned texture { 0 };
        std::unique_ptr<GLFence> releaseFence;
    };

    std::optional<Lease> acquire()
    {
        Locker locker { m_lock };
        while (true) {
            for (unsigned i = 0; i < m_buffers.size(); ++i) {
                auto& buffer = m_buffers[i];
                if (buffer.inUse)
                    continue;
                buffer.inUse = true;
                return Lease { i, buffer.texture, WTF::move(buffer.releaseFence) };
            }
            if (m_buffers.size() < maximumBufferCount) {
                unsigned texture = 0;
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_size.width(), m_size.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                m_buffers.append({ texture, true, nullptr });
                return Lease { static_cast<unsigned>(m_buffers.size() - 1), texture, nullptr };
            }
            if (!m_condition.waitFor(m_lock, 100_ms))
                return std::nullopt;
        }
    }

    void release(unsigned index, std::unique_ptr<GLFence>&& releaseFence)
    {
        Locker locker { m_lock };
        auto& buffer = m_buffers[index];
        buffer.inUse = false;
        buffer.releaseFence = WTF::move(releaseFence);
        m_condition.notifyAll();
    }

private:
    explicit CompositorBufferPool(const IntSize& size)
        : m_size(size)
    {
    }

    struct Buffer {
        unsigned texture { 0 };
        bool inUse { false };
        std::unique_ptr<GLFence> releaseFence;
    };

    const IntSize m_size;
    Lock m_lock;
    Condition m_condition;
    Vector<Buffer, maximumBufferCount> m_buffers WTF_GUARDED_BY_LOCK(m_lock);
};

struct CompositorBufferPromiseContext {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(CompositorBufferPromiseContext);

    Ref<CompositorBufferPool> pool;
    CompositorBufferPool::Lease lease;
    std::unique_ptr<GLFence> readyFence;

    sk_sp<GrPromiseImageTexture> fulfill()
    {
        auto* glContext = PlatformDisplay::sharedDisplay().skiaGLContext();
        if (!glContext || !glContext->makeContextCurrent())
            return nullptr;
        if (readyFence) {
            readyFence->serverWait();
            readyFence = nullptr;
        }
        GrGLTextureInfo textureInfo;
        textureInfo.fTarget = GL_TEXTURE_2D;
        textureInfo.fID = lease.texture;
        textureInfo.fFormat = GL_RGBA8;
        const auto& size = pool->size();
        return GrPromiseImageTexture::Make(GrBackendTextures::MakeGL(size.width(), size.height(), skgpu::Mipmapped::kNo, textureInfo));
    }

    void release()
    {
        std::unique_ptr<GLFence> releaseFence;
        auto* glContext = PlatformDisplay::sharedDisplay().skiaGLContext();
        if (glContext && glContext->makeContextCurrent())
            releaseFence = GLFence::create(PlatformDisplay::sharedDisplay().glDisplay());
        pool->release(lease.index, WTF::move(releaseFence));
    }
};

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(CompositorBufferPromiseContext);
#endif

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImageBufferSkiaAcceleratedBackend);

static inline bool shouldEnableDynamicMSAA()
{
    static std::once_flag onceFlag;
    static bool enableDynamicMSAA = false;
    std::call_once(onceFlag, [] {
        if (const char* enableDynamicMSAAEnv = getenv("WEBKIT_SKIA_ENABLE_DYNAMIC_MSAA")) {
            enableDynamicMSAA = *enableDynamicMSAAEnv != '0';
            return;
        }

#if PLATFORM(GTK)
        enableDynamicMSAA = true;
#else
        enableDynamicMSAA = false;
#endif
    });
    return enableDynamicMSAA;
}

std::unique_ptr<ImageBufferSkiaAcceleratedBackend> ImageBufferSkiaAcceleratedBackend::create(const ImageBufferParameters& parameters, const ImageBufferCreationContext& creationContext)
{
    IntSize backendSize = calculateSafeBackendSize(parameters);
    if (backendSize.isEmpty())
        return nullptr;

    // We always want to accelerate the canvas when Accelerated2DCanvas setting is true, even if skia CPU is enabled.
    if (parameters.purpose != RenderingPurpose::Canvas && !ProcessCapabilities::canUseAcceleratedBuffers())
        return nullptr;

#if !PLATFORM(WIN)
    if (!PlatformDisplay::sharedDisplayIfExists())
        return nullptr;
#endif

    auto* glContext = PlatformDisplay::sharedDisplay().skiaGLContext();
    if (!glContext || !glContext->makeContextCurrent())
        return nullptr;

    auto* grContext = PlatformDisplay::sharedDisplay().skiaGrContext();
    RELEASE_ASSERT(grContext);

    auto imageInfo = SkImageInfo::Make(backendSize.width(), backendSize.height(), kRGBA_8888_SkColorType, kPremul_SkAlphaType, parameters.colorSpace.platformColorSpace());
    auto msaaSampleCount = PlatformDisplay::sharedDisplay().msaaSampleCount();
    uint32_t flags = 0;
    if (parameters.purpose == RenderingPurpose::Canvas && msaaSampleCount && shouldEnableDynamicMSAA()) {
        flags |= SkSurfaceProps::kDynamicMSAA_Flag;
        msaaSampleCount = 1;
    }
    SkSurfaceProps properties = FontRenderOptions::singleton().createSurfaceProps(flags);
    auto surface = SkSurfaces::RenderTarget(grContext, skgpu::Budgeted::kYes, imageInfo, msaaSampleCount, kTopLeft_GrSurfaceOrigin, &properties);
    if (!surface || !surface->getCanvas())
        return nullptr;

    return create(parameters, creationContext, WTF::move(surface));
}

std::unique_ptr<ImageBufferSkiaAcceleratedBackend> ImageBufferSkiaAcceleratedBackend::create(const ImageBufferParameters& parameters, const ImageBufferCreationContext&, sk_sp<SkSurface>&& surface)
{
    ASSERT(surface);
    ASSERT(surface->getCanvas());
    return std::unique_ptr<ImageBufferSkiaAcceleratedBackend>(new ImageBufferSkiaAcceleratedBackend(parameters, WTF::move(surface)));
}

ImageBufferSkiaAcceleratedBackend::ImageBufferSkiaAcceleratedBackend(const ImageBufferParameters& parameters, sk_sp<SkSurface>&& surface)
    : ImageBufferSkiaSurfaceBackend(parameters, WTF::move(surface), RenderingMode::Accelerated)
{
#if USE(COORDINATED_GRAPHICS)
    // Use a content layer for canvas.
    if (parameters.purpose == RenderingPurpose::Canvas)
        lazyInitialize(m_layerContentsDisplayDelegate, GraphicsLayerContentsDisplayDelegateCoordinated::create());
#endif
}

ImageBufferSkiaAcceleratedBackend::~ImageBufferSkiaAcceleratedBackend()
{
    // Unwind the surface context's save/restore stack before destruction
    if (m_canvasRecordingContext)
        m_canvasRecordingContext->unwindStateStack();
}

GraphicsContext& ImageBufferSkiaAcceleratedBackend::context()
{
    if (purpose() != RenderingPurpose::Canvas)
        return ImageBufferSkiaSurfaceBackend::context();

    ensureCanvasRecordingContext();
    return *m_canvasRecordingContext;
}

GrDirectContext* ImageBufferSkiaAcceleratedBackend::grContext() const
{
    auto* recordingContext = m_surface->recordingContext();
    return recordingContext ? recordingContext->asDirectContext() : nullptr;
}

void ImageBufferSkiaAcceleratedBackend::ensureCanvasRecordingContext()
{
    if (m_canvasRecordingContext)
        return;

    // Create a switchable canvas that will delegate to either recording or surface canvas.
    // GraphicsContextSkia holds a reference to this canvas, which never changes - only the
    // target canvas it delegates to changes.
    lazyInitialize(m_switchableCanvas, makeUnique<SkiaSwitchableCanvas>(size()));

    auto* recordingCanvas = m_pictureRecorder.beginRecording(size().width(), size().height());
    m_switchableCanvas->switchToCanvas(recordingCanvas);

    lazyInitialize(m_canvasRecordingContext, makeUnique<GraphicsContextSkia>(static_cast<SkCanvas&>(*m_switchableCanvas), RenderingMode::Accelerated, RenderingPurpose::Canvas));
    m_canvasRecordingContext->applyDeviceScaleFactor(resolutionScale());
    m_canvasRecordingContext->beginRecording(GraphicsContextSkia::RecordingMode::Canvas);
    m_hasActiveRecording = true;
}

void ImageBufferSkiaAcceleratedBackend::replayCanvasRecordingContextIfNeeded()
{
    if (!m_canvasRecordingContext || !m_hasActiveRecording)
        return;

    auto* grContext = this->grContext();
    if (!grContext)
        return;

    if (!PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent())
        return;

    ASSERT(grContext == PlatformDisplay::sharedDisplay().skiaGrContext());

    IntRect recordRect(IntPoint(), size());
    auto recordingData = m_canvasRecordingContext->endRecording();
    auto picture = m_pictureRecorder.finishRecordingAsPicture();

    RefPtr<SkiaRecordingResult> recording = SkiaRecordingResult::create(WTF::move(picture), WTF::move(recordingData), recordRect, RenderingMode::Accelerated, false, 1.0);

    // Save the surface canvas save count before playback so we can undo any
    // unbalanced saves that the picture introduces.
    auto* surfaceCanvas = m_surface->getCanvas();
    auto surfaceSaveCount = surfaceCanvas->getSaveCount();

#if USE(TEXTURE_MAPPER)
    ASSERT(!recording->hasFences());
#endif
    recording->picture()->playback(surfaceCanvas);

    // Undo unbalanced saves from the picture playback on the surface canvas.
    surfaceCanvas->restoreToCount(surfaceSaveCount);

    // Switch the switchable canvas to target the surface canvas, then replay
    // state-replay state to bring the surface into the correct save/clip/CTM nesting.
    m_switchableCanvas->switchToCanvas(surfaceCanvas);
    m_canvasRecordingContext->replayStateOnCanvas(*surfaceCanvas);

    m_hasActiveRecording = false;
}

void ImageBufferSkiaAcceleratedBackend::restartCanvasRecording()
{
    if (!m_canvasRecordingContext || m_hasActiveRecording)
        return;

    // Clean up state-replayed saves on the surface canvas before switching away.
    m_surface->getCanvas()->restoreToCount(1);

    auto* recordingCanvas = m_pictureRecorder.beginRecording(size().width(), size().height());
    m_switchableCanvas->switchToCanvas(recordingCanvas);

    // Replay state onto the new recording canvas to give it the exact same save/clip/CTM nesting.
    m_canvasRecordingContext->replayStateOnCanvas(*recordingCanvas);
    m_canvasRecordingContext->beginRecording(GraphicsContextSkia::RecordingMode::Canvas);
    m_hasActiveRecording = true;
}

void ImageBufferSkiaAcceleratedBackend::flushContext()
{
    replayCanvasRecordingContextIfNeeded();

    auto* grContext = this->grContext();
    if (!grContext)
        return;

    if (!PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent())
        return;

    ASSERT(grContext == PlatformDisplay::sharedDisplay().skiaGrContext());
    grContext->flushAndSubmit(m_surface.get(), GrSyncCpu::kNo);
}

void ImageBufferSkiaAcceleratedBackend::prepareForDisplay()
{
#if USE(COORDINATED_GRAPHICS)
    if (!m_layerContentsDisplayDelegate)
        return;

    auto image = createNativeImageReference();
    if (!image)
        return;

    auto* grContext = this->grContext();
    if (!grContext)
        return;

    if (!PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent())
        return;

    ASSERT(grContext == PlatformDisplay::sharedDisplay().skiaGrContext());

#if USE(TEXTURE_MAPPER)
    m_layerContentsDisplayDelegate->setDisplayBuffer(CoordinatedPlatformLayerBufferNativeImage::create(image.releaseNonNull(),
        SkiaUtilities::flushAndSubmitSurfaceWithFence(grContext, m_surface.get())));
#else
    if (auto threadSafeGrContext = m_layerContentsDisplayDelegate->threadSafeGrContext())
        m_layerContentsDisplayDelegate->setDisplayBuffer(CoordinatedPlatformLayerBufferSkiaImage::create(image->platformImage(), threadSafeGrContext));
#endif

    // Re-enable recording mode for subsequent drawing operations.
    // This allows batching to occur again after each prepareForDisplay() cycle.
    restartCanvasRecording();
#endif
}

RefPtr<NativeImage> ImageBufferSkiaAcceleratedBackend::copyNativeImage()
{
    // SkSurface uses a copy-on-write mechanism for makeImageSnapshot(), so it's
    // always safe to return the SkImage without copying.
    return createNativeImageReference();
}

RefPtr<NativeImage> ImageBufferSkiaAcceleratedBackend::createNativeImageReference()
{
    replayCanvasRecordingContextIfNeeded();

    auto* grContext = this->grContext();

    // If we're using MSAA, we need to flush the surface before calling makeImageSnapshot(),
    // because that call doesn't force the MSAA resolution, which can produce outdated results
    // in the resulting SkImage.
    auto& display = PlatformDisplay::sharedDisplay();
    if (grContext && display.msaaSampleCount() > 0 && display.skiaGLContext()->makeContextCurrent()) {
        ASSERT(grContext == display.skiaGrContext());
        grContext->flush(m_surface.get());
    }

    return NativeImage::create(m_surface->makeImageSnapshot(), grContext);
}

void ImageBufferSkiaAcceleratedBackend::getPixelBuffer(const IntRect& srcRect, PixelBuffer& destination)
{
    if (!PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent())
        return;

    // CPU needs to read pixels now, replay the recording.
    replayCanvasRecordingContextIfNeeded();

    ImageBufferSkiaSurfaceBackend::getPixelBuffer(srcRect, destination);
}

void ImageBufferSkiaAcceleratedBackend::putPixelBuffer(const PixelBufferSourceView& pixelBuffer, const IntRect& srcRect, const IntPoint& destPoint, AlphaPremultiplication destFormat)
{
    if (!PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent())
        return;

    // CPU needs to write pixels now, replay the recording.
    replayCanvasRecordingContextIfNeeded();

    ImageBufferSkiaSurfaceBackend::putPixelBuffer(pixelBuffer, srcRect, destPoint, destFormat);
}

#if USE(COORDINATED_GRAPHICS)
RefPtr<GraphicsLayerContentsDisplayDelegate> ImageBufferSkiaAcceleratedBackend::layerContentsDisplayDelegate() const
{
    return m_layerContentsDisplayDelegate;
}

#if !USE(TEXTURE_MAPPER)
std::unique_ptr<CoordinatedPlatformLayerBuffer> ImageBufferSkiaAcceleratedBackend::createCompositorDisplayBuffer(const sk_sp<GrContextThreadSafeProxy>& threadSafeGrContext)
{
    if (!threadSafeGrContext)
        return nullptr;

    auto& display = PlatformDisplay::sharedDisplay();
    auto* glContext = display.skiaGLContext();
    if (!glContext || !glContext->makeContextCurrent())
        return nullptr;

    auto* grContext = this->grContext();
    if (!grContext || grContext != display.skiaGrContext())
        return nullptr;

    replayCanvasRecordingContextIfNeeded();

    grContext->flush(GrFlushInfo { });

    auto renderTarget = skgpu::ganesh::TopLayerBackendRenderTarget(m_surface->getCanvas());
    GrGLFramebufferInfo framebufferInfo;
    if (!renderTarget.isValid() || renderTarget.sampleCnt() <= 1 || !GrBackendRenderTargets::GetGLFramebufferInfo(renderTarget, &framebufferInfo) || framebufferInfo.fFormat != GL_RGBA8)
        return nullptr;

    IntSize size(m_surface->width(), m_surface->height());
    if (!m_compositorBufferPool || m_compositorBufferPool->size() != size)
        m_compositorBufferPool = CompositorBufferPool::create(size);
    Ref pool = *m_compositorBufferPool;

    auto lease = pool->acquire();
    if (!lease) {
        grContext->resetContext(kRenderTarget_GrGLBackendState | kTextureBinding_GrGLBackendState | kView_GrGLBackendState);
        return nullptr;
    }
    if (lease->releaseFence) {
        lease->releaseFence->serverWait();
        lease->releaseFence = nullptr;
    }

    GLuint drawFramebuffer = 0;
    glGenFramebuffers(1, &drawFramebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFramebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lease->texture, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebufferInfo.fFBOID);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, size.width(), size.height(), 0, 0, size.width(), size.height(), GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &drawFramebuffer);
    auto readyFence = GLFence::create(display.glDisplay());
    grContext->resetContext(kRenderTarget_GrGLBackendState | kTextureBinding_GrGLBackendState | kView_GrGLBackendState);

    restartCanvasRecording();

    const auto& imageInfo = m_surface->imageInfo();
    auto backendFormat = threadSafeGrContext->defaultBackendFormat(kRGBA_8888_SkColorType, GrRenderable::kNo);
    auto* context = new CompositorBufferPromiseContext { WTF::move(pool), WTF::move(*lease), WTF::move(readyFence) };
    auto image = SkImages::PromiseTextureFrom(threadSafeGrContext, backendFormat, SkISize::Make(size.width(), size.height()), skgpu::Mipmapped::kNo,
        kTopLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType, imageInfo.alphaType(), imageInfo.refColorSpace(),
        +[](void* userData) -> sk_sp<GrPromiseImageTexture> {
            return static_cast<CompositorBufferPromiseContext*>(userData)->fulfill();
        },
        +[](void* userData) {
            std::unique_ptr<CompositorBufferPromiseContext> context(static_cast<CompositorBufferPromiseContext*>(userData));
            context->release();
        }, context);
    if (!image)
        return nullptr;

    using Buffer = CoordinatedPlatformLayerBufferSkiaImage;
    return makeUnique<Buffer>(WTF::move(image), imageInfo.alphaType() == kOpaque_SkAlphaType ? Buffer::AlphaMode::Opaque : Buffer::AlphaMode::Premultiplied, Buffer::Rotation::None);
}
#endif
#endif

} // namespace WebCore

#endif // USE(SKIA)
