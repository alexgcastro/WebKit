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

#pragma once

#if USE(COORDINATED_GRAPHICS)

#include "BitmapTexture.h"
#include "IntSize.h"
#include "PixelFormat.h"
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/Ref.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>

#if USE(SKIA)
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkCanvas.h>
#include <skia/core/SkSurface.h>
#include <skia/private/chromium/GrDeferredDisplayList.h>
#include <skia/private/chromium/GrDeferredDisplayListRecorder.h>
#include <skia/private/chromium/GrSurfaceCharacterization.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#endif

#if USE(CAIRO)
#include "RefPtrCairo.h"
#endif

namespace WebCore {
class BitmapTexture;
class GLFence;

class CoordinatedTileBuffer : public ThreadSafeRefCounted<CoordinatedTileBuffer> {
public:
    enum Flag {
        NoFlags = 0,
        SupportsAlpha = 1 << 0,
    };
    using Flags = unsigned;

    WEBCORE_EXPORT virtual ~CoordinatedTileBuffer();

    virtual IntSize size() const = 0;
    virtual PixelFormat pixelFormat() const = 0;
    virtual bool isBackedByOpenGL() const = 0;

    bool supportsAlpha() const { return m_flags & SupportsAlpha; }

    enum class PaintingState {
        Pending,
        Painting,
        Complete,
        Cancelled
    };

    PaintingState paintingState() const;

    virtual bool beginPainting();
    virtual bool completePainting();
    void cancelPainting();
    bool cancelPaintingOrResetRenderingUpdate();
    unsigned distance();
    void waitUntilPaintingFinished();

    enum class UpdateDistanceResult {
        NoChange,
        BecameVisible,
        BecameHidden
    };
    UpdateDistanceResult updateDistance(unsigned distance, uint64_t renderingUpdateID);

#if USE(SKIA)
    virtual SkCanvas* canvas() = 0;
#endif

    static void resetMemoryUsage();
    static double getMemoryUsage();

protected:
    CoordinatedTileBuffer(Flags, unsigned distance, std::optional<uint64_t> renderingUpdateID);

#if USE(SKIA)
    sk_sp<SkSurface> m_surface;
#endif

    static Lock s_layersMemoryUsageLock;
    static double s_currentLayersMemoryUsage;
    static double s_maxLayersMemoryUsage;

    struct {
        mutable Lock lock;
        Condition condition;
        PaintingState state WTF_GUARDED_BY_LOCK(lock) { PaintingState::Pending };
        std::optional<uint64_t> renderingUpdateID WTF_GUARDED_BY_LOCK(lock);
        unsigned distance WTF_GUARDED_BY_LOCK(lock) { 0 };
    } m_painting;

private:
    Flags m_flags;
};

class CoordinatedUnacceleratedTileBuffer final : public CoordinatedTileBuffer {
public:
    WEBCORE_EXPORT static Ref<CoordinatedTileBuffer> create(const IntSize&, Flags, unsigned distance, std::optional<uint64_t> renderingUpdateID);
    WEBCORE_EXPORT virtual ~CoordinatedUnacceleratedTileBuffer();

    int stride() const { return m_size.width() * 4; }

#if USE(CAIRO)
    RefPtr<cairo_surface_t> createSurface();
#endif

    Vector<unsigned char> takeData();

private:
    CoordinatedUnacceleratedTileBuffer(const IntSize&, Flags, unsigned distance, std::optional<uint64_t> renderingUpdateID);

    bool isBackedByOpenGL() const final { return false; }
    IntSize size() const final { return m_size; }
    PixelFormat pixelFormat() const final { return PixelFormat::BGRA8; }

#if USE(SKIA)
    SkCanvas* canvas() final;
#endif

    Vector<unsigned char> m_data;
    IntSize m_size;
};

#if USE(SKIA)
class CoordinatedAcceleratedTileBuffer final : public CoordinatedTileBuffer {
public:
#if USE(TEXTURE_MAPPER)
    WEBCORE_EXPORT static Ref<CoordinatedTileBuffer> create(Ref<BitmapTexture>&&, unsigned distance, std::optional<uint64_t> renderingUpdateID);
#else
    WEBCORE_EXPORT static Ref<CoordinatedTileBuffer> create(const sk_sp<GrContextThreadSafeProxy>&, const IntSize&, Flags, unsigned distance, std::optional<uint64_t> renderingUpdateID);
#endif
    WEBCORE_EXPORT virtual ~CoordinatedAcceleratedTileBuffer();

#if USE(TEXTURE_MAPPER)
    Ref<BitmapTexture> texture() const { return m_texture; }
    void serverWait();
#else
    sk_sp<GrDeferredDisplayList> takeDisplayList() { return std::exchange(m_displayList, nullptr); }
#endif

private:
#if USE(TEXTURE_MAPPER)
    CoordinatedAcceleratedTileBuffer(Ref<BitmapTexture>&&, Flags, unsigned distance, std::optional<uint64_t> renderingUpdateID);
#else
    CoordinatedAcceleratedTileBuffer(GrSurfaceCharacterization&&, Flags, unsigned distance, std::optional<uint64_t> renderingUpdateID);
#endif

    bool isBackedByOpenGL() const final { return true; }
    IntSize size() const final;
    PixelFormat pixelFormat() const final { return PixelFormat::RGBA8; }

    SkCanvas* canvas() final;
    bool completePainting() final;

#if USE(TEXTURE_MAPPER)
    const Ref<BitmapTexture> m_texture;
    std::unique_ptr<GLFence> m_fence;
#else
    GrSurfaceCharacterization m_characterization;
    std::optional<GrDeferredDisplayListRecorder> m_recorder;
    sk_sp<GrDeferredDisplayList> m_displayList;
#endif
};
#endif // USE(SKIA)

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS)
