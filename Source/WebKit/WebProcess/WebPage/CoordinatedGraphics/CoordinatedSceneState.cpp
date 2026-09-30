/*
 * Copyright (C) 2024 Igalia S.L.
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
#include "CoordinatedSceneState.h"

#if USE(COORDINATED_GRAPHICS)
#include <WebCore/CoordinatedPlatformLayer.h>
#include <wtf/MainThread.h>
#include <wtf/TZoneMallocInlines.h>

#if OS(ANDROID)
#include <sys/system_properties.h>
#endif

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(CoordinatedSceneState);

CoordinatedSceneState::CoordinatedSceneState()
    : m_rootLayer(CoordinatedPlatformLayer::create())
{
    ASSERT(isMainRunLoop());
}

CoordinatedSceneState::~CoordinatedSceneState()
{
    ASSERT(m_layers.isEmpty());
    ASSERT(m_pendingLayers.isEmpty());
    ASSERT(m_pendingLayersToRemove.isEmpty());
    ASSERT(m_committedLayers.isEmpty());
}

void CoordinatedSceneState::setRootLayerChildren(Vector<Ref<CoordinatedPlatformLayer>>&& children)
{
    ASSERT(isMainRunLoop());

    {
        Locker locker { m_rootLayer->lock() };
        m_rootLayer->setChildren(WTF::move(children));
    }
    m_didChangeLayers = true;
}

void CoordinatedSceneState::addLayer(CoordinatedPlatformLayer& layer)
{
    ASSERT(isMainRunLoop());
    {
        Locker locker { m_layersLock };
        m_layers.add(layer);
    }
    m_didChangeLayers = true;
}

void CoordinatedSceneState::removeLayer(CoordinatedPlatformLayer& layer)
{
    ASSERT(isMainRunLoop());
    {
        Locker locker { m_layersLock };
        m_layers.remove(layer);
    }
    m_layersToRemove.add(layer);
    m_didChangeLayers = true;
}

bool CoordinatedSceneState::flush()
{
    ASSERT(isMainRunLoop());

    bool didChangeLayers = m_didChangeLayers.exchange(false);
    if (didChangeLayers) {
        Locker pendingLayersLock { m_pendingLayersLock };
        {
            Locker locker { m_layersLock };
            m_pendingLayers = m_layers;
        }
        if (m_pendingLayersToRemove.isEmpty())
            m_pendingLayersToRemove = WTF::move(m_layersToRemove);
        else
            m_pendingLayersToRemove.addAll(std::exchange(m_layersToRemove, { }));
    }

    flushPendingState();

    return didChangeLayers;
}

void CoordinatedSceneState::flushPendingState()
{
    Locker stateLock { m_stateLock };
    Locker layersLock { m_layersLock };
    for (Ref layer : m_layers)
        layer->flushPendingState();
}

void CoordinatedSceneState::commitPendingLayers()
{
    ASSERT(!isMainRunLoop());
    Locker pendingLayersLock { m_pendingLayersLock };
    while (!m_pendingLayersToRemove.isEmpty()) {
        auto layer = m_pendingLayersToRemove.takeAny();
        layer->invalidateTarget();
    }

    if (!m_pendingLayers.isEmpty())
        m_committedLayers = WTF::move(m_pendingLayers);
}

#if OS(ANDROID)
static std::optional<float> floatProperty(const char* name)
{
    char value[PROP_VALUE_MAX] = { };
    if (__system_property_get(name, value) > 0)
        return static_cast<float>(atof(value));
    return std::nullopt;
}
#endif

// Tiles away from the viewport are rasterized after a frame has been handed over rather than
// inside it: a composite that replays even one or two tiles already misses the display
// deadline, and the compositor is otherwise idle for most of each frame.
// debug.wpe.tiles.postframe=0 applies every tile in the frame again (upstream), optionally
// bounded by debug.wpe.tiles.budget.
struct TileSchedule {
    bool postFrame { true };
    float nearMargin { 0.15 }; // debug.wpe.tiles.margin: fraction of the viewport applied in the frame.
    unsigned budget { 2 }; // debug.wpe.tiles.budget: tiles per pass beyond the near ones, 0 = no limit.
};

static const TileSchedule& tileSchedule()
{
    static const TileSchedule schedule = [] {
        TileSchedule schedule;
#if OS(ANDROID)
        if (auto postFrame = floatProperty("debug.wpe.tiles.postframe"))
            schedule.postFrame = *postFrame;
        if (!schedule.postFrame) {
            schedule.nearMargin = 0.5;
            schedule.budget = 0;
        }
        if (auto margin = floatProperty("debug.wpe.tiles.margin"))
            schedule.nearMargin = *margin;
        if (auto budget = floatProperty("debug.wpe.tiles.budget"))
            schedule.budget = *budget;
#else
        schedule.postFrame = false;
        schedule.budget = 0;
#endif
        return schedule;
    }();
    return schedule;
}

static unsigned budgetOrUnlimited(unsigned budget)
{
    return budget ? budget : std::numeric_limits<unsigned>::max();
}

bool CoordinatedSceneState::rasterizesTilesAfterFrame()
{
    return tileSchedule().postFrame;
}

bool CoordinatedSceneState::rasterizeDeferredTiles()
{
    ASSERT(!isMainRunLoop());
    if (!tileSchedule().postFrame)
        return false;

    unsigned tileBudget = budgetOrUnlimited(tileSchedule().budget);
    bool hasDeferredTileUpdates = false;
    for (auto& layer : m_committedLayers) {
        if (!layer->hasPendingBackingStoreTileUpdates())
            continue;
        layer->processPendingBackingStoreTileUpdates(std::nullopt, tileBudget);
        hasDeferredTileUpdates |= layer->hasPendingBackingStoreTileUpdates();
    }
    return hasDeferredTileUpdates;
}

bool CoordinatedSceneState::flushCompositingState(const OptionSet<CompositionReason>& reasons)
{
    commitPendingLayers();

    {
        Locker stateLock { m_stateLock };
        m_rootLayer->flushPositionChanges(reasons);
        for (auto& layer : m_committedLayers)
            layer->flushPositionChanges(reasons);
    }

    Vector<Ref<CoordinatedPlatformLayer>, 16> layersWithPendingTileUpdates;
    m_rootLayer->flushCompositingState(reasons);
    for (auto& layer : m_committedLayers) {
        layer->flushCompositingState(reasons);
        if (layer->hasPendingBackingStoreTileUpdates())
            layersWithPendingTileUpdates.append(Ref { layer });
    }

    const auto& schedule = tileSchedule();
    unsigned tileBudget = schedule.postFrame ? 0 : budgetOrUnlimited(schedule.budget);
    bool hasDeferredTileUpdates = false;
    for (auto& layer : layersWithPendingTileUpdates) {
        layer->processPendingBackingStoreTileUpdates(schedule.nearMargin, tileBudget);
        hasDeferredTileUpdates |= layer->hasPendingBackingStoreTileUpdates();
    }
    return hasDeferredTileUpdates;
}

void CoordinatedSceneState::invalidateCommittedLayers()
{
    ASSERT(!isMainRunLoop());
    commitPendingLayers();
    m_rootLayer->invalidateTarget();
    while (!m_committedLayers.isEmpty()) {
        auto layer = m_committedLayers.takeAny();
        layer->invalidateTarget();
    }
}

void CoordinatedSceneState::invalidate()
{
    ASSERT(isMainRunLoop());
    // Root layer doesn't have client nor backing stores to invalidate.
    HashSet<Ref<CoordinatedPlatformLayer>> layers;
    {
        Locker locker { m_layersLock };
        layers = WTF::move(m_layers);
    }
    for (Ref layer : layers)
        layer->invalidateClient();

    Locker pendingLayersLock { m_pendingLayersLock };
    m_pendingLayers = { };
    m_pendingLayersToRemove = { };
}

void CoordinatedSceneState::waitUntilPaintingComplete()
{
    ASSERT(isMainRunLoop());
    Locker pendingLayersLock { m_pendingLayersLock };
    for (auto& layer : m_pendingLayers)
        layer->waitUntilPaintingComplete();
}

void CoordinatedSceneState::willPaintTile()
{
    m_pendingTiles++;
}

void CoordinatedSceneState::didPaintTile()
{
    ASSERT(m_pendingTiles.load() > 0);
    m_pendingTiles--;
}

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
