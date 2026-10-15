/*
 * Copyright (C) 2019 Apple Inc. All rights reserved.
 * Copyright (C) 2019, 2021, 2024 Igalia S.L.
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
#include "ScrollingTreeScrollingNodeDelegateCoordinated.h"

#if ENABLE(ASYNC_SCROLLING) && USE(COORDINATED_GRAPHICS)
#include "CoordinatedPlatformLayer.h"
#include "ScrollerCoordinated.h"
#include "ScrollingTreeCoordinated.h"
#include "ScrollingStateFrameScrollingNode.h"
#include "ScrollingTreeOverflowScrollingNode.h"
#include "ThreadedScrollingTree.h"
#include <wtf/SystemTracing.h>

#if OS(ANDROID)
#include <sys/system_properties.h>
#endif

namespace WebCore {

// debug.wpe.scroll.devicepixelsnap=0 rounds scroll positions to whole CSS pixels again. With
// device pixels, the main thread lays fixed elements out against the same fractional viewport
// (see AsyncScrollingCoordinator::reconcileScrollingState()), or they would shimmer.
static bool snapScrollPositionToDevicePixels()
{
#if OS(ANDROID)
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = { };
        return !(__system_property_get("debug.wpe.scroll.devicepixelsnap", value) > 0 && !strcmp(value, "0"));
    }();
    return enabled;
#else
    return false;
#endif
}

// debug.wpe.scroll.inputresampling=0 applies touch drag deltas as they arrive.
static bool inputResamplingEnabled()
{
#if OS(ANDROID)
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = { };
        return !(__system_property_get("debug.wpe.scroll.inputresampling", value) > 0 && !strcmp(value, "0"));
    }();
    return enabled;
#else
    return false;
#endif
}

// debug.wpe.scroll.resamplelead=<ms> moves the resampling target that much closer to the frame time.
// The composite samples a few milliseconds after the vsync its frame time is anchored to, so samples
// newer than "one sample period before" have often arrived; a constant lead keeps the target spacing
// even and cuts the lag, as long as it stays below that sampling delay (beyond it, frames predict).
static Seconds inputResamplingLead()
{
#if OS(ANDROID)
    static const Seconds lead = [] {
        char value[PROP_VALUE_MAX] = { };
        if (__system_property_get("debug.wpe.scroll.resamplelead", value) > 0)
            return Seconds::fromMilliseconds(std::clamp(atof(value), 0.0, 16.0));
        return 0_s;
    }();
    return lead;
#else
    return 0_s;
#endif
}

// debug.wpe.scroll.resamplesmooth=0 lets the resampling target follow each frame's time as it comes.
static bool inputResamplingTargetSmoothed()
{
#if OS(ANDROID)
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = { };
        return !(__system_property_get("debug.wpe.scroll.resamplesmooth", value) > 0 && !strcmp(value, "0"));
    }();
    return enabled;
#else
    return false;
#endif
}

FloatPoint ScrollingTreeScrollingNodeDelegateCoordinated::adjustedScrollPosition(const FloatPoint& position) const
{
    // Rounding to whole CSS pixels leaves a scroll animation only multiples of the device
    // scale factor to move by: at 2.625 a slow fling alternates 2.6 and 5.2 physical pixels per
    // frame. Rounding to the pixel grid the tiles are painted on keeps the contents sharp and
    // moves in steps of one physical pixel, as Chrome does.
    if (snapScrollPositionToDevicePixels()) {
        if (auto* layer = static_cast<CoordinatedPlatformLayer*>(scrollingNode()->scrolledContentsLayer())) {
            float scale = layer->contentsScaleForScrolling();
            if (scale > 1)
                return { roundf(position.x() * scale) / scale, roundf(position.y() * scale) / scale };
        }
    }
    return ThreadedScrollingTreeScrollingNodeDelegate::adjustedScrollPosition(position);
}

ScrollingTreeScrollingNodeDelegateCoordinated::ScrollingTreeScrollingNodeDelegateCoordinated(ScrollingTreeScrollingNode& scrollingNode, bool scrollAnimatorEnabled)
    : ThreadedScrollingTreeScrollingNodeDelegate(scrollingNode)
    , m_scrollAnimatorEnabled(scrollAnimatorEnabled)
#if USE(COORDINATED_GRAPHICS_ASYNC_SCROLLBAR)
    , m_scrollerPair(ScrollerPairCoordinated::create(scrollingNode))
#endif
{
    ASSERT(isMainThread());
#if USE(COORDINATED_GRAPHICS_ASYNC_SCROLLBAR)
    if (is<ScrollingTreeOverflowScrollingNode>(scrollingNode) && ScrollbarTheme::theme().usesOverlayScrollbars()) {
        protect(m_scrollerPair->horizontalScroller())->setOverlayScrollbarEnabled(true);
        protect(m_scrollerPair->verticalScroller())->setOverlayScrollbarEnabled(true);
    }
#endif
}

ScrollingTreeScrollingNodeDelegateCoordinated::~ScrollingTreeScrollingNodeDelegateCoordinated() = default;

void ScrollingTreeScrollingNodeDelegateCoordinated::updateVisibleLengths()
{
    m_scrollController.contentsSizeChanged();
#if USE(COORDINATED_GRAPHICS_ASYNC_SCROLLBAR)
    m_scrollerPair->updateValues();
#endif
}

bool ScrollingTreeScrollingNodeDelegateCoordinated::handleWheelEvent(const PlatformWheelEvent& wheelEvent)
{
    auto deferrer = ScrollingTreeWheelEventTestMonitorCompletionDeferrer { *scrollingTree(), scrollingNode()->scrollingNodeID(), WheelEventTestMonitor::DeferReason::HandlingWheelEvent };

    updateUserScrollInProgressForEvent(wheelEvent);

    // Touch drags arrive about once a frame, but within a millisecond of the composite that
    // shows them, so each frame showed zero, one or two of them: the page moved 24, 1, 25, 1 px
    // under a finger moving 13 px per frame. Instead, record the finger's movement and apply,
    // on every frame, where it was at that frame's time, as Chrome does.
    if (wheelEvent.hasPreciseScrollingDeltas())
        WPE_TRACE_COUNTER("wpe.scroll.wheel-age-us", (MonotonicTime::now() - wheelEvent.timestamp()).microsecondsAs<int64_t>());
    bool isDragStart = wheelEvent.phase() == PlatformWheelEventPhase::Began;
    bool resample = inputResamplingEnabled() && wheelEvent.hasPreciseScrollingDeltas()
        && (wheelEvent.phase() == PlatformWheelEventPhase::Changed || isDragStart) && wheelEvent.momentumPhase() == PlatformWheelEventPhase::None;
    if (resample && isDragStart) {
        // The first move of a drag carries everything since the finger went down (the touch
        // slop). Spread it over the frame before it rather than jumping by it, and give the
        // moves that follow something to interpolate from straight away.
        flushResampledInput();
        m_inputSamples.append({ wheelEvent.timestamp() - 16_ms, { }, wheelEvent.timestamp() - 16_ms, true });
    }
    bool isDragEnd = wheelEvent.phase() == PlatformWheelEventPhase::Ended && wheelEvent.momentumPhase() == PlatformWheelEventPhase::None;
    if (!resample && isDragEnd && !m_inputSamples.isEmpty()) {
        // The fling continues from what is on screen, one frame after the finger's last frame.
        // Jumping to the newest sample first (up to a frame of travel ahead or, when the frame
        // predicted past it, behind) and starting the fling at the current time, which the next
        // frame's vsync could precede, showed every release as a jump and then a stall.
        auto lastFrameTime = m_lastResampledFrameTime;
        endResampledInput();
        bool handled = m_scrollController.handleWheelEvent(wheelEvent);
        if (lastFrameTime && MonotonicTime::now() - *lastFrameTime < 50_ms)
            m_scrollController.setKineticAnimationStartTime(*lastFrameTime);
        return handled;
    }
    if (!resample) {
        // Anything else (the start of another gesture, a fling) starts from where the finger
        // really is.
        flushResampledInput();
        return m_scrollController.handleWheelEvent(wheelEvent);
    }

    m_resamplingEventTime = wheelEvent.timestamp();
    bool handled = m_scrollController.handleWheelEvent(wheelEvent);
    m_resamplingEventTime = std::nullopt;
    return handled;
}

void ScrollingTreeScrollingNodeDelegateCoordinated::immediateScrollBy(const FloatSize& delta, ScrollClamping clamping)
{
    if (!m_resamplingEventTime) {
        ThreadedScrollingTreeScrollingNodeDelegate::immediateScrollBy(delta, clamping);
        return;
    }

    m_receivedInput += delta;
    if (m_inputSamples.size() == 8)
        m_inputSamples.removeAt(0);
    m_inputSamples.append({ *m_resamplingEventTime, m_receivedInput, MonotonicTime::now() });
    // How old a drag sample is when the scrolling thread gets it, and where the finger is.
    WPE_TRACE_COUNTER("wpe.scroll.input-age-us", (MonotonicTime::now() - *m_resamplingEventTime).microsecondsAs<int64_t>());
    WPE_TRACE_COUNTER("wpe.scroll.input-total-y", static_cast<int64_t>(m_receivedInput.height() * 100));

    // Nothing has moved yet, so nothing asked for a frame; the frame is what applies it.
    if (RefPtr tree = scrollingTree())
        static_cast<ScrollingTreeCoordinated&>(*tree).requestCompositionForResampledInput();
}

bool ScrollingTreeScrollingNodeDelegateCoordinated::serviceResampledInput(MonotonicTime frameTime, bool forComposite)
{
    if (m_inputSamples.isEmpty())
        return false;

    // Android batches touch input once per frame and resamples it to 5 ms before the frame
    // time. That sample reaches this thread a few milliseconds after the frame starts, so a
    // frame could only show it by predicting, and prediction from a jittery finger made steps
    // uneven (and overshoot at the start of a drag, where the first move carries the touch
    // slop). Instead each frame shows the finger one frame period earlier, which the samples
    // that arrived before the frame started always cover, so it only ever interpolates. The
    // same frame of latency as Chrome, which also shows smooth drags.
    static constexpr Seconds inputLatency = 5_ms;
    size_t usable = 0;
    while (usable < m_inputSamples.size() && m_inputSamples[usable].arrival <= frameTime)
        ++usable;
    if (!usable)
        return true;
    // Samples come one per frame of the UI process, so their spacing is the frame period.
    // Until a drag has three samples, use the spacing measured in earlier drags: starting at 16 ms and
    // switching to the measured 8.3 ms of unbatched 120 Hz input moved the target 7.7 ms at once, which
    // showed as a catch-up step at the start of every flick.
    Seconds period = inputResamplingTargetSmoothed() && m_measuredInputPeriod ? *m_measuredInputPeriod : 16_ms;
    if (usable >= 3) {
        // The synthetic point at the start of a drag is 16 ms before the first sample: with smoothing on,
        // leave that interval out, or with three samples the median is 16 ms, not the real spacing.
        bool skipSynthetic = inputResamplingTargetSmoothed();
        Vector<Seconds, 8> intervals;
        for (size_t i = std::max<size_t>(usable, 5) - 4; i < usable; ++i) {
            if (i && !(skipSynthetic && m_inputSamples[i - 1].synthetic))
                intervals.append(m_inputSamples[i].time - m_inputSamples[i - 1].time);
        }
        if (!intervals.isEmpty()) {
            std::ranges::sort(intervals);
            period = std::clamp(intervals[intervals.size() / 2], 8_ms, 17_ms);
            m_measuredInputPeriod = period;
        }
    }
    auto target = frameTime + inputResamplingLead() - inputLatency - period;
    // Keep the target moving one display period per frame. A frame's time can jump back by several
    // milliseconds: the first frames of a drag after a pause sample at the current time because the vsync
    // clock has no grid yet, later ones at the grid. That frame then showed an older finger position than
    // the one before (a step of almost nothing, then a catch-up: the first stroke after a page load in
    // 5 of 6 runs). Move towards the frame's own target by at most a millisecond per frame instead.
    // Only the call right before a composite decides what a frame shows and advances the target; the
    // one on the display refresh of the same frame only keeps it from going back. Catching up (the
    // target behind where it should be) goes twice as fast as falling back, so a forward correction
    // doesn't keep the frames showing an older finger position for long.
    auto displayPeriod = ThreadedScrollingTree::displayRefreshPeriod();
    if (inputResamplingTargetSmoothed() && m_lastResampleTarget && m_lastCompositeFrameTime && displayPeriod && *displayPeriod > 0_s) {
        if (forComposite) {
            double frames = std::max(1.0, std::round((frameTime - *m_lastCompositeFrameTime) / *displayPeriod));
            auto expected = *m_lastResampleTarget + *displayPeriod * frames;
            target = std::clamp(target, expected - 1_ms, expected + 2_ms);
        } else
            target = std::max(target, *m_lastResampleTarget);
    }
    auto& last = m_inputSamples[usable - 1];
    FloatSize desired;
    if (target >= last.time)
        desired = last.total;
    else if (target <= m_inputSamples.first().time)
        desired = m_appliedInput;
    else {
        size_t i = 1;
        while (m_inputSamples[i].time < target)
            ++i;
        auto& a = m_inputSamples[i - 1];
        auto& b = m_inputSamples[i];
        float fraction = b.time > a.time ? static_cast<float>((target - a.time) / (b.time - a.time)) : 1;
        desired = a.total + (b.total - a.total) * fraction;
    }

    // Positive: the frame shows a finger position predicted past the newest sample.
    WPE_TRACE_COUNTER("wpe.scroll.resample-ahead-us", (target - last.time).microsecondsAs<int64_t>());
    WPE_TRACE_COUNTER("wpe.scroll.resample-target-age-us", (MonotonicTime::now() - target).microsecondsAs<int64_t>());
    WPE_TRACE_COUNTER("wpe.scroll.resample-behind-y", static_cast<int64_t>((m_receivedInput - desired).height() * 100));
    m_lastResampledFrameTime = frameTime;
    if (forComposite) {
        m_lastResampleTarget = target;
        m_lastCompositeFrameTime = frameTime;
    }
    auto delta = desired - m_appliedInput;
    if (!delta.isZero()) {
        ThreadedScrollingTreeScrollingNodeDelegate::immediateScrollBy(delta);
        m_appliedInput = desired;
    }
    return true;
}

void ScrollingTreeScrollingNodeDelegateCoordinated::endResampledInput()
{
    WPE_TRACE_COUNTER("wpe.scroll.resample-dropped-y", static_cast<int64_t>((m_receivedInput - m_appliedInput).height() * 100));
    m_inputSamples.clear();
    m_receivedInput = { };
    m_appliedInput = { };
    m_lastResampledFrameTime = std::nullopt;
    m_lastResampleTarget = std::nullopt;
    m_lastCompositeFrameTime = std::nullopt;
}

void ScrollingTreeScrollingNodeDelegateCoordinated::flushResampledInput()
{
    if (m_inputSamples.isEmpty())
        return;
    auto remaining = m_receivedInput - m_appliedInput;
    WPE_TRACE_COUNTER("wpe.scroll.resample-flush-y", static_cast<int64_t>(remaining.height() * 100));
    m_inputSamples.clear();
    m_receivedInput = { };
    m_appliedInput = { };
    m_lastResampledFrameTime = std::nullopt;
    m_lastResampleTarget = std::nullopt;
    m_lastCompositeFrameTime = std::nullopt;
    if (!remaining.isZero())
        ThreadedScrollingTreeScrollingNodeDelegate::immediateScrollBy(remaining);
}

#if USE(COORDINATED_GRAPHICS_ASYNC_SCROLLBAR)
void ScrollingTreeScrollingNodeDelegateCoordinated::updateFromStateNode(const ScrollingStateScrollingNode& scrollingStateNode)
{
    ASSERT(isMainThread());
    CheckedRef horizontalScroller = m_scrollerPair->horizontalScroller();
    CheckedRef verticalScroller = m_scrollerPair->verticalScroller();

    if (scrollingStateNode.hasChangedProperty(ScrollingStateNode::Property::PainterForScrollbar)) {
        horizontalScroller->setScrollerImp(scrollingStateNode.horizontalScrollerImp());
        verticalScroller->setScrollerImp(scrollingStateNode.verticalScrollerImp());
    }

    if (scrollingStateNode.hasChangedProperty(ScrollingStateNode::Property::ScrollbarHoverState)) {
        auto hoverState = scrollingStateNode.scrollbarHoverState();
        verticalScroller->setHoveredAndPressedParts(hoverState.hoveredPartInVerticalScrollbar, hoverState.pressedPartInVerticalScrollbar);
        horizontalScroller->setHoveredAndPressedParts(hoverState.hoveredPartInHorizontalScrollbar, hoverState.pressedPartInHorizontalScrollbar);
    }

    if (scrollingStateNode.hasChangedProperty(ScrollingStateNode::Property::HorizontalScrollbarLayer))
        horizontalScroller->setHostLayer(static_cast<CoordinatedPlatformLayer*>(scrollingStateNode.horizontalScrollbarLayer()));

    if (scrollingStateNode.hasChangedProperty(ScrollingStateNode::Property::VerticalScrollbarLayer))
        verticalScroller->setHostLayer(static_cast<CoordinatedPlatformLayer*>(scrollingStateNode.verticalScrollbarLayer()));

    if (scrollingStateNode.hasChangedProperty(ScrollingStateNode::Property::ScrollbarEnabledState)) {
        auto scrollbarEnabledState = scrollingStateNode.scrollbarEnabledState();
        horizontalScroller->setEnabled(scrollbarEnabledState.horizontalScrollbarIsEnabled);
        verticalScroller->setEnabled(scrollbarEnabledState.verticalScrollbarIsEnabled);
    }

    if (scrollingStateNode.hasChangedProperty(ScrollingStateNode::Property::ScrollbarLayoutDirection)) {
        auto scrollbarLayoutDirection = scrollingStateNode.scrollbarLayoutDirection();
        horizontalScroller->setScrollbarLayoutDirection(scrollbarLayoutDirection);
        verticalScroller->setScrollbarLayoutDirection(scrollbarLayoutDirection);
    }

    if (scrollingStateNode.hasChangedProperty(ScrollingStateNode::Property::UseDarkAppearanceForScrollbars)) {
        bool useDarkAppearanceForScrollbars = scrollingStateNode.useDarkAppearanceForScrollbars();
        horizontalScroller->setUseDarkAppearance(useDarkAppearanceForScrollbars);
        verticalScroller->setUseDarkAppearance(useDarkAppearanceForScrollbars);
    }

    if (scrollingStateNode.hasChangedProperty(ScrollingStateNode::Property::OverlayScrollbarsEnabled)) {
        if (auto* scrollingStateFrameScrollingNode = dynamicDowncast<ScrollingStateFrameScrollingNode>(&scrollingStateNode)) {
            auto overlayScrollbarsEnabled = scrollingStateFrameScrollingNode->overlayScrollbarsEnabled();
            horizontalScroller->setOverlayScrollbarEnabled(overlayScrollbarsEnabled);
            verticalScroller->setOverlayScrollbarEnabled(overlayScrollbarsEnabled);
        }
    }

    if (scrollingStateNode.hasChangedProperty(ScrollingStateNode::Property::ScrollbarOpacity)) {
        float scrollbarOpacity = scrollingStateNode.scrollbarOpacity();
        horizontalScroller->setOpacity(scrollbarOpacity);
        verticalScroller->setOpacity(scrollbarOpacity);
    }

    if (scrollingStateNode.hasChangedProperty(ScrollingStateNode::Property::ScrollbarColor)) {
        horizontalScroller->setScrollbarColor(scrollingStateNode.scrollbarColor());
        verticalScroller->setScrollbarColor(scrollingStateNode.scrollbarColor());
    }

    ThreadedScrollingTreeScrollingNodeDelegate::updateFromStateNode(scrollingStateNode);
}
#endif

} // namespace WebCore

#endif // ENABLE(ASYNC_SCROLLING) && USE(COORDINATED_GRAPHICS)
