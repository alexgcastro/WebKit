/*
 * Copyright (C) 2012 Apple Inc. All rights reserved.
 * Copyright (C) 2019, 2024 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ScrollingTreeFixedNodeCoordinated.h"

#include "ScrollingTreeFrameScrollingNode.h"

#if ENABLE(ASYNC_SCROLLING) && USE(COORDINATED_GRAPHICS)
#include "CoordinatedPlatformLayer.h"
#include "Logging.h"
#include "ScrollingStateFixedNode.h"
#include "ScrollingThread.h"
#include <wtf/text/TextStream.h>

namespace WebCore {

Ref<ScrollingTreeFixedNodeCoordinated> ScrollingTreeFixedNodeCoordinated::create(ScrollingTree& scrollingTree, ScrollingNodeID nodeID)
{
    return adoptRef(*new ScrollingTreeFixedNodeCoordinated(scrollingTree, nodeID));
}

ScrollingTreeFixedNodeCoordinated::ScrollingTreeFixedNodeCoordinated(ScrollingTree& scrollingTree, ScrollingNodeID nodeID)
    : ScrollingTreeFixedNode(scrollingTree, nodeID)
{
}

ScrollingTreeFixedNodeCoordinated::~ScrollingTreeFixedNodeCoordinated() = default;

bool ScrollingTreeFixedNodeCoordinated::commitStateBeforeChildren(const ScrollingStateNode& stateNode)
{
    if (!is<ScrollingStateFixedNode>(stateNode))
        return false;

    auto& fixedStateNode = downcast<ScrollingStateFixedNode>(stateNode);
    if (fixedStateNode.hasChangedProperty(ScrollingStateNode::Property::Layer))
        m_layer = static_cast<CoordinatedPlatformLayer*>(fixedStateNode.layer());

    return ScrollingTreeFixedNode::commitStateBeforeChildren(stateNode);
}

void ScrollingTreeFixedNodeCoordinated::applyLayerPositions()
{
    auto layerPosition = computeLayerPosition();

    LOG_WITH_STREAM(Scrolling, stream << "ScrollingTreeFixedNode " << scrollingNodeID() << " relatedNodeScrollPositionDidChange: viewportRectAtLastLayout " << m_constraints.viewportRectAtLastLayout() << " last layer pos " << m_constraints.layerPositionAtLastLayout() << " layerPosition " << layerPosition);

    auto topLeft = layerPosition - m_constraints.alignmentOffset();

    // The layout viewport is kept in LayoutUnits (1/64 of a CSS pixel). When the scrolling tree
    // positions the contents between CSS pixels, that truncation leaves the fixed layer a varying
    // few hundredths of a device pixel off, which the compositor's filtering shows as shimmer.
    // While the layout viewport follows the scroll position, take the truncation back out.
    if (RefPtr frameNode = dynamicDowncast<ScrollingTreeFrameScrollingNode>(parent())) {
        auto quantization = frameNode->layoutViewport().location() - frameNode->currentScrollPosition();
        constexpr float layoutUnit = 1.f / 64;
        if (std::abs(quantization.width()) < layoutUnit && std::abs(quantization.height()) < layoutUnit)
            topLeft -= quantization;
    }

    m_layer->setTopLeftPositionForScrolling(topLeft);
}

void ScrollingTreeFixedNodeCoordinated::dumpProperties(TextStream& ts, OptionSet<ScrollingStateTreeAsTextBehavior> behavior) const
{
    ScrollingTreeFixedNode::dumpProperties(ts, behavior);

    if (behavior & ScrollingStateTreeAsTextBehavior::IncludeLayerPositions) {
        FloatPoint layerTopLeft = m_layer->topLeftPositionForScrolling() + m_constraints.alignmentOffset();
        ts.dumpProperty("layer top left"_s, layerTopLeft);
    }
}

} // namespace WebCore

#endif // ENABLE(ASYNC_SCROLLING) && USE(COORDINATED_GRAPHICS)
