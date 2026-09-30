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
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "PlatformDisplay.h"

#if USE(SKIA)
#include "FontRenderOptions.h"
#include "GLContext.h"

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkExecutor.h>
#include <skia/gpu/ganesh/GrBackendSurface.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
#include <skia/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <skia/gpu/ganesh/gl/GrGLDirectContext.h>
#include <skia/gpu/ganesh/gl/GrGLInterface.h>

#if USE(LIBEPOXY)
#include <epoxy/egl.h>
#include <skia/gpu/ganesh/gl/epoxy/GrGLMakeEpoxyEGLInterface.h>
#else
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <skia/gpu/ganesh/gl/egl/GrGLMakeEGLInterface.h>
#endif
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

#include <wtf/FileSystem.h>
#include <wtf/MemoryPressureHandler.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RAMSize.h>
#include <wtf/SystemTracing.h>
#include <wtf/SHA1.h>
#include <wtf/ThreadSafeWeakPtr.h>
#include <wtf/WorkQueue.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

#if OS(ANDROID)
#include <sys/system_properties.h>
#endif

#if USE(LIBDRM)
#include <fcntl.h>
#include <unistd.h>
#include <wtf/unix/UnixFileDescriptor.h>
#include <xf86drm.h>
#endif

namespace WebCore {

#if OS(ANDROID)
// GPU program cache shared by every GrDirectContext in the process and kept on disk.
//
// Each thread that paints or composites with Skia gets its own GrDirectContext, and without
// a cache every one of them compiles every program it needs from scratch, on every run. On
// Mali a compile costs around 13ms and they land inside the compositor's flush(), mostly
// on frames that bring in new tiles, which is what drops frames during a fling. Skia hands
// over the driver's program binary (kBackendBinary), so a hit skips the compile entirely.
//
// The whole directory is read when the first context is created, so load() never touches
// the disk; new programs are written from a background queue.
//
// debug.wpe.skia.programcache: "off" (no cache, the upstream behaviour), "memory" (shared
// between contexts only) or unset/"disk".
class PersistentProgramCache final : public GrContextOptions::PersistentCache {
public:
    enum class Mode { Off, Memory, Disk };

    static Mode mode()
    {
        static const Mode mode = [] {
            char value[PROP_VALUE_MAX] = { };
            if (__system_property_get("debug.wpe.skia.programcache", value) > 0) {
                if (!strcmp(value, "off") || !strcmp(value, "0"))
                    return Mode::Off;
                if (!strcmp(value, "memory"))
                    return Mode::Memory;
            }
            return Mode::Disk;
        }();
        return mode;
    }

    static PersistentProgramCache& singleton()
    {
        static NeverDestroyed<PersistentProgramCache> cache;
        return cache.get();
    }

    sk_sp<SkData> load(const SkData& key) final
    {
        Locker locker { m_lock };
        auto it = m_programs.find(fileNameForKey(key));
        if (it == m_programs.end() || it->value.key->size() != key.size() || memcmp(it->value.key->data(), key.data(), key.size())) {
            WPE_TRACE_COUNTER("wpe.skia.program-cache-misses", ++m_misses);
            return nullptr;
        }
        WPE_TRACE_COUNTER("wpe.skia.program-cache-hits", ++m_hits);
        return it->value.data;
    }

    void store(const SkData& key, const SkData& data, const SkString& description) final
    {
        WPE_TRACE_INSTANT("skia: compiled a GPU program (%zu bytes) %s", data.size(), description.c_str());
        WPE_TRACE_COUNTER("wpe.skia.programs-compiled", ++m_compilations);

        auto fileName = fileNameForKey(key);
        auto keyCopy = SkData::MakeWithCopy(key.data(), key.size());
        auto dataCopy = SkData::MakeWithCopy(data.data(), data.size());
        {
            Locker locker { m_lock };
            m_programs.set(fileName, Entry { keyCopy, dataCopy });
        }

        if (m_directory.isNull())
            return;

        // File layout: key size (uint32), key, program.
        Vector<uint8_t> contents;
        uint32_t keySize = key.size();
        contents.append(std::span { reinterpret_cast<const uint8_t*>(&keySize), sizeof(keySize) });
        contents.append(std::span { keyCopy->bytes(), keyCopy->size() });
        contents.append(std::span { dataCopy->bytes(), dataCopy->size() });
        m_writeQueue->dispatch([path = FileSystem::pathByAppendingComponent(m_directory, fileName), contents = WTF::move(contents)] {
            auto temporaryPath = makeString(path, ".tmp"_s);
            if (FileSystem::overwriteEntireFile(temporaryPath, contents.span()))
                FileSystem::moveFile(temporaryPath, path);
        });
    }

private:
    friend class NeverDestroyed<PersistentProgramCache>;

