/*
 * Copyright (C) 2026 Igalia S.L.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#pragma once

#include <wtf/Platform.h>

#if OS(ANDROID)

#include <cstdint>
#include <type_traits>
#include <wtf/ExportMacros.h>
#include <wtf/Noncopyable.h>

namespace WTF {

// Bridges WebKit tracepoints, plus the scrolling and touch annotations added on top of
// them, onto Android's ATrace. Everything lands in the same systrace/Perfetto timeline as
// SurfaceFlinger, input dispatch and the scheduler, which is what makes it possible to
// line a scroll stall up with whatever the web process main thread was doing at the time.
//
// Tracing is off until a capture session enables the app tag for the process (`atrace -a`
// or Perfetto's atrace_apps), so the cost in a normal run is one relaxed atomic load per
// call site.
//
// The `debug.wpe.atrace` system property selects how much is emitted:
//   unset / "scroll"  only the scrolling, touch, rendering-update and compositing codes
//   "layers"          the above plus a slice per layer painted, which is voluminous but is
//                     the only way to see which layer makes a composite overrun its frame
//   "shaders"         the scroll set plus a marker every time Skia compiles a GPU program;
//                     this one perturbs what it measures, see shaderTracingRequested()
//   "all"             every tracepoint WebKit emits, plus the per-layer slices
//   "off"             nothing
class ATraceAnnotator {
public:
    WTF_EXPORT_PRIVATE static bool isEnabled();

    // Per-layer paint slices are opt-in: there are hundreds per frame, enough to fill a
    // trace buffer during a single gesture.
    WTF_EXPORT_PRIVATE static bool layersEnabled();

    // Reporting GPU program compilation costs something to observe - Skia has to fetch the
    // compiled binary to hand it over - so it is opt-in and read once, before any tracing
    // session can have started.
    WTF_EXPORT_PRIVATE static bool shaderTracingRequested();

    // Generic tracepoint bridge, called from WTF::tracePoint(). The code is a
    // TracePointCode, passed as unsigned because that enum cannot be forward declared.
    WTF_EXPORT_PRIVATE static void tracePoint(unsigned code, uint64_t data1, uint64_t data2, uint64_t data3, uint64_t data4);

    // Thread-scoped slices. These nest, so every begin needs its end on the same thread.
    WTF_EXPORT_PRIVATE static void begin(const char* name);
    WTF_EXPORT_PRIVATE static void beginFormatted(const char* format, ...) WTF_ATTRIBUTE_PRINTF(1, 2);
    WTF_EXPORT_PRIVATE static void end();

    // Process-scoped slices, for spans that begin and end on different threads.
    WTF_EXPORT_PRIVATE static void beginAsync(const char* name, int32_t cookie);
    WTF_EXPORT_PRIVATE static void endAsync(const char* name, int32_t cookie);
    WTF_EXPORT_PRIVATE static int32_t nextCookie();

    // Zero-length marker whose name carries the reason.
    WTF_EXPORT_PRIVATE static void instant(const char* format, ...) WTF_ATTRIBUTE_PRINTF(1, 2);
    // Signposts are keyed by a pointer and may begin and end on different threads, so they
    // map to process-scoped sections with a cookie derived from that pointer. The section
    // name has to match between begin and end for them to pair, so any description the
    // signpost carries is emitted separately as a marker.
    // Call sites key a signpost on either an address or an opaque integer identifier, so
    // both have to fold down to a cookie. The XOR keeps small identifiers distinct, which a
    // plain shift would collapse onto zero.
    static int32_t cookieForValue(uintptr_t value)
    {
        return static_cast<int32_t>(((value >> 4) ^ value) & 0x7fffffff);
    }

    static int32_t cookieForPointer(const void* pointer)
    {
        return cookieForValue(reinterpret_cast<uintptr_t>(pointer));
    }

    template<typename T> requires std::is_integral_v<T>
    static int32_t cookieForPointer(T value)
    {
        return cookieForValue(static_cast<uintptr_t>(value));
    }

    WTF_EXPORT_PRIVATE static void counter(const char* name, int64_t);
};

// Lets a macro reference its arguments without evaluating them, so a disabled signpost
// costs nothing and still does not provoke unused-variable warnings.
template<typename... Args> int aTraceIgnore(Args&&...);

class ATraceScope {
    WTF_MAKE_NONCOPYABLE(ATraceScope);
public:
    explicit ATraceScope(const char* name) { ATraceAnnotator::begin(name); }
    ~ATraceScope() { ATraceAnnotator::end(); }
};

// A scope whose name is built from the values it describes, and which can be switched off
// wholesale so the arguments are not even formatted.
class ATraceFormattedScope {
    WTF_MAKE_NONCOPYABLE(ATraceFormattedScope);
public:
    WTF_EXPORT_PRIVATE ATraceFormattedScope(bool enabled, const char* format, ...) WTF_ATTRIBUTE_PRINTF(3, 4);
    WTF_EXPORT_PRIVATE ~ATraceFormattedScope();

private:
    bool m_active { false };
};

} // namespace WTF

#endif // OS(ANDROID)
