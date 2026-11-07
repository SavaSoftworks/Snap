/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 2004-2005 Allan Sandfeld Jensen (kde@carewolf.com)
 * Copyright (C) 2006, 2007 Nicholas Shanks (webkit@nickshanks.com)
 * Copyright (C) 2005-2023 Apple Inc. All rights reserved.
 * Copyright (C) 2007 Alexey Proskuryakov <ap@webkit.org>
 * Copyright (C) 2007, 2008 Eric Seidel <eric@webkit.org>
 * Copyright (C) 2008, 2009 Torch Mobile Inc. All rights reserved. (http://www.torchmobile.com/)
 * Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 * Copyright (C) Research In Motion Limited 2011. All rights reserved.
 * Copyright (C) 2012 Google Inc. All rights reserved.
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

#include "config.h"
#include "ElementRuleCollector.h"
#include <cstdlib>
#include <cstring>

#include "CSSKeyframeRule.h"
#include "CSSRuleList.h"
#include "CSSSelector.h"
#include "CSSSelectorList.h"
#include "CSSStyleSheet.h"
#include "CSSValueKeywords.h"
#include "ContainerQueryEvaluator.h"
#include "DeclarationOrigin.h"
#include "DocumentFullscreen.h"
#include "ElementInlines.h"
#include "ElementRareData.h"
#include "ElementTextDirection.h"
#include "HTMLElement.h"
#include "HTMLNames.h"
#include "HTMLSlotElement.h"
#include "SVGElement.h"
#include "SelectorCheckerTestFunctions.h"
#include "SelectorCompiler.h"
#include "SelectorMatchingState.h"
#include "ShadowRoot.h"
#include "StyleProperties.h"
#include "StyleResolver.h"
#include "StyleRuleImport.h"
#include "StyleScope.h"
#include "StyleScopeOrdinal.h"
#include "StyleScopeRuleSets.h"
#include "StyleSheetContents.h"
#include "StyledElement.h"
#include "Supplementable.h"
#include "UserAgentStyle.h"
#if ENABLE(VIDEO)
#include "UserAgentParts.h"
#endif
#include <array>
#include <limits>
#include <ranges>
#include <wtf/HashMap.h>
#include <wtf/Hasher.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/SetForScope.h>
#include <wtf/text/StringHash.h>

namespace WebCore {
namespace Style {

static const StyleProperties& leftToRightDeclaration()
{
IGNORE_GCC_WARNINGS_BEGIN("dangling-reference")
    static auto& declaration = [] () -> const StyleProperties& {
        auto properties = MutableStyleProperties::create();
        properties->setProperty(CSSPropertyDirection, CSSValueLtr);
        return properties.leakRef();
    }();
IGNORE_GCC_WARNINGS_END
    return declaration;
}

static const StyleProperties& rightToLeftDeclaration()
{
IGNORE_GCC_WARNINGS_BEGIN("dangling-reference")
    static auto& declaration = [] () -> const StyleProperties& {
        auto properties = MutableStyleProperties::create();
        properties->setProperty(CSSPropertyDirection, CSSValueRtl);
        return properties.leakRef();
    }();
IGNORE_GCC_WARNINGS_END
    return declaration;
}

struct MatchRequest {
    MatchRequest(const RuleSet& ruleSet, ScopeOrdinal styleScopeOrdinal = ScopeOrdinal::Element)
        : ruleSet(ruleSet)
        , styleScopeOrdinal(styleScopeOrdinal)
    {
    }
    const RuleSet& ruleSet;
    ScopeOrdinal styleScopeOrdinal;
    bool matchingPartPseudoElementRules { false };
};

ElementRuleCollector::ElementRuleCollector(const Element& element, const ScopeRuleSets& ruleSets, SelectorMatchingState* selectorMatchingState, SelectorChecker::Mode mode)
    : m_element(element)
    , m_authorStyle(ruleSets.authorStyle())
    , m_userStyle(ruleSets.userStyle())
    , m_userAgentMediaQueryStyle(ruleSets.userAgentMediaQueryStyle())
    , m_dynamicViewTransitionsStyle(ruleSets.dynamicViewTransitionsStyle())
    , m_selectorMatchingState(selectorMatchingState)
    , m_mode(mode)
    , m_result(MatchResult::create(element.isLink()))
{
    ASSERT(!m_selectorMatchingState || m_selectorMatchingState->selectorFilter.parentStackIsConsistent(element.parentNode()));
}

ElementRuleCollector::ElementRuleCollector(const Element& element, const RuleSet& authorStyle, SelectorMatchingState* selectorMatchingState, SelectorChecker::Mode mode)
    : m_element(element)
    , m_authorStyle(authorStyle)
    , m_selectorMatchingState(selectorMatchingState)
    , m_mode(mode)
    , m_result(MatchResult::create(element.isLink()))
{
    ASSERT(!m_selectorMatchingState || m_selectorMatchingState->selectorFilter.parentStackIsConsistent(element.parentNode()));
}

const MatchResult& ElementRuleCollector::matchResult() const
{
    ASSERT(m_mode == SelectorChecker::Mode::ResolvingStyle);
    return m_result;
}

Ref<MatchResult> ElementRuleCollector::releaseMatchResult()
{
    return WTF::move(m_result);
}

const Vector<Ref<const StyleRule>>& ElementRuleCollector::matchedRuleList() const
{
    ASSERT(m_mode == SelectorChecker::Mode::CollectingRules);
    return m_matchedRuleList;
}

inline void ElementRuleCollector::addMatchedRule(const RuleData& ruleData, unsigned specificity, unsigned scopingRootDistance, const MatchRequest& matchRequest)
{
    auto cascadeLayerPriority = matchRequest.ruleSet.cascadeLayerPriorityFor(ruleData);
    m_matchedRules.append({ &ruleData, specificity, scopingRootDistance, matchRequest.styleScopeOrdinal, cascadeLayerPriority });
}

void ElementRuleCollector::clearMatchedRules()
{
    m_matchedRules.clear();
    m_matchedRuleTransferIndex = 0;
}

inline void ElementRuleCollector::addElementStyleProperties(const StyleProperties* propertySet, CascadeLayerPriority priority, IsCacheable isCacheable, FromStyleAttribute fromStyleAttribute)
{
    if (!propertySet || propertySet->isEmpty())
        return;

    auto matchedProperty = MatchedProperties { *propertySet };
    matchedProperty.cascadeLayerPriority = priority;
    matchedProperty.fromStyleAttribute = fromStyleAttribute;
    matchedProperty.isCacheable = isCacheable;
    addMatchedProperties(WTF::move(matchedProperty), DeclarationOrigin::Author);
}

bool ElementRuleCollector::isFirstMatchModeAndHasMatchedAnyRules() const
{
    return m_mode == SelectorChecker::Mode::StyleInvalidation && !m_matchedRules.isEmpty();
}

void ElementRuleCollector::collectMatchingRules(DeclarationOrigin origin)
{
    switch (origin) {
    case DeclarationOrigin::Author: {
        MatchRequest matchRequest(m_authorStyle);
        collectMatchingRules(matchRequest);
        if (isFirstMatchModeAndHasMatchedAnyRules())
            return;
        break;
    }

    case DeclarationOrigin::User:
        if (m_userStyle) {
            MatchRequest matchRequest(*m_userStyle);
            collectMatchingRules(matchRequest);
            if (isFirstMatchModeAndHasMatchedAnyRules())
                return;
        }
        break;

    case DeclarationOrigin::UserAgent:
        ASSERT_NOT_REACHED();
        return;
    }

    auto* parent = element().parentElement();
    if (parent && parent->shadowRoot()) {
        matchSlottedPseudoElementRules(origin);
        if (isFirstMatchModeAndHasMatchedAnyRules())
            return;
    }

    if (element().shadowRoot()) {
        matchHostPseudoClassRules(origin);
        if (isFirstMatchModeAndHasMatchedAnyRules())
            return;
    }

    if (element().isInShadowTree()) {
        matchUserAgentPartRules(origin);
        if (isFirstMatchModeAndHasMatchedAnyRules())
            return;
        matchPartPseudoElementRules(origin);
        if (isFirstMatchModeAndHasMatchedAnyRules())
            return;
        matchSlottedPseudoElementRulesInUserAgentShadowTree(origin);
    }
}

void ElementRuleCollector::collectMatchingRules(const MatchRequest& matchRequest)
{
    ASSERT_WITH_MESSAGE(!(m_mode == SelectorChecker::Mode::StyleInvalidation && m_pseudoElementRequest), "When in StyleInvalidation or SharingRules, SelectorChecker does not try to match the pseudo ID. While ElementRuleCollector supports matching a particular pseudoId in this case, this would indicate a error at the call site since matching a particular element should be unnecessary.");

    auto& element = this->element();
    auto* shadowRoot = element.containingShadowRoot();
    if (shadowRoot && shadowRoot->mode() == ShadowRootMode::UserAgent)
        collectMatchingUserAgentPartRules(matchRequest);

    bool isHTMLElement = element.isHTMLElement();
    bool isCaseInsensitiveForHTML = isHTMLElement && element.document().isHTMLDocument();
    auto& ruleSet = matchRequest.ruleSet;

    // We need to collect the rules for id, class, tag, and everything else into a buffer and
    // then sort the buffer.
    auto& id = element.idForStyleResolution();
    if (!id.isNull())
        collectMatchingRulesForList(ruleSet.idRules(id), matchRequest);
    if (element.hasClass()) {
        for (auto& className : element.classNames())
            collectMatchingRulesForList(ruleSet.classRules(className), matchRequest);
    }
    if (element.hasAttributesWithoutUpdate() && ruleSet.hasAttributeRules()) {
        Vector<const RuleSet::RuleDataVector*, 4> ruleVectors;
        for (auto& attribute : element.attributes()) {
            if (auto* rules = ruleSet.attributeRules(attribute.localName(), isCaseInsensitiveForHTML))
                ruleVectors.append(rules);
        }
        for (auto* rules : ruleVectors)
            collectMatchingRulesForList(rules, matchRequest);
    }

    if (m_pseudoElementRequest) {
        if (m_pseudoElementRequest->type() == PseudoElementType::UserAgentPartFallback)
            collectMatchingRulesForList(ruleSet.userAgentPartRules(m_pseudoElementRequest->nameOrPart()), matchRequest);
        else if (!m_pseudoElementRequest->nameOrPart().isNull())
            collectMatchingRulesForList(ruleSet.namedPseudoElementRules(m_pseudoElementRequest->nameOrPart()), matchRequest);
    }

    if (element.isLink())
        collectMatchingRulesForList(ruleSet.linkPseudoClassRules(), matchRequest);
    if (matchesFocusPseudoClass(element))
        collectMatchingRulesForList(ruleSet.focusPseudoClassRules(), matchRequest);
    if (matchesFocusVisiblePseudoClass(element))
        collectMatchingRulesForList(ruleSet.focusVisiblePseudoClassRules(), matchRequest);
#if ENABLE(FULLSCREEN_API)
    if (auto* fullscreen = element.document().fullscreenIfExists(); fullscreen && fullscreen->isFullscreen())
        collectMatchingRulesForList(ruleSet.fullscreenPseudoClassRules(), matchRequest);
#endif
    if (&element == element.document().documentElement())
        collectMatchingRulesForList(ruleSet.rootElementRules(), matchRequest);
    collectMatchingRulesForList(ruleSet.tagRules(element.localName(), isCaseInsensitiveForHTML), matchRequest);
    collectMatchingRulesForList(ruleSet.universalRules(), matchRequest);

    // Shortcut selectors like "::marker" for HTML elements.
    auto pseudoElementTypes = isHTMLElement ? ruleSet.universalHTMLPseudoElementTypes() : ruleSet.universalPseudoElementTypes();
    if (m_pseudoElementRequest) {
        if (pseudoElementTypes.contains(m_pseudoElementRequest->type()))
            collectMatchingRulesForList(ruleSet.universalPseudoElementRules(), matchRequest);
    } else {
        // If pseudo-element is not requested then just mark the bits that tell that this element has these.
        m_matchedPseudoElements.add(pseudoElementTypes & allPublicPseudoElementTypes);
    }
}


Vector<MatchedProperties>& ElementRuleCollector::declarationsForOrigin(DeclarationOrigin declarationOrigin)
{
    switch (declarationOrigin) {
    case DeclarationOrigin::UserAgent: return m_result->userAgentDeclarations;
    case DeclarationOrigin::User: return m_result->userDeclarations;
    case DeclarationOrigin::Author: return m_result->authorDeclarations;
    }
    ASSERT_NOT_REACHED();
    return m_result->authorDeclarations;
}

void ElementRuleCollector::sortAndTransferMatchedRules(DeclarationOrigin declarationOrigin)
{
    if (m_matchedRules.isEmpty())
        return;

    sortMatchedRules();

    transferMatchedRules(declarationOrigin);
}

void ElementRuleCollector::transferMatchedRules(DeclarationOrigin declarationOrigin, std::optional<ScopeOrdinal> fromScope)
{
    if (m_mode != SelectorChecker::Mode::CollectingRules)
        declarationsForOrigin(declarationOrigin).reserveCapacity(m_matchedRules.size());

    for (; m_matchedRuleTransferIndex < m_matchedRules.size(); ++m_matchedRuleTransferIndex) {
        auto& matchedRule = m_matchedRules[m_matchedRuleTransferIndex];
        if (fromScope && matchedRule.styleScopeOrdinal < *fromScope)
            break;

        if (m_mode == SelectorChecker::Mode::CollectingRules) {
            m_matchedRuleList.append(matchedRule.ruleData->styleRule());
            continue;
        }

        addMatchedProperties({
            matchedRule.ruleData->styleRule().properties(),
            static_cast<uint8_t>(matchedRule.ruleData->linkMatchType()),
            matchedRule.ruleData->propertyAllowlist(),
            matchedRule.styleScopeOrdinal,
            FromStyleAttribute::No,
            matchedRule.cascadeLayerPriority,
            matchedRule.ruleData->isStartingStyle()
        }, declarationOrigin);
    }
}

void ElementRuleCollector::matchAuthorRules()
{
    clearMatchedRules();

    collectMatchingRules(DeclarationOrigin::Author);

    sortAndTransferMatchedRules(DeclarationOrigin::Author);
}

bool ElementRuleCollector::matchesAnyAuthorRules()
{
    clearMatchedRules();

    collectMatchingRules(DeclarationOrigin::Author);

    if (m_mode == SelectorChecker::Mode::StyleInvalidation && m_matchedPseudoElements)
        return true;

    return !m_matchedRules.isEmpty();
}

void ElementRuleCollector::matchUserAgentPartRules(DeclarationOrigin origin)
{
    ASSERT(element().isInShadowTree());
    auto* shadowRoot = element().containingShadowRoot();
    if (!shadowRoot || shadowRoot->mode() != ShadowRootMode::UserAgent)
        return;

    // Look up user agent parts also from the host scope style as they are web-exposed.
    auto* hostRules = Scope::forNode(*shadowRoot->host()).resolver().ruleSets().styleForDeclarationOrigin(origin);
    if (!hostRules)
        return;

    MatchRequest hostRequest { *hostRules, ScopeOrdinal::ContainingHost };
    collectMatchingUserAgentPartRules(hostRequest);
}

void ElementRuleCollector::matchHostPseudoClassRules(DeclarationOrigin origin)
{
    ASSERT(element().shadowRoot());

    auto* shadowRules = element().shadowRoot()->styleScope().resolver().ruleSets().styleForDeclarationOrigin(origin);
    if (!shadowRules)
        return;

    auto collect = [&] (const auto& rules) {
        if (rules.isEmpty())
            return;

        MatchRequest hostMatchRequest { *shadowRules, ScopeOrdinal::Shadow };
        collectMatchingRulesForList(&rules, hostMatchRequest);
    };

    if (shadowRules->hasHostOrScopePseudoClassRulesInUniversalBucket())
        collect(shadowRules->universalRules());

    collect(shadowRules->hostPseudoClassRules());
}

void ElementRuleCollector::matchSlottedPseudoElementRules(DeclarationOrigin origin)
{
    auto* slot = element().assignedSlot();
    auto styleScopeOrdinal = ScopeOrdinal::FirstSlot;

    for (; slot; slot = slot->assignedSlot(), ++styleScopeOrdinal) {
        auto& styleScope = Scope::forNode(*slot);
        if (!styleScope.resolver().ruleSets().isAuthorStyleDefined())
            continue;

        auto* scopeRules = styleScope.resolver().ruleSets().styleForDeclarationOrigin(origin);
        if (!scopeRules)
            continue;

        MatchRequest scopeMatchRequest(*scopeRules, styleScopeOrdinal);
        collectMatchingRulesForList(&scopeRules->slottedPseudoElementRules(), scopeMatchRequest);

        if (styleScopeOrdinal == ScopeOrdinal::SlotLimit)
            break;
    }
}

void ElementRuleCollector::matchPartPseudoElementRules(DeclarationOrigin origin)
{
    ASSERT(element().isInShadowTree());
    if (!element().containingShadowRoot())
        return;

    bool isUserAgentPart = element().containingShadowRoot()->mode() == ShadowRootMode::UserAgent && !element().userAgentPart().isNull();

    auto& partMatchingElement = isUserAgentPart ? *element().shadowHost() : element();
    if (partMatchingElement.partNames().isEmpty() || !partMatchingElement.isInShadowTree())
        return;

    matchPartPseudoElementRulesForScope(partMatchingElement, origin);
}

void ElementRuleCollector::matchPartPseudoElementRulesForScope(const Element& partMatchingElement, DeclarationOrigin origin)
{
    auto* element = &partMatchingElement;
    auto styleScopeOrdinal = ScopeOrdinal::Element;

    for (; element; element = element->shadowHost(), --styleScopeOrdinal) {
        auto& styleScope = Scope::forNode(const_cast<Element&>(*element));
        if (!styleScope.resolver().ruleSets().isAuthorStyleDefined())
            continue;

        auto* hostRules = styleScope.resolver().ruleSets().styleForDeclarationOrigin(origin);
        if (!hostRules)
            continue;

        MatchRequest scopeMatchRequest(*hostRules, styleScopeOrdinal);
        scopeMatchRequest.matchingPartPseudoElementRules = true;

        collectMatchingRulesForList(&hostRules->partPseudoElementRules(), scopeMatchRequest);

        // Element may only be exposed to styling from enclosing scopes via exportparts attributes.
        if (element != &partMatchingElement && element->shadowRoot()->partMappings().isEmpty())
            break;

        if (styleScopeOrdinal == ScopeOrdinal::ContainingHostLimit)
            break;
    }
}

void ElementRuleCollector::matchSlottedPseudoElementRulesInUserAgentShadowTree(DeclarationOrigin origin)
{
    ASSERT(element().isInShadowTree());
    auto* shadowRoot = element().containingShadowRoot();
    if (!shadowRoot || shadowRoot->mode() != ShadowRootMode::UserAgent)
        return;

    auto* host = shadowRoot->host();
    if (!host)
        return;

    auto* slot = host->assignedSlot();
    auto styleScopeOrdinal = ScopeOrdinal::FirstSlot;

    for (; slot; slot = slot->assignedSlot(), ++styleScopeOrdinal) {
        auto& styleScope = Scope::forNode(*slot);
        if (!styleScope.resolver().ruleSets().isAuthorStyleDefined())
            continue;

        auto* scopeRules = styleScope.resolver().ruleSets().styleForDeclarationOrigin(origin);
        if (!scopeRules)
            continue;

        MatchRequest scopeMatchRequest(*scopeRules, styleScopeOrdinal);
        collectMatchingRulesForList(&scopeRules->slottedPseudoElementRules(), scopeMatchRequest);

        if (styleScopeOrdinal == ScopeOrdinal::SlotLimit)
            break;
    }
}

void ElementRuleCollector::collectMatchingUserAgentPartRules(const MatchRequest& matchRequest)
{
    ASSERT(element().isInUserAgentShadowTree());

    auto& rules = matchRequest.ruleSet;
#if ENABLE(VIDEO)
    if (element().isWebVTTElement() || element().userAgentPart() == UserAgentParts::cue())
        collectMatchingRulesForList(&rules.cuePseudoRules(), matchRequest);
#endif
    if (auto& part = element().userAgentPart(); !part.isEmpty())
        collectMatchingRulesForList(rules.userAgentPartRules(part), matchRequest);
}

void ElementRuleCollector::matchUserRules()
{
    clearMatchedRules();

    collectMatchingRules(DeclarationOrigin::User);

    sortAndTransferMatchedRules(DeclarationOrigin::User);
}

void ElementRuleCollector::matchUARules()
{
    // First we match rules from the user agent sheet.
    auto* userAgentStyleSheet = m_isPrintStyle
        ? UserAgentStyle::defaultPrintStyle : UserAgentStyle::defaultStyle;
    matchUARules(*userAgentStyleSheet);

    // In quirks mode, we match rules from the quirks user agent sheet.
    if (element().document().inQuirksMode())
        matchUARules(*UserAgentStyle::defaultQuirksStyle);

    if (m_userAgentMediaQueryStyle)
        matchUARules(*m_userAgentMediaQueryStyle);

    if (m_dynamicViewTransitionsStyle)
        matchUARules(*m_dynamicViewTransitionsStyle);
}

void ElementRuleCollector::matchUARules(const RuleSet& rules)
{
    clearMatchedRules();

    if (!snapCollectUARulesWithMemo(rules))
        collectMatchingRules(MatchRequest(rules));

    sortAndTransferMatchedRules(DeclarationOrigin::UserAgent);
}

static Vector<AtomString> classListForNamedViewTransitionPseudoElement(const Document& document, const AtomString& name)
{
    auto* activeViewTransition = document.activeViewTransition();
    if (!activeViewTransition)
        return { };

    ASSERT(!name.isNull());

    auto* capturedElement = activeViewTransition->namedElements().find(name);
    if (!capturedElement)
        return { };

    return capturedElement->classList;
}

inline bool ElementRuleCollector::ruleMatches(const RuleData& ruleData, unsigned& specificity, ScopeOrdinal styleScopeOrdinal, const ScopingRootWithDistance* scopingRoot)
{
    // We know a sufficiently simple single part selector matches simply because we found it from the rule hash when filtering the RuleSet.
    // This is limited to HTML only so we don't need to check the namespace (because of tag name match).
    auto matchBasedOnRuleHash = ruleData.matchBasedOnRuleHash();
    if (matchBasedOnRuleHash != MatchBasedOnRuleHash::None && element().isHTMLElement()) {
        ASSERT_WITH_MESSAGE(!m_pseudoElementRequest, "If we match based on the rule hash while collecting for a particular pseudo element ID, we would add incorrect rules for that pseudo element ID. We should never end in ruleMatches() with a pseudo element if the ruleData cannot match any pseudo element.");

        switch (matchBasedOnRuleHash) {
        case MatchBasedOnRuleHash::None:
            ASSERT_NOT_REACHED();
            break;
        case MatchBasedOnRuleHash::Universal:
            specificity = 0;
            break;
        case MatchBasedOnRuleHash::ClassA:
            specificity = static_cast<unsigned>(SelectorSpecificityIncrement::ClassA);
            break;
        case MatchBasedOnRuleHash::ClassB:
            specificity = static_cast<unsigned>(SelectorSpecificityIncrement::ClassB);
            break;
        case MatchBasedOnRuleHash::ClassC:
            specificity = static_cast<unsigned>(SelectorSpecificityIncrement::ClassC);
            break;
        }
        return true;
    }

#if ENABLE(CSS_SELECTOR_JIT)
    auto& compiledSelector = ruleData.compiledSelector();
    const bool compilerEnabled = element().document().settings().cssSelectorJITCompilerEnabled();

    if (compilerEnabled) {
        if (compiledSelector.status == SelectorCompilationStatus::NotCompiled)
            SelectorCompiler::compileSelector(compiledSelector, ruleData.selector(), SelectorCompiler::SelectorContext::RuleCollector);

        if (compiledSelector.status == SelectorCompilationStatus::SimpleSelectorChecker) {
            compiledSelector.wasUsed();

#if !ASSERT_MSG_DISABLED
            unsigned ignoreSpecificity;
            ASSERT_WITH_MESSAGE(!SelectorCompiler::ruleCollectorSimpleSelectorChecker(compiledSelector, &element(), &ignoreSpecificity) || !m_pseudoElementRequest, "When matching pseudo elements, we should never compile a selector checker without context unless it cannot match anything.");
#endif
            bool selectorMatches = SelectorCompiler::ruleCollectorSimpleSelectorChecker(compiledSelector, &element(), &specificity);

            return selectorMatches;
        }
    }
#endif // ENABLE(CSS_SELECTOR_JIT)

    SelectorChecker::CheckingContext context(m_mode);
    if (m_pseudoElementRequest) {
        auto pseudoElementIdentifier = m_pseudoElementRequest->identifier();
        context.setRequestedPseudoElement(pseudoElementIdentifier);
        context.scrollbarState = m_pseudoElementRequest->scrollbarState();
        if (isNamedViewTransitionPseudoElement(pseudoElementIdentifier))
            context.classList = classListForNamedViewTransitionPseudoElement(element().document(), pseudoElementIdentifier.nameOrPart);
    }
    context.styleScopeOrdinal = styleScopeOrdinal;
    context.selectorMatchingState = m_selectorMatchingState;
    if (scopingRoot) {
        context.scope = scopingRoot->scopingRoot;
        context.scopingRootMatchesVisited = scopingRoot->matchesVisited;
    }

    bool selectorMatches;
#if ENABLE(CSS_SELECTOR_JIT)
    if (compilerEnabled && compiledSelector.status == SelectorCompilationStatus::SelectorCheckerWithCheckingContext) {
        compiledSelector.wasUsed();
        selectorMatches = SelectorCompiler::ruleCollectorSelectorCheckerWithCheckingContext(compiledSelector, &element(), &context, &specificity);
    } else
#endif // ENABLE(CSS_SELECTOR_JIT)
    {
        auto& selector = ruleData.selector();
        // Slow path.
        SelectorChecker selectorChecker(element().document());
        selectorMatches = selectorChecker.match(selector, element(), context);
        if (selectorMatches)
            specificity = selector.computeSpecificity();
    }

    m_matchedPseudoElements.add(context.publicPseudoElements);
    m_styleRelations.appendVector(context.styleRelations);

    return selectorMatches;
}

void ElementRuleCollector::collectMatchingRulesForListSlow(const RuleSet::RuleDataVector& rules, const MatchRequest& matchRequest)
{
    for (auto& ruleData : rules) {
        if (!ruleData.isEnabled()) [[unlikely]]
            continue;

        if (!ruleData.canMatchPseudoElement() && m_pseudoElementRequest)
            continue;

        if (m_selectorMatchingState && m_selectorMatchingState->selectorFilter.fastRejectSelector(ruleData.descendantSelectorIdentifierHashes()))
            continue;

        if (matchRequest.ruleSet.hasContainerQueries() && !containerQueriesMatch(ruleData, matchRequest))
            continue;

        std::optional<Vector<ScopingRootWithDistance>> scopingRoots;
        if (matchRequest.ruleSet.hasScopeRules()) {
            auto [result, roots] = scopeRulesMatch(ruleData, matchRequest);
            if (!result)
                continue;
            scopingRoots = WTF::move(roots);
        }

        auto& rule = ruleData.styleRule();

        // If the rule has no properties to apply, then ignore it in the non-debug mode.
        if (rule.properties().isEmpty() && !m_shouldIncludeEmptyRules)
            continue;

        auto addRuleIfMatches = [&] (const ScopingRootWithDistance* scopingRootWithDistance = nullptr) {
            unsigned specificity;
            auto distance = scopingRootWithDistance ? scopingRootWithDistance->distance : std::numeric_limits<unsigned>::max();
            if (ruleMatches(ruleData, specificity, matchRequest.styleScopeOrdinal, scopingRootWithDistance))
                addMatchedRule(ruleData, specificity, distance, matchRequest);
        };

        if (scopingRoots) {
            for (auto& scopingRoot : *scopingRoots) {
                addRuleIfMatches(&scopingRoot);
                if (isFirstMatchModeAndHasMatchedAnyRules())
                    return;
            }
            continue;
        }

        addRuleIfMatches();
        if (isFirstMatchModeAndHasMatchedAnyRules())
            return;
    }
}

bool ElementRuleCollector::containerQueriesMatch(const RuleData& ruleData, const MatchRequest& matchRequest)
{
    auto queries = matchRequest.ruleSet.containerQueriesFor(ruleData);

    if (queries.isEmpty())
        return true;

    // Style bits indicating which pseudo-elements match are set during regular element matching. Container queries need to be evaluate in the right mode.
    auto selectionMode = [&] {
        if (matchRequest.matchingPartPseudoElementRules)
            return ContainerQueryEvaluator::SelectionMode::PartPseudoElement;
        if (ruleData.canMatchPseudoElement() && !complexSelectorMatchesElementBackedPseudoElement(ruleData.selector()))
            return ContainerQueryEvaluator::SelectionMode::PseudoElement;
        return ContainerQueryEvaluator::SelectionMode::Element;
    }();

    auto* containerQueryEvaluationState = m_selectorMatchingState ? &m_selectorMatchingState->containerQueryEvaluationState : nullptr;

    // "Style rules defined on an element inside multiple nested container queries apply when all of the wrapping container queries are true for that element."
    ContainerQueryEvaluator evaluator(element(), selectionMode, matchRequest.styleScopeOrdinal, containerQueryEvaluationState);
    for (auto& containerRule : queries) {
        if (!evaluator.evaluate(containerRule->containerQuery()))
            return false;
    }
    return true;
}

std::pair<bool, std::optional<Vector<ElementRuleCollector::ScopingRootWithDistance>>> ElementRuleCollector::scopeRulesMatch(const RuleData& ruleData, const MatchRequest& matchRequest)
{
    auto scopeRules = matchRequest.ruleSet.scopeRulesFor(ruleData);

    if (scopeRules.isEmpty())
        return { true, { } };

    SelectorChecker checker(element().rootElement()->document());
    SelectorChecker::CheckingContext context(m_mode);
    context.styleScopeOrdinal = matchRequest.styleScopeOrdinal;

    Vector<ScopingRootWithDistance> scopingRoots;
    auto isWithinScope = [&](auto& rule) {
        auto previousScopingRoots = WTF::move(scopingRoots);
        // The last rule (=innermost @scope rule) determines the scoping roots
        scopingRoots.clear();

        auto findScopingRoots = [&](const auto& selectorList) {
            unsigned distance = 0;
            RefPtr ancestor = &element();
            bool shadowHostCrossed = false;
            // Scope root traversal normally stops at shadow boundaries so document-scoped rules don't
            // leak into shadow trees. However, slotted elements live in the light DOM, so the traversal
            // must cross the shadow host to find scope roots in the containing document.
            bool stopsAtShadowBoundary = element().containingShadowRoot() || context.styleScopeOrdinal == Style::ScopeOrdinal::Shadow;
            while (ancestor && !shadowHostCrossed) {
                auto subContext = context;
                if (stopsAtShadowBoundary) {
                    if (auto* shadowRoot = ancestor->shadowRoot(); shadowRoot && shadowRoot->mode() != ShadowRootMode::UserAgent) {
                        subContext.styleScopeOrdinal = Style::ScopeOrdinal::Shadow;
                        shadowHostCrossed = true;
                    }
                }
                for (const auto& selector : selectorList) {
                    auto appendIfMatch = [&] (std::optional<ScopingRootWithDistance> previousScopingRoot = { }) {
                        if (previousScopingRoot)
                            subContext.scope = previousScopingRoot->scopingRoot;
                        // Reset visited flag for each scoping root evaluation
                        subContext.scopingRootMatchesVisited = false;
                        subContext.isEvaluatingScopingRoot = true;
                        auto match = checker.match(selector, *ancestor, subContext);
                        if (match)
                            scopingRoots.append({ ancestor, distance, subContext.scopingRootMatchesVisited });
                    };
                    if (previousScopingRoots.isEmpty())
                        appendIfMatch();
                    else {
                        for (const auto& previousScopingRoot : previousScopingRoots)
                            appendIfMatch(previousScopingRoot);
                    }
                }

                ++distance;
                ancestor = ancestor->parentOrShadowHostElement();
            }
        };
        /* 
        An element is in scope if:
            It is an inclusive descendant of the scoping root, and
            It is not an inclusive descendant of a scoping limit.
        */
        auto isWithinScopingRootsAndScopeEnd = [&](const CSSSelectorList& scopeEnd) {
            auto match = [&] (const auto* scopingRoot, const auto& selector) {
                auto subContext = context;
                subContext.scope = scopingRoot;
                const auto* ancestor = &element();
                while (ancestor) {
                    auto match = checker.match(selector, *ancestor, subContext);
                    if (match)
                        return true;
                    if (ancestor == scopingRoot) {
                        // The end of the scope can't be an ancestor of the start of the scope.
                        return false;
                    }
                    ancestor = ancestor->parentOrShadowHostElement();
                }
                return false;
            };

            Vector<ScopingRootWithDistance> scopingRootsWithinScope;
            for (auto& scopingRootWithDistance : scopingRoots) {
                bool anyScopingLimitMatch = false;
                for (const auto& selector : scopeEnd) {
                    if (match(scopingRootWithDistance.scopingRoot.get(), selector)) {
                        anyScopingLimitMatch = true;
                        break;
                    }
                }
                if (!anyScopingLimitMatch)
                    scopingRootsWithinScope.append(scopingRootWithDistance);
            }
            return scopingRootsWithinScope;
        };

        const auto& scopeStart = rule->scopeStart();
        if (!scopeStart.isEmpty()) {
            findScopingRoots(scopeStart);
            if (scopingRoots.isEmpty())
                return false;
        } else {
            // The scoping root is the parent element of the owner node of the stylesheet where the @scope rule is defined. (If no such element exists, then the scoping root is the root of the containing node tree.
            RefPtr styleSheetContents = rule->styleSheetContents().get();
            if (!styleSheetContents)
                return false;

            styleSheetContents = styleSheetContents->rootStyleSheet();
            ASSERT(styleSheetContents);
            if (!styleSheetContents)
                return false;

            auto appendImplicitScopingRoot = [&](const auto* client) {

                auto addScopingRootWithDistance = [&](auto* scopingRoot) {
                    const auto* ancestor = &element();
                    unsigned distance = 0;
                    while (ancestor) {
                        if (ancestor == scopingRoot)
                            break;
                        ancestor = ancestor->parentElement();
                        ++distance;
                    }
                    scopingRoots.append({ scopingRoot, distance });
                };

                const auto* owner = client->ownerNode();
                if (owner) {
                    // Regular stylesheet with owner node (e.g., <style> element).
                    // Find the parent node of the <style>.
                    const auto* implicitParentNode = owner->parentOrShadowHostElement();
                    if (!implicitParentNode)
                        return;
                    addScopingRootWithDistance(implicitParentNode);
                } else {
                    // Constructed stylesheet without owner node.
                    // Check if it's adopted by shadow roots and use the shadow host as scoping root.
                    for (const auto& adoptingTreeScope : client->adoptingTreeScopes()) {
                        if (auto* shadowRoot = dynamicDowncast<ShadowRoot>(adoptingTreeScope)) {
                            const auto* shadowHost = shadowRoot->host();
                            if (shadowHost && &shadowHost->document() == &this->element().document())
                                addScopingRootWithDistance(shadowHost);
                        }
                    }
                }
            };

            // Each client might act as a scoping root.
            for (const auto& client : styleSheetContents->clients())
                appendImplicitScopingRoot(client);
        }

        const auto& scopeEnd = rule->scopeEnd();
        if (!scopeEnd.isEmpty()) {
            auto scopingRootsWithinScope = isWithinScopingRootsAndScopeEnd(scopeEnd);
            if (scopingRootsWithinScope.isEmpty())
                return false;
            scopingRoots = WTF::move(scopingRootsWithinScope);
        }
        // element is in the @scope donut
        return true;
    };

    // We need to respect each nested @scope to collect this rule
    for (auto& rule : scopeRules) {
        if (!isWithinScope(rule))
            return { false, { } };
    }

    return { true, WTF::move(scopingRoots) };
}

static inline bool NODELETE compareRules(MatchedRule r1, MatchedRule r2)
{
    // For normal properties the earlier scope wins. This may be reversed by !important which is handled when resolving cascade.
    if (r1.styleScopeOrdinal != r2.styleScopeOrdinal)
        return r1.styleScopeOrdinal > r2.styleScopeOrdinal;

    if (r1.cascadeLayerPriority != r2.cascadeLayerPriority)
        return r1.cascadeLayerPriority < r2.cascadeLayerPriority;

    if (r1.specificity != r2.specificity)
        return r1.specificity < r2.specificity;

    // Rule with the smallest distance has priority.
    if (r1.scopingRootDistance != r2.scopingRootDistance)
        return r2.scopingRootDistance < r1.scopingRootDistance;

    return r1.ruleData->position() < r2.ruleData->position();
}

void ElementRuleCollector::sortMatchedRules()
{
    std::ranges::sort(m_matchedRules, compareRules);
}

static bool snapUARuleMemoEnabled()
{
    static bool enabled = [] {
        const char* value = std::getenv("WEBKIT_SNAP_DISABLE_UA_RULE_MEMO");
        return !value || std::strcmp(value, "1");
    }();
    return enabled;
}

static bool snapUARuleMemoCountersEnabled()
{
    static bool enabled = [] {
        const char* value = std::getenv("WEBKIT_SNAP_PATCH_COUNTERS");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

enum class SnapUARuleMemoEvent : uint8_t { Hit,
    Miss,
    NonMemoizable };

static void snapUARuleMemoCount(SnapUARuleMemoEvent event)
{
    if (!snapUARuleMemoCountersEnabled()) {
        return;
    }
    static std::array<unsigned long long, 3> counts {};
    static unsigned long long total = 0;
    ++counts[static_cast<size_t>(event)];
    if (++total % 4096) {
        return;
    }
    WTFLogAlways("Snap UA rule memo: hits %llu misses %llu non-memoizable %llu", counts[0], counts[1], counts[2]);
}

static constexpr uint8_t snapUAShapeHTMLDocument = 1u << 0;
static constexpr uint8_t snapUAShapeQuirksMode = 1u << 1;
static constexpr uint8_t snapUAShapeLink = 1u << 2;
static constexpr uint8_t snapUAShapeUserAgentShadowTree = 1u << 3;

static constexpr size_t snapUAMemoMaximumAttributes = 4;
static constexpr size_t snapUAMemoMaximumVariants = 16;
static constexpr unsigned snapUAMemoMaximumShapes = 512;

static bool snapUARuleSelectorIsStatic(const CSSSelector& firstSelector, bool& valuesMatter)
{
    for (auto* selector = &firstSelector; selector; selector = selector->precedingInComplexSelector()) {
        if (selector->precedingInComplexSelector() && selector->relation() != CSSSelector::Relation::Subselector) {
            return false;
        }

        switch (selector->match()) {
        case CSSSelector::Match::Tag:
        case CSSSelector::Match::Set:
            continue;
        case CSSSelector::Match::Exact:
        case CSSSelector::Match::List:
        case CSSSelector::Match::Hyphen:
        case CSSSelector::Match::Contain:
        case CSSSelector::Match::Begin:
        case CSSSelector::Match::End:
            valuesMatter = true;
            continue;
        default:
            return false;
        }
    }
    return true;
}

struct SnapUARuleMemoVariant {
    Vector<AtomString, snapUAMemoMaximumAttributes> values;
    Vector<MatchedRule, 4> staticMatches;
};

struct SnapUARuleMemoShape {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(SnapUARuleMemoShape);

    AtomString localName;
    AtomString namespaceURI;
    AtomString userAgentPart;
    Vector<QualifiedName, snapUAMemoMaximumAttributes> attributeNames;
    uint8_t flags { 0 };
    bool broken { false };
    bool valuesMatter { false };
    Vector<const RuleData*> staticRules;
    Vector<const RuleData*> dynamicRules;
    Vector<SnapUARuleMemoVariant> variants;
};

struct SnapUARuleMemo {
    unsigned version { 0 };
    bool initialized { false };
    bool usable { false };
    unsigned shapeCount { 0 };
    HashMap<unsigned, Vector<std::unique_ptr<SnapUARuleMemoShape>>, AlreadyHashed> shapes;
};

class SnapUARuleMemoStore final : public Supplement<Document> {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(SnapUARuleMemoStore);

public:
    static SnapUARuleMemoStore& from(Document& document)
    {
        constexpr auto name = "SnapUARuleMemoStore"_s;
        auto* store = static_cast<SnapUARuleMemoStore*>(Supplement<Document>::from(&document, name));
        if (!store) {
            auto owned = makeUnique<SnapUARuleMemoStore>();
            store = owned.get();
            Supplement<Document>::provideTo(&document, name, WTF::move(owned));
        }
        return *store;
    }
    std::array<SnapUARuleMemo, 3> memos;
};

static SnapUARuleMemo* snapUARuleMemoFor(Document& document, const RuleSet& rules)
{
    size_t index;
    if (&rules == UserAgentStyle::defaultStyle) {
        index = 0;
    } else if (&rules == UserAgentStyle::defaultPrintStyle) {
        index = 1;
    } else if (&rules == UserAgentStyle::defaultQuirksStyle) {
        index = 2;
    } else {
        return nullptr;
    }

    auto& memo = SnapUARuleMemoStore::from(document).memos[index];
    if (!memo.initialized || memo.version != UserAgentStyle::defaultStyleVersion) {
        memo.initialized = true;
        memo.version = UserAgentStyle::defaultStyleVersion;
        memo.shapes.clear();
        memo.shapeCount = 0;

        auto& features = rules.features();
        auto& styleAttributeName = HTMLNames::styleAttr->localName();
        bool referencesStyleAttribute = features.attributeLocalNamesInRules.contains(styleAttributeName) || features.attributeLowercaseLocalNamesInRules.contains(styleAttributeName);
        memo.usable = !referencesStyleAttribute && !rules.hasContainerQueries() && !rules.hasScopeRules() && features.idsInRules.isEmpty() && features.idRules.isEmpty() && features.classRules.isEmpty();
    }
    return memo.usable ? &memo : nullptr;
}

static void snapUARuleMemoBuildShape(SnapUARuleMemoShape& shape, const RuleSet& rules, bool isCaseInsensitiveForHTML)
{
    auto visit = [&](const RuleSet::RuleDataVector* bucket) {
        if (!bucket) {
            return;
        }
        for (auto& ruleData : *bucket) {
            if (!ruleData.isEnabled()) {
                shape.broken = true;
                continue;
            }
            if (ruleData.styleRule().properties().isEmpty()) {
                continue;
            }

            bool valuesMatter = false;
            bool isStatic = !ruleData.canMatchPseudoElement() && !ruleData.descendantSelectorIdentifierHashes()[0] && snapUARuleSelectorIsStatic(ruleData.selector(), valuesMatter);
            if (isStatic) {
                shape.staticRules.append(&ruleData);
                shape.valuesMatter = shape.valuesMatter || valuesMatter;
            } else {
                shape.dynamicRules.append(&ruleData);
            }
        }
    };

    if ((shape.flags & snapUAShapeUserAgentShadowTree) && !shape.userAgentPart.isEmpty()) {
        visit(rules.userAgentPartRules(shape.userAgentPart));
    }
    for (auto& name : shape.attributeNames) {
        visit(rules.attributeRules(name.localName(), isCaseInsensitiveForHTML));
    }
    if (shape.flags & snapUAShapeLink) {
        visit(&rules.linkPseudoClassRules());
    }
    visit(rules.tagRules(shape.localName, isCaseInsensitiveForHTML));
    visit(&rules.universalRules());
}

void ElementRuleCollector::snapCollectDynamicUARule(const RuleData& ruleData, const MatchRequest& matchRequest)
{
    if (!ruleData.isEnabled()) [[unlikely]] {
        return;
    }
    if (m_selectorMatchingState && m_selectorMatchingState->selectorFilter.fastRejectSelector(ruleData.descendantSelectorIdentifierHashes())) {
        return;
    }
    unsigned specificity;
    if (ruleMatches(ruleData, specificity, matchRequest.styleScopeOrdinal)) {
        addMatchedRule(ruleData, specificity, std::numeric_limits<unsigned>::max(), matchRequest);
    }
}

#ifdef WEBKIT_SNAP_VERIFY_PATCHES
void ElementRuleCollector::snapVerifyUARuleMemoHit(
    const RuleSet& rules,
    size_t relationCountBefore,
    EnumSet<PseudoElementType> pseudoElementsBefore)
{
    auto memoRules = m_matchedRules;
    Relations memoRelations;
    for (size_t i = relationCountBefore; i < m_styleRelations.size(); ++i) {
        memoRelations.append(m_styleRelations[i]);
    }
    auto memoPseudoElements = m_matchedPseudoElements;

    m_matchedRules.clear();
    m_styleRelations.shrink(relationCountBefore);
    m_matchedPseudoElements = pseudoElementsBefore;

    collectMatchingRules(MatchRequest(rules));

    auto stockRules = m_matchedRules;
    std::ranges::sort(memoRules, compareRules);
    std::ranges::sort(stockRules, compareRules);

    bool equal = memoRules.size() == stockRules.size() && memoPseudoElements == m_matchedPseudoElements && memoRelations.size() == m_styleRelations.size() - relationCountBefore;
    for (size_t i = 0; equal && i < memoRules.size(); ++i) {
        auto& memoRule = memoRules[i];
        auto& stockRule = stockRules[i];
        equal = memoRule.ruleData == stockRule.ruleData && memoRule.specificity == stockRule.specificity && memoRule.scopingRootDistance == stockRule.scopingRootDistance && memoRule.styleScopeOrdinal == stockRule.styleScopeOrdinal && memoRule.cascadeLayerPriority == stockRule.cascadeLayerPriority;
    }
    for (size_t i = 0; equal && i < memoRelations.size(); ++i) {
        auto& memoRelation = memoRelations[i];
        auto& stockRelation = m_styleRelations[relationCountBefore + i];
        equal = memoRelation.element == stockRelation.element && memoRelation.type == stockRelation.type && memoRelation.value == stockRelation.value;
    }
    if (!equal) {
        WTFLogAlways(
            "Snap UA rule memo: mismatch for <%s>, memo rules %zu stock rules %zu, memo relations %zu stock relations "
            "%zu, using stock result",
            element().localName().string().utf8().data(),
            memoRules.size(),
            stockRules.size(),
            memoRelations.size(),
            m_styleRelations.size() - relationCountBefore);
        RELEASE_ASSERT_NOT_REACHED();
    }
}
#endif

bool ElementRuleCollector::snapCollectUARulesWithMemo(const RuleSet& rules)
{
    if (!snapUARuleMemoEnabled()) {
        return false;
    }
    if (!isMainThread()) {
        return false;
    }

    auto nonMemoizable = [] {
        snapUARuleMemoCount(SnapUARuleMemoEvent::NonMemoizable);
        return false;
    };

    if (m_mode != SelectorChecker::Mode::ResolvingStyle || m_pseudoElementRequest || m_shouldIncludeEmptyRules) {
        return nonMemoizable();
    }

    auto& element = this->element();
    if (!element.isHTMLElement() || element.localName().length() > 128 || element.namespaceURI().length() > 128) {
        return nonMemoizable();
    }

    auto& document = element.document();
    if (&element == document.documentElement()) {
        return nonMemoizable();
    }
    if (matchesFocusPseudoClass(element) || matchesFocusVisiblePseudoClass(element)) {
        return nonMemoizable();
    }
#if ENABLE(FULLSCREEN_API)
    if (auto* fullscreen = document.fullscreenIfExists(); fullscreen && fullscreen->isFullscreen()) {
        return nonMemoizable();
    }
#endif

    auto* memo = snapUARuleMemoFor(document, rules);
    if (!memo) {
        return nonMemoizable();
    }

    uint8_t flags = 0;
    AtomString userAgentPart;
    if (auto* shadowRoot = element.containingShadowRoot();
        shadowRoot && shadowRoot->mode() == ShadowRootMode::UserAgent) {
#if ENABLE(VIDEO)
        if (element.isWebVTTElement() || element.userAgentPart() == UserAgentParts::cue()) {
            return nonMemoizable();
        }
#endif
        flags |= snapUAShapeUserAgentShadowTree;
        userAgentPart = element.userAgentPart();
        if (userAgentPart.length() > 128) {
            return nonMemoizable();
        }
    }

    bool isCaseInsensitiveForHTML = document.isHTMLDocument();
    if (isCaseInsensitiveForHTML) {
        flags |= snapUAShapeHTMLDocument;
    }
    if (document.inQuirksMode()) {
        flags |= snapUAShapeQuirksMode;
    }
    if (element.isLink()) {
        flags |= snapUAShapeLink;
    }

    Vector<QualifiedName, snapUAMemoMaximumAttributes> attributeNames;
    Vector<AtomString, snapUAMemoMaximumAttributes> attributeValues;
    if (element.hasAttributesWithoutUpdate()) {
        auto& features = rules.features();
        auto& names = isCaseInsensitiveForHTML ? features.attributeLowercaseLocalNamesInRules
                                               : features.attributeLocalNamesInRules;
        if (!names.isEmpty()) {
            for (auto& attribute : element.attributes()) {
                if (!names.contains(attribute.localName())) {
                    continue;
                }
                if (attributeNames.size() == snapUAMemoMaximumAttributes || attribute.value().length() > 256 || attribute.namespaceURI().length() > 128 || attribute.prefix().length() > 128) {
                    return nonMemoizable();
                }
                attributeNames.append(attribute.name());
                attributeValues.append(attribute.value());
            }
        }
    }

    auto& localName = element.localName();
    auto& namespaceURI = element.namespaceURI();

    Hasher hasher;
    add(hasher, localName.impl(), namespaceURI.impl(), userAgentPart.impl(), static_cast<unsigned>(flags));
    for (auto& name : attributeNames) {
        add(hasher, name.localName().impl(), name.namespaceURI().impl(), name.prefix().impl());
    }
    unsigned hash = hasher.hash();
    if (!hash) {
        hash = 1;
    }
    hash = AlreadyHashed::avoidDeletedValue(hash);

    auto shapeMatches = [&](const SnapUARuleMemoShape& candidate) {
        if (candidate.flags != flags || candidate.localName != localName || candidate.namespaceURI != namespaceURI || candidate.userAgentPart != userAgentPart) {
            return false;
        }
        if (candidate.attributeNames.size() != attributeNames.size()) {
            return false;
        }
        for (size_t i = 0; i < attributeNames.size(); ++i) {
            if (candidate.attributeNames[i] != attributeNames[i]) {
                return false;
            }
        }
        return true;
    };

    auto findShape = [&]() -> SnapUARuleMemoShape* {
        auto it = memo->shapes.find(hash);
        if (it == memo->shapes.end()) {
            return nullptr;
        }
        for (auto& candidate : it->value) {
            if (shapeMatches(*candidate)) {
                return candidate.get();
            }
        }
        return nullptr;
    };

    auto findVariant = [&](SnapUARuleMemoShape& shape) -> SnapUARuleMemoVariant* {
        for (auto& candidate : shape.variants) {
            if (shape.valuesMatter && candidate.values != attributeValues) {
                continue;
            }
            return &candidate;
        }
        return nullptr;
    };

    auto* shape = findShape();
    if (shape && !shape->broken) {
        if (auto* variant = findVariant(*shape)) {
#ifdef WEBKIT_SNAP_VERIFY_PATCHES
            auto relationCountBefore = m_styleRelations.size();
            auto pseudoElementsBefore = m_matchedPseudoElements;
#endif
            for (auto& matchedRule : variant->staticMatches) {
                if (matchedRule.ruleData->isEnabled()) [[likely]] {
                    m_matchedRules.append(matchedRule);
                }
            }
            if (!shape->dynamicRules.isEmpty()) {
                MatchRequest matchRequest(rules);
                for (auto* ruleData : shape->dynamicRules) {
                    snapCollectDynamicUARule(*ruleData, matchRequest);
                }
            }
            m_matchedPseudoElements.add(rules.universalHTMLPseudoElementTypes() & allPublicPseudoElementTypes);
            snapUARuleMemoCount(SnapUARuleMemoEvent::Hit);
#ifdef WEBKIT_SNAP_VERIFY_PATCHES
            snapVerifyUARuleMemoHit(rules, relationCountBefore, pseudoElementsBefore);
#endif
            return true;
        }
    }

    collectMatchingRules(MatchRequest(rules));

    if (!shape) {
        if (memo->shapeCount >= snapUAMemoMaximumShapes) {
            memo->shapes.clear();
            memo->shapeCount = 0;
        }
        auto newShape = makeUnique<SnapUARuleMemoShape>();
        newShape->localName = localName;
        newShape->namespaceURI = namespaceURI;
        newShape->userAgentPart = userAgentPart;
        newShape->flags = flags;
        newShape->attributeNames = attributeNames;
        snapUARuleMemoBuildShape(*newShape, rules, isCaseInsensitiveForHTML);
        shape = newShape.get();
        memo->shapes.add(hash, Vector<std::unique_ptr<SnapUARuleMemoShape>> {})
            .iterator->value.append(WTF::move(newShape));
        ++memo->shapeCount;
    }

    if (shape->broken) {
        snapUARuleMemoCount(SnapUARuleMemoEvent::NonMemoizable);
        return true;
    }

    snapUARuleMemoCount(SnapUARuleMemoEvent::Miss);

    if (shape->variants.size() >= snapUAMemoMaximumVariants) {
        return true;
    }

    SnapUARuleMemoVariant variant;
    if (findVariant(*shape)) {
        return true;
    }

    for (auto& matchedRule : m_matchedRules) {
        if (shape->staticRules.contains(matchedRule.ruleData)) {
            variant.staticMatches.append(matchedRule);
            continue;
        }
        if (!shape->dynamicRules.contains(matchedRule.ruleData)) {
            shape->broken = true;
            shape->variants.clear();
            return true;
        }
    }

    if (shape->valuesMatter) {
        variant.values = attributeValues;
    }
    shape->variants.append(WTF::move(variant));
    return true;
}

void ElementRuleCollector::matchAllRules(bool matchAuthorAndUserStyles, bool includeSMILProperties)
{
    matchUARules();

    // Now we check user sheet rules.
    if (matchAuthorAndUserStyles)
        matchUserRules();

    if (auto* styledElement = dynamicDowncast<StyledElement>(element())) {
        if (auto* presentationalHintStyle = styledElement->presentationalHintStyle()) {
            // https://html.spec.whatwg.org/#presentational-hints

            // Presentation attributes in SVG elements tend to be unique and not restyled often. Avoid bloating the cache.
            // FIXME: Refcount is an imperfect proxy for sharing within a single document.
            static constexpr auto matchedDeclarationsCacheSharingThreshold = 4;
            bool allowFullCaching = !styledElement->isSVGElement() || presentationalHintStyle->refCount() > matchedDeclarationsCacheSharingThreshold;

            auto isCacheable = allowFullCaching ? IsCacheable::Yes : IsCacheable::Partially;
            addElementStyleProperties(presentationalHintStyle, RuleSet::cascadeLayerPriorityForPresentationalHints, isCacheable);
        }

        // Tables and table cells share an additional presentation style that must be applied
        // after all attributes, since their style depends on the values of multiple attributes.
        addElementStyleProperties(styledElement->additionalPresentationalHintStyle(), RuleSet::cascadeLayerPriorityForPresentationalHints);

        if (auto* htmlElement = dynamicDowncast<HTMLElement>(*styledElement)) {
            if (auto textDirection = computeTextDirectionIfDirIsAuto(*htmlElement)) {
                auto& properties = *textDirection == TextDirection::LTR ? leftToRightDeclaration() : rightToLeftDeclaration();
                addMatchedProperties({ properties }, DeclarationOrigin::Author);
            }
        }
    }

    if (matchAuthorAndUserStyles) {
        clearMatchedRules();

        collectMatchingRules(DeclarationOrigin::Author);
        sortMatchedRules();

        transferMatchedRules(DeclarationOrigin::Author, ScopeOrdinal::Element);

        // Inline style behaves as if it has higher specificity than any rule.
        addElementInlineStyleProperties(includeSMILProperties);

        // Rules from the host scopes override inline style.
        transferMatchedRules(DeclarationOrigin::Author);
    }
}

void ElementRuleCollector::addElementInlineStyleProperties(bool includeSMILProperties)
{
    auto* styledElement = dynamicDowncast<StyledElement>(element());
    if (!styledElement)
        return;

    if (auto* inlineStyle = styledElement->inlineStyle()) {
        auto isInlineStyleCacheable = inlineStyle->isMutable() ? IsCacheable::No : IsCacheable::Yes;
        addElementStyleProperties(inlineStyle, RuleSet::cascadeLayerPriorityForUnlayered, isInlineStyleCacheable, FromStyleAttribute::Yes);
    }

    if (includeSMILProperties) {
        if (auto* svgElement = dynamicDowncast<SVGElement>(element()))
            addElementStyleProperties(svgElement->animatedSMILStyleProperties(), RuleSet::cascadeLayerPriorityForUnlayered, IsCacheable::No);
    }
}

void ElementRuleCollector::addMatchedProperties(MatchedProperties&& matchedProperties, DeclarationOrigin declarationOrigin)
{
    auto& declarations = declarationsForOrigin(declarationOrigin);
    if (!declarations.isEmpty() && declarations.last() == matchedProperties) {
        // It might also be beneficial to overwrite the previous declaration (insteading of appending) if it affects the same exact properties.
        return;
    }
    if (matchedProperties.isStartingStyle == IsStartingStyle::Yes)
        m_result->hasStartingStyle = true;

    if (matchedProperties.isCacheable == IsCacheable::Partially && !m_result->isCompletelyNonCacheable) {
        for (auto property : matchedProperties.properties.get())
            m_result->nonCacheablePropertyIds.append(property.id());
    }
    if (matchedProperties.isCacheable == IsCacheable::No) {
        m_result->isCompletelyNonCacheable = true;
        m_result->nonCacheablePropertyIds.clear();
    }

    declarations.append(WTF::move(matchedProperties));
}

void ElementRuleCollector::addAuthorKeyframeRules(const StyleRuleKeyframe& keyframe)
{
    ASSERT(m_result->authorDeclarations.isEmpty());
    auto propertyAllowlist = m_pseudoElementRequest ? propertyAllowlistForPseudoElement(m_pseudoElementRequest->type()) : PropertyAllowlist::None;
    m_result->authorDeclarations.append({ keyframe.properties(), SelectorChecker::MatchAll, propertyAllowlist });
}

}
} // namespace WebCore
