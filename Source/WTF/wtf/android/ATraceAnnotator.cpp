/*
 * Copyright (C) 2026 Igalia S.L.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include <wtf/android/ATraceAnnotator.h>

#if OS(ANDROID)

#include <algorithm>
#include <array>
#include <android/trace.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <sys/system_properties.h>
#include <wtf/SystemTracing.h>

namespace WTF {

// Every TracePointCode, so a slice name can be derived from the enumerator itself instead
// of a hand-written table that silently goes stale when upstream adds a code.
#define WTF_TRACE_POINT_CODE_LIST(M) \
    M(VMEntryScopeStart) \
    M(VMEntryScopeEnd) \
    M(WebAssemblyCompileStart) \
    M(WebAssemblyCompileEnd) \
    M(WebAssemblyExecuteStart) \
    M(WebAssemblyExecuteEnd) \
    M(DumpJITMemoryStart) \
    M(DumpJITMemoryStop) \
    M(FromJSStart) \
    M(FromJSStop) \
    M(IncrementalSweepStart) \
    M(IncrementalSweepEnd) \
    M(MainResourceLoadDidStartProvisional) \
    M(MainResourceLoadDidEnd) \
    M(SubresourceLoadWillStart) \
    M(SubresourceLoadDidEnd) \
    M(FetchCookiesStart) \
    M(FetchCookiesEnd) \
    M(StyleRecalcStart) \
    M(StyleRecalcEnd) \
    M(RenderTreeBuildStart) \
    M(RenderTreeBuildEnd) \
    M(PerformLayoutStart) \
    M(PerformLayoutEnd) \
    M(PaintLayerStart) \
    M(PaintLayerEnd) \
    M(AsyncImageDecodeStart) \
    M(AsyncImageDecodeEnd) \
    M(RAFCallbackStart) \
    M(RAFCallbackEnd) \
    M(MemoryPressureHandlerStart) \
    M(MemoryPressureHandlerEnd) \
    M(UpdateTouchRegionsStart) \
    M(UpdateTouchRegionsEnd) \
    M(DisplayListRecordStart) \
    M(DisplayListRecordEnd) \
    M(DisplayRefreshDispatchingToMainThread) \
    M(ComputeEventRegionsStart) \
    M(ComputeEventRegionsEnd) \
    M(ScheduleRenderingUpdate) \
    M(TriggerRenderingUpdate) \
    M(RenderingUpdateStart) \
    M(RenderingUpdateEnd) \
    M(CompositingUpdateStart) \
    M(CompositingUpdateEnd) \
    M(DispatchTouchEventsStart) \
    M(DispatchTouchEventsEnd) \
    M(ParseHTMLStart) \
    M(ParseHTMLEnd) \
    M(DisplayListReplayStart) \
    M(DisplayListReplayEnd) \
    M(ScrollingThreadRenderUpdateSyncStart) \
    M(ScrollingThreadRenderUpdateSyncEnd) \
    M(ScrollingThreadDisplayDidRefreshStart) \
    M(ScrollingThreadDisplayDidRefreshEnd) \
    M(ScrollingTreeDisplayDidRefresh) \
    M(RenderTreeLayoutStart) \
    M(RenderTreeLayoutEnd) \
    M(PerformOpportunisticallyScheduledTasksStart) \
    M(PerformOpportunisticallyScheduledTasksEnd) \
    M(WebXRLayerStartFrameStart) \
    M(WebXRLayerStartFrameEnd) \
    M(WebXRLayerEndFrameStart) \
    M(WebXRLayerEndFrameEnd) \
    M(WebXRSessionFrameCallbacksStart) \
    M(WebXRSessionFrameCallbacksEnd) \
    M(ProgrammaticScroll) \
    M(FixedContainerEdgeSamplingStart) \
    M(FixedContainerEdgeSamplingEnd) \
    M(ThreadTimersStart) \
    M(ThreadTimersEnd) \
    M(TimerFiredStart) \
    M(TimerFiredEnd) \
    M(CoreImageRenderStart) \
    M(CoreImageRenderEnd) \
    M(WebHTMLViewPaintStart) \
    M(WebHTMLViewPaintEnd) \
    M(BackingStoreFlushStart) \
    M(BackingStoreFlushEnd) \
    M(BuildTransactionStart) \
    M(BuildTransactionEnd) \
    M(SyncMessageStart) \
    M(SyncMessageEnd) \
    M(SyncTouchEventStart) \
    M(SyncTouchEventEnd) \
    M(InitializeWebProcessStart) \
    M(InitializeWebProcessEnd) \
    M(RenderingUpdateRunLoopObserverStart) \
    M(RenderingUpdateRunLoopObserverEnd) \
    M(LayerTreeFreezeStart) \
    M(LayerTreeFreezeEnd) \
    M(FlushRemoteImageBufferStart) \
    M(FlushRemoteImageBufferEnd) \
    M(CreateInjectedBundleStart) \
    M(CreateInjectedBundleEnd) \
    M(PaintSnapshotStart) \
    M(PaintSnapshotEnd) \
    M(RenderServerSnapshotStart) \
    M(RenderServerSnapshotEnd) \
    M(TakeSnapshotStart) \
    M(TakeSnapshotEnd) \
    M(SyntheticMomentumStart) \
    M(SyntheticMomentumEnd) \
    M(SyntheticMomentumEvent) \
    M(RemoteLayerTreeScheduleRenderingUpdate) \
    M(DisplayLinkUpdate) \
    M(ProcessInitializeStart) \
    M(ProcessInitializeEnd) \
    M(CommitLayerTreeStart) \
    M(CommitLayerTreeEnd) \
    M(ProcessLaunchStart) \
    M(ProcessLaunchEnd) \
    M(InitializeSandboxStart) \
    M(InitializeSandboxEnd) \
    M(WebXRCPFrameWaitStart) \
    M(WebXRCPFrameWaitEnd) \
    M(WebXRCPFrameStartSubmissionStart) \
    M(WebXRCPFrameStartSubmissionEnd) \
    M(WebXRCPFrameEndSubmissionStart) \
    M(WebXRCPFrameEndSubmissionEnd) \
    M(TextExtractionStart) \
    M(TextExtractionEnd) \
    M(RemoteLayerTreeAnimationsUpdateStart) \
    M(RemoteLayerTreeAnimationsUpdateEnd) \
    M(WakeUpAndApplyDisplayListStart) \
    M(WakeUpAndApplyDisplayListEnd) \
    M(LayerTreeHostRenderingUpdateStart) \
    M(LayerTreeHostRenderingUpdateEnd) \
    M(WaitForCompositionCompletionStart) \
    M(WaitForCompositionCompletionEnd) \
    M(RenderLayerTreeStart) \
    M(RenderLayerTreeEnd) \
    M(UpdateLayerContentBuffersStart) \
    M(UpdateLayerContentBuffersEnd)

namespace {

enum class SliceKind : uint8_t { Begin, End, Instant };

enum class Coverage : uint8_t { Off, Scroll, Layers, Shaders, All };

} // namespace

static const char* rawCodeName(unsigned code)
{
    switch (static_cast<TracePointCode>(code)) {
#define WTF_TRACE_POINT_CODE_NAME(name) case name: return #name;
    WTF_TRACE_POINT_CODE_LIST(WTF_TRACE_POINT_CODE_NAME)
#undef WTF_TRACE_POINT_CODE_NAME
    default:
        break;
    }
    return nullptr;
}

// The subset that says something about a scroll: what the main thread is busy with, when
// the scrolling thread is blocked on it, and when a frame actually reaches the compositor.
static bool isScrollRelevant(unsigned code)
{
    switch (static_cast<TracePointCode>(code)) {
    case StyleRecalcStart:
    case StyleRecalcEnd:
    case RenderTreeBuildStart:
    case RenderTreeBuildEnd:
    case PerformLayoutStart:
    case PerformLayoutEnd:
    case RenderTreeLayoutStart:
    case RenderTreeLayoutEnd:
    case PaintLayerStart:
    case PaintLayerEnd:
    case RAFCallbackStart:
    case RAFCallbackEnd:
    case UpdateTouchRegionsStart:
    case UpdateTouchRegionsEnd:
    case ComputeEventRegionsStart:
    case ComputeEventRegionsEnd:
    case ScheduleRenderingUpdate:
    case TriggerRenderingUpdate:
    case RenderingUpdateStart:
    case RenderingUpdateEnd:
    case CompositingUpdateStart:
    case CompositingUpdateEnd:
    case DispatchTouchEventsStart:
    case DispatchTouchEventsEnd:
    case ParseHTMLStart:
    case ParseHTMLEnd:
    case ScrollingThreadRenderUpdateSyncStart:
    case ScrollingThreadRenderUpdateSyncEnd:
    case ScrollingThreadDisplayDidRefreshStart:
    case ScrollingThreadDisplayDidRefreshEnd:
    case ScrollingTreeDisplayDidRefresh:
    case ProgrammaticScroll:
    case PerformOpportunisticallyScheduledTasksStart:
    case PerformOpportunisticallyScheduledTasksEnd:
    case ThreadTimersStart:
    case ThreadTimersEnd:
    case TimerFiredStart:
    case TimerFiredEnd:
    case IncrementalSweepStart:
    case IncrementalSweepEnd:
    case SyncTouchEventStart:
    case SyncTouchEventEnd:
    case SyncMessageStart:
    case SyncMessageEnd:
    case LayerTreeHostRenderingUpdateStart:
    case LayerTreeHostRenderingUpdateEnd:
    case WaitForCompositionCompletionStart:
    case WaitForCompositionCompletionEnd:
    case RenderLayerTreeStart:
    case RenderLayerTreeEnd:
    case UpdateLayerContentBuffersStart:
    case UpdateLayerContentBuffersEnd:
        return true;
    default:
        return false;
    }
}

// ThreadedScrollingTree opens this span on the main thread and closes it on the scrolling
// thread, so it cannot use a thread-scoped slice.
static bool isCrossThreadSpan(unsigned code)
{
    return code == ScrollingThreadRenderUpdateSyncStart || code == ScrollingThreadRenderUpdateSyncEnd;
}

static Coverage readCoverage()
{
    char value[PROP_VALUE_MAX] = { };
    if (__system_property_get("debug.wpe.atrace", value) <= 0)
        return Coverage::Scroll;
    if (!strcmp(value, "off") || !strcmp(value, "0"))
        return Coverage::Off;
    if (!strcmp(value, "all"))
        return Coverage::All;
    if (!strcmp(value, "layers"))
        return Coverage::Layers;
    if (!strcmp(value, "shaders"))
        return Coverage::Shaders;
    return Coverage::Scroll;
}

static Coverage coverage()
{
    static Coverage coverage = readCoverage();
    return coverage;
}

// The enumerators are named <Span><Start|Begin|End|Stop>, so the span name and whether the
// code opens or closes it both fall out of the suffix.
static SliceKind sliceKindAndName(std::string_view rawName, std::span<char> buffer)
{
    auto store = [&](std::string_view name) {
        size_t bytes = std::min(name.length(), buffer.size() - 1);
        for (size_t i = 0; i < bytes; ++i)
            buffer[i] = name[i];
        buffer[bytes] = '\0';
    };

    auto withoutSuffix = [&](size_t suffixLength) {
        return rawName.substr(0, rawName.length() - suffixLength);
    };

    if (rawName.ends_with("Start") || rawName.ends_with("Begin")) {
        store(withoutSuffix(5));
        return SliceKind::Begin;
    }
    if (rawName.ends_with("End")) {
        store(withoutSuffix(3));
        return SliceKind::End;
    }
    if (rawName.ends_with("Stop")) {
        store(withoutSuffix(4));
        return SliceKind::End;
    }
    store(rawName);
    return SliceKind::Instant;
}

bool ATraceAnnotator::isEnabled()
{
    return coverage() != Coverage::Off && ATrace_isEnabled();
}

bool ATraceAnnotator::shaderTracingRequested()
{
    return coverage() == Coverage::Shaders;
}

bool ATraceAnnotator::layersEnabled()
{
    auto requested = coverage();
    return (requested == Coverage::Layers || requested == Coverage::All) && ATrace_isEnabled();
}

ATraceFormattedScope::ATraceFormattedScope(bool enabled, const char* format, ...)
{
    if (!enabled)
        return;

    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    ATrace_beginSection(buffer);
    m_active = true;
}

ATraceFormattedScope::~ATraceFormattedScope()
{
    if (m_active)
        ATrace_endSection();
}

int32_t ATraceAnnotator::nextCookie()
{
    static std::atomic<int32_t> cookie { 1 };
    return cookie.fetch_add(1, std::memory_order_relaxed) & 0x7fffffff;
}

void ATraceAnnotator::begin(const char* name)
{
    if (!isEnabled())
        return;
    ATrace_beginSection(name);
}

void ATraceAnnotator::beginFormatted(const char* format, ...)
{
    if (!isEnabled())
        return;

    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    ATrace_beginSection(buffer);
}

void ATraceAnnotator::end()
{
    if (!isEnabled())
        return;
    ATrace_endSection();
}

void ATraceAnnotator::beginAsync(const char* name, int32_t cookie)
{
    if (!isEnabled())
        return;
    ATrace_beginAsyncSection(name, cookie);
}

void ATraceAnnotator::endAsync(const char* name, int32_t cookie)
{
    if (!isEnabled())
        return;
    ATrace_endAsyncSection(name, cookie);
}

void ATraceAnnotator::instant(const char* format, ...)
{
    if (!isEnabled())
        return;

    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    // ATrace has no instant event, so a zero-length slice stands in for one.
    ATrace_beginSection(buffer);
    ATrace_endSection();
}

void ATraceAnnotator::counter(const char* name, int64_t value)
{
    if (!isEnabled())
        return;
    ATrace_setCounter(name, value);
}

void ATraceAnnotator::tracePoint(unsigned code, uint64_t data1, uint64_t, uint64_t, uint64_t)
{
    auto requested = coverage();
    if (requested == Coverage::Off || !ATrace_isEnabled())
        return;
    if (requested != Coverage::All && !isScrollRelevant(code))
        return;

    const char* rawName = rawCodeName(code);
    if (!rawName)
        return;

    std::array<char, 128> name;
    auto kind = sliceKindAndName(rawName, std::span { name });

    if (isCrossThreadSpan(code)) {
        // Only one of these is ever in flight per process, so a fixed cookie is enough.
        if (kind == SliceKind::Begin)
            ATrace_beginAsyncSection(name.data(), 0);
        else {
            ATrace_endAsyncSection(name.data(), 0);
            if (code == ScrollingThreadRenderUpdateSyncEnd) {
                // data1 is set when the scrolling thread gave up waiting for the main thread.
                ATrace_setCounter("wpe.scroll.desynchronized", data1 ? 1 : 0);
                if (data1) {
                    ATrace_beginSection("scroll: desynchronized, main thread rendering update too slow");
                    ATrace_endSection();
                }
            }
        }
        return;
    }

    switch (kind) {
    case SliceKind::Begin:
        ATrace_beginSection(name.data());
        break;
    case SliceKind::End:
        ATrace_endSection();
        break;
    case SliceKind::Instant:
        ATrace_beginSection(name.data());
        ATrace_endSection();
        break;
    }
}

} // namespace WTF

#endif // OS(ANDROID)
