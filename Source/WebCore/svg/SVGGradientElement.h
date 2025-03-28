/*
 * Copyright (C) 2004, 2005, 2006, 2008 Nikolas Zimmermann <zimmermann@kde.org>
 * Copyright (C) 2004, 2005, 2006 Rob Buis <buis@kde.org>
 * Copyright (C) 2018-2019 Apple Inc. All rights reserved.
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

#include "Gradient.h"
#include "SVGElement.h"
#include "SVGNames.h"
#include "SVGURIReference.h"
#include "SVGUnitTypes.h"
#include <wtf/TZoneMalloc.h>

namespace WebCore {

enum SVGSpreadMethodType {
    SVGSpreadMethodUnknown = 0,
    SVGSpreadMethodPad,
    SVGSpreadMethodReflect,
    SVGSpreadMethodRepeat
};

template<>
struct SVGPropertyTraits<SVGSpreadMethodType> {
    static unsigned highestEnumValue() { return SVGSpreadMethodRepeat; }

    static String toString(SVGSpreadMethodType type)
    {
        switch (type) {
        case SVGSpreadMethodUnknown:
            return emptyString();
        case SVGSpreadMethodPad:
            return "pad"_s;
        case SVGSpreadMethodReflect:
            return "reflect"_s;
        case SVGSpreadMethodRepeat:
            return "repeat"_s;
        }

        ASSERT_NOT_REACHED();
        return emptyString();
    }

    static SVGSpreadMethodType fromString(const String& value)
    {
        if (value == "pad"_s)
            return SVGSpreadMethodPad;
        if (value == "reflect"_s)
            return SVGSpreadMethodReflect;
        if (value == "repeat"_s)
            return SVGSpreadMethodRepeat;
        return SVGSpreadMethodUnknown;
    }
};

class SVGGradientElement : public SVGElement, public SVGURIReference {
    WTF_MAKE_TZONE_OR_ISO_ALLOCATED(SVGGradientElement);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(SVGGradientElement);
public:
    enum {
        SVG_SPREADMETHOD_UNKNOWN = SVGSpreadMethodUnknown,
        SVG_SPREADMETHOD_PAD = SVGSpreadMethodPad,
        SVG_SPREADMETHOD_REFLECT = SVGSpreadMethodReflect,
        SVG_SPREADMETHOD_REPEAT = SVGSpreadMethodRepeat
    };

    GradientColorStops buildStops();

    using PropertyRegistry = SVGPropertyOwnerRegistry<SVGGradientElement, SVGElement, SVGURIReference>;

    SVGSpreadMethodType spreadMethod() const { return m_spreadMethod->currentValue<SVGSpreadMethodType>(); }
    SVGUnitTypes::SVGUnitType gradientUnits() const { return m_gradientUnits->currentValue<SVGUnitTypes::SVGUnitType>(); }
    const SVGTransformList& gradientTransform() const { return m_gradientTransform->currentValue(); }

    SVGAnimatedEnumeration& spreadMethodAnimated() { return m_spreadMethod; }
    SVGAnimatedEnumeration& gradientUnitsAnimated() { return m_gradientUnits; }
    SVGAnimatedTransformList& gradientTransformAnimated() { return m_gradientTransform; }

protected:
    SVGGradientElement(const QualifiedName&, Document&, UniqueRef<SVGPropertyRegistry>&&);

    void attributeChanged(const QualifiedName&, const AtomString& oldValue, const AtomString& newValue, AttributeModificationReason) override;
    void svgAttributeChanged(const QualifiedName&) override;

    void invalidateGradientResource();

private:
    bool needsPendingResourceHandling() const override { return false; }
    void childrenChanged(const ChildChange&) override;

    Ref<SVGAnimatedEnumeration> m_spreadMethod { SVGAnimatedEnumeration::create(this, SVGSpreadMethodPad) };
    Ref<SVGAnimatedEnumeration> m_gradientUnits { SVGAnimatedEnumeration::create(this, SVGUnitTypes::SVG_UNIT_TYPE_OBJECTBOUNDINGBOX) };
    Ref<SVGAnimatedTransformList> m_gradientTransform { SVGAnimatedTransformList::create(this) };
};

} // namespace WebCore

SPECIALIZE_TYPE_TRAITS_BEGIN(WebCore::SVGGradientElement)
static bool isType(const WebCore::SVGElement& element)
{
    return element.hasTagName(WebCore::SVGNames::radialGradientTag) || element.hasTagName(WebCore::SVGNames::linearGradientTag);
}
static bool isType(const WebCore::Node& node)
{
    auto* svgElement = dynamicDowncast<WebCore::SVGElement>(node);
    return svgElement && isType(*svgElement);
}
SPECIALIZE_TYPE_TRAITS_END()
