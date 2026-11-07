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

#include "config.h"
#include "Font.h"
#include <cstdlib>
#include <cstring>

#include "FontCascadeInlines.h"
#include "GlyphBuffer.h"
#include "HbUniquePtr.h"
#include "NotImplemented.h"
#include "PathSkia.h"
#include "SkiaHarfBuzzFont.h"
#include "SurrogatePairAwareTextIterator.h"
#include <algorithm>
#include <array>
#include <hb-icu.h>
#include <hb.h>
#include <limits>
#include <skia/core/SkFont.h>
#include <skia/core/SkFontMetrics.h>
#include <span>
#include <unicode/uscript.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/AtomString.h>

namespace WebCore {

static bool snapSimpleTextTransformsEnabled()
{
    static bool enabled = [] {
        const char* value = std::getenv("WEBKIT_SNAP_DISABLE_SIMPLE_TEXT");
        return !value || std::strcmp(value, "1");
    }();
    return enabled;
}

static hb_language_t snapLanguageForLocale(const AtomString& locale)
{
    struct Entry {
        String locale;
        hb_language_t language { HB_LANGUAGE_INVALID };
    };
    static thread_local std::array<Entry, 8> entries;
    static thread_local unsigned next = 0;
    for (auto& entry : entries) {
        if (entry.language != HB_LANGUAGE_INVALID && entry.locale == locale.string()) {
            return entry.language;
        }
    }
    auto language = hb_language_from_string(locale.string().utf8().data(), -1);
    if (locale.length() <= 128) {
        entries[next] = { locale.string().isolatedCopy(), language };
        next = (next + 1) % entries.size();
    }
    return language;
}

struct SnapSimpleScriptRun {
    unsigned start;
    unsigned end;
    UScriptCode script;
};

static std::optional<UScriptCode> snapCharacterScript(char32_t character)
{
    UErrorCode errorCode = U_ZERO_ERROR;
    UScriptCode script = uscript_getScript(character, &errorCode);
    if (U_FAILURE(errorCode)) {
        return std::nullopt;
    }
    return script;
}

static bool snapIsLatinBelowIPA(char16_t character)
{
    if (character < 0x80) {
        return isASCIIAlpha(character);
    }
    if (character < 0x100) {
        return character == 0xaa || character == 0xba || (character >= 0xc0 && character != 0xd7 && character != 0xf7);
    }
    return true;
}

static std::optional<SnapSimpleScriptRun> snapNextScriptRun(std::span<const char16_t> characters, unsigned offset)
{
    SurrogatePairAwareTextIterator textIterator(characters.subspan(offset), offset, characters.size());
    char32_t character;
    unsigned clusterLength = 0;
    if (!textIterator.consume(character, clusterLength)) {
        return std::nullopt;
    }

    auto currentScript = snapCharacterScript(character);
    if (!currentScript) {
        return std::nullopt;
    }

    unsigned startIndex = offset;
    for (textIterator.advance(clusterLength); textIterator.consume(character, clusterLength);
        textIterator.advance(clusterLength)) {
        if (FontCascade::treatAsZeroWidthSpace(character)) {
            continue;
        }

        auto nextScript = snapCharacterScript(character);
        if (!nextScript) {
            return std::nullopt;
        }

        if (nextScript == USCRIPT_INHERITED || nextScript == USCRIPT_COMMON) {
            continue;
        }
        if (currentScript == USCRIPT_INHERITED || currentScript == USCRIPT_COMMON) {
            currentScript = nextScript;
            continue;
        }

        if (currentScript != nextScript && !uscript_hasScript(character, currentScript.value())) {
            return SnapSimpleScriptRun { startIndex, textIterator.currentIndex(), currentScript.value() };
        }
    }

    return SnapSimpleScriptRun { startIndex, textIterator.currentIndex(), currentScript.value() };
}

static bool snapCharactersAreShapingNeutral(StringView text)
{
    if (text.is8Bit()) {
        for (auto character : text.span8()) {
            if (character == 0xad) {
                return false;
            }
        }
        return true;
    }
    for (auto character : text.span16()) {
        if (character >= 0x1100 || character == 0xad || character == 0x34f || character == 0x61c) {
            return false;
        }
    }
    return true;
}

GlyphBufferAdvance Font::applyTransforms(
    GlyphBuffer& glyphBuffer,
    unsigned beginningGlyphIndex,
    unsigned beginningStringIndex,
    bool enableKerning,
    bool requiresShaping,
    const AtomString& locale,
    StringView text,
    TextDirection direction) const
{
    if (!snapSimpleTextTransformsEnabled() || beginningGlyphIndex >= glyphBuffer.size() || !platformData().size()) {
        return makeGlyphBufferAdvance();
    }
    if (!enableKerning && !requiresShaping && !platformData().simpleTextNeedsShaping(enableKerning)) {
        return makeGlyphBufferAdvance();
    }

    auto lastStringIndex = static_cast<unsigned>(glyphBuffer.uncheckedStringOffsetAt(glyphBuffer.size() - 1));
    if (lastStringIndex >= text.length() || text.length() > std::numeric_limits<unsigned>::max()) {
        return makeGlyphBufferAdvance();
    }
    unsigned end = lastStringIndex + 1;
    if (end < text.length() && U16_IS_LEAD(text[end - 1]) && U16_IS_TRAIL(text[end])) {
        ++end;
    }
    if (beginningStringIndex >= end || end > text.length()) {
        return makeGlyphBufferAdvance();
    }

    auto substring = text.substring(beginningStringIndex, end - beginningStringIndex);

    if (!platformData().simpleTextNeedsShaping(enableKerning) && snapCharactersAreShapingNeutral(substring)) {
        auto advances = glyphBuffer.advances(beginningGlyphIndex);
        for (auto& advance : advances) {
            WebCore::setWidth(advance, static_cast<float>(clampTo<int>(WebCore::width(advance) * 65536.0f)) / 65536.0f);
        }
        return makeGlyphBufferAdvance();
    }

    auto inputGlyphs = glyphBuffer.glyphs(beginningGlyphIndex);
    if (std::ranges::find(inputGlyphs, deletedGlyph) != inputGlyphs.end()) {
        return makeGlyphBufferAdvance();
    }

    auto upconverted = substring.upconvertedCharacters<256>();
    std::span<const char16_t> characters = upconverted.span();
    if (characters.empty() || characters.size() > std::numeric_limits<unsigned>::max() || characters.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return makeGlyphBufferAdvance();
    }

    Vector<SnapSimpleScriptRun, 1> runs;
    bool belowIPA = true;
    bool hasLatin = false;
    for (auto character : characters) {
        if (character >= 0x250) {
            belowIPA = false;
            break;
        }
        hasLatin |= snapIsLatinBelowIPA(character);
    }
    if (belowIPA) {
        runs.append({ 0, static_cast<unsigned>(characters.size()), hasLatin ? USCRIPT_LATIN : USCRIPT_COMMON });
    } else {
        unsigned offset = 0;
        while (offset < characters.size()) {
            auto run = snapNextScriptRun(characters, offset);
            if (!run || run->start != offset || run->end <= offset) {
                return makeGlyphBufferAdvance();
            }
            runs.append(*run);
            offset = run->end;
        }
    }

    if (runs.isEmpty() || runs.last().end != characters.size()) {
        return makeGlyphBufferAdvance();
    }

    auto* hbFont = platformData().hbFont();
    if (!hbFont) {
        return makeGlyphBufferAdvance();
    }
    auto featureSpan = platformData().simpleTextFeatures(enableKerning).span();

    static thread_local HbUniquePtr<hb_buffer_t> buffer(hb_buffer_create());
    if (!buffer) {
        return makeGlyphBufferAdvance();
    }
    auto language = snapLanguageForLocale(locale);
    bool rtl = direction == TextDirection::RTL;
    struct ShapedGlyph {
        Glyph glyph;
        unsigned stringOffset;
        GlyphBufferAdvance advance;
        GlyphBufferOrigin origin;
    };
    Vector<ShapedGlyph, 16> shapedGlyphs;

    for (auto& run : runs) {
        hb_buffer_reset(buffer.get());
        hb_buffer_set_cluster_level(buffer.get(), HB_BUFFER_CLUSTER_LEVEL_CHARACTERS);
        hb_buffer_set_flags(
            buffer.get(),
            static_cast<hb_buffer_flags_t>(
                (run.start ? 0 : HB_BUFFER_FLAG_BOT) | (run.end == characters.size() ? HB_BUFFER_FLAG_EOT : 0)));
        hb_buffer_set_language(buffer.get(), language);
        hb_buffer_set_script(buffer.get(), hb_icu_script_to_script(run.script));
        hb_buffer_set_direction(buffer.get(), rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_add_utf16(
            buffer.get(),
            reinterpret_cast<const uint16_t*>(characters.data()),
            characters.size(),
            run.start,
            run.end - run.start);
        if (!hb_buffer_allocation_successful(buffer.get())) {
            return makeGlyphBufferAdvance();
        }
        hb_shape(hbFont, buffer.get(), featureSpan.empty() ? nullptr : featureSpan.data(), featureSpan.size());
        if (!hb_buffer_allocation_successful(buffer.get())) {
            return makeGlyphBufferAdvance();
        }

        unsigned count = hb_buffer_get_length(buffer.get());
        if (shapedGlyphs.size() > std::numeric_limits<unsigned>::max() - count) {
            return makeGlyphBufferAdvance();
        }
        if (!count) {
            continue;
        }
        unsigned infoCount = 0;
        unsigned positionCount = 0;
        auto* infos = hb_buffer_get_glyph_infos(buffer.get(), &infoCount);
        auto* positions = hb_buffer_get_glyph_positions(buffer.get(), &positionCount);
        if (!infos || !positions || infoCount != count || positionCount != count) {
            return makeGlyphBufferAdvance();
        }
        for (unsigned i = 0; i < count; ++i) {
            unsigned source = rtl ? count - 1 - i : i;
            auto cluster = infos[source].cluster;
            if (cluster < run.start || cluster >= run.end || cluster > text.length() - beginningStringIndex) {
                return makeGlyphBufferAdvance();
            }
            Glyph glyph = infos[source].codepoint;
            auto advance = makeGlyphBufferAdvance(positions[source].x_advance / 65536.0f, positions[source].y_advance / 65536.0f);
            auto origin = isZeroWidthSpaceGlyph(glyph)
                ? makeGlyphBufferOrigin()
                : makeGlyphBufferOrigin(
                      { positions[source].x_offset / 65536.0f, positions[source].y_offset / 65536.0f });
            if (isZeroWidthSpaceGlyph(glyph)) {
                advance = makeGlyphBufferAdvance();
            }
            shapedGlyphs.append({ glyph, beginningStringIndex + cluster, advance, origin });
        }
    }

    if (shapedGlyphs.size() > std::numeric_limits<unsigned>::max() - beginningGlyphIndex) {
        return makeGlyphBufferAdvance();
    }
    auto newEnd = beginningGlyphIndex + shapedGlyphs.size();
    if (newEnd > glyphBuffer.size()) {
        glyphBuffer.makeHole(glyphBuffer.size(), newEnd - glyphBuffer.size(), this);
    }
    auto glyphs = glyphBuffer.glyphs(beginningGlyphIndex);
    auto advances = glyphBuffer.advances(beginningGlyphIndex);
    auto origins = glyphBuffer.origins(beginningGlyphIndex);
    auto offsets = glyphBuffer.offsetsInString(beginningGlyphIndex);
    for (size_t i = 0; i < shapedGlyphs.size(); ++i) {
        glyphs[i] = shapedGlyphs[i].glyph;
        advances[i] = shapedGlyphs[i].advance;
        origins[i] = shapedGlyphs[i].origin;
        offsets[i] = shapedGlyphs[i].stringOffset;
    }
    if (newEnd < glyphBuffer.size()) {
        glyphBuffer.shrink(newEnd);
    }
    return makeGlyphBufferAdvance();
}

Path Font::platformPathForGlyph(Glyph glyph) const
{
    const auto& font = m_platformData.skFont();
    if (auto skPath = font.getPath(glyph))
        return { PathSkia::create(WTF::move(*skPath)) };

    return { };
}

FloatRect Font::platformBoundsForGlyph(Glyph glyph) const
{
    if (!m_platformData.size())
        return { };

    const auto& font = m_platformData.skFont();
    SkRect bounds = font.getBounds(glyph, nullptr);
    if (!font.isSubpixel()) {
        SkIRect rect;
        bounds.roundOut(&rect);
        bounds.set(rect);
    }
    return bounds;
}

Vector<FloatRect, Font::inlineGlyphRunCapacity> Font::platformBoundsForGlyphs(const Vector<Glyph, inlineGlyphRunCapacity>& glyphs) const
{
    if (!m_platformData.size())
        return { };

    static_assert(sizeof(Glyph) == sizeof(SkGlyphID));

    Vector<SkRect, inlineGlyphRunCapacity> bounds(glyphs.size());
    const auto& font = m_platformData.skFont();
    font.getBounds(glyphs.span(), bounds.mutableSpan(), nullptr);
    return bounds.map<Vector<FloatRect, inlineGlyphRunCapacity>>([&](const auto& boundsRect) -> auto {
        if (font.isSubpixel())
            return boundsRect;

        SkRect returnValue = boundsRect;
        SkIRect rect;
        returnValue.roundOut(&rect);
        returnValue.set(rect);
        return returnValue;
    });
}

float Font::platformWidthForGlyph(Glyph glyph) const
{
    if (!m_platformData.size())
        return 0;

    const auto& font = m_platformData.skFont();
    SkScalar width = font.getWidth(glyph);

    if (!font.isSubpixel())
        width = SkScalarRoundToInt(width);

    return SkScalarToFloat(width);
}

void Font::platformInit()
{
    if (!m_platformData.size())
        return;

    const auto& font = m_platformData.skFont();
    SkFontMetrics metrics;
    font.getMetrics(&metrics);

    auto ascent = SkScalarRoundToScalar(-metrics.fAscent);
    auto descent = SkScalarRoundToScalar(metrics.fDescent);
    m_fontMetrics.setAscent(ascent);
    m_fontMetrics.setDescent(descent);

    auto lineGap = SkScalarToFloat(metrics.fLeading);
    m_fontMetrics.setLineGap(lineGap);
    m_fontMetrics.setLineSpacing(lroundf(ascent) + lroundf(descent) + lroundf(lineGap));

    m_fontMetrics.setCapHeight(metrics.fCapHeight);

    float underlinePosition;
    if (metrics.hasUnderlinePosition(&underlinePosition))
        m_fontMetrics.setUnderlinePosition(underlinePosition);
    float underlineThickness;
    if (metrics.hasUnderlineThickness(&underlineThickness))
        m_fontMetrics.setUnderlineThickness(underlineThickness);

    if (metrics.fXHeight)
        m_fontMetrics.setXHeight(metrics.fXHeight);

    m_maxCharWidth = SkScalarRoundToInt(metrics.fXMax - metrics.fXMin);
    if (metrics.fAvgCharWidth)
        m_avgCharWidth = SkScalarToFloat(metrics.fAvgCharWidth);

    m_fontMetrics.setUnitsPerEm(font.getTypeface()->getUnitsPerEm());

    // FIXME: add support for SomeEmojiGlyphs once Skia provides API for that.
    // See https://issues.skia.org/issues/374078818.
    if (m_platformData.isColorBitmapFont())
        m_emojiType = AllEmojiGlyphs { };
    else
        m_emojiType = NoEmojiGlyphs { };

    SkString familyName;
    font.getTypeface()->getFamilyName(&familyName);
    if (equalIgnoringASCIICase(familyName.c_str(), "Ahem"_s))
        m_allowsAntialiasing = false;
}

void Font::platformCharWidthInit()
{
    m_avgCharWidth = 0.f;
    m_maxCharWidth = 0.f;
    initCharWidths();
}

RefPtr<Font> Font::platformCreateScaledFont(const FontDescription&, float scaleFactor) const
{
    return Font::create(FontPlatformData(m_platformData.skFont().refTypeface(), scaleFactor * m_platformData.size(),
        m_platformData.syntheticBold(),
        m_platformData.syntheticOblique(),
        m_platformData.orientation(),
        m_platformData.widthVariant(),
        m_platformData.textRenderingMode(),
        Vector<hb_feature_t> { m_platformData.features() },
        m_platformData.customPlatformData()),
        origin(), IsInterstitial::No);
}

RefPtr<Font> Font::platformCreateHalfWidthFont() const
{
    return Font::create(FontPlatformData(m_platformData.skFont().refTypeface(), m_platformData.size(),
        m_platformData.syntheticBold(),
        m_platformData.syntheticOblique(),
        m_platformData.orientation(),
        FontWidthVariant::HalfWidth,
        m_platformData.textRenderingMode(),
        Vector<hb_feature_t> { m_platformData.features() },
        m_platformData.customPlatformData()),
        origin(), IsInterstitial::No);
}

void Font::determinePitch()
{
    m_treatAsFixedPitch = m_platformData.isFixedPitch();
}

bool Font::variantCapsSupportedForSynthesis(FontVariantCaps fontVariantCaps) const
{
    switch (fontVariantCaps) {
    case FontVariantCaps::Small:
    case FontVariantCaps::Petite:
    case FontVariantCaps::AllSmall:
    case FontVariantCaps::AllPetite:
        return false;
    default:
        // Synthesis only supports the variant-caps values listed above.
        return true;
    }
}

bool Font::platformSupportsCodePoint(char32_t character, std::optional<char32_t> variation) const
{
    if (auto* skiaHarfBuzzFont = m_platformData.skiaHarfBuzzFont())
        return !!skiaHarfBuzzFont->glyph(character, variation);

    return m_platformData.skFont().getTypeface()->unicharToGlyph(character);
}

static inline SkFont::Edging edgingForFontSmoothingMode(const SkFont& font, FontSmoothingMode smoothingMode)
{
    switch (smoothingMode) {
    case FontSmoothingMode::Auto:
        return font.getEdging();
    case FontSmoothingMode::Antialiased:
        return SkFont::Edging::kAntiAlias;
    case FontSmoothingMode::SubpixelAntialiased:
        return SkFont::Edging::kSubpixelAntiAlias;
    case FontSmoothingMode::None:
        return SkFont::Edging::kAlias;
    }

    RELEASE_ASSERT_NOT_REACHED();
}

sk_sp<SkTextBlob> Font::buildTextBlob(std::span<const GlyphBufferGlyph> glyphs, std::span<const GlyphBufferAdvance> advances, FontSmoothingMode smoothingMode) const
{
    if (!m_platformData.size() || !glyphs.size())
        return nullptr;

    const auto& font = m_platformData.skFont();
    auto edging = allowsAntialiasing() ? edgingForFontSmoothingMode(font, smoothingMode) : SkFont::Edging::kAlias;
    bool isVertical = m_platformData.orientation() == FontOrientation::Vertical;

    SkTextBlobBuilder builder;
    const auto& buffer = [&]() {
        if (font.getEdging() == edging)
            return isVertical ? builder.allocRunPos(font, glyphs.size()) : builder.allocRunPosH(font, glyphs.size(), 0);

        SkFont copiedFont = font;
        copiedFont.setEdging(edging);
        return isVertical ? builder.allocRunPos(copiedFont, glyphs.size()) : builder.allocRunPosH(copiedFont, glyphs.size(), 0);
    }();

    auto bufferGlyphs = unsafeMakeSpan<SkGlyphID>(buffer.glyphs, glyphs.size());
    auto bufferPositions = unsafeMakeSpan<SkScalar>(buffer.pos, glyphs.size() * (isVertical ? 2 : 1));

    FloatSize glyphPosition;
    for (size_t i = 0; i < glyphs.size(); ++i) {
        bufferGlyphs[i] = glyphs[i];

        if (isVertical) {
            glyphPosition += advances[i];
            bufferPositions[2 * i] = glyphPosition.height();
            bufferPositions[2 * i + 1] = glyphPosition.width();
        } else {
            bufferPositions[i] = glyphPosition.width();
            glyphPosition += advances[i];
        }
    }

    return builder.make();
}

bool Font::enableAntialiasing(FontSmoothingMode smoothingMode) const
{
    return allowsAntialiasing() && edgingForFontSmoothingMode(m_platformData.skFont(), smoothingMode) != SkFont::Edging::kAlias;
}

} // namespace WebCore