    struct Entry {
        sk_sp<SkData> key;
        sk_sp<SkData> data;
    };

    PersistentProgramCache()
        : m_writeQueue(WorkQueue::create("org.webkit.SkiaProgramCache"_s, WorkQueue::QOS::Background))
    {
        if (mode() != Mode::Disk)
            return;

        auto base = FileSystem::userCacheDirectory();
        if (base.isEmpty())
            return;
        m_directory = FileSystem::pathByAppendingComponent(base, "wpe-skia-programs"_s);
        if (!FileSystem::makeAllDirectories(m_directory)) {
            m_directory = { };
            return;
        }

        WPE_TRACE_SCOPE("skia: load program cache");
        size_t bytes = 0;
        for (auto& fileName : FileSystem::listDirectory(m_directory)) {
            if (fileName.endsWith(".tmp"_s))
                continue;
            auto contents = FileSystem::readEntireFile(FileSystem::pathByAppendingComponent(m_directory, fileName));
            if (!contents || contents->size() < sizeof(uint32_t))
                continue;
            uint32_t keySize;
            memcpy(&keySize, contents->span().data(), sizeof(keySize));
            if (contents->size() <= sizeof(uint32_t) + keySize)
                continue;
            auto bytesAfterSize = contents->span().subspan(sizeof(uint32_t));
            auto keyBytes = bytesAfterSize.first(keySize);
            auto programBytes = bytesAfterSize.subspan(keySize);
            m_programs.set(fileName, Entry {
                SkData::MakeWithCopy(keyBytes.data(), keyBytes.size()),
                SkData::MakeWithCopy(programBytes.data(), programBytes.size())
            });
            bytes += contents->size();
        }
        WPE_TRACE_INSTANT("skia: program cache loaded, %u programs, %zu bytes", m_programs.size(), bytes);
    }

    static String fileNameForKey(const SkData& key)
    {
        SHA1 sha1;
        sha1.addBytes(std::span { key.bytes(), key.size() });
        return String::fromLatin1(sha1.computeHexDigest().data());
    }

