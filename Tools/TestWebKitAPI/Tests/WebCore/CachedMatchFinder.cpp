/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "SnapListRenumberingFixture.h"
#include "SnapRemovalAggregateFixture.h"
#include "TestPageHarness.h"
#include <WebCore/AsyncNodeDeletionQueueInlines.h>
#include <WebCore/CSSParserContext.h>
#include <WebCore/CSSSelectorList.h>
#include <WebCore/ContainerNodeInlines.h>
#include <WebCore/GarbageCollectionController.h>
#include <WebCore/HTMLBodyElement.h>
#include <WebCore/HTMLNames.h>
#include <WebCore/HTMLOListElement.h>
#include <WebCore/RenderListItem.h>
#include <WebCore/SelectorFilter.h>
#include <WebCore/ShadowRootInit.h>
#include <WebCore/StyleRule.h>
#include <WebCore/StyleSheetContents.h>
#include <wtf/text/StringBuilder.h>

#include <WebCore/CSSPropertyNames.h>
#include <WebCore/CSSValueKeywords.h>
#include <WebCore/CachedMatchFinder.h>
#include <WebCore/Element.h>
#include <WebCore/FindOptions.h>
#include <WebCore/HTMLCollection.h>
#include <WebCore/ShadowRoot.h>
#include <WebCore/SimpleRange.h>
#include <WebCore/StyledElement.h>
#include <WebCore/TextIterator.h>

namespace TestWebKitAPI {

using namespace WebCore;

static String snapListMarkerText(TestPageHarness& testPage, const String& identifier)
{
    auto element = testPage.getElementById(identifier);
    if (!element) {
        return {};
    }
    auto* listItem = dynamicDowncast<RenderListItem>(element->renderer());
    return listItem ? listItem->markerTextWithSuffix() : String {};
}

TEST(ListRenumbering, PreservesMarkersAcrossMutation)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML(snapListRenumberingFixture());
    testPage.document().updateLayoutIgnorePendingStylesheets();

    EXPECT_EQ(snapListMarkerText(testPage, "reverse-one"_s), "3. "_s);
    EXPECT_EQ(snapListMarkerText(testPage, "reverse-two"_s), "2. "_s);
    EXPECT_EQ(snapListMarkerText(testPage, "reverse-three"_s), "1. "_s);
    EXPECT_EQ(snapListMarkerText(testPage, "explicit-one"_s), "7. "_s);
    EXPECT_EQ(snapListMarkerText(testPage, "explicit-value"_s), "12. "_s);
    EXPECT_EQ(snapListMarkerText(testPage, "explicit-three"_s), "13. "_s);
    EXPECT_EQ(snapListMarkerText(testPage, "outer-one"_s), "4. "_s);
    EXPECT_EQ(snapListMarkerText(testPage, "outer-two"_s), "5. "_s);
    EXPECT_EQ(snapListMarkerText(testPage, "inner-one"_s), "1. "_s);
    EXPECT_EQ(snapListMarkerText(testPage, "inner-two"_s), "2. "_s);
    EXPECT_EQ(snapListMarkerText(testPage, "css-one"_s), "21. "_s);
    EXPECT_EQ(snapListMarkerText(testPage, "css-set"_s), "7. "_s);
    EXPECT_EQ(snapListMarkerText(testPage, "css-three"_s), "8. "_s);

    auto descending = dynamicDowncast<HTMLOListElement>(testPage.getElementById("descending"_s));
    auto reverseThree = testPage.getElementById("reverse-three"_s);
    ASSERT_TRUE(descending);
    ASSERT_TRUE(reverseThree);
    auto removed = descending->removeChild(*reverseThree);
    ASSERT_FALSE(removed.hasException());
    testPage.document().updateLayoutIgnorePendingStylesheets();

    EXPECT_EQ(snapListMarkerText(testPage, "reverse-one"_s), "2. "_s);
    EXPECT_EQ(snapListMarkerText(testPage, "reverse-two"_s), "1. "_s);

    auto inserted = descending->appendChild(*reverseThree);
    ASSERT_FALSE(inserted.hasException());
    testPage.document().updateLayoutIgnorePendingStylesheets();

    EXPECT_EQ(snapListMarkerText(testPage, "reverse-one"_s), "3. "_s);
    EXPECT_EQ(snapListMarkerText(testPage, "reverse-two"_s), "2. "_s);
    EXPECT_EQ(snapListMarkerText(testPage, "reverse-three"_s), "1. "_s);

    auto explicitList = testPage.getElementById("explicit"_s);
    ASSERT_TRUE(explicitList);
    explicitList->setAttribute(HTMLNames::startAttr, "3"_s);
    testPage.document().updateLayoutIgnorePendingStylesheets();

    EXPECT_EQ(snapListMarkerText(testPage, "explicit-one"_s), "3. "_s);
    EXPECT_EQ(snapListMarkerText(testPage, "explicit-value"_s), "12. "_s);
    EXPECT_EQ(snapListMarkerText(testPage, "explicit-three"_s), "13. "_s);
}

