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
#include "GraphicsLayerAsyncContentsDisplayDelegateCoordinated.h"

#if USE(COORDINATED_GRAPHICS)
#include "CoordinatedPlatformLayer.h"
#include "CoordinatedPlatformLayerBufferProxy.h"
#include "GraphicsLayer.h"
#include "GraphicsLayerContentsDisplayDelegateCoordinated.h"
#include "GraphicsLayerCoordinated.h"
#include "ImageBuffer.h"
#include "NativeImage.h"
#include <wtf/MainThread.h>

#if USE(TEXTURE_MAPPER)
#include "CoordinatedPlatformLayerBufferNativeImage.h"
#else
#include "CoordinatedPlatformLayerBufferSkiaImage.h"
#endif

namespace WebCore {

GraphicsLayerAsyncContentsDisplayDelegateCoordinated::GraphicsLayerAsyncContentsDisplayDelegateCoordinated(GraphicsLayer& layer)
    : m_delegate(GraphicsLayerContentsDisplayDelegateCoordinated::create())
{
    layer.setContentsDisplayDelegate(m_delegate.ptr(), GraphicsLayer::ContentsLayerPurpose::Canvas);
    bindBufferProxy(layer);
}

GraphicsLayerAsyncContentsDisplayDelegateCoordinated::~GraphicsLayerAsyncContentsDisplayDelegateCoordinated()
{
    RefPtr bufferProxy = WTF::move(m_bufferProxy);
    if (!bufferProxy)
        return;
#if !USE(TEXTURE_MAPPER)
    bufferProxy->releaseImageOnCompositingThread(takeLastImage());
#endif
    ensureOnMainThread([bufferProxy = WTF::move(bufferProxy)] {
        bufferProxy->invalidate();
    });
}

void GraphicsLayerAsyncContentsDisplayDelegateCoordinated::bindBufferProxy(GraphicsLayer& layer)
{
    assertIsMainThread();
    auto* coordinatedLayer = dynamicDowncast<GraphicsLayerCoordinated>(layer);
    RefPtr platformLayer = coordinatedLayer ? &coordinatedLayer->coordinatedPlatformLayer() : nullptr;
    if (m_boundLayer == platformLayer)
        return;
    if (RefPtr bufferProxy = std::exchange(m_bufferProxy, nullptr))
        bufferProxy->invalidate();
    m_boundLayer = platformLayer;
    if (!platformLayer)
        return;
    m_bufferProxy = CoordinatedPlatformLayerBufferProxy::create(platformLayer.releaseNonNull());
#if !USE(TEXTURE_MAPPER)
    if (auto lastImage = this->lastImage()) {
        auto alphaMode = lastImage->isOpaque() ? CoordinatedPlatformLayerBufferSkiaImage::AlphaMode::Opaque : CoordinatedPlatformLayerBufferSkiaImage::AlphaMode::Premultiplied;
        Ref { *m_bufferProxy }->setDisplayBuffer(makeUnique<CoordinatedPlatformLayerBufferSkiaImage>(WTF::move(lastImage), alphaMode, CoordinatedPlatformLayerBufferSkiaImage::Rotation::None));
    }
#endif
}

bool GraphicsLayerAsyncContentsDisplayDelegateCoordinated::tryCopyToLayer(ImageBuffer& imageBuffer, bool opaque, PlaceholderFrameIdentifier frame)
{
    return tryPresent(imageBuffer, opaque, frame, nullptr);
}

bool GraphicsLayerAsyncContentsDisplayDelegateCoordinated::tryPresent(ImageBuffer& imageBuffer, bool, PlaceholderFrameIdentifier, RefPtr<GraphicsLayerFrameDisplayNotifier>&& frameDisplayNotifier)
{
    RefPtr bufferProxy = m_bufferProxy;
    if (!bufferProxy)
        return false;

#if USE(TEXTURE_MAPPER)
    auto image = ImageBuffer::sinkIntoNativeImage(imageBuffer.clone());
    if (!image)
        return false;
    auto buffer = CoordinatedPlatformLayerBufferNativeImage::create(image.releaseNonNull(), nullptr);
    buffer->setDisplayNotifier(WTF::move(frameDisplayNotifier));
    bufferProxy->setDisplayBuffer(WTF::move(buffer));
#else
    auto threadSafeGrContext = bufferProxy->threadSafeGrContext();
    if (!threadSafeGrContext)
        return false;

    auto buffer = imageBuffer.createCompositorDisplayBuffer(threadSafeGrContext);
    if (!buffer) {
        RefPtr image = imageBuffer.createNativeImageReference();
        if (!image)
            return false;
        buffer = CoordinatedPlatformLayerBufferSkiaImage::create(image->platformImage(), threadSafeGrContext);
    }
    sk_sp<SkImage> previousImage;
    {
        Locker locker { m_lastImageLock };
        previousImage = std::exchange(m_lastImage, buffer->skiaImage());
    }
    bufferProxy->releaseImageOnCompositingThread(WTF::move(previousImage));
    buffer->setDisplayNotifier(WTF::move(frameDisplayNotifier));
    bufferProxy->setDisplayBuffer(WTF::move(buffer));
#endif

    return true;
}

#if !USE(TEXTURE_MAPPER)
RefPtr<NativeImage> GraphicsLayerAsyncContentsDisplayDelegateCoordinated::copyCurrentBuffer()
{
    assertIsMainThread();
    RefPtr bufferProxy = m_bufferProxy;
    if (!bufferProxy)
        return nullptr;
    auto image = lastImage();
    RefPtr copy = bufferProxy->copyImage(image);
    bufferProxy->releaseImageOnCompositingThread(WTF::move(image));
    return copy;
}

sk_sp<SkImage> GraphicsLayerAsyncContentsDisplayDelegateCoordinated::lastImage()
{
    Locker locker { m_lastImageLock };
    return m_lastImage;
}

sk_sp<SkImage> GraphicsLayerAsyncContentsDisplayDelegateCoordinated::takeLastImage()
{
    Locker locker { m_lastImageLock };
    return std::exchange(m_lastImage, nullptr);
}
#endif

void GraphicsLayerAsyncContentsDisplayDelegateCoordinated::updateGraphicsLayer(GraphicsLayer& layer)
{
    layer.setContentsDisplayDelegate(m_delegate.ptr(), GraphicsLayer::ContentsLayerPurpose::Canvas);
    bindBufferProxy(layer);
}

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS)
