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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "WPEToplevelWayland.h"
#include "linux-explicit-synchronization-unstable-v1-client-protocol.h"

struct DMABufBuffer {
    WTF_MAKE_STRUCT_FAST_ALLOCATED;

    explicit DMABufBuffer(wl_buffer* buffer)
        : wlBuffer(buffer)
    {
    }

    ~DMABufBuffer() {
        if (wlBuffer)
            wl_buffer_destroy(wlBuffer);
        if (release)
            zwp_linux_buffer_release_v1_destroy(release);
    }

    struct wl_buffer* wlBuffer { nullptr };
    struct zwp_linux_buffer_release_v1* release { nullptr };
};

void wpeToplevelWaylandSetOpaqueRectangles(WPEToplevelWayland*, WPERectangle*, unsigned);
void wpeToplevelWaylandUpdateOpaqueRegion(WPEToplevelWayland*);
void wpeToplevelWaylandSetHasPointer(WPEToplevelWayland*, bool);
WPEView* wpeToplevelWaylandGetVisibleViewUnderPointer(WPEToplevelWayland*);
void wpeToplevelWaylandSetIsFocused(WPEToplevelWayland*, bool);
WPEView* wpeToplevelWaylandGetVisibleFocusedView(WPEToplevelWayland*);
void wpeToplevelWaylandViewVisibilityChanged(WPEToplevelWayland*, WPEView*);
gboolean wpeToplevelWaylandHasSyncObject(WPEToplevelWayland*);
void wpeToplevelWaylandAddSyncListener(WPEToplevelWayland*, WPEBuffer*);