TEST(ListRenumbering, CacheCountTracksRendererTeardownAndAdoption)
{
    auto sourcePage = TestPageHarness::create();
    sourcePage.loadHTML("<ol><li id='moving-item'>item</li></ol>"_s);
    sourcePage.document().updateLayoutIgnorePendingStylesheets();
    EXPECT_EQ(snapListMarkerText(sourcePage, "moving-item"_s), "1. "_s);
    EXPECT_GT(sourcePage.document().listItemsWithCachedValues(), 0u);

    auto movingItem = sourcePage.getElementById("moving-item"_s);
    ASSERT_TRUE(movingItem);
    auto destinationPage = TestPageHarness::create();
    destinationPage.loadHTML("<ol id='host'></ol>"_s);
    auto host = destinationPage.getElementById("host"_s);
    ASSERT_TRUE(host);
    auto adopted = destinationPage.document().adoptNode(*movingItem);
    ASSERT_FALSE(adopted.hasException());
    EXPECT_EQ(sourcePage.document().listItemsWithCachedValues(), 0u);
    EXPECT_EQ(destinationPage.document().listItemsWithCachedValues(), 0u);

    auto adoptedItem = adopted.releaseReturnValue();
    auto inserted = host->appendChild(adoptedItem.get());
    ASSERT_FALSE(inserted.hasException());
    destinationPage.document().updateLayoutIgnorePendingStylesheets();
    EXPECT_EQ(snapListMarkerText(destinationPage, "moving-item"_s), "1. "_s);
    EXPECT_GT(destinationPage.document().listItemsWithCachedValues(), 0u);

    auto removed = host->removeChild(adoptedItem.get());
    ASSERT_FALSE(removed.hasException());
    EXPECT_EQ(destinationPage.document().listItemsWithCachedValues(), 0u);
}

static void snapExpectConnectedRemovalAggregate(bool largeShadowChildFirst)
{
    unsigned removalStepsCount = 0;
    unsigned destructionCount = 0;
    auto testPage = TestPageHarness::create();
    auto tree = snapMakeRemovalAggregateTree(testPage.document(), removalStepsCount, destructionCount, largeShadowChildFirst);
    auto* body = testPage.document().body();
    ASSERT_TRUE(body);

    auto appendResult = body->appendChild(*tree.root);
    ASSERT_FALSE(appendResult.hasException());
    for (auto* element : tree.elements) {
        EXPECT_TRUE(element->isConnected());
    }
    EXPECT_TRUE(tree.shadowRoot->isConnected());

    auto removalResult = body->removeChild(*tree.root);
    ASSERT_FALSE(removalResult.hasException());
    EXPECT_EQ(tree.elements.size(), removalStepsCount);
    for (auto* element : tree.elements) {
        EXPECT_FALSE(element->isConnected());
    }
    EXPECT_FALSE(tree.shadowRoot->isConnected());

    tree.root = nullptr;
    GarbageCollectionController::singleton().garbageCollectNow();
    EXPECT_EQ(tree.elements.size(), destructionCount);
}

TEST(RemovalAggregate, ConnectedEarlyTerminalStillRunsEveryRemovalStep)
{
    snapExpectConnectedRemovalAggregate(true);
}

TEST(RemovalAggregate, ConnectedLateTerminalStillRunsEveryRemovalStep)
{
    snapExpectConnectedRemovalAggregate(false);
}

TEST(RemovalAggregate, DisconnectedShadowTreeRunsEveryRemovalStep)
{
    unsigned removalStepsCount = 0;
    unsigned destructionCount = 0;
    auto testPage = TestPageHarness::create();
    auto container = HTMLElement::create(HTMLNames::divTag, testPage.document());
    auto tree = snapMakeRemovalAggregateTree(testPage.document(), removalStepsCount, destructionCount, true);
    auto appendResult = container->appendChild(*tree.root);
    ASSERT_FALSE(appendResult.hasException());
    for (auto* element : tree.elements) {
        EXPECT_FALSE(element->isConnected());
    }

    auto removalResult = container->removeChild(*tree.root);
    ASSERT_FALSE(removalResult.hasException());
    EXPECT_EQ(tree.elements.size(), removalStepsCount);
    EXPECT_FALSE(tree.shadowRoot->isConnected());

    tree.root = nullptr;
    GarbageCollectionController::singleton().garbageCollectNow();
    EXPECT_EQ(tree.elements.size(), destructionCount);
}

