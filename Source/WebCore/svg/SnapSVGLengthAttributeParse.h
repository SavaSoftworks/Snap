#pragma once

#include "SVGProperty.h"
#include <cstdlib>
#include <wtf/text/AtomString.h>
#include <wtf/text/CStringView.h>

namespace WebCore {

inline bool snapSVGLengthAttributeParseSkipEnabled()
{
    static const bool enabled = CStringView::unsafeFromUTF8(std::getenv("WEBKIT_SNAP_DISABLE_SVG_LENGTH_PARSE_SKIP")) != "1"_s;
    return enabled;
}

template<typename AnimatedLength>
inline bool snapShouldSkipSVGLengthAttributeParse(const AtomString& oldValue, const AtomString& newValue, const AnimatedLength& length)
{
    if (!snapSVGLengthAttributeParseSkipEnabled() || oldValue.isNull() || oldValue != newValue || length.isDirty() || length.isAnimating())
        return false;
    const auto& baseValue = length.baseVal();
    return baseValue->hasSuccessfulLengthAttributeParse() && baseValue->contextElement();
}

}
