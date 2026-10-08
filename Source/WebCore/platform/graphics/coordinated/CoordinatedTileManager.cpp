/*
 * Copyright (C) 2026 Igalia S.L.
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
#include "CoordinatedTileManager.h"

#if USE(COORDINATED_GRAPHICS)
#include "CoordinatedPlatformLayer.h"
#include "CoordinatedTileBuffer.h"
#include "GraphicsLayerCoordinated.h"
#include "IntRect.h"
#include "PlatformDisplay.h"
#include "ProcessCapabilities.h"
#include "RenderingMode.h"
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMallocInlines.h>

#if USE(SKIA)
#include "SkiaPaintingEngine.h"
#include "SkiaRecordingResult.h"
#elif USE(CAIRO)
#include "CairoPaintingEngine.h"
#include "FloatRect.h"
#endif

#if USE(TEXTURE_MAPPER)
#include "BitmapTexturePool.h"
#endif

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(CoordinatedTileManager);

#if USE(SKIA)

static bool canPerformAcceleratedRendering()
{
    return ProcessCapabilities::canUseAcceleratedBuffers() && PlatformDisplay::sharedDisplay().skiaGLContext();
}

static Ref<WorkerPool> createWorkerPool()
{
#if USE(TEXTURE_MAPPER)
    if (canPerformAcceleratedRendering())
        return WorkerPool::create("SkiaGPUWorker"_s, SkiaPaintingEngine::numberOfGPUPaintingThreads());
#endif

    return WorkerPool::create("SkiaCPUWorker"_s, SkiaPaintingEngine::numberOfCPUPaintingThreads());
}

std::unique_ptr<CoordinatedTileManager> CoordinatedTileManager::create(sk_sp<GrContextThreadSafeProxy>&& threadSafeGrContext)
{
    return makeUnique<CoordinatedTileManager>(WTF::move(threadSafeGrContext));
}

CoordinatedTileManager::CoordinatedTileManager(sk_sp<GrContextThreadSafeProxy>&& threadSafeGrContext)
    : m_paintingEngine(SkiaPaintingEngine::create(WTF::move(threadSafeGrContext)))
    , m_paintingWorkerPool(createWorkerPool())
{
}

CoordinatedTileManager::~CoordinatedTileManager()
{
    Locker locker { m_paintingTaskQueueLock };
    for (auto& task : m_paintingTaskQueue)
        task.buffer->cancelPainting();
    m_paintingTaskQueue.clear();
}

const sk_sp<GrContextThreadSafeProxy>& CoordinatedTileManager::threadSafeGrContext() const
{
    return m_paintingEngine->threadSafeGrContext();
}

Ref<SkiaRecordingResult> CoordinatedTileManager::record(const GraphicsLayerCoordinated& layer, const IntRect& recordRect, bool contentsOpaque, float contentsScale, unsigned dirtyTilesCount)
{
    return m_paintingEngine->record(layer, recordRect, canPerformAcceleratedRendering() ? RenderingMode::Accelerated : RenderingMode::Unaccelerated, contentsOpaque, contentsScale, dirtyTilesCount);
}

static Ref<CoordinatedTileBuffer> createBuffer(RenderingMode renderingMode, const IntSize& size, bool contentsOpaque, unsigned tileDistance, uint64_t renderingUpdateID, const sk_sp<GrContextThreadSafeProxy>& threadSafeGrContext)
{
    std::optional<uint64_t> tileRenderingUpdateID;
    if (!tileDistance)
        tileRenderingUpdateID = renderingUpdateID;

    if (renderingMode == RenderingMode::Accelerated) {
#if USE(TEXTURE_MAPPER)
        PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent();

        OptionSet<BitmapTexture::Flags> textureFlags;
        if (!contentsOpaque)
            textureFlags.add(BitmapTexture::Flags::SupportsAlpha);

        return CoordinatedAcceleratedTileBuffer::create(BitmapTexturePool::singleton().acquireTexture(size, textureFlags), tileDistance, tileRenderingUpdateID);
#else
        ASSERT(threadSafeGrContext);
        return CoordinatedAcceleratedTileBuffer::create(threadSafeGrContext, size, contentsOpaque ? CoordinatedTileBuffer::NoFlags : CoordinatedTileBuffer::SupportsAlpha, tileDistance, tileRenderingUpdateID);
#endif
    }

    return CoordinatedUnacceleratedTileBuffer::create(size, contentsOpaque ? CoordinatedTileBuffer::NoFlags : CoordinatedTileBuffer::SupportsAlpha, tileDistance, tileRenderingUpdateID);
}

Ref<CoordinatedTileBuffer> CoordinatedTileManager::scheduleReplay(const GraphicsLayerCoordinated& layer, Ref<SkiaRecordingResult>&& recording, const IntRect& tileRect, const IntRect& dirtyRect, unsigned distance)
{
    Ref platformLayer = layer.coordinatedPlatformLayer();

    auto renderingMode = recording->renderingMode();
#if USE(TEXTURE_MAPPER)
    auto bufferSize = dirtyRect.size();
#else
    auto bufferSize = renderingMode == RenderingMode::Accelerated ? tileRect.size() : dirtyRect.size();
#endif
    Ref buffer = createBuffer(renderingMode, bufferSize, recording->contentsOpaque(), distance, m_currentRenderingUpdate, threadSafeGrContext());

    if (!distance)
        platformLayer->willPaintTile();

    WTFBeginSignpost(this, QueueTilePaintingTask);
    {
        Locker locker { m_paintingTaskQueueLock };
        PaintingTaskPriority priority = { distance, m_nextTaskSequence++ };
        size_t position = std::upper_bound(m_paintingTaskQueue.begin(), m_paintingTaskQueue.end(), priority, [](auto& priority, auto& task) {
            return priority < task.priority;
        }) - m_paintingTaskQueue.begin();
        m_paintingTaskQueue.insert(position, { WTF::move(priority), WTF::move(platformLayer), buffer.copyRef(), WTF::move(recording), tileRect, dirtyRect });
    }
    WTFEndSignpost(this, QueueTilePaintingTask);

    m_paintingWorkerPool->postTask([this] {
        auto task = [&] -> std::optional<PaintingTask> {
            Locker locker { m_paintingTaskQueueLock };
            // This can happen if task was cancelled and removed from the queue.
            if (m_paintingTaskQueue.isEmpty())
                return std::nullopt;
            return m_paintingTaskQueue.takeLast();
        }();

        if (!task || !task->buffer->beginPainting())
            return;

        m_paintingEngine->replay(task->buffer, task->recording, task->tileRect, task->dirtyRect);
        bool wasVisible = task->buffer->completePainting();
        if (wasVisible)
            task->layer->didPaintTile();
        else
            task->layer->didPaintPreRenderedTile();
    });
    return buffer;
}

void CoordinatedTileManager::didFinishRenderingUpdate()
{
    if (!m_paintingTaskQueueNeedsUpdate)
        return;

    m_paintingTaskQueueNeedsUpdate = false;

    WTFBeginSignpost(this, TilePaintingTaskQueueSort);

    Locker locker { m_paintingTaskQueueLock };
    m_paintingTaskQueue.removeAllMatching([](auto& task) {
        if (task.buffer->paintingState() == CoordinatedTileBuffer::PaintingState::Cancelled)
            return true;

        task.priority.distance = task.buffer->distance();
        return false;
    });

    std::sort(m_paintingTaskQueue.begin(), m_paintingTaskQueue.end(), [](auto& a, auto& b) {
        return a.priority < b.priority;
    });

    WTFEndSignpost(this, TilePaintingTaskQueueSort);
}

#elif USE(CAIRO)

std::unique_ptr<CoordinatedTileManager> CoordinatedTileManager::create()
{
    return makeUnique<CoordinatedTileManager>();
}

CoordinatedTileManager::CoordinatedTileManager()
    : m_paintingEngine(Cairo::PaintingEngine::create())
{
}

Ref<CoordinatedTileBuffer> CoordinatedTileManager::schedulePaint(const GraphicsLayerCoordinated& layer, const IntRect& dirtyRect, bool contentsOpaque, float contentsScale)
{
    FloatRect scaledDirtyRect(dirtyRect);
    scaledDirtyRect.scale(1 / contentsScale);

    auto buffer = CoordinatedUnacceleratedTileBuffer::create(dirtyRect.size(), contentsOpaque ? CoordinatedTileBuffer::NoFlags : CoordinatedTileBuffer::SupportsAlpha, 0, std::nullopt);
    m_paintingEngine->paint(layer, buffer.get(), dirtyRect, enclosingIntRect(scaledDirtyRect), IntRect { { }, dirtyRect.size() }, contentsScale);
    return buffer;
}

CoordinatedTileManager::~CoordinatedTileManager() = default;

#endif

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS)