TEST(RemovalAggregate, ObservableShadowNodeDoesNotSkipLaterCallbacks)
{
    unsigned removalStepsCount = 0;
    unsigned destructionCount = 0;
    auto testPage = TestPageHarness::create();
    auto tree = snapMakeRemovalAggregateTree(testPage.document(), removalStepsCount, destructionCount, true);
    RefPtr heldShadowChild = tree.elements[5];
    auto* body = testPage.document().body();
    ASSERT_TRUE(body);
    auto appendResult = body->appendChild(*tree.root);
    ASSERT_FALSE(appendResult.hasException());

    auto removalResult = body->removeChild(*tree.root);
    ASSERT_FALSE(removalResult.hasException());
    EXPECT_EQ(tree.elements.size(), removalStepsCount);

    tree.root = nullptr;
    ASSERT_TRUE(heldShadowChild->parentNode());
    EXPECT_TRUE(heldShadowChild->parentNode()->isShadowRoot());
    heldShadowChild = nullptr;
    GarbageCollectionController::singleton().garbageCollectNow();
    EXPECT_EQ(tree.elements.size(), destructionCount);
}

#if HAVE(OPPORTUNISTIC_TASK_SCHEDULER)
TEST(RemovalAggregate, LargeShadowNodeKeepsSynchronousDeletionChoice)
{
    unsigned removalStepsCount = 0;
    unsigned destructionCount = 0;
    auto testPage = TestPageHarness::create();
    auto container = HTMLElement::create(HTMLNames::divTag, testPage.document());
    auto tree = snapMakeRemovalAggregateTree(testPage.document(), removalStepsCount, destructionCount, true);
    auto appendResult = container->appendChild(*tree.root);
    ASSERT_FALSE(appendResult.hasException());

    container->removeChildren();
    EXPECT_EQ(tree.elements.size(), removalStepsCount);
    tree.root = nullptr;
    GarbageCollectionController::singleton().garbageCollectNow();
    EXPECT_EQ(tree.elements.size(), destructionCount);
    testPage.document().asyncNodeDeletionQueue().deleteNodesNow();
    GarbageCollectionController::singleton().garbageCollectNow();
    EXPECT_EQ(tree.elements.size(), destructionCount);
}

TEST(RemovalAggregate, SmallShadowTreeKeepsDeferredDeletionChoice)
{
    unsigned removalStepsCount = 0;
    unsigned destructionCount = 0;
    auto testPage = TestPageHarness::create();
    auto container = HTMLElement::create(HTMLNames::divTag, testPage.document());
    auto tree = snapMakeRemovalAggregateTree(testPage.document(), removalStepsCount, destructionCount, true, false);
    auto appendResult = container->appendChild(*tree.root);
    ASSERT_FALSE(appendResult.hasException());

    container->removeChildren();
    EXPECT_EQ(tree.elements.size(), removalStepsCount);
    tree.root = nullptr;
    GarbageCollectionController::singleton().garbageCollectNow();
    EXPECT_EQ(0u, destructionCount);
    testPage.document().asyncNodeDeletionQueue().deleteNodesNow();
    GarbageCollectionController::singleton().garbageCollectNow();
    EXPECT_EQ(tree.elements.size(), destructionCount);
}
#endif

static String snapSelectorFilterFixture()
{
    StringBuilder builder;
    builder.append(
        "<div id='root' class='outer' data-Mixed='x'>"
        "<section id='wide' class='c0 c1 c2 c3 c4 c5 c6 c7 c8 c9 c10 c11' data-one='1' data-two='2' title='wide'>"_s);
    for (unsigned depth = 0; depth < 30; ++depth) {
        builder.append("<div id='deep"_s, depth, "' class='level"_s, depth % 3, "' data-depth='"_s, depth, "'>"_s);
    }
    builder.append("<span id='leaf'></span>"_s);
    for (unsigned depth = 0; depth < 30; ++depth) {
        builder.append("</div>"_s);
    }
    builder.append(
        "</section>"
        "<div id='dup' class='same'><div id='dup-child' class='same'><span id='dup-leaf'></span></div></div>"
        "<svg id='vector' viewBox='0 0 10 10' preserveAspectRatio='none'>"
        "<g id='group' class='shape'><rect id='shape' width='4' height='4'></rect></g></svg></div>"_s);
    return builder.toString();
}

static Vector<unsigned> snapElementIdentifierHashes(const Element& element)
{
    Vector<unsigned, 4> collected;
    SelectorFilter::collectElementIdentifierHashes(element, collected);
    Vector<unsigned> hashes;
    hashes.appendVector(collected);
    return hashes;
}

static Vector<Ref<Element>> snapAncestorChain(Element& element)
{
    Vector<Ref<Element>> chain;
    for (RefPtr ancestor = &element; ancestor; ancestor = ancestor->parentElement()) {
        chain.insert(0, *ancestor);
    }
    return chain;
}

static bool snapFilterMayContain(const SelectorFilter& filter, unsigned hash)
{
    return !filter.fastRejectSelector(SelectorFilter::Hashes { hash, 0, 0, 0 });
}

