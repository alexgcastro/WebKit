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

#pragma once

#include "ScrollingTreeScrollingNodeDelegate.h"

#if ENABLE(ASYNC_SCROLLING) && USE(COORDINATED_GRAPHICS)
#include "ThreadedScrollingTreeScrollingNodeDelegate.h"

namespace WebCore {

class ScrollerPairCoordinated;

class ScrollingTreeScrollingNodeDelegateCoordinated final : public ThreadedScrollingTreeScrollingNodeDelegate {
public:
    explicit ScrollingTreeScrollingNodeDelegateCoordinated(ScrollingTreeScrollingNode&, bool scrollAnimatorEnabled);
    virtual ~ScrollingTreeScrollingNodeDelegateCoordinated();

    void updateVisibleLengths();
    bool handleWheelEvent(const PlatformWheelEvent&);

    // Applies the finger movement of a touch drag as of the given frame time. Returns true
    // while there is resampled input still to apply.
    bool serviceResampledInput(MonotonicTime frameTime, bool forComposite) final;

private:
#if USE(COORDINATED_GRAPHICS_ASYNC_SCROLLBAR)
    // ThreadedScrollingTreeScrollingNodeDelegate
    void updateFromStateNode(const ScrollingStateScrollingNode&) final;
#endif

    FloatPoint adjustedScrollPosition(const FloatPoint&) const final;

    // ScrollingEffectsControllerClient.
    bool scrollAnimationEnabled() const final { return m_scrollAnimatorEnabled && !m_resamplingEventTime; }
    void immediateScrollBy(const FloatSize&, ScrollClamping = ScrollClamping::Clamped) final;

    void flushResampledInput();
    void endResampledInput();

    struct InputSample {
        MonotonicTime time;
        FloatSize total;
        MonotonicTime arrival;
        // The point added at the start of a drag to interpolate from, not a real sample.
        bool synthetic { false };
    };
    Vector<InputSample, 8> m_inputSamples;
    FloatSize m_receivedInput;
    FloatSize m_appliedInput;
    std::optional<MonotonicTime> m_resamplingEventTime;
    std::optional<MonotonicTime> m_lastResampledFrameTime;
    std::optional<MonotonicTime> m_lastResampleTarget;
    std::optional<MonotonicTime> m_lastCompositeFrameTime;
    std::optional<Seconds> m_measuredInputPeriod;

    // The 1-euro filter Chrome applies to extrapolated scroll positions (ui/base/prediction/one_euro_filter):
    // a low-pass whose cutoff rises with speed, one per axis.
    struct OneEuroFilter {
        double filter(double value, double timeSeconds);
        void reset() { *this = { }; }
        double frequency { 60 };
        std::optional<double> lastTime;
        std::optional<double> filtered;
        double filteredDerivative { 0 };
    };
    OneEuroFilter m_positionFilterX;
    OneEuroFilter m_positionFilterY;
    FloatSize m_receivedInputAtLastComposite;
    FloatSize m_lastRawMovement;
    // Composites sample the input (debug.wpe.scroll.samplepercomposite, on by default); set on the first such call.
    bool m_compositesSampleInput { false };

    bool m_scrollAnimatorEnabled { false };
#if USE(COORDINATED_GRAPHICS_ASYNC_SCROLLBAR)
    const Ref<ScrollerPairCoordinated> m_scrollerPair;
#endif
};

} // namespace WebCore

#endif // ENABLE(ASYNC_SCROLLING) && USE(COORDINATED_GRAPHICS)
