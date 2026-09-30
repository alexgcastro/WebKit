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
    bool isDragStart = wheelEvent.phase() == PlatformWheelEventPhase::Began;
    bool resample = inputResamplingEnabled() && wheelEvent.hasPreciseScrollingDeltas()
        && (wheelEvent.phase() == PlatformWheelEventPhase::Changed || isDragStart) && wheelEvent.momentumPhase() == PlatformWheelEventPhase::None;
    if (resample && isDragStart) {
        // The first move of a drag carries everything since the finger went down (the touch
        // slop). Spread it over the frame before it rather than jumping by it, and give the
        // moves that follow something to interpolate from straight away.
        flushResampledInput();
        m_inputSamples.append({ wheelEvent.timestamp() - 16_ms, { } });
    }
    if (!resample) {
        // Anything else (the start or end of the gesture, a fling) starts from where the
        // finger really is.
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
    m_inputSamples.append({ *m_resamplingEventTime, m_receivedInput });

    // Nothing has moved yet, so nothing asked for a frame; the frame is what applies it.
    if (RefPtr tree = scrollingTree())
        static_cast<ScrollingTreeCoordinated&>(*tree).requestCompositionForResampledInput();
}

bool ScrollingTreeScrollingNodeDelegateCoordinated::serviceResampledInput(MonotonicTime frameTime)
{
    if (m_inputSamples.isEmpty())
        return false;

    // Android batches touch input once per frame and resamples it to 5 ms before the frame
    // time, so that is the point of the finger's path this frame should show. When the frame's
    // own sample has not arrived yet, extrapolate from the last two, by at most one frame.
    static constexpr Seconds inputLatency = 5_ms;
    static constexpr Seconds maximumPrediction = 17_ms;
    auto target = frameTime - inputLatency;
    auto& last = m_inputSamples.last();
    FloatSize desired;
    if (target >= last.time) {
        desired = last.total;
        if (m_inputSamples.size() >= 2) {
            auto& previous = m_inputSamples[m_inputSamples.size() - 2];
            auto interval = last.time - previous.time;
            if (interval > 0_s && interval <= 2 * maximumPrediction) {
                auto ahead = std::min(target - last.time, maximumPrediction);
                desired = last.total + (last.total - previous.total) * static_cast<float>(ahead / interval);
            }
        }
    } else if (target <= m_inputSamples.first().time)
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

    auto delta = desired - m_appliedInput;
    if (!delta.isZero()) {
        ThreadedScrollingTreeScrollingNodeDelegate::immediateScrollBy(delta);
        m_appliedInput = desired;
    }
    return true;
}

void ScrollingTreeScrollingNodeDelegateCoordinated::flushResampledInput()
{
    if (m_inputSamples.isEmpty())
        return;
    auto remaining = m_receivedInput - m_appliedInput;
    m_inputSamples.clear();
    m_receivedInput = { };
    m_appliedInput = { };
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