static SelectorFilter::Hashes snapSelectorHashes(Document& document, const String& selector)
{
    auto sheet = StyleSheetContents::create(CSSParserContext { document });
    EXPECT_TRUE(sheet->parseString(makeString(selector, " { color: green; }"_s)));
    EXPECT_EQ(1u, sheet->ruleCount());
    RefPtr rule = sheet->ruleCount() ? dynamicDowncast<StyleRule>(sheet->ruleAt(0)) : nullptr;
    EXPECT_TRUE(rule);
    if (!rule) {
        return {};
    }
    return SelectorFilter::collectHashes(rule->selectorList().first());
}

static Vector<unsigned> snapSelectorFilterProbeHashes(TestPageHarness& testPage)
{
    Vector<unsigned> probes;
    for (auto& identifier : { "root"_s, "wide"_s, "dup"_s, "dup-child"_s, "vector"_s, "group"_s, "shape"_s }) {
        RefPtr element = testPage.getElementById(identifier);
        EXPECT_TRUE(element);
        if (element) {
            probes.appendVector(snapElementIdentifierHashes(*element));
        }
    }
    for (unsigned depth = 0; depth < 30; ++depth) {
        RefPtr element = testPage.getElementById(makeString("deep"_s, depth));
        EXPECT_TRUE(element);
        if (element) {
            probes.appendVector(snapElementIdentifierHashes(*element));
        }
    }
    for (unsigned value = 1; value <= 64; ++value) {
        probes.append(value * 2654435761u);
    }
    return probes;
}

TEST(SelectorFilterStorage, ElementHashOrderMatchesSelectorHashes)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML(snapSelectorFilterFixture());
    RefPtr wide = testPage.getElementById("wide"_s);
    ASSERT_TRUE(wide);
    auto& document = testPage.document();

    auto hashes = snapElementIdentifierHashes(*wide);
    ASSERT_EQ(17u, hashes.size());
    EXPECT_EQ(snapSelectorHashes(document, "section *"_s)[0], hashes[0]);
    EXPECT_EQ(snapSelectorHashes(document, "#wide *"_s)[0], hashes[1]);
    for (unsigned index = 0; index < 12; ++index) {
        EXPECT_EQ(snapSelectorHashes(document, makeString(".c"_s, index, " *"_s))[0], hashes[2 + index]);
    }
    EXPECT_EQ(snapSelectorHashes(document, "[data-one] *"_s)[0], hashes[14]);
    EXPECT_EQ(snapSelectorHashes(document, "[data-two] *"_s)[0], hashes[15]);
    EXPECT_EQ(snapSelectorHashes(document, "[title] *"_s)[0], hashes[16]);
}

TEST(SelectorFilterStorage, OverflowingParentFrameKeepsEveryHash)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML(snapSelectorFilterFixture());
    RefPtr wide = testPage.getElementById("wide"_s);
    ASSERT_TRUE(wide);
    auto& document = testPage.document();
    auto wideHashes = snapElementIdentifierHashes(*wide);
    EXPECT_GT(wideHashes.size(), 10u);

    SelectorFilter filter;
    filter.pushParentInitializingIfNeeded(*wide);
    EXPECT_TRUE(filter.parentStackIsConsistent(wide.get()));
    Vector<unsigned> ancestorHashes;
    for (auto& ancestor : snapAncestorChain(*wide)) {
        ancestorHashes.appendVector(snapElementIdentifierHashes(ancestor));
    }
    for (auto hash : ancestorHashes) {
        EXPECT_TRUE(snapFilterMayContain(filter, hash));
    }
    EXPECT_FALSE(filter.fastRejectSelector(snapSelectorHashes(document, "section.c11[data-two] span"_s)));
    EXPECT_FALSE(filter.fastRejectSelector(snapSelectorHashes(document, "#wide.c0.c10[TITLE] > div"_s)));
    EXPECT_FALSE(filter.fastRejectSelector(snapSelectorHashes(document, ".c9.c10.c11[data-one] div"_s)));

    filter.popParentsUntil(nullptr);
    EXPECT_TRUE(filter.parentStackIsEmpty());
    for (auto hash : ancestorHashes) {
        EXPECT_FALSE(snapFilterMayContain(filter, hash));
    }
    EXPECT_TRUE(filter.fastRejectSelector(snapSelectorHashes(document, "section.c11[data-two] span"_s)));
}

