/*
 * Copyright (C) 2017-2025 Apple Inc. All rights reserved.
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
#include "PlaceholderRenderingContext.h"

#if ENABLE(OFFSCREEN_CANVAS)

#include "Chrome.h"
#include "ChromeClient.h"
#include "ContextDestructionObserverInlines.h"
#include "Document.h"
#include "DocumentPage.h"
#include "GraphicsContext.h"
#include "GraphicsLayer.h"
#include "GraphicsLayerContentsDisplayDelegate.h"
#include "HTMLCanvasElement.h"
#include "ImageBuffer.h"
#include "NativeImage.h"
#include "NodeDocument.h"
#include "OffscreenCanvas.h"
#include "Page.h"
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(PlaceholderLayerContents);

bool PlaceholderLayerContents::copyFrame(ImageBuffer& imageBuffer, bool originClean, bool opaque, PlaceholderFrameIdentifier frame, RefPtr<GraphicsLayerFrameDisplayNotifier>&& displayNotifier)
{
    Locker locker { m_lock };
    if (!m_delegate || frame <= m_frame)
        return false;
    if (!protect(m_delegate)->tryPresent(imageBuffer, opaque, frame, WTF::move(displayNotifier)))
        return false;
    m_frame = frame;
    m_frameMetadata = FrameMetadata { imageBuffer.truncatedLogicalSize(), originClean, opaque };
    return true;
}

void PlaceholderLayerContents::setFrameForNextDisplay(ImageBuffer& imageBuffer, bool opaque, PlaceholderFrameIdentifier frame)
{
    assertIsMainThread();
    Locker locker { m_lock };
    if (!m_delegate || frame <= m_frame)
        return;
    protect(m_delegate)->setContentsForNextDisplay(imageBuffer, opaque, frame);
    m_frame = frame;
    m_frameMetadata = std::nullopt;
}

std::optional<PlatformLayerIdentifier> PlaceholderLayerContents::attach(GraphicsLayer& layer, ImageBuffer* buffer, bool opaque, PlaceholderFrameIdentifier frame)
{
    assertIsMainThread();
    Locker locker { m_lock };
    if (!(m_delegate = layer.createAsyncContentsDisplayDelegate(m_delegate.get())))
        return std::nullopt;
    Ref delegate = *m_delegate;
    if (buffer && frame > m_frame) {
        if (delegate->tryCopyToLayer(*buffer, opaque, frame)) {
            m_frame = frame;
            m_frameMetadata = std::nullopt;
        }
    }
    return delegate->destinationLayerID();
}

PlaceholderFrameIdentifier PlaceholderLayerContents::currentFrame()
{
    Locker locker { m_lock };
    return m_frame;
}

auto PlaceholderLayerContents::currentFrameMetadataIfNewerThan(PlaceholderFrameIdentifier frame) -> std::optional<Frame>
{
    Locker locker { m_lock };
    if (m_frame <= frame)
        return std::nullopt;
    return Frame { m_frame, m_frameMetadata, nullptr };
}

auto PlaceholderLayerContents::copyCurrentFrameIfNewerThan(PlaceholderFrameIdentifier frame) -> std::optional<Frame>
{
    assertIsMainThread();
    Locker locker { m_lock };
    if (!m_delegate || m_frame <= frame)
        return std::nullopt;
    return Frame { m_frame, m_frameMetadata, protect(m_delegate)->copyCurrentBuffer() };
}

bool PlaceholderLayerContents::canCopyCurrentFrame()
{
    if (m_needsFramesOnMainThread.load(std::memory_order_relaxed))
        return false;
    Locker locker { m_lock };
    return m_delegate && m_delegate->canCopyCurrentBuffer();
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(LocalPlaceholderRenderingContextSource);

Ref<LocalPlaceholderRenderingContextSource> LocalPlaceholderRenderingContextSource::create(PlaceholderRenderingContext& context)
{
    return adoptRef(*new LocalPlaceholderRenderingContextSource(context));
}

LocalPlaceholderRenderingContextSource::LocalPlaceholderRenderingContextSource(PlaceholderRenderingContext& placeholder)
    : PlaceholderRenderingContextSource({ placeholder.identifier(), Process::identifier() })
    , m_placeholder(placeholder)
    , m_layerContents(placeholder.layerContents())
{
}

LocalPlaceholderRenderingContextSource::~LocalPlaceholderRenderingContextSource() = default;

void LocalPlaceholderRenderingContextSource::setPlaceholderBuffer(ImageBuffer& imageBuffer, bool originClean, bool opaque, RefPtr<GraphicsLayerFrameDisplayNotifier>&& displayNotifier)
{
    auto frame = m_lastFrame.increment();
    if (m_layerContents->copyFrame(imageBuffer, originClean, opaque, frame, WTF::move(displayNotifier)) && m_layerContents->canCopyCurrentFrame()) {
        FrameMetadata metadata { imageBuffer.truncatedLogicalSize(), originClean, opaque };
        if (m_lastMetadata == metadata)
            return;
        m_lastMetadata = metadata;
        callOnMainThread([protectedThis = Ref { *this }] {
            if (RefPtr placeholder = protectedThis->m_placeholder.get())
                placeholder->updateFrameMetadata();
        });
        return;
    }
    m_lastMetadata = std::nullopt;

    RefPtr clone = imageBuffer.clone();
    if (!clone)
        return;
    std::unique_ptr serializedClone = ImageBuffer::sinkIntoSerializedImageBuffer(WTF::move(clone));
    if (!serializedClone)
        return;

    std::optional<PendingFrame> replacedFrame;
    {
        Locker locker { m_pendingFrameLock };
        replacedFrame = std::exchange(m_pendingFrame, PendingFrame { WTF::move(serializedClone), frame, originClean, opaque });
    }
    if (replacedFrame)
        return;
    callOnMainThread([protectedThis = Ref { *this }] {
        protectedThis->commitPendingFrame();
    });
}

void LocalPlaceholderRenderingContextSource::offscreenCanvasWillBeDestroyed()
{
    if (!m_layerContents->canCopyCurrentFrame())
        return;
    auto currentFrame = m_layerContents->currentFrame();
    std::optional<PendingFrame> pendingFrame;
    {
        Locker locker { m_pendingFrameLock };
        if (m_pendingFrame && m_pendingFrame->frame <= currentFrame)
            pendingFrame = std::exchange(m_pendingFrame, std::nullopt);
    }
}

void LocalPlaceholderRenderingContextSource::commitPendingFrame()
{
    assertIsMainThread();
    std::optional<PendingFrame> pendingFrame;
    {
        Locker locker { m_pendingFrameLock };
        pendingFrame = std::exchange(m_pendingFrame, std::nullopt);
    }
    if (!pendingFrame)
        return;
    RefPtr placeholder = m_placeholder.get();
    if (!placeholder)
        return;
    RefPtr imageBuffer = SerializedImageBuffer::sinkIntoImageBuffer(WTF::move(pendingFrame->buffer), protect(protect(placeholder->canvas())->scriptExecutionContext())->graphicsClient());
    if (!imageBuffer)
        return;
    // Compares the frames, so that a possibly already historical buffer in this main thread
    // task does not override the newest buffer that the worker thread already set.
    protect(placeholder->layerContents())->copyFrame(*imageBuffer, pendingFrame->originClean, pendingFrame->opaque, pendingFrame->frame);
    placeholder->setPlaceholderBuffer(imageBuffer.releaseNonNull(), pendingFrame->frame, pendingFrame->originClean, pendingFrame->opaque);
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(PlaceholderRenderingContext);

static HashMap<PlaceholderRenderingContextIdentifier, WeakPtr<PlaceholderRenderingContext>>& placeholderRenderingContexts()
{
    assertIsMainThread();
    static NeverDestroyed<HashMap<PlaceholderRenderingContextIdentifier, WeakPtr<PlaceholderRenderingContext>>> contexts;
    return contexts;
}

static PlaceholderRenderingContextSource::PlaceholderLifetimeHandler placeholderCreatedHandler;
static PlaceholderRenderingContextSource::PlaceholderLifetimeHandler placeholderDestroyedHandler;

void PlaceholderRenderingContextSource::setPlaceholderLifetimeHandlers(PlaceholderLifetimeHandler created, PlaceholderLifetimeHandler destroyed)
{
    assertIsMainThread();
    placeholderCreatedHandler = created;
    placeholderDestroyedHandler = destroyed;
}

PlaceholderRenderingContext* PlaceholderRenderingContext::fromIdentifier(PlaceholderRenderingContextIdentifier identifier)
{
    return placeholderRenderingContexts().get(identifier);
}

std::unique_ptr<PlaceholderRenderingContext> PlaceholderRenderingContext::create(HTMLCanvasElement& element)
{
    return std::unique_ptr<PlaceholderRenderingContext> { new PlaceholderRenderingContext(element) };
}

PlaceholderRenderingContext::PlaceholderRenderingContext(HTMLCanvasElement& canvas)
    : CanvasRenderingContext(canvas, Type::Placeholder)
    , m_identifier(WTF::UUID::createVersion4())
    , m_layerContents(PlaceholderLayerContents::create())
{
    placeholderRenderingContexts().add(m_identifier, *this);
    if (placeholderCreatedHandler)
        placeholderCreatedHandler(m_identifier);
    m_layerContents->setNeedsFramesOnMainThread(canvas.hasObservers());
}

PlaceholderRenderingContext::~PlaceholderRenderingContext()
{
    placeholderRenderingContexts().remove(m_identifier);
    if (placeholderDestroyedHandler)
        placeholderDestroyedHandler(m_identifier);
}

HTMLCanvasElement& PlaceholderRenderingContext::canvas() const
{
    return downcast<HTMLCanvasElement>(canvasBase());
}

IntSize PlaceholderRenderingContext::size() const
{
    return canvas().size();
}

void PlaceholderRenderingContext::setContentsToLayer(GraphicsLayer& layer)
{
    auto layerID = m_layerContents->attach(layer, protect(m_buffer).get(), m_opaque, m_frame);
    if (m_reportedLayerID.asOptional() == layerID)
        return;
    m_reportedLayerID = layerID;
    // Lets a frame committed from another process be applied to the layer on the way here.
    if (RefPtr page = canvas().document().page())
        page->chrome().client().offscreenCanvasPlaceholderLayerChanged(m_identifier, layerID);
}

bool PlaceholderRenderingContextSource::commitFrameFromAnotherProcess(PlaceholderRenderingContextIdentifier identifier, const ImageBufferTransferHandle& transferHandle, PlaceholderFrameIdentifier frame, bool originClean, bool opaque)
{
    assertIsMainThread();
    RefPtr placeholder = PlaceholderRenderingContext::fromIdentifier(identifier);
    if (!placeholder)
        return false;
    RefPtr imageBuffer = ImageBuffer::createFromTransferHandle(transferHandle, protect(placeholder->canvas().document())->graphicsClient());
    if (!imageBuffer)
        return false;
    placeholder->setPlaceholderBufferFromAnotherProcess(imageBuffer.releaseNonNull(), frame, originClean, opaque);
    return true;
}

void PlaceholderRenderingContext::setPlaceholderBufferFromAnotherProcess(Ref<ImageBuffer>&& imageBuffer, PlaceholderFrameIdentifier frame, bool originClean, bool opaque)
{
    m_layerContents->setFrameForNextDisplay(imageBuffer, opaque, frame);
    setPlaceholderBuffer(WTF::move(imageBuffer), frame, originClean, opaque);
}

void PlaceholderRenderingContext::setPlaceholderBuffer(Ref<ImageBuffer>&& newBuffer, PlaceholderFrameIdentifier frame, bool originClean, bool opaque)
{
    if (frame <= m_frame)
        return;
    m_frame = frame;
    IntSize newSize = newBuffer->truncatedLogicalSize();
    Ref canvas = this->canvas();
    canvas->willUpdateContents(FloatRect { { }, newSize }, ShouldApplyPostProcessingToDirtyRect::No);
    updateMemoryCost(newBuffer->memoryCost());
    m_buffer = WTF::move(newBuffer);
    m_bufferNativeImage = nullptr;
    if (m_metadataFrame < frame)
        m_metadataFrame = frame;
    applyFrameMetadata({ newSize, originClean, opaque });
}

void PlaceholderRenderingContext::updateFrameMetadata()
{
    auto currentFrame = m_layerContents->currentFrameMetadataIfNewerThan(m_metadataFrame);
    if (!currentFrame || !currentFrame->metadata)
        return;
    m_metadataFrame = currentFrame->identifier;
    applyFrameMetadata(*currentFrame->metadata);
}

void PlaceholderRenderingContext::applyFrameMetadata(const PlaceholderLayerContents::FrameMetadata& metadata)
{
    m_opaque = metadata.opaque;
    Ref canvas = this->canvas();
    canvas->setSizeForControllingContext(metadata.size);
    if (metadata.originClean)
        canvas->setOriginClean();
    else
        canvas->setOriginTainted();
}

void PlaceholderRenderingContext::updateFromCurrentFrameIfNeeded()
{
    auto currentFrame = m_layerContents->copyCurrentFrameIfNewerThan(m_frame);
    if (!currentFrame)
        return;
    bool wasOriginClean = canvas().originClean();
    if (currentFrame->metadata && m_metadataFrame < currentFrame->identifier) {
        m_metadataFrame = currentFrame->identifier;
        applyFrameMetadata(*currentFrame->metadata);
    }
    if (!currentFrame->image || (wasOriginClean && !canvas().originClean()))
        return;
    m_frame = currentFrame->identifier;
    m_buffer = nullptr;
    m_bufferNativeImage = WTF::move(currentFrame->image);
    updateMemoryCost(m_bufferNativeImage->sizeInBytes());
}

RefPtr<ImageBuffer> PlaceholderRenderingContext::ensureBufferFromNativeImage()
{
    if (m_buffer || !m_bufferNativeImage)
        return m_buffer;
    auto size = m_bufferNativeImage->size();
    m_buffer = ImageBuffer::create(size, RenderingMode::Unaccelerated, RenderingPurpose::Unspecified, 1, m_bufferNativeImage->colorSpace(), PixelFormat::BGRA8);
    if (m_buffer)
        m_buffer->context().drawNativeImage(*m_bufferNativeImage, FloatRect { { }, size }, FloatRect { { }, size }, { CompositeOperator::Copy });
    return m_buffer;
}

void PlaceholderRenderingContext::didChangeCanvasObservers()
{
    m_layerContents->setNeedsFramesOnMainThread(canvas().hasObservers());
}

PixelFormat PlaceholderRenderingContext::pixelFormat() const
{
    if (auto* buffer = m_buffer.get())
        return buffer->pixelFormat();
    return CanvasRenderingContext::pixelFormat();
}

RefPtr<ImageBuffer> PlaceholderRenderingContext::surfaceBufferToImageBuffer(SurfaceBuffer)
{
    updateFromCurrentFrameIfNeeded();
    if (!ensureBufferFromNativeImage()) {
        // Transparent black bitmaps are not cached.
        return protect(canvas())->createTransparentBlackImageBuffer();
    }
    return m_buffer;
}

RefPtr<NativeImage> PlaceholderRenderingContext::surfaceBufferToNativeImage(SurfaceBuffer)
{
    updateFromCurrentFrameIfNeeded();
    if (m_bufferNativeImage)
        return m_bufferNativeImage;
    RefPtr buffer = m_buffer;
    if (!buffer) {
        // No frame has been committed yet, so the placeholder reads as transparent black.
        return ImageBuffer::sinkIntoNativeImage(protect(canvas())->createTransparentBlackImageBuffer());
    }
    m_bufferNativeImage = buffer->copyNativeImage();
    return m_bufferNativeImage;
}

bool PlaceholderRenderingContext::isSurfaceBufferTransparentBlack(SurfaceBuffer) const
{
    return !m_buffer && !m_bufferNativeImage && !m_layerContents->currentFrame();
}

void PlaceholderRenderingContext::didUpdateCanvasSizeProperties(bool)
{
}

}

#endif
