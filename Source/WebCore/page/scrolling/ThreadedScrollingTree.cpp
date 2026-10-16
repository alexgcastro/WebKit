/*
 * Copyright (C) 2014-2015 Apple Inc. All rights reserved.
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
#include "ThreadedScrollingTree.h"

#if ENABLE(ASYNC_SCROLLING) && ENABLE(SCROLLING_THREAD)

#include "AnimationFrameRate.h"
#include "AsyncScrollingCoordinator.h"
#include "Logging.h"
#include "PlatformWheelEvent.h"
#include "ScrollingThread.h"
#include "ScrollingTreeFrameScrollingNode.h"
#include "ScrollingTreeNode.h"
#include "ScrollingTreeOverflowScrollProxyNode.h"
#include "ScrollingTreeScrollingNode.h"
#include <wtf/RunLoop.h>
#include <wtf/SetForScope.h>
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/TextStream.h>
#include <wtf/threads/BinarySemaphore.h>

#if OS(ANDROID)
#include <android/looper.h>
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Threading.h>

struct AChoreographer;
#endif

namespace WebCore {

#if OS(ANDROID)
// Vsync timing read straight from Android's Choreographer, on a looper thread of its own.
//
// Scroll animations used to be sampled at whatever time the display refresh or the composite
// happened to be handled, which jitters by a millisecond or two and shows up as uneven steps.
// Sampling at a point of the vsync grid instead gives every frame exactly one frame period of
// animation. The vsync that started the current frame is used, as Chrome does with the frame
// time of a BeginFrame: it is the same for the display refresh and the composite that produce
// one frame. (Sampling at the next vsync made the first step of a fling cover two frames.)
class AndroidVSyncClock {
    WTF_MAKE_NONCOPYABLE(AndroidVSyncClock);
public:
    static AndroidVSyncClock& singleton()
    {
        static NeverDestroyed<AndroidVSyncClock> clock;
        return clock.get();
    }

    // Keeps the clock ticking for a while, so the grid is known before an animation needs it.
    void keepAlive()
    {
        m_wantedUntilNanos.store(nowNanos() + 1'000'000'000);
        if (!m_callbackPending.load() && m_looper) {
            m_needsPost.store(true);
            m_functions.wake(m_looper);
        }
    }

    std::optional<Seconds> period() const
    {
        int64_t period = m_periodNanos.load();
        return period ? std::optional { Seconds::fromNanoseconds(period) } : std::nullopt;
    }

    std::optional<MonotonicTime> nextVSync(MonotonicTime now)
    {
        keepAlive();
        int64_t vsync = m_vsyncNanos.load();
        int64_t period = m_periodNanos.load();
        if (!vsync || !period)
            return std::nullopt;
        int64_t current = static_cast<int64_t>(now.secondsSinceEpoch().value() * 1e9);
        if (current < vsync || current - vsync > 4 * period)
            return std::nullopt;
        int64_t frames = (current - vsync) / period;
        return MonotonicTime::fromRawSeconds((vsync + frames * period) / 1e9);
    }

private:
    friend class NeverDestroyed<AndroidVSyncClock>;

    struct Functions {
        AChoreographer* (*getInstance)() { nullptr };
        void (*postFrameCallback64)(AChoreographer*, void (*)(int64_t, void*), void*) { nullptr };
        void (*wake)(ALooper*) { nullptr };
    };

    AndroidVSyncClock()
    {
        void* android = dlopen("libandroid.so", RTLD_NOW);
        if (!android)
            return;
        m_functions.getInstance = reinterpret_cast<decltype(m_functions.getInstance)>(dlsym(android, "AChoreographer_getInstance"));
        m_functions.postFrameCallback64 = reinterpret_cast<decltype(m_functions.postFrameCallback64)>(dlsym(android, "AChoreographer_postFrameCallback64"));
        m_functions.wake = reinterpret_cast<decltype(m_functions.wake)>(dlsym(android, "ALooper_wake"));
        if (!m_functions.getInstance || !m_functions.postFrameCallback64 || !m_functions.wake)
            return;

        BinarySemaphore started;
        Thread::create("WPE VSync"_s, [this, &started] {
            m_looper = ALooper_prepare(0);
            m_choreographer = m_functions.getInstance();
            started.signal();
            while (true) {
                ALooper_pollOnce(-1, nullptr, nullptr, nullptr);
                if (m_needsPost.exchange(false))
                    postIfNeeded();
            }
        }, ThreadType::Graphics, Thread::QOS::UserInteractive)->detach();
        started.wait();
    }

    static int64_t nowNanos()
    {
        return static_cast<int64_t>(MonotonicTime::now().secondsSinceEpoch().value() * 1e9);
    }

    void postIfNeeded()
    {
        if (m_callbackPending.load() || !m_choreographer)
            return;
        m_callbackPending.store(true);
        m_functions.postFrameCallback64(m_choreographer, +[](int64_t frameTimeNanos, void* data) {
            static_cast<AndroidVSyncClock*>(data)->didVSync(frameTimeNanos);
        }, this);
    }

    void didVSync(int64_t frameTimeNanos)
    {
        int64_t previous = m_vsyncNanos.exchange(frameTimeNanos);
        if (previous && frameTimeNanos > previous) {
            // Consecutive callbacks are one period apart unless a vsync was skipped, so take
            // the smallest recent interval, rounded against the occasional missed callback.
            int64_t interval = frameTimeNanos - previous;
            int64_t period = m_periodNanos.load();
            if (!period || interval < period * 3 / 2)
                m_periodNanos.store(period ? (period * 7 + interval) / 8 : interval);
        }
        m_callbackPending.store(false);
        if (nowNanos() < m_wantedUntilNanos.load())
            postIfNeeded();
    }

    Functions m_functions;
    ALooper* m_looper { nullptr };
    AChoreographer* m_choreographer { nullptr };
    std::atomic<int64_t> m_vsyncNanos { 0 };
    std::atomic<int64_t> m_periodNanos { 0 };
    std::atomic<int64_t> m_wantedUntilNanos { 0 };
    std::atomic<bool> m_callbackPending { false };
    std::atomic<bool> m_needsPost { false };
};

// debug.wpe.scroll.vsyncsampling=0 samples scroll animations at the time they are serviced.
static bool vsyncSamplingEnabled()
{
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = { };
        return !(__system_property_get("debug.wpe.scroll.vsyncsampling", value) > 0 && !strcmp(value, "0"));
    }();
    return enabled;
}
#endif

static MonotonicTime scrollAnimationTime()
{
    auto now = MonotonicTime::now();
#if OS(ANDROID)
    if (vsyncSamplingEnabled()) {
        if (auto vsync = AndroidVSyncClock::singleton().nextVSync(now)) {
            WPE_TRACE_COUNTER("wpe.scroll.sample-ahead-us", (*vsync - now).microsecondsAs<int64_t>());
            return *vsync;
        }
        WPE_TRACE_INSTANT("scroll: no vsync grid, sampling at the current time");
    }
#endif
    return now;
}

static void keepScrollAnimationClockAlive()
{
#if OS(ANDROID)
    if (vsyncSamplingEnabled())
        AndroidVSyncClock::singleton().keepAlive();
#endif
}

// debug.wpe.scroll.animationwaitsformain=1 restores the upstream behaviour, where a scroll
// animation is synchronized with main thread rendering updates and only reaches the screen
// once the main thread schedules one.
static bool scrollAnimationsNeedMainThreadRequested()
{
#if OS(ANDROID)
    static const bool requested = [] {
        char value[PROP_VALUE_MAX] = { };
        return __system_property_get("debug.wpe.scroll.animationwaitsformain", value) > 0 && !strcmp(value, "1");
    }();
    return requested;
#else
    return true;
#endif
}


WTF_MAKE_TZONE_ALLOCATED_IMPL(ThreadedScrollingTree);

ThreadedScrollingTree::ThreadedScrollingTree(AsyncScrollingCoordinator& scrollingCoordinator)
    : m_scrollingCoordinator(&scrollingCoordinator)
    , m_scrollAnimatorEnabled(scrollingCoordinator.scrollAnimatorEnabled())
{
}

ThreadedScrollingTree::~ThreadedScrollingTree()
{
    // invalidate() should have cleared m_scrollingCoordinator.
    ASSERT(!m_scrollingCoordinator);
}

WheelEventHandlingResult ThreadedScrollingTree::handleWheelEvent(const PlatformWheelEvent& wheelEvent, OptionSet<WheelEventProcessingSteps> processingSteps)
{
    ASSERT(ScrollingThread::isCurrentThread());
    return ScrollingTree::handleWheelEvent(wheelEvent, processingSteps);
}

bool ThreadedScrollingTree::handleWheelEventAfterMainThread(const PlatformWheelEvent& wheelEvent, ScrollingNodeID targetNodeID, std::optional<WheelScrollGestureState> gestureState)
{
    ASSERT(ScrollingThread::isCurrentThread());

    LOG_WITH_STREAM(Scrolling, stream << "ThreadedScrollingTree::handleWheelEventAfterMainThread " << wheelEvent << " node " << targetNodeID << " gestureState " << gestureState);

    Locker locker { m_treeLock };

    bool allowLatching = false;
    OptionSet<WheelEventProcessingSteps> processingSteps;
    if (gestureState.value_or(WheelScrollGestureState::Blocking) == WheelScrollGestureState::NonBlocking) {
        allowLatching = true;
        processingSteps = { WheelEventProcessingSteps::AsyncScrolling, WheelEventProcessingSteps::NonBlockingDOMEventDispatch };
    }

    SetForScope disallowLatchingScope(m_allowLatching, allowLatching);
    RefPtr<ScrollingTreeNode> targetNode = nodeForID(targetNodeID);
    auto result = handleWheelEventWithNode(wheelEvent, processingSteps, targetNode.get(), EventTargeting::NodeOnly);
    return result.wasHandled;
}

void ThreadedScrollingTree::wheelEventWasProcessedByMainThread(const PlatformWheelEvent& wheelEvent, std::optional<WheelScrollGestureState> gestureState)
{
    LOG_WITH_STREAM(Scrolling, stream << "ThreadedScrollingTree::wheelEventWasProcessedByMainThread - gestureState " << gestureState);

    ASSERT(isMainThread());
    
    Locker locker { m_treeLock };
    if (m_receivedBeganEventFromMainThread || !wheelEvent.isGestureStart())
        return;

    setGestureState(gestureState);

    m_receivedBeganEventFromMainThread = true;
    m_waitingForBeganEventCondition.notifyOne();
}

void ThreadedScrollingTree::willSendEventToMainThread(const PlatformWheelEvent&)
{
    ASSERT(ScrollingThread::isCurrentThread());

    Locker locker { m_treeLock };
    m_receivedBeganEventFromMainThread = false;
}

void ThreadedScrollingTree::waitForEventToBeProcessedByMainThread(const PlatformWheelEvent& wheelEvent)
{
    ASSERT(ScrollingThread::isCurrentThread());

    if (!wheelEvent.isGestureStart())
        return;

    Locker locker { m_treeLock };

    static constexpr auto maxAllowableMainThreadDelay = 50_ms;
    auto startTime = MonotonicTime::now();
    auto timeoutTime = startTime + maxAllowableMainThreadDelay;

    WPE_TRACE_BEGIN("scroll: gesture start blocked on main thread wheel handler");
    bool receivedEvent = m_waitingForBeganEventCondition.waitUntil(m_treeLock, timeoutTime, [&] {
        assertIsHeld(m_treeLock);
        return m_receivedBeganEventFromMainThread;
    });
    WPE_TRACE_END();

    auto waitDuration = MonotonicTime::now() - startTime;
    WPE_TRACE_COUNTER("wpe.scroll.gesture-start-wait-us", waitDuration.microsecondsAs<int64_t>());

    if (!receivedEvent) {
        // Timed out, go asynchronous.
        WPE_TRACE_INSTANT("scroll: gesture start timed out after %d ms, going non-blocking", static_cast<int>(waitDuration.millisecondsAs<int64_t>()));
        setGestureState(WheelScrollGestureState::NonBlocking);
    }

    LOG_WITH_STREAM(Scrolling, stream << "ThreadedScrollingTree::waitForBeganEventFromMainThread done - timed out " << !receivedEvent << " gesture state is " << gestureState());
}

void ThreadedScrollingTree::invalidate()
{
    // Invalidate is dispatched by the ScrollingCoordinator class on the ScrollingThread
    // to break the reference cycle between ScrollingTree and ScrollingCoordinator when the
    // ScrollingCoordinator's page is destroyed.
    ASSERT(ScrollingThread::isCurrentThread());

    Locker locker { m_treeLock };
    
    removeAllNodes();
    m_delayedRenderingUpdateDetectionTimer = nullptr;

    // Since this can potentially be the last reference to the scrolling coordinator,
    // we need to release it on the main thread since it has member variables (such as timers)
    // that expect to be destroyed from the main thread.
    RunLoop::mainSingleton().dispatch([scrollingCoordinator = WTF::move(m_scrollingCoordinator)] {
    });
}

void ThreadedScrollingTree::didCommitTree()
{
    ScrollingThread::dispatch([protectedThis = Ref { *this }]() {
        Locker treeLocker { protectedThis->m_treeLock };
        protectedThis->didCommitTreeOnScrollingThread();
    });
}

void ThreadedScrollingTree::didCommitTreeOnScrollingThread()
{
    ASSERT(ScrollingThread::isCurrentThread());

    auto nodesWithPendingScrollAnimations = std::exchange(m_nodesWithPendingScrollAnimations, { });
    for (const auto& it : nodesWithPendingScrollAnimations) {
        RefPtr targetNode = dynamicDowncast<ScrollingTreeScrollingNode>(nodeForID(it.key));
        if (!targetNode)
            continue;
        targetNode->startAnimatedScrollToPosition(it.value.destinationPosition(targetNode->currentScrollPosition()));
    }

    auto nodesWithPendingKeyboardScrollAnimations = std::exchange(m_nodesWithPendingKeyboardScrollAnimations, { });
    for (const auto& [key, value] : nodesWithPendingKeyboardScrollAnimations) {
        RefPtr targetNode = dynamicDowncast<ScrollingTreeScrollingNode>(nodeForID(key));
        if (!targetNode)
            continue;
        targetNode->handleKeyboardScrollRequest(value);
    }
}

RequestsScrollHandling ThreadedScrollingTree::scrollingTreeNodeRequestsScroll(ScrollingNodeID nodeID, const RequestedScrollData& request)
{
    if (isAnimatedUpdate(request.requestType)) {
        m_nodesWithPendingScrollAnimations.set(nodeID, request);
        return RequestsScrollHandling::Handled;
    }
    return RequestsScrollHandling::Unhandled;
}

bool ThreadedScrollingTree::scrollingTreeNodeRequestsKeyboardScroll(ScrollingNodeID nodeID, const RequestedKeyboardScrollData& request)
{
    m_nodesWithPendingKeyboardScrollAnimations.set(nodeID, request);
    return true;
}

void ThreadedScrollingTree::propagateSynchronousScrollingReasons(const HashSet<ScrollingNodeID>& synchronousScrollingNodes)
{
    auto propagateStateToAncestors = [&](ScrollingTreeNode& node) {
        ASSERT(is<ScrollingTreeScrollingNode>(node) && !downcast<ScrollingTreeScrollingNode>(node).synchronousScrollingReasons().isEmpty());

        if (is<ScrollingTreeFrameScrollingNode>(node))
            return;

        auto currNode = node.parent();

        while (currNode) {
            if (RefPtr scrollingNode = dynamicDowncast<ScrollingTreeScrollingNode>(currNode))
                scrollingNode->addSynchronousScrollingReason(SynchronousScrollingReason::DescendantScrollersHaveSynchronousScrolling);

            if (RefPtr proxyNode = dynamicDowncast<ScrollingTreeOverflowScrollProxyNode>(currNode)) {
                currNode = nodeForID(proxyNode->overflowScrollingNodeID());
                continue;
            }

            if (is<ScrollingTreeFrameScrollingNode>(currNode))
                break;

            currNode = currNode->parent();
        }
    };

    m_hasNodesWithSynchronousScrollingReasons = !synchronousScrollingNodes.isEmpty();

    // With any of these set the scrolling thread may not move layers by itself, so every
    // frame of the scroll has to come from the main thread no matter how the touch was
    // classified.
    WPE_TRACE_COUNTER("wpe.scroll.nodes-forcing-main-thread", synchronousScrollingNodes.size());

    for (auto nodeID : synchronousScrollingNodes) {
        if (RefPtr node = nodeForID(nodeID)) {
            if (WPE_TRACE_ENABLED()) {
                if (RefPtr scrollingNode = dynamicDowncast<ScrollingTreeScrollingNode>(*node)) {
                    TextStream description;
                    description << "scroll: node " << nodeID << " forces main thread scrolling: " << scrollingNode->synchronousScrollingReasons();
                    WPE_TRACE_INSTANT("%s", reinterpret_cast<const char*>(description.release().utf8().data()));
                }
            }
            propagateStateToAncestors(*node);
        }
    }
}

bool ThreadedScrollingTree::canUpdateLayersOnScrollingThread() const
{
    return !m_hasNodesWithSynchronousScrollingReasons;
}

void ThreadedScrollingTree::scrollingTreeNodeDidScroll(ScrollingTreeScrollingNode& node, ScrollingLayerPositionAction scrollingLayerPositionAction)
{
    ScrollingTree::scrollingTreeNodeDidScroll(node, scrollingLayerPositionAction);

    RefPtr scrollingCoordinator = m_scrollingCoordinator;
    if (!scrollingCoordinator)
        return;

    if (isHandlingProgrammaticScroll())
        return;

    std::optional<FloatPoint> layoutViewportOrigin;
    if (auto* scrollingNode = dynamicDowncast<ScrollingTreeFrameScrollingNode>(node))
        layoutViewportOrigin = scrollingNode->layoutViewport().location();

    auto scrollPosition = node.currentScrollPosition();
    auto scrollUpdate = ScrollUpdate {
        .nodeID = node.scrollingNodeID(),
        .scrollPosition = scrollPosition,
        .data = ScrollUpdateData {
            .updateType = ScrollUpdateType::PositionUpdate,
            .updateLayerPositionAction = scrollingLayerPositionAction,
            .layoutViewportOriginOrOverrideRect = layoutViewportOrigin,
        },
    };

    if (RunLoop::isMain()) {
        scrollingCoordinator->applyScrollUpdate(WTF::move(scrollUpdate));
        return;
    }

    LOG_WITH_STREAM(Scrolling, stream << "ThreadedScrollingTree::scrollingTreeNodeDidScroll " << node.scrollingNodeID() << " to " << scrollPosition << " triggering main thread rendering update");
    addPendingScrollUpdateWithDeferReason(WTF::move(scrollUpdate), WheelEventTestMonitor::DeferReason::ScrollingThreadSyncNeeded);
}

void ThreadedScrollingTree::didHandleScrollRequestForNode(ScrollingNodeID nodeID, ScrollRequestType requestType, FloatPoint scrollPosition, ShouldFireScrollEnd shouldFireScrollEnd, Markable<ScrollRequestIdentifier>)
{
    RefPtr scrollingCoordinator = m_scrollingCoordinator;
    if (!scrollingCoordinator)
        return;

    auto scrollUpdate = ScrollUpdate {
        .nodeID = nodeID,
        .scrollPosition = scrollPosition,
        .shouldFireScrollEnd = shouldFireScrollEnd,
        .data = ScrollRequestResponseData {
            .requestType = requestType
        },
    };
    if (RunLoop::isMain()) {
        scrollingCoordinator->applyScrollUpdate(WTF::move(scrollUpdate));
        return;
    }

    addPendingScrollUpdate(WTF::move(scrollUpdate));
}

void ThreadedScrollingTree::scrollingTreeNodeScrollUpdated(ScrollingTreeScrollingNode& node, ScrollUpdateType scrollUpdateType)
{
    RefPtr scrollingCoordinator = m_scrollingCoordinator;
    if (!scrollingCoordinator)
        return;

    LOG_WITH_STREAM(Scrolling, stream << "ThreadedScrollingTree::scrollingTreeNodeScrollUpdated " << node.scrollingNodeID() << " update type " << scrollUpdateType);

    auto scrollUpdate = ScrollUpdate {
        .nodeID = node.scrollingNodeID(),
        .scrollPosition = { },
        .data = ScrollUpdateData {
            .updateType = scrollUpdateType
        }
    };

    if (RunLoop::isMain()) {
        scrollingCoordinator->applyScrollUpdate(WTF::move(scrollUpdate));
        return;
    }

    addPendingScrollUpdate(WTF::move(scrollUpdate));
}

void ThreadedScrollingTree::didAddPendingScrollUpdate()
{
    RunLoop::mainSingleton().dispatch([protectedThis = Ref { *this }] {
        if (RefPtr scrollingCoordinator = protectedThis->m_scrollingCoordinator)
            scrollingCoordinator->scrollingThreadAddedPendingUpdate();
    });
}

void ThreadedScrollingTree::addPendingScrollUpdateWithDeferReason(ScrollUpdate&& update, WheelEventTestMonitor::DeferReason deferReason)
{
    auto nodeID = update.nodeID;
    addPendingScrollUpdateInternal(WTF::move(update));

    auto deferrer = ScrollingTreeWheelEventTestMonitorCompletionDeferrer { *this, nodeID, deferReason };
    RunLoop::mainSingleton().dispatch([protectedThis = Ref { *this }, deferrer = WTF::move(deferrer)] {
        if (RefPtr scrollingCoordinator = protectedThis->m_scrollingCoordinator)
            scrollingCoordinator->scrollingThreadAddedPendingUpdate();
    });
}

void ThreadedScrollingTree::scrollingTreeNodeWillStartAnimatedScroll(ScrollingTreeScrollingNode& node)
{
    scrollingTreeNodeScrollUpdated(node, ScrollUpdateType::AnimatedScrollWillStart);
}

void ThreadedScrollingTree::scrollingTreeNodeDidStopAnimatedScroll(ScrollingTreeScrollingNode& node)
{
    scrollingTreeNodeScrollUpdated(node, ScrollUpdateType::AnimatedScrollDidEnd);
}

void ThreadedScrollingTree::scrollingTreeNodeWillStartWheelEventScroll(ScrollingTreeScrollingNode& node)
{
    scrollingTreeNodeScrollUpdated(node, ScrollUpdateType::WheelEventScrollWillStart);
}

void ThreadedScrollingTree::scrollingTreeNodeDidStopWheelEventScroll(ScrollingTreeScrollingNode& node)
{
    scrollingTreeNodeScrollUpdated(node, ScrollUpdateType::WheelEventScrollDidEnd);
}

void ThreadedScrollingTree::reportSynchronousScrollingReasonsChanged(MonotonicTime timestamp, OptionSet<SynchronousScrollingReason> reasons)
{
    auto scrollingCoordinator = m_scrollingCoordinator;
    if (!scrollingCoordinator)
        return;

    RunLoop::mainSingleton().dispatch([scrollingCoordinator = WTF::move(scrollingCoordinator), timestamp, reasons] {
        scrollingCoordinator->reportSynchronousScrollingReasonsChanged(timestamp, reasons);
    });
}

void ThreadedScrollingTree::reportExposedUnfilledArea(MonotonicTime timestamp, unsigned unfilledArea)
{
    auto scrollingCoordinator = m_scrollingCoordinator;
    if (!scrollingCoordinator)
        return;
    RunLoop::mainSingleton().dispatch([scrollingCoordinator = WTF::move(scrollingCoordinator), timestamp, unfilledArea] {
        scrollingCoordinator->reportExposedUnfilledArea(timestamp, unfilledArea);
    });
}

#if PLATFORM(COCOA)
void ThreadedScrollingTree::currentSnapPointIndicesDidChange(ScrollingNodeID nodeID, std::optional<unsigned> horizontal, std::optional<unsigned> vertical)
{
    auto scrollingCoordinator = m_scrollingCoordinator;
    if (!scrollingCoordinator)
        return;

    RunLoop::mainSingleton().dispatch([scrollingCoordinator = WTF::move(scrollingCoordinator), nodeID, horizontal, vertical] {
        scrollingCoordinator->setActiveScrollSnapIndices(nodeID, horizontal, vertical);
    });
}
#endif

#if PLATFORM(MAC)
void ThreadedScrollingTree::handleWheelEventPhase(ScrollingNodeID nodeID, PlatformWheelEventPhase phase)
{
    auto scrollingCoordinator = m_scrollingCoordinator;
    if (!scrollingCoordinator)
        return;

    RunLoop::mainSingleton().dispatch([scrollingCoordinator = WTF::move(scrollingCoordinator), nodeID, phase] {
        scrollingCoordinator->handleWheelEventPhase(nodeID, phase);
    });
}
#endif

void ThreadedScrollingTree::setActiveScrollSnapIndices(ScrollingNodeID nodeID, std::optional<unsigned> horizontalIndex, std::optional<unsigned> verticalIndex)
{
    auto scrollingCoordinator = m_scrollingCoordinator;
    if (!scrollingCoordinator)
        return;

    RunLoop::mainSingleton().dispatch([scrollingCoordinator = WTF::move(scrollingCoordinator), nodeID, horizontalIndex, verticalIndex] {
        scrollingCoordinator->setActiveScrollSnapIndices(nodeID, horizontalIndex, verticalIndex);
    });
}

void ThreadedScrollingTree::lockLayersForHitTesting()
{
    m_layerHitTestMutex.lock();
}

void ThreadedScrollingTree::unlockLayersForHitTesting()
{
    m_layerHitTestMutex.unlock();
}

void ThreadedScrollingTree::didScheduleRenderingUpdate()
{
    m_renderingUpdateWasScheduled = true;
}

void ThreadedScrollingTree::willStartRenderingUpdate()
{
    ASSERT(isMainThread());

    m_renderingUpdateWasScheduled = false;

    if (!hasRecentActivity())
        return;

    // While a scroll animation runs, the scrolling thread moves the layers on every display
    // refresh by itself. Synchronizing with the rendering update made it wait up to half a
    // frame for a composite that could only happen after the previous frame was presented, so
    // the animated position regularly missed a frame and then jumped two steps.
    if (scrollingThreadDrivesFrames()) {
        Locker locker { m_treeLock };
        m_state = SynchronizationState::Desynchronized;
        WPE_TRACE_INSTANT("scroll: rendering update not synchronized, scrolling thread drives frames");
        return;
    }

    tracePoint(ScrollingThreadRenderUpdateSyncStart);

    // Wait for the scrolling thread to acquire m_treeLock. This ensures that any pending wheel events are processed.
    BinarySemaphore semaphore;
    ScrollingThread::dispatch([protectedThis = Ref { *this }, &semaphore]() {
        Locker treeLocker { protectedThis->m_treeLock };
        semaphore.signal();
        protectedThis->waitForRenderingUpdateCompletionOrTimeout();
    });
    semaphore.wait();

    Locker locker { m_treeLock };
    m_state = SynchronizationState::InRenderingUpdate;
}

void ThreadedScrollingTree::hasNodeWithAnimatedScrollChanged(bool hasNodeWithAnimatedScroll)
{
    auto scrollingCoordinator = m_scrollingCoordinator;
    if (!scrollingCoordinator)
        return;

    RunLoop::mainSingleton().dispatch([scrollingCoordinator = WTF::move(scrollingCoordinator), hasNodeWithAnimatedScroll] {
        scrollingCoordinator->hasNodeWithAnimatedScrollChanged(hasNodeWithAnimatedScroll);
    });
}

// This code allows the main thread about half a frame to complete its rendering update. If the main thread
// is responsive (i.e. managing to render every frame), then we expect to get a didCompletePlatformRenderingUpdate()
// within 8ms of willStartRenderingUpdate(). We time this via m_stateCondition, which blocks the scrolling
// thread (with m_treeLock locked at the start and end) so that we don't handle wheel events while waiting.
// If the condition times out, we know the main thread is being slow, and allow the scrolling thread to
// commit layer positions.
void ThreadedScrollingTree::waitForRenderingUpdateCompletionOrTimeout()
{
    ASSERT(ScrollingThread::isCurrentThread());
    ASSERT(m_treeLock.isLocked());

    if (m_delayedRenderingUpdateDetectionTimer)
        m_delayedRenderingUpdateDetectionTimer->stop();

    auto currentTime = MonotonicTime::now();
    auto estimatedNextDisplayRefreshTime = std::max(m_lastDisplayDidRefreshTime + frameDuration(), currentTime);
    auto timeoutTime = std::min(currentTime + maxAllowableRenderingUpdateDurationForSynchronization(), estimatedNextDisplayRefreshTime);

    bool becameIdle = m_stateCondition.waitUntil(m_treeLock, timeoutTime, [&] {
        assertIsHeld(m_treeLock);
        return m_state == SynchronizationState::Idle;
    });

    ASSERT(m_treeLock.isLocked());

    WPE_TRACE_COUNTER("wpe.scroll.rendering-update-wait-us", (MonotonicTime::now() - currentTime).microsecondsAs<int64_t>());

    if (!becameIdle) {
        m_state = SynchronizationState::Desynchronized;
        // At this point we know the main thread is taking too long in the rendering update,
        // so we give up trying to sync with the main thread and update layers here on the scrolling thread.
        if (canUpdateLayersOnScrollingThread()) {
            // Dispatch to allow for the scrolling thread to handle any outstanding wheel events before we commit layers.
            ScrollingThread::dispatch([protectedThis = Ref { *this }]() {
                protectedThis->applyLayerPositions();
            });
        }
        tracePoint(ScrollingThreadRenderUpdateSyncEnd, 1);
    } else
        tracePoint(ScrollingThreadRenderUpdateSyncEnd);
}

void ThreadedScrollingTree::didCompleteRenderingUpdate()
{
    renderingUpdateComplete();
}

void ThreadedScrollingTree::renderingUpdateComplete()
{
    ASSERT(isMainThread());

    Locker locker { m_treeLock };

    if (m_state == SynchronizationState::InRenderingUpdate)
        m_stateCondition.notifyOne();

    m_state = SynchronizationState::Idle;
}

void ThreadedScrollingTree::scheduleDelayedRenderingUpdateDetectionTimer(Seconds delay)
{
    ASSERT(ScrollingThread::isCurrentThread());
    ASSERT(m_treeLock.isLocked());

    if (!m_delayedRenderingUpdateDetectionTimer)
        m_delayedRenderingUpdateDetectionTimer = makeUnique<RunLoop::Timer>(RunLoop::currentSingleton(), "ThreadedScrollingTree::DelayedRenderingUpdateDetectionTimer"_s,  this, &ThreadedScrollingTree::delayedRenderingUpdateDetectionTimerFired);

    m_delayedRenderingUpdateDetectionTimer->startOneShot(delay);
}

void ThreadedScrollingTree::delayedRenderingUpdateDetectionTimerFired()
{
    ASSERT(ScrollingThread::isCurrentThread());

    Locker locker { m_treeLock };
    if (canUpdateLayersOnScrollingThread())
        applyLayerPositionsInternal();
    m_state = SynchronizationState::Desynchronized;
}

bool ThreadedScrollingTree::hasActiveScrollAnimations()
{
    return scrollingThreadDrivesFrames();
}

// While the user is scrolling or a scroll animation runs, the scrolling thread moves the layers
// on every frame by itself instead of synchronizing with main thread rendering updates, which
// made positions wait for the main thread and regularly miss a frame. Chrome's compositor
// thread works the same way.
bool ThreadedScrollingTree::scrollingThreadDrivesFrames()
{
    if (scrollAnimationsNeedMainThreadRequested())
        return false;
    return !nodesWithActiveScrollAnimations().isEmpty() || hasNodesWithActiveUserScrolls();
}

// debug.wpe.touch.flingcontinues=1 restores the upstream behaviour, where a finger touching down
// during a fling does not stop it: it only stopped once the next drag was recognised, after the
// touch slop, so the page kept moving under the finger for 100-400 ms and then jumped.
static bool flingStopsOnTouchStart()
{
#if OS(ANDROID)
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = { };
        return !(__system_property_get("debug.wpe.touch.flingcontinues", value) > 0 && !strcmp(value, "1"));
    }();
    return enabled;
#else
    return false;
#endif
}

#if OS(ANDROID)
// debug.wpe.scroll.vsyncwake=0 leaves starting the vsync clock to the first composite that needs it.
static bool vsyncClockWakesOnTouchStart()
{
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = { };
        return !(__system_property_get("debug.wpe.scroll.vsyncwake", value) > 0 && !strcmp(value, "0"));
    }();
    return enabled;
}
#endif

std::optional<Seconds> ThreadedScrollingTree::displayRefreshPeriod()
{
#if OS(ANDROID)
    return AndroidVSyncClock::singleton().period();
#else
    return std::nullopt;
#endif
}

std::optional<MonotonicTime> ThreadedScrollingTree::vsyncAtOrBefore(MonotonicTime time)
{
#if OS(ANDROID)
    return AndroidVSyncClock::singleton().nextVSync(time);
#else
    UNUSED_PARAM(time);
    return std::nullopt;
#endif
}

void ThreadedScrollingTree::wakeVSyncClockForTouchStart()
{
    ASSERT(ScrollingThread::isCurrentThread());
#if OS(ANDROID)
    // The vsync clock stops a second after its last use. When a drag starts after a longer pause, its
    // first composites found no grid and sampled at the current time; once the grid was back, the
    // sample time jumped back to it, so that frame showed a finger position ~6 ms older than the one
    // before it: a small step, then a catch-up. Starting the clock at touch down gives it the touch
    // slop to get a vsync before the first drag frame.
    if (vsyncSamplingEnabled() && vsyncClockWakesOnTouchStart())
        AndroidVSyncClock::singleton().keepAlive();
#endif
}

void ThreadedScrollingTree::stopFlingsForTouchStart()
{
    ASSERT(ScrollingThread::isCurrentThread());
    if (!flingStopsOnTouchStart())
        return;

    Locker locker { m_treeLock };
    for (auto nodeID : nodesWithActiveScrollAnimations()) {
        if (RefPtr node = dynamicDowncast<ScrollingTreeScrollingNode>(nodeForID(nodeID))) {
            WPE_TRACE_INSTANT("scroll: touch start stops the fling");
            node->stopAnimatedScroll({ AnimatedScrollType::User });
        }
    }
}

void ThreadedScrollingTree::serviceScrollAnimationsForComposite()
{
    ASSERT(ScrollingThread::isCurrentThread());

    Locker locker { m_treeLock };
    if (!scrollingThreadDrivesFrames() || !canUpdateLayersOnScrollingThread())
        return;

    WPE_TRACE_SCOPE("scroll: animation sampled for composite");
    auto time = scrollAnimationTime();
    serviceScrollAnimations(time);
    serviceResampledInput(time, true);
    m_state = SynchronizationState::Desynchronized;
    applyLayerPositionsInternal();
}

void ThreadedScrollingTree::displayDidRefreshOnScrollingThread()
{
    TraceScope tracingScope(ScrollingThreadDisplayDidRefreshStart, ScrollingThreadDisplayDidRefreshEnd, displayID());
    ASSERT(ScrollingThread::isCurrentThread());

    Locker locker { m_treeLock };
    
    auto now = MonotonicTime::now();
    // Scroll animations are sampled at the time the tick is handled, so any jitter in its
    // delivery becomes uneven motion.
    WPE_TRACE_COUNTER("wpe.scroll.tick-interval-us", (now - m_lastDisplayDidRefreshTime).microsecondsAs<int64_t>());
    m_lastDisplayDidRefreshTime = now;
    // Display refreshes arrive while the finger is still down, so the vsync grid is already
    // known when the fling starts.
    keepScrollAnimationClockAlive();
    auto time = scrollAnimationTime();
    serviceScrollAnimations(time);
    serviceResampledInput(time, false);

    if (scrollingThreadDrivesFrames() && canUpdateLayersOnScrollingThread()) {
        m_state = SynchronizationState::Desynchronized;
        applyLayerPositionsInternal();
        return;
    }

    if (m_state != SynchronizationState::Idle && canUpdateLayersOnScrollingThread())
        applyLayerPositionsInternal();

    switch (m_state) {
    case SynchronizationState::Idle: {
        // A scroll animation (a fling) or a scroll from the scrolling thread needs a rendering
        // update to reach the screen, but it is the main thread that schedules one, and it may
        // be busy running script. Waiting for it would freeze the fling for as long as the main
        // thread is busy, so give it the same short chance as a scheduled update and move the
        // layers here if it does not start.
        bool needsRenderingUpdate = m_renderingUpdateWasScheduled;
        if (!needsRenderingUpdate && !scrollAnimationsNeedMainThreadRequested())
            needsRenderingUpdate = !nodesWithActiveScrollAnimations().isEmpty() || hasPendingScrollUpdates();
        if (needsRenderingUpdate) {
            if (!m_renderingUpdateWasScheduled)
                WPE_TRACE_INSTANT("scroll: animating while the main thread has not scheduled a rendering update");
            m_state = SynchronizationState::WaitingForRenderingUpdate;
            constexpr auto maxStartRenderingUpdateDelay = 1_ms;
            scheduleDelayedRenderingUpdateDetectionTimer(maxStartRenderingUpdateDelay);
        }
        break;
    }
    case SynchronizationState::WaitingForRenderingUpdate:
    case SynchronizationState::InRenderingUpdate:
    case SynchronizationState::Desynchronized:
        break;
    }
}

void ThreadedScrollingTree::displayDidRefresh(PlatformDisplayID displayID)
{
    bool scrollingThreadIsActive = hasRecentActivity();

    // We're on the EventDispatcher thread or in the ThreadedCompositor thread here.
    tracePoint(ScrollingTreeDisplayDidRefresh, displayID, scrollingThreadIsActive);

    if (displayID != this->displayID())
        return;

    if (!scrollingThreadIsActive)
        return;

    ScrollingThread::dispatch([protectedThis = Ref { *this }]() {
        protectedThis->displayDidRefreshOnScrollingThread();
    });
}

void ThreadedScrollingTree::removePendingScrollAnimationForNode(ScrollingNodeID nodeID)
{
    m_nodesWithPendingScrollAnimations.remove(nodeID);
}

bool ThreadedScrollingTree::isScrollingSynchronizedWithMainThread()
{
    return m_state != SynchronizationState::Desynchronized;
}

void ThreadedScrollingTree::receivedWheelEventWithPhases(PlatformWheelEventPhase phase, PlatformWheelEventPhase momentumPhase)
{
    auto scrollingCoordinator = m_scrollingCoordinator;
    if (!scrollingCoordinator)
        return;

    RunLoop::mainSingleton().dispatch([scrollingCoordinator = WTF::move(scrollingCoordinator), phase, momentumPhase] {
        scrollingCoordinator->receivedWheelEventWithPhases(phase, momentumPhase);
    });
}

void ThreadedScrollingTree::deferWheelEventTestCompletionForReason(ScrollingNodeID nodeID, WheelEventTestMonitor::DeferReason reason)
{
    if (!isMonitoringWheelEvents())
        return;

    auto scrollingCoordinator = m_scrollingCoordinator;
    if (!scrollingCoordinator)
        return;

    RunLoop::mainSingleton().dispatch([scrollingCoordinator = WTF::move(scrollingCoordinator), nodeID, reason] {
        scrollingCoordinator->deferWheelEventTestCompletionForReason(nodeID, reason);
    });
}

void ThreadedScrollingTree::removeWheelEventTestCompletionDeferralForReason(ScrollingNodeID nodeID, WheelEventTestMonitor::DeferReason reason)
{
    if (!isMonitoringWheelEvents())
        return;

    auto scrollingCoordinator = m_scrollingCoordinator;
    if (!scrollingCoordinator)
        return;

    RunLoop::mainSingleton().dispatch([scrollingCoordinator = WTF::move(scrollingCoordinator), nodeID, reason] {
        scrollingCoordinator->removeWheelEventTestCompletionDeferralForReason(nodeID, reason);
    });
}

} // namespace WebCore

#endif // ENABLE(ASYNC_SCROLLING) && ENABLE(SCROLLING_THREAD)