TEST(SelectorFilterStorage, DeepParentStackMatchesFreshFiltersAfterPops)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML(snapSelectorFilterFixture());
    auto& document = testPage.document();
    RefPtr deepest = testPage.getElementById("deep29"_s);
    RefPtr middle = testPage.getElementById("deep9"_s);
    ASSERT_TRUE(deepest);
    ASSERT_TRUE(middle);
    auto chain = snapAncestorChain(*deepest);
    ASSERT_GT(chain.size(), 20u);
    auto probes = snapSelectorFilterProbeHashes(testPage);

    SelectorFilter incremental;
    incremental.pushParentInitializingIfNeeded(chain[0]);
    for (size_t index = 1; index < chain.size(); ++index) {
        incremental.pushParent(chain[index].ptr());
    }
    EXPECT_TRUE(incremental.parentStackIsConsistent(deepest.get()));
    SelectorFilter initialized;
    initialized.pushParentInitializingIfNeeded(*deepest);
    EXPECT_TRUE(initialized.parentStackIsConsistent(deepest.get()));
    for (auto hash : probes) {
        EXPECT_EQ(snapFilterMayContain(initialized, hash), snapFilterMayContain(incremental, hash));
    }
    for (auto& ancestor : chain) {
        for (auto hash : snapElementIdentifierHashes(ancestor)) {
            EXPECT_TRUE(snapFilterMayContain(incremental, hash));
        }
    }
    EXPECT_FALSE(incremental.fastRejectSelector(snapSelectorHashes(document, "#deep0 .level2[data-depth] span"_s)));
    EXPECT_FALSE(incremental.fastRejectSelector(snapSelectorHashes(document, "#deep29.level2 > span"_s)));

    incremental.popParentsUntil(middle.get());
    EXPECT_TRUE(incremental.parentStackIsConsistent(middle.get()));
    SelectorFilter fresh;
    fresh.pushParentInitializingIfNeeded(*middle);
    for (auto hash : probes) {
        EXPECT_EQ(snapFilterMayContain(fresh, hash), snapFilterMayContain(incremental, hash));
    }
    EXPECT_FALSE(incremental.fastRejectSelector(snapSelectorHashes(document, "#deep9.level0 span"_s)));

    incremental.popParentsUntil(nullptr);
    initialized.popParentsUntil(nullptr);
    EXPECT_TRUE(incremental.parentStackIsEmpty());
    EXPECT_TRUE(initialized.parentStackIsEmpty());
    for (auto hash : probes) {
        EXPECT_FALSE(snapFilterMayContain(incremental, hash));
        EXPECT_FALSE(snapFilterMayContain(initialized, hash));
    }
}

TEST(SelectorFilterStorage, DuplicateAncestorHashesSurviveChildPop)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML(snapSelectorFilterFixture());
    auto& document = testPage.document();
    RefPtr duplicate = testPage.getElementById("dup"_s);
    RefPtr duplicateChild = testPage.getElementById("dup-child"_s);
    ASSERT_TRUE(duplicate);
    ASSERT_TRUE(duplicateChild);
    auto sameClass = snapSelectorHashes(document, ".same *"_s)[0];
    auto divTag = snapSelectorHashes(document, "div *"_s)[0];
    auto probes = snapSelectorFilterProbeHashes(testPage);

    SelectorFilter filter;
    filter.pushParentInitializingIfNeeded(*duplicate);
    filter.pushParent(duplicateChild.get());
    EXPECT_TRUE(snapFilterMayContain(filter, sameClass));
    EXPECT_TRUE(snapFilterMayContain(filter, divTag));
    filter.popParent();
    EXPECT_TRUE(filter.parentStackIsConsistent(duplicate.get()));
    EXPECT_TRUE(snapFilterMayContain(filter, sameClass));
    EXPECT_TRUE(snapFilterMayContain(filter, divTag));
    EXPECT_FALSE(filter.fastRejectSelector(snapSelectorHashes(document, "#dup.same span"_s)));

    SelectorFilter fresh;
    fresh.pushParentInitializingIfNeeded(*duplicate);
    for (auto hash : probes) {
        EXPECT_EQ(snapFilterMayContain(fresh, hash), snapFilterMayContain(filter, hash));
    }
}

TEST(SelectorFilterStorage, SvgAndHtmlAttributeCasingMatchesSelectors)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML(snapSelectorFilterFixture());
    auto& document = testPage.document();
    RefPtr vector = testPage.getElementById("vector"_s);
    RefPtr group = testPage.getElementById("group"_s);
    RefPtr root = testPage.getElementById("root"_s);
    ASSERT_TRUE(vector);
    ASSERT_TRUE(group);
    ASSERT_TRUE(root);

    auto viewBox = snapSelectorHashes(document, "[viewBox] *"_s)[0];
    EXPECT_EQ(viewBox, snapSelectorHashes(document, "[viewbox] *"_s)[0]);
    EXPECT_TRUE(snapElementIdentifierHashes(*vector).contains(viewBox));
    auto mixed = snapSelectorHashes(document, "[DATA-MIXED] *"_s)[0];
    EXPECT_TRUE(snapElementIdentifierHashes(*root).contains(mixed));

    SelectorFilter filter;
    filter.pushParentInitializingIfNeeded(*group);
    EXPECT_FALSE(filter.fastRejectSelector(snapSelectorHashes(document, "svg[viewBox] rect"_s)));
    EXPECT_FALSE(filter.fastRejectSelector(snapSelectorHashes(document, "svg[viewbox][preserveAspectRatio] g.shape > rect"_s)));
    EXPECT_FALSE(filter.fastRejectSelector(snapSelectorHashes(document, "div.outer[data-mixed] rect"_s)));
    filter.popParentsUntil(nullptr);
    EXPECT_TRUE(filter.fastRejectSelector(snapSelectorHashes(document, "svg[viewBox] rect"_s)));
}