    Lock m_lock;
    HashMap<String, Entry> m_programs WTF_GUARDED_BY_LOCK(m_lock);
    String m_directory;
    Ref<WorkQueue> m_writeQueue;
    std::atomic<int64_t> m_hits { 0 };
    std::atomic<int64_t> m_misses { 0 };
    std::atomic<int64_t> m_compilations { 0 };
};

static bool reducedShaderVariationsRequested()
{
    static const bool requested = [] {
        char value[PROP_VALUE_MAX] = { };
        return __system_property_get("debug.wpe.skia.reducedvariants", value) > 0 && !strcmp(value, "1");
    }();
    return requested;
}

// Skia's small-path renderer turns every antialiased path into a distance field on the CPU,
// and tiles are recorded as DDLs, so that work happens when the tile is replayed: inside the
// compositor's flush(), on the frame that needed to present. It was the largest single cost
// of the composites that drop frames during a fling. Chrome and Android's HWUI both disable
// it; paths then take the triangulating or software renderer, and the software renderer draws
// its mask while the DDL is recorded, on the painting thread. debug.wpe.skia.dfpaths=1
// restores the upstream behaviour.
static bool distanceFieldPathsRequested()
{
    static const bool requested = [] {
        char value[PROP_VALUE_MAX] = { };
        return __system_property_get("debug.wpe.skia.dfpaths", value) > 0 && !strcmp(value, "1");
    }();
    return requested;
}
#endif


#if PLATFORM(GTK) || PLATFORM(WPE)
#if CPU(X86) || CPU(X86_64)
// On x86 ot x86_64 we need at least 8 samples for the antialiasing result to be similar
// to non MSAA.
static const unsigned s_defaultSampleCount = 8;
#else
// On embedded, we sacrifice a bit of antialiasing quality to save memory and improve
// performance.
static const unsigned s_defaultSampleCount = 4;
#endif
#else
// Disable MSAA by default.
static const unsigned s_defaultSampleCount = 0;
#endif

#if !(PLATFORM(PLAYSTATION) && USE(COORDINATED_GRAPHICS))
static sk_sp<const GrGLInterface> skiaGLInterface()
{
    static NeverDestroyed<sk_sp<const GrGLInterface>> grGLInterface {
#if USE(LIBEPOXY)
        GrGLInterfaces::MakeEpoxyEGL()
#else
        GrGLInterfaces::MakeEGL()
#endif
    };

    return grGLInterface.get();
}

static thread_local RefPtr<SkiaGLContext> s_skiaGLContext;

#if USE(LIBDRM)
static inline bool isNewIntelDevice()
{
    auto eglDisplay = eglGetCurrentDisplay();
    if (eglDisplay == EGL_NO_DISPLAY)
        return false;

    if (!GLContext::isExtensionSupported(eglQueryString(nullptr, EGL_EXTENSIONS), "EGL_EXT_device_query"))
        return false;

    EGLDeviceEXT eglDevice;
    if (!eglQueryDisplayAttribEXT(eglDisplay, EGL_DEVICE_EXT, reinterpret_cast<EGLAttrib*>(&eglDevice)))
        return false;

    if (!GLContext::isExtensionSupported(eglQueryDeviceStringEXT(eglDevice, EGL_EXTENSIONS), "EGL_EXT_device_drm"))
        return false;

    const char* device = eglQueryDeviceStringEXT(eglDevice, EGL_DRM_DEVICE_FILE_EXT);
    if (!device || !*device)
        return false;

    auto fd = UnixFileDescriptor { open(device, O_RDWR | O_CLOEXEC), UnixFileDescriptor::Adopt };
    if (!fd)
        return false;

    drmDevicePtr drmDevice;
    if (drmGetDevice2(fd.value(), 0, &drmDevice))
        return false;

    if (drmDevice->bustype != DRM_BUS_PCI) {
        drmFreeDevice(&drmDevice);
        return false;
    }

    auto vendorID = drmDevice->deviceinfo.pci->vendor_id;
    auto deviceID = drmDevice->deviceinfo.pci->device_id;
    drmFreeDevice(&drmDevice);

    static constexpr uint32_t intelVendorID = 0x8086;
    if (vendorID != intelVendorID)
        return false;

    // On pre-Ice Lake Intel GPUs MSAA performance is not acceptable.
    uint32_t maskedDeviceID = deviceID & 0xFF00;
    switch (maskedDeviceID) {
    case 0x2900: // Broadwater
    case 0x2A00: // Broadwater or Eaglelake
    case 0x2E00: // Eaglelake
    case 0x0000: // Ironlake
    case 0x0100: // Ivybridge, Baytrail or Sandybridge
    case 0x0F00: // Baytrail
    case 0x0A00: // Apollolake or Haswell
    case 0x0400: // Haswell
    case 0x0C00: // Haswell
    case 0x0D00: // Haswell
    case 0x2200: // Cherrytrail
    case 0x1600: // Broadwell
    case 0x5A00: // Apollolake or Cannonlake
    case 0x1900: // Skylake
    case 0x1A00: // Apollolake
    case 0x3100: // Geminilake
    case 0x5900: // Amberlake or Kabylake
    case 0x8700: // Kabylake or Coffeelake
    case 0x3E00: // Whiskeylake or Coffeelake
    case 0x9B00: // Cometlake
        return false;
    case 0x8A00: // Icelake
    case 0x4500: // Elkhartlake
    case 0x4E00: // Jasperlake
    case 0x9A00: // Tigerlake
    case 0x4c00: // Rocketlake
    case 0x4900: // DG1
    case 0x4600: // Alderlake
    case 0x4F00: // Alchemist
    case 0x5600: // Alchemist
    case 0xA700: // Raptorlake
    case 0x7D00: // Arrowlake or Meteorlake
    case 0xB600: // Arrowlake or Meteorlake
    case 0x6400: // Lunarlake
    case 0xE200: // Battlemage
    case 0xB000: // Pantherlake
        return true;
    default:
        break;
    }

    return false;
}
#endif

static bool shouldAllowMSAAOnNewIntel()
{
#if USE(LIBDRM)
    static std::once_flag onceFlag;
    static bool allowMSAAOnNewIntel;
    std::call_once(onceFlag, [] {
        allowMSAAOnNewIntel = isNewIntelDevice();
    });
    return allowMSAAOnNewIntel;
#else
    return false;
#endif
}

static unsigned initializeMSAASampleCount(GrDirectContext* grContext)
{
    static std::once_flag onceFlag;
    static int sampleCount = s_defaultSampleCount;

    std::call_once(onceFlag, [grContext] {
        // Let the user override the default sample count if they want to.
        String envString = String::fromLatin1(getenv("WEBKIT_SKIA_MSAA_SAMPLE_COUNT"));
        if (!envString.isEmpty())
            sampleCount = parseInteger<unsigned>(envString).value_or(0);

        if (sampleCount <= 1) {
            // Values of 0 or 1 mean disabling MSAA.
            sampleCount = 0;
            return;
        }

        // Skia checks internally whether MSAA is supported, but also disables it for several platforms where it
        // knows there are bugs. The only way to know whether our sample count will work is trying to create a
        // surface with that value and check whether it works.
        auto imageInfo = SkImageInfo::Make(512, 512, kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
        SkSurfaceProps properties = FontRenderOptions::singleton().createSurfaceProps();
        auto surface = SkSurfaces::RenderTarget(grContext, skgpu::Budgeted::kNo, imageInfo, sampleCount, kTopLeft_GrSurfaceOrigin, &properties);

        // If the creation of the surface failed, disable MSAA.
        if (!surface)
            sampleCount = 0;
    });

    return sampleCount;
}

static constexpr size_t s_lowEndDeviceMemoryThresholdBytes = 2 * GB;
static constexpr size_t s_highEndDeviceMemoryThresholdBytes = 4 * GB;

static constexpr size_t s_lowEndDeviceGlyphCacheTextureMaximumBytes = 2 * MB;
static constexpr size_t s_lowEndMaxResourceCacheBytes = 48 * MB;
static constexpr size_t s_maxResourceCacheBytes = 80 * MB;

static std::optional<size_t> glyphCacheTextureMaximumBytes()
{
    size_t memorySize = WTF::ramSize();
    if (memorySize < s_lowEndDeviceMemoryThresholdBytes)
        return s_lowEndDeviceGlyphCacheTextureMaximumBytes;
    return std::nullopt;
}

static size_t initializeMaxResourceCacheBytes(GrDirectContext* grContext)
{
    size_t memorySize = WTF::ramSize();
    if (memorySize < s_lowEndDeviceMemoryThresholdBytes)
        grContext->setResourceCacheLimit(s_lowEndMaxResourceCacheBytes);
    else if (memorySize < s_highEndDeviceMemoryThresholdBytes)
        grContext->setResourceCacheLimit(s_maxResourceCacheBytes);
    // Use GrDirectContext default (256MB) for high end devices.
    return grContext->getResourceCacheLimit();
}

class SkiaGLContext : public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<SkiaGLContext> {
public:
    static Ref<SkiaGLContext> create(PlatformDisplay& display)
    {
        return adoptRef(*new SkiaGLContext(display));
    }

    static Ref<SkiaGLContext> create(std::unique_ptr<GLContext>&& glContext)
    {
        return adoptRef(*new SkiaGLContext(WTF::move(glContext)));
    }

    ~SkiaGLContext()
    {
        auto glContext = WTF::move(m_skiaGLContext);
        auto grContext = WTF::move(m_skiaGrContext);
        if (!glContext)
            return;

        // The context of a thread is destroyed when the thread exits, which can happen while the main thread
        // terminates the display. Once it is terminated, drop the Skia resources without calling into GL.
        RefPtr display = glContext->display();
        bool didRelease = display && display->runIfNotTerminated([&](EGLDisplay) {
            glContext->makeContextCurrent();
            grContext->releaseResourcesAndAbandonContext();
            grContext = nullptr;
        });
        if (!didRelease)
            grContext->abandonContext();
    }

    GLContext* skiaGLContext() const
    {
        Locker locker { m_lock };
        return m_skiaGLContext.get();
    }

    GrDirectContext* skiaGrContext() const
    {
        Locker locker { m_lock };
        return m_skiaGrContext.get();
    }

    unsigned sampleCount() const
    {
        return m_sampleCount;
    }

    size_t maxResourceCacheBytes() const
    {
        return m_maxResourceCacheBytes;
    }

    void releaseUnusedResources(WTF::Critical critical)
    {
        Locker locker { m_lock };
        if (!m_skiaGLContext)
            return;

        GLContext::ScopedGLContextCurrent scopedCurrent(*m_skiaGLContext);
        switch (critical) {
        case Critical::No:
            m_skiaGrContext->purgeUnlockedResources(GrPurgeResourceOptions::kScratchResourcesOnly);
            break;
        case Critical::Yes:
            m_skiaGrContext->freeGpuResources();
            break;
        }
    }

private:
    explicit SkiaGLContext(PlatformDisplay& display)
        : SkiaGLContext(GLContext::createOffscreen(display))
    {
    }

    explicit SkiaGLContext(std::unique_ptr<GLContext>&& glContext)
    {
        if (!glContext || !glContext->makeContextCurrent())
            return;

        GrContextOptions options;
#if OS(ANDROID)
        if (PersistentProgramCache::mode() != PersistentProgramCache::Mode::Off)
            options.fPersistentCache = &PersistentProgramCache::singleton();
        options.fReducedShaderVariations = reducedShaderVariationsRequested();
        options.fDisableDistanceFieldPaths = !distanceFieldPathsRequested();
#endif
        options.fAllowMSAAOnNewIntel = shouldAllowMSAAOnNewIntel();
        // Skia defaults this to true, which alters the texture2D() calls in all generated shaders,
        // applying a so-called negative LOD bias, forcing texture2D() sampling into a LOD-computing
        // lookup which is a slow-path in certain tiled GPUs (such as Vivante/etnaviv). Since we
        // don't use any mipmapping, it's safe to disable that, and avoid performance hits.
        options.fSharpenMipmappedTextures = false;
        thread_local std::unique_ptr<SkExecutor> s_executor = SkExecutor::MakeFIFOThreadPool(2);
        options.fExecutor = s_executor.get();
        if (auto bytes = glyphCacheTextureMaximumBytes())
            options.fGlyphCacheTextureMaximumBytes = *bytes;

        if (auto grContext = GrDirectContexts::MakeGL(skiaGLInterface(), options)) {
            m_skiaGLContext = WTF::move(glContext);
            m_skiaGrContext = WTF::move(grContext);
            m_sampleCount = initializeMSAASampleCount(m_skiaGrContext.get());
            m_maxResourceCacheBytes = initializeMaxResourceCacheBytes(m_skiaGrContext.get());
        }
    }

    std::unique_ptr<GLContext> m_skiaGLContext WTF_GUARDED_BY_LOCK(m_lock);
    sk_sp<GrDirectContext> m_skiaGrContext WTF_GUARDED_BY_LOCK(m_lock);
    mutable Lock m_lock;
    unsigned m_sampleCount { 0 };
    size_t m_maxResourceCacheBytes { 0 };
};
#endif

GLContext* PlatformDisplay::skiaGLContext()
{
#if PLATFORM(GTK) || PLATFORM(WPE) || (PLATFORM(PLAYSTATION) && !USE(COORDINATED_GRAPHICS))
    if (!s_skiaGLContext) {
        s_skiaGLContext = SkiaGLContext::create(*this);
        m_skiaGLContexts.add(*s_skiaGLContext);
    }
    return s_skiaGLContext->skiaGLContext();
#else
    // The PlayStation OpenGL implementation does not dispatch to the context bound to
    // the current thread so Skia cannot use OpenGL with coordinated graphics.
    return nullptr;
#endif
}

void PlatformDisplay::setSkiaGLContextForCurrentThread(std::unique_ptr<GLContext>&& glContext)
{
    if (!glContext) {
        clearSkiaGLContext();
        return;
    }
    ASSERT(!s_skiaGLContext);
    s_skiaGLContext = SkiaGLContext::create(WTF::move(glContext));
    m_skiaGLContexts.add(*s_skiaGLContext);
}

GrDirectContext* PlatformDisplay::skiaGrContext() const
{
    RELEASE_ASSERT(s_skiaGLContext);
    return s_skiaGLContext->skiaGrContext();
}

unsigned PlatformDisplay::msaaSampleCount() const
{
    return s_skiaGLContext ? s_skiaGLContext->sampleCount() : 0;
}

size_t PlatformDisplay::maxSkiaResourceCacheBytes() const
{
    return s_skiaGLContext ? s_skiaGLContext->maxResourceCacheBytes() : 0;
}

void PlatformDisplay::skiaReleaseUnusedResources(WTF::Critical critical)
{
    if (s_skiaGLContext)
        s_skiaGLContext->releaseUnusedResources(critical);
}

void PlatformDisplay::clearSkiaGLContext()
{
    s_skiaGLContext = nullptr;
}

} // namespace WebCore

#endif // USE(SKIA)
