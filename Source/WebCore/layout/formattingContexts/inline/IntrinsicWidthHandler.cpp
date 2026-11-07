/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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
#include "IntrinsicWidthHandler.h"

#include "InlineFormattingContext.h"
#include "InlineLineBuilder.h"
#include "LayoutElementBox.h"
#include "StyleComputedStyle+GettersInlines.h"
#include "StyleComputedStyle+InitialInlines.h"
#include "TextOnlySimpleLineBuilder.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <wtf/Logging.h>
#include <wtf/MonotonicTime.h>
#include <wtf/unicode/CharacterNames.h>

namespace WebCore {
namespace Layout {

static bool snapIntrinsicWidthProbeEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("WEBKIT_SNAP_PATCH_COUNTERS");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

static bool snapIntrinsicWidthProbeReserveEvent(uint64_t& event)
{
    static std::atomic<uint64_t> nextEvent { 0 };
    static constexpr uint64_t eventLimit = 262144;
    auto next = nextEvent.fetch_add(1, std::memory_order_relaxed);
    if (next >= eventLimit) {
        if (next == eventLimit)
            WTFLogAlways("Snap intrinsic width probe: truncated=1 event_limit=%llu", static_cast<unsigned long long>(eventLimit));
        return false;
    }
    event = next + 1;
    return true;
}

static void snapIntrinsicWidthProbeLog(uint64_t event, const char* path, bool candidate, bool eligible, unsigned lines, uint64_t start, uint64_t duration, uint64_t eligibilityDuration)
{
    WTFLogAlways("Snap intrinsic width probe: event=%llu path=%s candidate=%u eligible=%u lines=%u start_ns=%llu duration_ns=%llu eligibility_ns=%llu",
        static_cast<unsigned long long>(event), path, static_cast<unsigned>(candidate), static_cast<unsigned>(eligible), lines,
        static_cast<unsigned long long>(start), static_cast<unsigned long long>(duration),
        static_cast<unsigned long long>(eligibilityDuration));
}

static bool NODELETE isBoxEligibleForNonLineBuilderMinimumWidth(const ElementBox& box)
{
    // Note that hanging trailing content needs line builder (combination of wrapping is allowed but whitespace is preserved).
    auto& style = box.style();
    // 'white-space-trim' discards some inline items, which this character-walking fast path (it iterates the text content directly, not the inline items) can't account for.
    if (!style.whiteSpaceTrim().isNone())
        return false;
    return TextUtil::isWrappingAllowed(style) && (style.lineBreak() == LineBreak::Anywhere || style.wordBreak() == WordBreak::BreakAll || style.wordBreak() == WordBreak::BreakWord) && style.whiteSpaceCollapse() != WhiteSpaceCollapse::Preserve;
}

static bool isContentEligibleForNonLineBuilderMaximumWidth(const ElementBox& rootBox, const InlineItemList& inlineItemList)
{
    if (inlineItemList.size() != 1 || rootBox.style().textIndent() != Style::ComputedStyle::initialTextIndent())
        return false;

    auto* inlineTextItem = dynamicDowncast<InlineTextItem>(inlineItemList[0]);
    return inlineTextItem && !inlineTextItem->isWhitespace();
}

static bool NODELETE isSubtreeEligibleForNonLineBuilderMinimumWidth(const ElementBox& root)
{
    auto isSimpleBreakableContent = isBoxEligibleForNonLineBuilderMinimumWidth(root);
    for (auto* child = root.firstChild(); child && isSimpleBreakableContent; child = child->nextSibling()) {
        if (child->isFloatingPositioned()) {
            isSimpleBreakableContent = false;
            break;
        }
        auto isInlineBoxWithInlineContent = child->isInlineBox() && !child->isInlineTextBox() && !child->isLineBreakBox();
        if (isInlineBoxWithInlineContent)
            isSimpleBreakableContent = isSubtreeEligibleForNonLineBuilderMinimumWidth(downcast<ElementBox>(*child));
    }
    return isSimpleBreakableContent;
}

static bool NODELETE isContentEligibleForNonLineBuilderMinimumWidth(const ElementBox& rootBox, const InlineContentCache::InlineItems& inlineItems, bool mayUseSimplifiedTextOnlyInlineLayout)
{
    auto isEligible = (mayUseSimplifiedTextOnlyInlineLayout && isBoxEligibleForNonLineBuilderMinimumWidth(rootBox)) || (!mayUseSimplifiedTextOnlyInlineLayout && isSubtreeEligibleForNonLineBuilderMinimumWidth(rootBox));
    if (!isEligible || !mayUseSimplifiedTextOnlyInlineLayout || !inlineItems.hasInlineBoxes())
        return isEligible;

    // Text only content in range e.g. <span>text content</span> or <span><span>text content</span></span>. Check if enclosing inline box is eligible.
    auto& inlineItemList = inlineItems.content();
    auto inlineBoxIndex = std::optional<size_t> { };
    for (size_t index = 0; index < inlineItemList.size(); ++index) {
        auto& inlineItem = inlineItemList[index];
        if (inlineItem.isInlineBoxStart()) {
            inlineBoxIndex = index;
            continue;
        }

        if (inlineItem.isText() && inlineBoxIndex)
            return isBoxEligibleForNonLineBuilderMinimumWidth(downcast<ElementBox>(inlineItemList[*inlineBoxIndex].layoutBox()));

        // This is unexpected content. We should always find inline box(es) followed by text content.
        break;
    }
    ASSERT_NOT_REACHED();
    return false;
}

static bool NODELETE mayUseContentWidthBetweenLineBreaksAsMaximumSize(const ElementBox& rootBox, const InlineItemList& inlineItemList)
{
    if (!TextUtil::shouldPreserveSpacesAndTabs(rootBox))
        return false;
    for (auto& inlineItem : inlineItemList) {
        if (auto* inlineTextItem = dynamicDowncast<InlineTextItem>(inlineItem); inlineTextItem && !inlineTextItem->width()) {
            // We can't accumulate individual inline items when width depends on position (e.g. tab).
            return false;
        }
    }
    return true;
}

IntrinsicWidthHandler::IntrinsicWidthHandler(InlineFormattingContext& inlineFormattingContext, const InlineContentCache::InlineItems& inlineItems)
    : m_inlineFormattingContext(inlineFormattingContext)
    , m_inlineItems(inlineItems)
{
    auto initializeRangeAndTextOnlyBuilderEligibility = [&] {
        m_inlineItemRange = { 0, inlineItems.content().size() };
        m_mayUseSimplifiedTextOnlyInlineLayoutInRange = TextOnlySimpleLineBuilder::isEligibleForSimplifiedInlineLayoutByStyle(formattingContextRoot());
        if (!m_mayUseSimplifiedTextOnlyInlineLayoutInRange)
            return;

        m_mayUseSimplifiedTextOnlyInlineLayoutInRange = inlineItems.hasTextAndLineBreakOnlyContent() && !inlineItems.requiresVisualReordering();
        if (!m_mayUseSimplifiedTextOnlyInlineLayoutInRange)
            return;
        // Non-bidi text only content maybe nested inside inline boxes e.g. <div>simple text</div>, <div><span>simple text inside inline box</span></div> or
        // <div>some text<span>and some more inside inline box</span></div>
        auto inlineBoxCount = inlineItems.inlineBoxCount();
        if (!inlineBoxCount)
            return;

        auto& inlineItemList = inlineItems.content();
        auto inlineBoxStartAndEndInlineItemsCount = 2 * inlineBoxCount;
        ASSERT(inlineBoxStartAndEndInlineItemsCount <= inlineItemList.size());

        m_mayUseSimplifiedTextOnlyInlineLayoutInRange = inlineBoxStartAndEndInlineItemsCount < inlineItemList.size();
        if (!m_mayUseSimplifiedTextOnlyInlineLayoutInRange)
            return;

        for (size_t index = 0; index < inlineBoxCount; ++index) {
            auto& inlineItem = inlineItemList[index];
            auto isNestingInlineBox = inlineItem.isInlineBoxStart() && inlineItemList[inlineItems.size() - 1 - index].isInlineBoxEnd();
            auto& inlineBoxGeometry = formattingContext().geometryForBox(inlineItem.layoutBox());
            m_mayUseSimplifiedTextOnlyInlineLayoutInRange = isNestingInlineBox && !inlineBoxGeometry.horizontalMarginBorderAndPadding() && inlineBoxGeometry.marginStart() >= 0 && inlineBoxGeometry.marginEnd() >= 0 && TextOnlySimpleLineBuilder::isEligibleForSimplifiedInlineLayoutByStyle(inlineItem.layoutBox());
            if (!m_mayUseSimplifiedTextOnlyInlineLayoutInRange)
                return;
        }
        m_inlineItemRange = { inlineBoxCount, inlineItemList.size() - inlineBoxCount };
    };
    initializeRangeAndTextOnlyBuilderEligibility();
}

InlineLayoutUnit IntrinsicWidthHandler::minimumContentSize()
{
    auto event = uint64_t { 0 };
    if (!snapIntrinsicWidthProbeEnabled() || !snapIntrinsicWidthProbeReserveEvent(event)) {
        if (isContentEligibleForNonLineBuilderMinimumWidth(formattingContextRoot(), m_inlineItems, m_mayUseSimplifiedTextOnlyInlineLayoutInRange))
            return simplifiedMinimumWidth(formattingContextRoot());

        if (m_mayUseSimplifiedTextOnlyInlineLayoutInRange) {
            auto simplifiedLineBuilder = TextOnlySimpleLineBuilder { formattingContext(), lineBuilderRoot(), {}, inlineItemList() };
            return computedIntrinsicWidthForConstraint(IntrinsicWidthMode::Minimum, simplifiedLineBuilder, MayCacheLayoutResult::No);
        }

        auto lineBuilder = LineBuilder { formattingContext(), {}, inlineItemList() };
        return computedIntrinsicWidthForConstraint(IntrinsicWidthMode::Minimum, lineBuilder, MayCacheLayoutResult::No);
    }

    auto candidate = false;
    auto eligible = false;
    auto eligibilityDuration = uint64_t { 0 };
    auto lineCount = 0u;
    auto path = "full";
    auto eligibilityStart = MonotonicTime {};
    auto result = InlineLayoutUnit {};

    if (isContentEligibleForNonLineBuilderMinimumWidth(formattingContextRoot(), m_inlineItems, m_mayUseSimplifiedTextOnlyInlineLayoutInRange)) {
        path = "existing-simplified";
        auto pathStart = MonotonicTime::now();
        result = simplifiedMinimumWidth(formattingContextRoot());
        auto pathDuration = static_cast<uint64_t>((MonotonicTime::now() - pathStart).nanoseconds());
        snapIntrinsicWidthProbeLog(event, path, candidate, eligible, lineCount,
            static_cast<uint64_t>(pathStart.secondsSinceEpoch().nanoseconds()), pathDuration, eligibilityDuration);
        return result;
    } else if (m_mayUseSimplifiedTextOnlyInlineLayoutInRange) {
        path = "text-only";
        candidate = true;
        eligibilityStart = MonotonicTime::now();
        auto proposedGate = [&] {
            if (!m_mayUseSimplifiedTextOnlyInlineLayoutInRange || m_inlineItemRange.isEmpty() || m_inlineItems.hasTextAutospace())
                return false;

            auto& rootBox = lineBuilderRoot();
            auto& rootStyle = rootBox.style();
            if (&rootStyle != &rootBox.firstLineStyle())
                return false;
            if (!TextUtil::isWrappingAllowed(rootStyle))
                return false;
            if (rootStyle.lineBreak() == LineBreak::Anywhere || rootStyle.lineBreak() == LineBreak::AfterWhiteSpace)
                return false;
            if (rootStyle.wordBreak() == WordBreak::BreakAll || rootStyle.wordBreak() == WordBreak::BreakWord)
                return false;
            if (rootStyle.overflowWrap() == OverflowWrap::Anywhere || rootStyle.hyphens() == Hyphens::Auto || rootStyle.usedLetterSpacing() < 0)
                return false;

            auto whiteSpaceCollapse = rootStyle.whiteSpaceCollapse();
            if (whiteSpaceCollapse != WhiteSpaceCollapse::Collapse
                && whiteSpaceCollapse != WhiteSpaceCollapse::PreserveBreaks
                && whiteSpaceCollapse != WhiteSpaceCollapse::Preserve
                && whiteSpaceCollapse != WhiteSpaceCollapse::BreakSpaces)
                return false;

            const InlineTextBox* textBox = nullptr;
            for (auto index = m_inlineItemRange.startIndex(); index < m_inlineItemRange.endIndex(); ++index) {
                auto& inlineItem = inlineItemList()[index];
                if (inlineItem.isLineBreak())
                    continue;
                auto* inlineTextItem = dynamicDowncast<InlineTextItem>(inlineItem);
                if (!inlineTextItem)
                    return false;
                auto width = inlineTextItem->width();
                if (!inlineTextItem->length() || !width || *width <= 0 || !std::isfinite(*width) || inlineTextItem->hasTrailingSoftHyphen()
                    || inlineTextItem->isZeroWidthSpaceSeparator() || inlineTextItem->isQuirkNonBreakingSpace())
                    return false;
                if (textBox && textBox != &inlineTextItem->inlineTextBox())
                    return false;
                textBox = &inlineTextItem->inlineTextBox();
            }
            if (!textBox)
                return false;
            auto& textStyle = textBox->style();
            return &textStyle == &textBox->firstLineStyle()
                && textStyle.whiteSpaceCollapse() == whiteSpaceCollapse
                && textStyle.textWrapMode() == rootStyle.textWrapMode()
                && textStyle.lineBreak() == rootStyle.lineBreak()
                && textStyle.wordBreak() == rootStyle.wordBreak()
                && textStyle.overflowWrap() == rootStyle.overflowWrap()
                && textStyle.hyphens() == rootStyle.hyphens();
        };
        eligible = proposedGate();
        eligibilityDuration = static_cast<uint64_t>((MonotonicTime::now() - eligibilityStart).nanoseconds());
        auto pathStart = MonotonicTime::now();
        auto simplifiedLineBuilder = TextOnlySimpleLineBuilder { formattingContext(), lineBuilderRoot(), {}, inlineItemList() };
        result = computedIntrinsicWidthForConstraint(IntrinsicWidthMode::Minimum, simplifiedLineBuilder, MayCacheLayoutResult::No, &lineCount);
        auto pathDuration = static_cast<uint64_t>((MonotonicTime::now() - pathStart).nanoseconds());
        snapIntrinsicWidthProbeLog(event, path, candidate, eligible, lineCount,
            static_cast<uint64_t>(pathStart.secondsSinceEpoch().nanoseconds()), pathDuration, eligibilityDuration);
        return result;
    } else {
        auto pathStart = MonotonicTime::now();
        auto lineBuilder = LineBuilder { formattingContext(), {}, inlineItemList() };
        result = computedIntrinsicWidthForConstraint(IntrinsicWidthMode::Minimum, lineBuilder, MayCacheLayoutResult::No, &lineCount);
        auto pathDuration = static_cast<uint64_t>((MonotonicTime::now() - pathStart).nanoseconds());
        snapIntrinsicWidthProbeLog(event, path, candidate, eligible, lineCount,
            static_cast<uint64_t>(pathStart.secondsSinceEpoch().nanoseconds()), pathDuration, eligibilityDuration);
        return result;
    }
}

InlineLayoutUnit IntrinsicWidthHandler::maximumContentSize()
{
    auto mayCacheLayoutResult = m_mayUseSimplifiedTextOnlyInlineLayoutInRange && !m_inlineItemRange.startIndex() ? MayCacheLayoutResult::Yes : MayCacheLayoutResult::No;
    auto maximumContentSize = InlineLayoutUnit { };

    if (isContentEligibleForNonLineBuilderMaximumWidth(formattingContextRoot(), inlineItemList()))
        maximumContentSize = simplifiedMaximumWidth(mayCacheLayoutResult);
    else if (m_mayUseSimplifiedTextOnlyInlineLayoutInRange) {
        if (m_maximumContentWidthBetweenLineBreaks && mayUseContentWidthBetweenLineBreaksAsMaximumSize(formattingContextRoot(), inlineItemList())) {
            maximumContentSize = *m_maximumContentWidthBetweenLineBreaks;
#ifndef NDEBUG
            auto simplifiedLineBuilder = TextOnlySimpleLineBuilder { formattingContext(), lineBuilderRoot(), { }, inlineItemList() };
            ASSERT(std::abs(maximumContentSize - computedIntrinsicWidthForConstraint(IntrinsicWidthMode::Maximum, simplifiedLineBuilder, MayCacheLayoutResult::No)) < 1);
#endif
        } else {
            auto simplifiedLineBuilder = TextOnlySimpleLineBuilder { formattingContext(), lineBuilderRoot(), { }, inlineItemList() };
            maximumContentSize = computedIntrinsicWidthForConstraint(IntrinsicWidthMode::Maximum, simplifiedLineBuilder, mayCacheLayoutResult);
        }
    } else {
        auto lineBuilder = LineBuilder { formattingContext(), { }, inlineItemList() };
        maximumContentSize = computedIntrinsicWidthForConstraint(IntrinsicWidthMode::Maximum, lineBuilder, mayCacheLayoutResult);
    }

    return maximumContentSize;
}

InlineLayoutUnit IntrinsicWidthHandler::computedIntrinsicWidthForConstraint(IntrinsicWidthMode intrinsicWidthMode, AbstractLineBuilder& lineBuilder, MayCacheLayoutResult mayCacheLayoutResult, unsigned* lineCount)
{
    auto layoutRange = m_inlineItemRange;
    if (layoutRange.isEmpty())
        return { };

    auto availableWidth = intrinsicWidthMode == IntrinsicWidthMode::Maximum ? maxInlineLayoutUnit() : 0.f;
    auto maximumContentWidth = InlineLayoutUnit { };
    struct ContentWidthBetweenLineBreaks {
        InlineLayoutUnit maximum { };
        InlineLayoutUnit current { };
    };
    auto contentWidthBetweenLineBreaks = ContentWidthBetweenLineBreaks { };
    auto previousLineEnd = std::optional<InlineItemPosition> { };
    auto previousLine = std::optional<PreviousLine> { };
    auto lineIndex = 0lu;
    auto isFirstFormattedLineCandidate = true;
    lineBuilder.setIntrinsicWidthMode(intrinsicWidthMode);

    while (true) {
        auto lineLayoutResult = lineBuilder.layoutInlineContent({ layoutRange, { 0.f, 0.f, availableWidth, 0.f } }, previousLine, isFirstFormattedLineCandidate);
        auto floatContentWidth = [&] {
            auto leftWidth = InlineLayoutUnit { };
            auto rightWidth = InlineLayoutUnit { };
            for (auto& floatItem : lineLayoutResult.floatContent.placedFloats) {
                mayCacheLayoutResult = MayCacheLayoutResult::No;
                auto marginBoxRect = BoxGeometry::marginBoxRect(floatItem.boxGeometry());
                if (floatItem.isStartPositioned())
                    leftWidth = std::max<InlineLayoutUnit>(leftWidth, marginBoxRect.right());
                else
                    rightWidth = std::max<InlineLayoutUnit>(rightWidth, availableWidth - marginBoxRect.left());
            }
            return InlineLayoutUnit { leftWidth + rightWidth };
        };

        auto lineContentLogicalWidth = [&] {
            auto contentWidth = lineLayoutResult.lineGeometry.logicalTopLeft.x() + lineLayoutResult.contentGeometry.logicalWidth + floatContentWidth();
            if (lineLayoutResult.runs.isEmpty())
                return contentWidth;
            auto& leadingRun = lineLayoutResult.runs.first();
            if (leadingRun.isListMarkerOutside())
                contentWidth -= leadingRun.logicalRight();
            return contentWidth;
        }();
        maximumContentWidth = std::max(maximumContentWidth, lineContentLogicalWidth);
        contentWidthBetweenLineBreaks.current += (lineContentLogicalWidth + lineLayoutResult.hangingContent.logicalWidth);
        if (lineLayoutResult.endsWithLineBreak())
            contentWidthBetweenLineBreaks = { std::max(contentWidthBetweenLineBreaks.maximum, contentWidthBetweenLineBreaks.current), { } };

        layoutRange.start = InlineFormattingUtils::leadingInlineItemPositionForNextLine(lineLayoutResult.inlineItemRange.end, previousLineEnd, !lineLayoutResult.floatContent.hasIntrusiveFloat.isEmpty() || !lineLayoutResult.floatContent.placedFloats.isEmpty(), layoutRange.end);
        if (layoutRange.isEmpty()) {
            auto cacheLineBreakingResultForSubsequentLayoutIfApplicable = [&] {
                m_maximumIntrinsicWidthResultForSingleLine = { };
                if (mayCacheLayoutResult == MayCacheLayoutResult::No)
                    return;
                m_maximumIntrinsicWidthResultForSingleLine = WTF::move(lineLayoutResult);
            };
            cacheLineBreakingResultForSubsequentLayoutIfApplicable();
            break;
        }

        // Support single line only.
        mayCacheLayoutResult = MayCacheLayoutResult::No;
        previousLineEnd = layoutRange.start;
        previousLine = PreviousLine { lineIndex++, lineLayoutResult.contentGeometry.trailingOverflowingContentWidth, lineLayoutResult.endsWithLineBreak(), lineLayoutResult.hasContentfulInFlowContent(), { }, WTF::move(lineLayoutResult.floatContent.suspendedFloats) };
        isFirstFormattedLineCandidate &= !lineLayoutResult.hasContentfulInFlowContent();
    }
    m_maximumContentWidthBetweenLineBreaks = std::max(contentWidthBetweenLineBreaks.current, contentWidthBetweenLineBreaks.maximum);
    if (lineCount)
        *lineCount = static_cast<unsigned>(lineIndex + 1);
    return maximumContentWidth;
}

InlineLayoutUnit IntrinsicWidthHandler::simplifiedMinimumWidth(const ElementBox& root) const
{
    auto maximumWidth = InlineLayoutUnit { };

    for (auto* child = root.firstChild(); child; child = child->nextInFlowSibling()) {
        if (auto* inlineTextBox = dynamicDowncast<InlineTextBox>(*child)) {
            ASSERT(inlineTextBox->style().whiteSpaceCollapse() != WhiteSpaceCollapse::Preserve);
            auto& fontCascade = inlineTextBox->style().fontCascade();
            auto contentLength = inlineTextBox->content().length();
            size_t index = 0;
            auto isTreatedAsSpaceCharacter = [&](auto character) {
                return character == space || character == newlineCharacter || character == tabCharacter;
            };
            while (index < contentLength) {
                auto characterLength = TextUtil::firstUserPerceivedCharacterLength(*inlineTextBox, index, contentLength - index);
                ASSERT(characterLength);
                auto isCollapsedWhitespace = characterLength == 1 && isTreatedAsSpaceCharacter(inlineTextBox->content()[index]);
                if (!isCollapsedWhitespace)
                    maximumWidth = std::max(maximumWidth, TextUtil::width(*inlineTextBox, fontCascade, index, index + characterLength, { }, TextUtil::UseTrailingWhitespaceMeasuringOptimization::No));
                index += characterLength;
            }
            continue;
        }
        if (child->isAtomicInlineBox() || child->isReplacedBox()) {
            maximumWidth = std::max<InlineLayoutUnit>(maximumWidth, formattingContext().geometryForBox(*child).marginBoxWidth());
            continue;
        }
        auto isInlineBoxWithInlineContent = child->isInlineBox() && !child->isLineBreakBox();
        if (isInlineBoxWithInlineContent) {
            auto& boxGeometry = formattingContext().geometryForBox(*child);
            maximumWidth = std::max(maximumWidth, std::max<InlineLayoutUnit>(boxGeometry.marginBorderAndPaddingStart(), boxGeometry.marginBorderAndPaddingEnd()));
            maximumWidth = std::max(maximumWidth, simplifiedMinimumWidth(downcast<ElementBox>(*child)));
            continue;
        }
    }
    return maximumWidth;
}

InlineLayoutUnit IntrinsicWidthHandler::simplifiedMaximumWidth(MayCacheLayoutResult mayCacheLayoutResult)
{
    ASSERT(formattingContextRoot().firstChild() && formattingContextRoot().firstChild() == formattingContextRoot().lastChild());
    auto& inlineTextItem = downcast<InlineTextItem>(inlineItemList()[0]);
    auto& style = inlineTextItem.firstLineStyle();

    auto contentLogicalWidth = [&] {
        if (auto width = inlineTextItem.width())
            return *width;
        return TextUtil::width(inlineTextItem, style.fontCascade(), { });
    }();
    if (mayCacheLayoutResult == MayCacheLayoutResult::No)
        return contentLogicalWidth;

    auto line = Line { formattingContext() };
    line.initialize({ }, true);
    line.appendTextFast(inlineTextItem, style, contentLogicalWidth);
    auto lineContent = line.close();

    ASSERT(contentLogicalWidth == lineContent.contentLogicalWidth);

    m_maximumIntrinsicWidthResultForSingleLine = LineLayoutResult { { 0, 1 }
        , WTF::move(lineContent.runs)
        , { }
        , { { }, lineContent.contentLogicalWidth, lineContent.contentLogicalRight, { } }
        , { }
        , { }
        , { }
        , { IsFirstFormattedLine::Yes, true }
        , { }
        , { LineLayoutResult::InlineContentEnding::Generic }
        , { }
        , { }
        , { }
        , { }
    };
    return contentLogicalWidth;
}

InlineFormattingContext& IntrinsicWidthHandler::formattingContext()
{
    return m_inlineFormattingContext;
}

const InlineFormattingContext& IntrinsicWidthHandler::formattingContext() const
{
    return m_inlineFormattingContext;
}

const ElementBox& IntrinsicWidthHandler::formattingContextRoot() const
{
    return m_inlineFormattingContext.root();
}

const ElementBox& IntrinsicWidthHandler::lineBuilderRoot() const
{
    if (!m_inlineItemRange.startIndex())
        return formattingContextRoot();

    auto rootBoxIndex = m_inlineItemRange.startIndex() - 1;
    auto& inlineItems = inlineItemList();
    if (rootBoxIndex >= inlineItems.size()) {
        ASSERT_NOT_REACHED();
        return formattingContextRoot();
    }

    if (auto* inlineBox = dynamicDowncast<ElementBox>(inlineItems[rootBoxIndex].layoutBox()); inlineBox && inlineBox->isInlineBox()) {
        // We are running a range based line building where we only need to layout the inner text content (e.g. <span>inner text content</span>)
        return *inlineBox;
    }

    ASSERT_NOT_REACHED();
    return formattingContextRoot();
}

}
}

