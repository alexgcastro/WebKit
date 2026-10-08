/*
 * Copyright (C) 2017 Metrological Group B.V.
 * Copyright (C) 2017, 2024 Igalia S.L.
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
#include "CoordinatedTileBuffer.h"

#if USE(COORDINATED_GRAPHICS)

#if USE(SKIA)
#include "FontRenderOptions.h"
#include "GLContext.h"
#include "GLFence.h"
#include "PlatformDisplay.h"
#include "ProcessCapabilities.h"
#include "SkiaUtilities.h"
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkStream.h>
#include <skia/gpu/ganesh/gl/GrGLDirectContext.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#include <wtf/MainThread.h>

#if USE(LIBEPOXY)
#include <epoxy/gl.h>
#else
#include <GLES3/gl3.h>
#endif
#endif

#if USE(CAIRO)
#include <cairo.h>
#endif

namespace WebCore {

Lock CoordinatedTileBuffer::s_layersMemoryUsageLock;
double CoordinatedTileBuffer::s_currentLayersMemoryUsage = 0.0;
double CoordinatedTileBuffer::s_maxLayersMemoryUsage = 0.0;

void CoordinatedTileBuffer::resetMemoryUsage()
{
    Locker locker { s_layersMemoryUsageLock };
    s_maxLayersMemoryUsage = s_currentLayersMemoryUsage;
}

double CoordinatedTileBuffer::getMemoryUsage()
{
    // The memory usage is max of memory usage since last resetMemoryUsage or getMemoryUsage.
    Locker locker { s_layersMemoryUsageLock };
    const auto memoryUsage = s_maxLayersMemoryUsage;
    s_maxLayersMemoryUsage = s_currentLayersMemoryUsage;
    return memoryUsage;
}

CoordinatedTileBuffer::PaintingState CoordinatedTileBuffer::paintingState() const
{
    Locker locker { m_painting.lock };
    return m_painting.state;
}

bool CoordinatedTileBuffer::beginPainting()
{
    Locker locker { m_painting.lock };
    if (m_painting.state == PaintingState::Cancelled)
        return false;

    ASSERT(m_painting.state == PaintingState::Pending);
    m_painting.state = PaintingState::Painting;
    return true;
}

bool CoordinatedTileBuffer::completePainting()
{
    Locker locker { m_painting.lock };
    ASSERT(m_painting.state == PaintingState::Painting);
    m_painting.state = PaintingState::Complete;
    m_painting.condition.notifyAll();

#if USE(SKIA)
    // Surface is no longer needed, destroy it (in the same thread that created it).
    m_surface = nullptr;
#endif

    return !!std::exchange(m_painting.renderingUpdateID, std::nullopt);
}

CoordinatedTileBuffer::UpdateDistanceResult CoordinatedTileBuffer::updateDistance(unsigned distance, uint64_t renderingUpdateID)
{
    Locker locker { m_painting.lock };
    m_painting.distance = distance;

    if (m_painting.state == PaintingState::Complete || m_painting.state == PaintingState::Cancelled)
        return UpdateDistanceResult::NoChange;

    bool didHaveRenderingUpdateID = m_painting.renderingUpdateID.has_value();

    if (distance) {
        // Passed renderingUpdateID is ignored for hidden tiles.
        m_painting.renderingUpdateID = std::nullopt;
        return didHaveRenderingUpdateID ? UpdateDistanceResult::BecameHidden : UpdateDistanceResult::NoChange;
    }

    m_painting.renderingUpdateID = renderingUpdateID;
    return !didHaveRenderingUpdateID ? UpdateDistanceResult::BecameVisible : UpdateDistanceResult::NoChange;
}

void CoordinatedTileBuffer::cancelPainting()
{
    Locker locker { m_painting.lock };
    if (m_painting.state != PaintingState::Pending)
        return;

    m_painting.state = PaintingState::Cancelled;
    m_painting.condition.notifyAll();
}

bool CoordinatedTileBuffer::cancelPaintingOrResetRenderingUpdate()
{
    Locker locker { m_painting.lock };
    switch (m_painting.state) {
    case PaintingState::Pending:
        m_painting.state = PaintingState::Cancelled;
        m_painting.condition.notifyAll();
        return !!std::exchange(m_painting.renderingUpdateID, std::nullopt);
    case PaintingState::Painting: {
        bool didHaveRenderingUpdateID = m_painting.renderingUpdateID.has_value();
        m_painting.renderingUpdateID = std::nullopt;
        return didHaveRenderingUpdateID;
    }
    case PaintingState::Complete:
    case PaintingState::Cancelled:
        break;
    }

    return false;
}

unsigned CoordinatedTileBuffer::distance()
{
    Locker locker { m_painting.lock };
    return m_painting.distance;
}

void CoordinatedTileBuffer::waitUntilPaintingFinished()
{
    Locker locker { m_painting.lock };
    m_painting.condition.wait(m_painting.lock, [this] {
        assertIsHeld(m_painting.lock);
        return m_painting.state == PaintingState::Complete || m_painting.state == PaintingState::Cancelled;
    });
}

Ref<CoordinatedTileBuffer> CoordinatedUnacceleratedTileBuffer::create(const IntSize& size, Flags flags, unsigned distance, std::optional<uint64_t> renderingUpdateID)
{
    return adoptRef(*new CoordinatedUnacceleratedTileBuffer(size, flags, distance, renderingUpdateID));
}

CoordinatedUnacceleratedTileBuffer::CoordinatedUnacceleratedTileBuffer(const IntSize& size, Flags flags, unsigned distance, std::optional<uint64_t> renderingUpdateID)
    : CoordinatedTileBuffer(flags, distance, renderingUpdateID)
    , m_size(size)
{
}

CoordinatedUnacceleratedTileBuffer::~CoordinatedUnacceleratedTileBuffer()
{
    if (m_data.isEmpty())
        return;

    Locker locker { s_layersMemoryUsageLock };
    s_currentLayersMemoryUsage -= m_data.size();
}

#if USE(CAIRO)
RefPtr<cairo_surface_t> CoordinatedUnacceleratedTileBuffer::createSurface()
{
    const auto checkedArea = m_size.area() * 4;
    if (checkedArea.hasOverflowed() || !m_data.tryReserveCapacity(checkedArea.value()))
        return nullptr;

    m_data.grow(checkedArea.value());

    {
        Locker locker { s_layersMemoryUsageLock };
        s_currentLayersMemoryUsage += checkedArea;
        s_maxLayersMemoryUsage = std::max(s_maxLayersMemoryUsage, s_currentLayersMemoryUsage);
    }

    return adoptRef(cairo_image_surface_create_for_data(m_data.mutableSpan().data(), CAIRO_FORMAT_ARGB32, m_size.width(), m_size.height(), m_size.width() * 4));
}
#endif

#if USE(SKIA)
SkCanvas* CoordinatedUnacceleratedTileBuffer::canvas()
{
    if (!m_surface) {
        const auto checkedArea = m_size.area() * 4;
        if (checkedArea.hasOverflowed() || !m_data.tryReserveCapacity(checkedArea.value()))
            return nullptr;

        m_data.grow(checkedArea.value());

        {
            Locker locker { s_layersMemoryUsageLock };
            s_currentLayersMemoryUsage += checkedArea;
            s_maxLayersMemoryUsage = std::max(s_maxLayersMemoryUsage, s_currentLayersMemoryUsage);
        }

        auto alphaType = supportsAlpha() ? kPremul_SkAlphaType : kOpaque_SkAlphaType;
        auto imageInfo = SkImageInfo::Make(m_size.width(), m_size.height(), kBGRA_8888_SkColorType, alphaType, SkColorSpace::MakeSRGB());
        auto properties = FontRenderOptions::singleton().createSurfaceProps();
        m_surface = SkSurfaces::WrapPixels(imageInfo, m_data.mutableSpan().data(), imageInfo.minRowBytes64(), &properties);
    }
    return m_surface->getCanvas();
}
#endif

Vector<unsigned char> CoordinatedUnacceleratedTileBuffer::takeData()
{
    auto data = WTF::move(m_data);

    {
        Locker locker { s_layersMemoryUsageLock };
        s_currentLayersMemoryUsage -= data.size();
    }

    return data;
}

#if USE(SKIA)
#if USE(TEXTURE_MAPPER)
Ref<CoordinatedTileBuffer> CoordinatedAcceleratedTileBuffer::create(Ref<BitmapTexture>&& texture, unsigned distance, std::optional<uint64_t> renderingUpdateID)
{
    auto flags = CoordinatedTileBuffer::Flags { texture->isOpaque() ? CoordinatedTileBuffer::NoFlags : CoordinatedTileBuffer::SupportsAlpha };
    return adoptRef(*new CoordinatedAcceleratedTileBuffer(WTF::move(texture), flags, distance, renderingUpdateID));
}

CoordinatedAcceleratedTileBuffer::CoordinatedAcceleratedTileBuffer(Ref<BitmapTexture>&& texture, Flags flags, unsigned distance, std::optional<uint64_t> renderingUpdateID)
    : CoordinatedTileBuffer(flags, distance, renderingUpdateID)
    , m_texture(WTF::move(texture))
{
}

#else

Ref<CoordinatedTileBuffer> CoordinatedAcceleratedTileBuffer::create(const sk_sp<GrContextThreadSafeProxy>& threadSafeGrContext, const IntSize& size, Flags flags, unsigned distance, std::optional<uint64_t> renderingUpdateID)
{
    auto alphaType = (flags & SupportsAlpha) ? kPremul_SkAlphaType : kOpaque_SkAlphaType;
    auto imageInfo = SkImageInfo::Make(size.width(), size.height(), kRGBA_8888_SkColorType, alphaType, SkColorSpace::MakeSRGB());
    auto backendFormat = threadSafeGrContext->defaultBackendFormat(kRGBA_8888_SkColorType, GrRenderable::kYes);
    ASSERT(backendFormat.isValid());
    auto properties = FontRenderOptions::singleton().createSurfaceProps();
    auto maxResourceCacheBytes = PlatformDisplay::sharedDisplay().maxSkiaResourceCacheBytes();
    auto characterization = threadSafeGrContext->createCharacterization(maxResourceCacheBytes, imageInfo, backendFormat, 0, kTopLeft_GrSurfaceOrigin, properties, skgpu::Mipmapped::kNo);
    return adoptRef(*new CoordinatedAcceleratedTileBuffer(WTF::move(characterization), flags, distance, renderingUpdateID));
}

CoordinatedAcceleratedTileBuffer::CoordinatedAcceleratedTileBuffer(GrSurfaceCharacterization&& characterization, Flags flags, unsigned distance, std::optional<uint64_t> renderingUpdateID)
    : CoordinatedTileBuffer(flags, distance, renderingUpdateID)
    , m_characterization(WTF::move(characterization))
{
}
#endif

CoordinatedAcceleratedTileBuffer::~CoordinatedAcceleratedTileBuffer() = default;

IntSize CoordinatedAcceleratedTileBuffer::size() const
{
#if USE(TEXTURE_MAPPER)
    return m_texture->size();
#else
    return { m_characterization.width(), m_characterization.height() };
#endif
}

SkCanvas* CoordinatedAcceleratedTileBuffer::canvas()
{
#if USE(TEXTURE_MAPPER)
    if (!m_surface) {
        if (!PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent())
            return nullptr;

        m_surface = m_texture->createSkiaSurface(PlatformDisplay::sharedDisplay().skiaGrContext());
    }
    return m_surface->getCanvas();
#else
    if (!m_recorder)
        m_recorder.emplace(m_characterization);
    return m_recorder->getCanvas();
#endif
}

bool CoordinatedAcceleratedTileBuffer::completePainting()
{
#if USE(TEXTURE_MAPPER)
    if (m_surface) {
        auto* recordingContext = m_surface->recordingContext();
        auto* grContext = recordingContext ? recordingContext->asDirectContext() : nullptr;
        if (!grContext)
            return CoordinatedTileBuffer::completePainting();

        m_fence = SkiaUtilities::flushAndSubmitSurfaceWithFence(grContext, m_surface.get());
    }
#else
    if (m_recorder)
        m_displayList = m_recorder->detach();
#endif

    return CoordinatedTileBuffer::completePainting();
}

#if USE(TEXTURE_MAPPER)
void CoordinatedAcceleratedTileBuffer::serverWait()
{
    if (!m_fence)
        return;

    m_fence->serverWait();
    m_fence = nullptr;
}
#endif
#endif // USE(SKIA)

CoordinatedTileBuffer::CoordinatedTileBuffer(Flags flags, unsigned distance, std::optional<uint64_t> renderingUpdateID)
    : m_flags(flags)
{
    m_painting.distance = distance;
    m_painting.renderingUpdateID = renderingUpdateID;
}

CoordinatedTileBuffer::~CoordinatedTileBuffer() = default;

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS)