TEST(CachedMatchFinder, CountMatchesFindsMatches)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p>The quick brown fox jumped over the brown dog.</p>"_s);

    CachedMatchFinder finder(testPage.document());
    auto result = finder.countMatches(std::nullopt, "brown"_s, { });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 2u);
}

TEST(CachedMatchFinder, CountMatchesReturnsZeroWhenNoMatches)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p>The quick brown fox jumped over the lazy dog.</p>"_s);

    CachedMatchFinder finder(testPage.document());
    auto result = finder.countMatches(std::nullopt, "purple"_s, { });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 0u);
}

TEST(CachedMatchFinder, CountMatchesWithEmptyTargetReturnsZero)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p>The quick brown fox jumped over the brown dog.</p>"_s);

    CachedMatchFinder finder(testPage.document());
    auto result = finder.countMatches(std::nullopt, ""_s, { });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 0u);
}

TEST(CachedMatchFinder, CountMatchesRespectsLimit)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p>brown brown brown brown</p>"_s);

    CachedMatchFinder finder(testPage.document());
    auto result = finder.countMatches(std::nullopt, "brown"_s, { }, 2u);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 2u);
}

TEST(CachedMatchFinder, FindMatchesReturnsAllMatchingRanges)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p>The quick brown fox jumped over the brown dog.</p>"_s);

    CachedMatchFinder finder(testPage.document());
    auto result = finder.findMatches(std::nullopt, "brown"_s, { });

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().size(), 2u);
    EXPECT_EQ(plainText(result.value()[0]), "brown"_s);
    EXPECT_EQ(plainText(result.value()[1]), "brown"_s);
}

TEST(CachedMatchFinder, FindMatchesReturnsEmptyVectorWhenNoMatches)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p>The quick brown fox jumped over the lazy dog.</p>"_s);

    CachedMatchFinder finder(testPage.document());
    auto result = finder.findMatches(std::nullopt, "purple"_s, { });

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().isEmpty());
}

TEST(CachedMatchFinder, FindMatchesRespectsLimit)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p>brown brown brown brown</p>"_s);

    CachedMatchFinder finder(testPage.document());
    auto result = finder.findMatches(std::nullopt, "brown"_s, { }, 1u);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().size(), 1u);
}

TEST(CachedMatchFinder, FindMatchFromReturnsFirstMatchWhenStartingWithoutReferenceRange)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p>The quick brown fox jumped over the brown dog.</p>"_s);

    CachedMatchFinder finder(testPage.document());
    auto result = finder.findMatchFrom(std::nullopt, "brown"_s, { });

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result.value().has_value());
    EXPECT_EQ(plainText(*result.value()), "brown"_s);
}

TEST(CachedMatchFinder, FindMatchFromReturnsNulloptWhenNoMatches)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p>The quick brown fox jumped over the lazy dog.</p>"_s);

    CachedMatchFinder finder(testPage.document());
    auto result = finder.findMatchFrom(std::nullopt, "purple"_s, { });

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result.value().has_value());
}

TEST(CachedMatchFinder, FindMatchFromAdvancesPastReferenceRangeToNextMatch)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p>The quick brown fox jumped over the brown dog.</p>"_s);

    CachedMatchFinder finder(testPage.document());
    auto firstMatch = finder.findMatchFrom(std::nullopt, "brown"_s, { });
    ASSERT_TRUE(firstMatch.has_value());
    ASSERT_TRUE(firstMatch.value().has_value());

    auto secondMatch = finder.findMatchFrom(*firstMatch.value(), "brown"_s, { });
    ASSERT_TRUE(secondMatch.has_value());
    ASSERT_TRUE(secondMatch.value().has_value());
    EXPECT_NE(*firstMatch.value(), *secondMatch.value());
    EXPECT_EQ(plainText(*secondMatch.value()), "brown"_s);

    auto thirdMatch = finder.findMatchFrom(*secondMatch.value(), "brown"_s, { });
    ASSERT_TRUE(thirdMatch.has_value());
    EXPECT_FALSE(thirdMatch.value().has_value());
}

TEST(CachedMatchFinder, FindMatchFromWithBackwardsOptionFindsPrecedingMatch)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p>The quick brown fox jumped over the brown dog.</p>"_s);

    CachedMatchFinder finder(testPage.document());
    auto lastMatch = finder.findMatchFrom(std::nullopt, "brown"_s, FindOption::Backwards);
    ASSERT_TRUE(lastMatch.has_value());
    ASSERT_TRUE(lastMatch.value().has_value());

    auto precedingMatch = finder.findMatchFrom(*lastMatch.value(), "brown"_s, FindOption::Backwards);
    ASSERT_TRUE(precedingMatch.has_value());
    ASSERT_TRUE(precedingMatch.value().has_value());
    EXPECT_NE(*lastMatch.value(), *precedingMatch.value());

    auto noMoreMatches = finder.findMatchFrom(*precedingMatch.value(), "brown"_s, FindOption::Backwards);
    ASSERT_TRUE(noMoreMatches.has_value());
    EXPECT_FALSE(noMoreMatches.value().has_value());
}

