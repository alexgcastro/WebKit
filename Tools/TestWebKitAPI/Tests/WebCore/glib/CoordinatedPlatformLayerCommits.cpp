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

#if USE(COORDINATED_GRAPHICS)
#include "Helpers/Test.h"
#include <WebCore/CoordinatedPlatformLayer.h>
#include <WebCore/CoordinatedPlatformLayerBuffer.h>
#include <wtf/Threading.h>
#include <wtf/threads/BinarySemaphore.h>

namespace TestWebKitAPI {
using namespace WebCore;

class TestContentsBuffer final : public CoordinatedPlatformLayerBuffer {
public:
    TestContentsBuffer()
        : CoordinatedPlatformLayerBuffer(Type::RGB, { 64, 64 }, { }, nullptr)
    {
    }

private:
#if USE(TEXTURE_MAPPER)
    void paintToTextureMapper(TextureMapper&, const FloatRect&, const TransformationMatrix&, float) final { }
#endif
};

static void runOnCompositorThread(Function<void()>&& function)
{
    BinarySemaphore semaphore;
    Thread::create("test-compositor"_s, [&] {
        function();
        semaphore.signal();
    })->detach();
    semaphore.wait();
}

// A composite requested by the media player for a decoded video frame runs on the compositor
// thread regardless of what the main thread is doing, so it must not consume contents staged
// by a rendering update that has not committed yet: applying them early presents a frame the
// main thread considers still under construction.
TEST(CoordinatedPlatformLayerCommits, VideoFrameCompositeDoesNotConsumeUncommittedContents)
{
    auto layer = CoordinatedPlatformLayer::create();
    {
        Locker locker { layer->lock() };
        layer->setContentsBuffer(makeUnique<TestContentsBuffer>(), std::nullopt, CoordinatedPlatformLayer::RequireComposition::No);
    }
    EXPECT_TRUE(layer->hasPendingContentsBufferForTesting());

    runOnCompositorThread([&] {
        layer->flushCompositingState({ CompositionReason::VideoFrame }, 1);
        layer->invalidateTarget();
    });

    EXPECT_TRUE(layer->hasPendingContentsBufferForTesting());
}

// Once the state is committed, a rendering-update composite consumes the queued commit.
TEST(CoordinatedPlatformLayerCommits, RenderingUpdateCompositeConsumesCommittedContents)
{
    auto layer = CoordinatedPlatformLayer::create();
    {
        Locker locker { layer->lock() };
        layer->setContentsBuffer(makeUnique<TestContentsBuffer>(), std::nullopt, CoordinatedPlatformLayer::RequireComposition::No);
    }

    layer->commitState(1);
    EXPECT_FALSE(layer->hasPendingContentsBufferForTesting());
    EXPECT_TRUE(layer->hasQueuedCommitsForTesting());

    runOnCompositorThread([&] {
        layer->flushCompositingState({ CompositionReason::RenderingUpdate }, 1);
        layer->invalidateTarget();
    });

    EXPECT_FALSE(layer->hasQueuedCommitsForTesting());
}

} // namespace TestWebKitAPI

#endif // USE(COORDINATED_GRAPHICS)
