#pragma once

#include <WebCore/ExceptionOr.h>
#include <WebCore/HTMLElement.h>
#include <WebCore/HTMLNames.h>
#include <WebCore/ShadowRoot.h>
#include <WebCore/ShadowRootInit.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Vector.h>

namespace TestWebKitAPI {

class SnapRemovalAggregateElement final : public WebCore::HTMLElement {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(SnapRemovalAggregateElement);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(SnapRemovalAggregateElement);

public:
    static WTF::Ref<SnapRemovalAggregateElement> create(
        WebCore::Document& document,
        const WebCore::QualifiedName& tagName,
        unsigned& removalStepsCount,
        unsigned& destructionCount)
    {
        return WTF::adoptRef(
            *new SnapRemovalAggregateElement(document, tagName, removalStepsCount, destructionCount));
    }

    ~SnapRemovalAggregateElement() final
    {
        ++m_destructionCount;
    }

protected:
    void
    removingSteps(WebCore::Node::RemovalType removalType, WebCore::ContainerNode& oldParentOfRemovedTree) final
    {
        WebCore::HTMLElement::removingSteps(removalType, oldParentOfRemovedTree);
        ++m_removalStepsCount;
    }

private:
    SnapRemovalAggregateElement(
        WebCore::Document& document,
        const WebCore::QualifiedName& tagName,
        unsigned& removalStepsCount,
        unsigned& destructionCount)
        : WebCore::HTMLElement(tagName, document)
        , m_removalStepsCount(removalStepsCount)
        , m_destructionCount(destructionCount)
    {
    }

    unsigned& m_removalStepsCount;
    unsigned& m_destructionCount;
};

struct SnapRemovalAggregateTree {
    WTF::RefPtr<SnapRemovalAggregateElement> root;
    WTF::Vector<SnapRemovalAggregateElement*> elements;
    WebCore::ShadowRoot* shadowRoot { nullptr };
};

inline SnapRemovalAggregateTree snapMakeRemovalAggregateTree(
    WebCore::Document& document,
    unsigned& removalStepsCount,
    unsigned& destructionCount,
    bool largeShadowChildFirst,
    bool includeLargeShadowChild = true)
{
    auto root = SnapRemovalAggregateElement::create(
        document,
        WebCore::HTMLNames::divTag,
        removalStepsCount,
        destructionCount);
    WTF::Vector<SnapRemovalAggregateElement*> elements;
    elements.append(root.ptr());

    WebCore::ShadowRootInit shadowRootInit;
    shadowRootInit.mode = WebCore::ShadowRootMode::Open;
    auto shadowRootResult = root->attachShadow(shadowRootInit);
    RELEASE_ASSERT(!shadowRootResult.hasException());
    auto* shadowRoot = &shadowRootResult.releaseReturnValue();

    auto lightFirst = SnapRemovalAggregateElement::create(
        document,
        WebCore::HTMLNames::spanTag,
        removalStepsCount,
        destructionCount);
    elements.append(lightFirst.ptr());
    auto firstAppend = root->appendChild(lightFirst.get());
    RELEASE_ASSERT(!firstAppend.hasException());

    auto lightMiddle = SnapRemovalAggregateElement::create(
        document,
        WebCore::HTMLNames::spanTag,
        removalStepsCount,
        destructionCount);
    elements.append(lightMiddle.ptr());
    auto middleAppend = root->appendChild(lightMiddle.get());
    RELEASE_ASSERT(!middleAppend.hasException());

    auto lightNested = SnapRemovalAggregateElement::create(
        document,
        WebCore::HTMLNames::spanTag,
        removalStepsCount,
        destructionCount);
    elements.append(lightNested.ptr());
    auto nestedAppend = lightMiddle->appendChild(lightNested.get());
    RELEASE_ASSERT(!nestedAppend.hasException());

    auto lightLast = SnapRemovalAggregateElement::create(
        document,
        WebCore::HTMLNames::spanTag,
        removalStepsCount,
        destructionCount);
    elements.append(lightLast.ptr());
    auto lastAppend = root->appendChild(lightLast.get());
    RELEASE_ASSERT(!lastAppend.hasException());

    auto shadowFirst = SnapRemovalAggregateElement::create(
        document,
        includeLargeShadowChild && largeShadowChildFirst ? WebCore::HTMLNames::imgTag : WebCore::HTMLNames::spanTag,
        removalStepsCount,
        destructionCount);
    elements.append(shadowFirst.ptr());
    auto shadowFirstAppend = shadowRoot->appendChild(shadowFirst.get());
    RELEASE_ASSERT(!shadowFirstAppend.hasException());

    auto shadowLast = SnapRemovalAggregateElement::create(
        document,
        includeLargeShadowChild && !largeShadowChildFirst ? WebCore::HTMLNames::imgTag
                                                          : WebCore::HTMLNames::spanTag,
        removalStepsCount,
        destructionCount);
    elements.append(shadowLast.ptr());
    auto shadowLastAppend = shadowRoot->appendChild(shadowLast.get());
    RELEASE_ASSERT(!shadowLastAppend.hasException());

    return { WTF::move(root), WTF::move(elements), shadowRoot };
}

}