TEST(CachedMatchFinder, FindMatchFromWithoutWrapAroundStopsAtEndOfDocument)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p>brown fox, then more brown text.</p>"_s);

    CachedMatchFinder finder(testPage.document());
    auto allMatches = finder.findMatches(std::nullopt, "brown"_s, { });
    ASSERT_TRUE(allMatches.has_value());
    ASSERT_EQ(allMatches.value().size(), 2u);

    auto result = finder.findMatchFrom(allMatches.value().last(), "brown"_s, { });
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result.value().has_value());
}

TEST(CachedMatchFinder, FindMatchFromWithWrapAroundWrapsToBeginningOfDocument)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p>brown fox, then more brown text.</p>"_s);

    CachedMatchFinder finder(testPage.document());
    auto allMatches = finder.findMatches(std::nullopt, "brown"_s, { });
    ASSERT_TRUE(allMatches.has_value());
    ASSERT_EQ(allMatches.value().size(), 2u);

    auto result = finder.findMatchFrom(allMatches.value().last(), "brown"_s, FindOption::WrapAround);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result.value().has_value());
    EXPECT_EQ(*result.value(), allMatches.value().first());
}

TEST(CachedMatchFinder, CaseInsensitiveOptionMatchesRegardlessOfCase)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p>Brown paper, brown bag.</p>"_s);

    CachedMatchFinder finder(testPage.document());

    auto caseSensitiveResult = finder.countMatches(std::nullopt, "brown"_s, { });
    ASSERT_TRUE(caseSensitiveResult.has_value());
    EXPECT_EQ(caseSensitiveResult.value(), 1u);

    auto caseInsensitiveResult = finder.countMatches(std::nullopt, "brown"_s, FindOption::CaseInsensitive);
    ASSERT_TRUE(caseInsensitiveResult.has_value());
    EXPECT_EQ(caseInsensitiveResult.value(), 2u);
}

TEST(CachedMatchFinder, AtWordStartsOptionExcludesMidWordMatches)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p>unbrown brown</p>"_s);

    CachedMatchFinder finder(testPage.document());

    auto withoutWordStarts = finder.countMatches(std::nullopt, "brown"_s, { });
    ASSERT_TRUE(withoutWordStarts.has_value());
    EXPECT_EQ(withoutWordStarts.value(), 2u);

    auto withWordStarts = finder.countMatches(std::nullopt, "brown"_s, FindOption::AtWordStarts);
    ASSERT_TRUE(withWordStarts.has_value());
    EXPECT_EQ(withWordStarts.value(), 1u);
}

TEST(CachedMatchFinder, TextBufferCacheIsInvalidatedByDOMMutation)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p id=\"content\">The quick brown fox jumped over the brown dog.</p>"_s);

    CachedMatchFinder finder(testPage.document());
    auto initialResult = finder.countMatches(std::nullopt, "brown"_s, { });
    ASSERT_TRUE(initialResult.has_value());
    EXPECT_EQ(initialResult.value(), 2u);

    RefPtr content = testPage.getElementById("content"_s);
    ASSERT_TRUE(content);
    auto setInnerHTMLResult = content->setInnerHTML(String { "brown brown brown"_s });
    EXPECT_FALSE(setInnerHTMLResult.hasException());

    auto updatedResult = finder.countMatches(std::nullopt, "brown"_s, { });
    ASSERT_TRUE(updatedResult.has_value());
    EXPECT_EQ(updatedResult.value(), 3u);
}

TEST(CachedMatchFinder, TextBufferCacheIsInvalidatedByStyleChangeWithoutDOMMutation)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p id=\"content\" style=\"display: none;\">brown fox</p>"_s);

    CachedMatchFinder finder(testPage.document());
    auto hiddenResult = finder.countMatches(std::nullopt, "brown"_s, { });
    ASSERT_TRUE(hiddenResult.has_value());
    EXPECT_EQ(hiddenResult.value(), 0u);

    RefPtr content = dynamicDowncast<StyledElement>(testPage.getElementById("content"_s));
    ASSERT_TRUE(content);
    content->setInlineStyleProperty(CSSPropertyDisplay, CSSValueBlock);
    testPage.document().updateLayoutIgnorePendingStylesheets();

    auto visibleResult = finder.countMatches(std::nullopt, "brown"_s, { });
    ASSERT_TRUE(visibleResult.has_value());
    EXPECT_EQ(visibleResult.value(), 1u);
}

