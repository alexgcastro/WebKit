/*
 * Copyright (C) 2013 Apple Inc. All rights reserved.
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

#pragma once

#include "PlatformCALayerRemote.h"

namespace WebCore {
class PlatformCALayerClient;

#if ENABLE(MODEL_PROCESS)
class ModelContext;
#endif
}

namespace WebKit {

class LayerHostingContext;

// PlatformCALayerRemoteCustom is used for CALayers that live in the web process and are hosted into the UI process via remote context.
class PlatformCALayerRemoteCustom final : public PlatformCALayerRemote {
    friend class PlatformCALayerRemote;
public:
    static Ref<PlatformCALayerRemote> create(PlatformLayer *, WebCore::PlatformCALayerClient*, RemoteLayerTreeContext&);
#if ENABLE(MODEL_PROCESS)
    static Ref<PlatformCALayerRemote> create(Ref<WebCore::ModelContext>, WebCore::PlatformCALayerClient*, RemoteLayerTreeContext&);
#endif
#if HAVE(AVKIT)
    static Ref<PlatformCALayerRemote> create(WebCore::HTMLVideoElement&, WebCore::PlatformCALayerClient* owner, RemoteLayerTreeContext&);
#endif

    virtual ~PlatformCALayerRemoteCustom();

    PlatformLayer* platformLayer() const override { return m_platformLayer.get(); }

    uint32_t hostingContextID() override;

    void setNeedsDisplayInRect(const WebCore::FloatRect& dirtyRect) override;
    void setNeedsDisplay() override;

    bool hasVideo() const { return m_hasVideo; }

private:
    PlatformCALayerRemoteCustom(WebCore::PlatformCALayer::LayerType, PlatformLayer *, WebCore::PlatformCALayerClient* owner, RemoteLayerTreeContext&);
    PlatformCALayerRemoteCustom(WebCore::PlatformCALayer::LayerType, LayerHostingContextID, WebCore::PlatformCALayerClient* owner, RemoteLayerTreeContext&);
    PlatformCALayerRemoteCustom(WebCore::HTMLVideoElement&, WebCore::PlatformCALayerClient* owner, RemoteLayerTreeContext&);
#if ENABLE(MODEL_PROCESS)
    PlatformCALayerRemoteCustom(WebCore::PlatformCALayer::LayerType, Ref<WebCore::ModelContext>, WebCore::PlatformCALayerClient* owner, RemoteLayerTreeContext&);
#endif

    Ref<WebCore::PlatformCALayer> clone(WebCore::PlatformCALayerClient* owner) const override;
    
    void populateCreationProperties(RemoteLayerTreeTransaction::LayerCreationProperties&, const RemoteLayerTreeContext&, WebCore::PlatformCALayer::LayerType) override;

    Type type() const final { return Type::RemoteCustom; }

    CFTypeRef contents() const override;
    void setContents(CFTypeRef) override;

    bool m_hasVideo { false };
    std::unique_ptr<LayerHostingContext> m_layerHostingContext;
    RetainPtr<PlatformLayer> m_platformLayer;
#if ENABLE(MODEL_PROCESS)
    RefPtr<WebCore::ModelContext> m_modelContext;
#endif
};

} // namespace WebKit

SPECIALIZE_TYPE_TRAITS_PLATFORM_CALAYER(WebKit::PlatformCALayerRemoteCustom, type() == WebCore::PlatformCALayer::Type::RemoteCustom)