TEST(CachedMatchFinder, FlatTreeAndLightTreeBufferCachesRemainIndependentAcrossRepeatedSwitches)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<div id=\"host\"><template shadowrootmode=\"open\"><p>brown fox</p></template></div>"_s);

    RefPtr host = testPage.getElementById("host"_s);
    ASSERT_TRUE(host);
    ASSERT_TRUE(host->shadowRoot());

    CachedMatchFinder finder(testPage.document());

    for (unsigned index = 0; index < 3; ++index) {
        auto flatTreeResult = finder.countMatches(std::nullopt, "brown"_s, { });
        ASSERT_TRUE(flatTreeResult.has_value());
        EXPECT_EQ(flatTreeResult.value(), 1u);

        auto lightTreeResult = finder.countMatches(std::nullopt, "brown"_s, FindOption::DoNotTraverseFlatTree);
        ASSERT_TRUE(lightTreeResult.has_value());
        EXPECT_EQ(lightTreeResult.value(), 0u);
    }
}

TEST(CachedMatchFinder, SearchResultCacheDistinguishesDifferentTargets)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p>The quick brown fox jumped over the brown dog.</p>"_s);

    CachedMatchFinder finder(testPage.document());

    auto brownResult = finder.countMatches(std::nullopt, "brown"_s, { });
    ASSERT_TRUE(brownResult.has_value());
    EXPECT_EQ(brownResult.value(), 2u);

    auto foxResult = finder.countMatches(std::nullopt, "fox"_s, { });
    ASSERT_TRUE(foxResult.has_value());
    EXPECT_EQ(foxResult.value(), 1u);

    auto brownResultAgain = finder.countMatches(std::nullopt, "brown"_s, { });
    ASSERT_TRUE(brownResultAgain.has_value());
    EXPECT_EQ(brownResultAgain.value(), 2u);
}

TEST(CachedMatchFinder, SearchResultCacheDistinguishesDifferentOptions)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p>Brown paper, brown bag.</p>"_s);

    CachedMatchFinder finder(testPage.document());

    auto caseSensitiveResult = finder.countMatches(std::nullopt, "brown"_s, { });
    ASSERT_TRUE(caseSensitiveResult.has_value());
    EXPECT_EQ(caseSensitiveResult.value(), 1u);

    auto caseInsensitiveResult = finder.countMatches(std::nullopt, "brown"_s, FindOption::CaseInsensitive);
    ASSERT_TRUE(caseInsensitiveResult.has_value());
    EXPECT_EQ(caseInsensitiveResult.value(), 2u);

    auto caseSensitiveResultAgain = finder.countMatches(std::nullopt, "brown"_s, { });
    ASSERT_TRUE(caseSensitiveResultAgain.has_value());
    EXPECT_EQ(caseSensitiveResultAgain.value(), 1u);
}

TEST(CachedMatchFinder, FindMatchFromWithinShadowTreeUsesShadowIncludingAncestorTraversal)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<div id=\"host\"><template shadowrootmode=\"open\"><p>brown fox, brown dog</p></template></div>"_s);

    RefPtr host = testPage.getElementById("host"_s);
    ASSERT_TRUE(host);
    RefPtr shadowRoot = host->shadowRoot();
    ASSERT_TRUE(shadowRoot);

    RefPtr paragraph = shadowRoot->firstElementChild();
    ASSERT_TRUE(paragraph);
    auto contentsRange = makeRangeSelectingNodeContents(*paragraph);
    SimpleRange referenceRange { contentsRange.start, contentsRange.start };

    CachedMatchFinder finder(testPage.document());
    auto result = finder.findMatchFrom(referenceRange, "brown"_s, FindOption::DoNotTraverseFlatTree);

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result.value().has_value());
    EXPECT_EQ(plainText(*result.value()), "brown"_s);
}

TEST(CachedMatchFinder, OversizedDocumentReturnsCacheUnusableError)
{
    auto testPage = TestPageHarness::create();
    testPage.loadHTML("<p>The quick brown fox jumped over the brown dog.</p>"_s);

    CachedMatchFinder::setMaximumRunCountForTesting(0u);

    CachedMatchFinder finder(testPage.document());
    auto countResult = finder.countMatches(std::nullopt, "brown"_s, { });
    EXPECT_FALSE(countResult.has_value());
    if (!countResult.has_value())
        EXPECT_EQ(countResult.error(), CachedMatchFinder::CacheUnusable::Oversized);

    auto matchesResult = finder.findMatches(std::nullopt, "brown"_s, { });
    EXPECT_FALSE(matchesResult.has_value());
    if (!matchesResult.has_value())
        EXPECT_EQ(matchesResult.error(), CachedMatchFinder::CacheUnusable::Oversized);

    auto matchFromResult = finder.findMatchFrom(std::nullopt, "brown"_s, { });
    EXPECT_FALSE(matchFromResult.has_value());
    if (!matchFromResult.has_value())
        EXPECT_EQ(matchFromResult.error(), CachedMatchFinder::CacheUnusable::Oversized);

    CachedMatchFinder::setMaximumRunCountForTesting(std::nullopt);
}

} // namespace TestWebKitAPI
