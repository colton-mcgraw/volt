#pragma once

#include <schematic/scene.hpp>

#include <math/vector.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// stroke font
// -------------------------------------------------------------------------
//
// Text for the schematic view, which draws nothing but lines
// (<schematic/scene.hpp>): a single-stroke vector font in the manner of the
// engineering lettering of drawings (Hershey's simplex, KiCad's stroke
// font). Each glyph is a few open polylines; curves are polylines of short
// pieces; a polyline of a single point is a dot.
//
// Covered: printable ASCII (32 to 126), and the symbols values are written
// with: Ω (U+03A9), µ (U+00B5, and the Greek U+03BC, drawn the same), °
// (U+00B0) and ± (U+00B1). Anything else, control characters included,
// draws as a small box of the width of a digit. Text is UTF-8; a byte that
// does not start or continue a valid sequence (or an overlong form, or a
// surrogate) counts as one unknown character.
//
// Metrics, as multiples of the height asked for, which is the cap height
// (the top of 'H' above the baseline): lowercase letters are 0.7 of it high
// (font_x_height); ascenders reach 1.0; descenders go 0.3 below the
// baseline (font_descent); brackets reach 1.1 above it (font_ascent). The
// font is proportional: every glyph's advance includes the space on its
// two sides, the same either side, so a word centred on a point looks
// centred. Digits share one advance, so columns of numbers line up.
//
// Placement. Text is anchored at a point and aligned to it: horizontally its
// left end, centre or right end; vertically its baseline (bottom), half the
// cap height (middle) or the cap height (top). It may be turned by quarter
// turns counter-clockwise, and never comes out upside down or mirrored:
// turned twice it reads as unturned, three times as once, the way schematic
// tools treat text. The box it is laid out in is still the one turned by the
// full amount, so the anchor and alignment keep their meaning on the sheet:
// left-aligned text turned twice ends at its anchor instead of starting
// there, and bottom-aligned has its top at it.
//
//   std::vector<segment> out;
//   append_text(out, "R1 10kΩ", {2.0f, 1.0f}, 0.5f, horizontal_align::center,
//               vertical_align::middle, 0, 0.06f, color);

namespace volt::schematic
{

    // Metrics, in heights (see the top of this file).
    inline constexpr float font_x_height = 0.7f;
    inline constexpr float font_descent = 0.3f; // below the baseline
    inline constexpr float font_ascent = 1.1f;  // above the baseline

    enum class horizontal_align : std::uint8_t
    {
        left,
        center,
        right,
    };

    enum class vertical_align : std::uint8_t
    {
        bottom, // the baseline at the anchor
        middle, // half the cap height at the anchor
        top,    // the cap height at the anchor
    };

    // A glyph of the font, at a height of 1: x from 0 (the left edge of its
    // advance) to `advance`, y from -font_descent to font_ascent, the
    // baseline at 0.
    struct stroke_glyph
    {
        // Open polylines, each one point (a dot) or more.
        std::vector<std::vector<math::vec2f>> strokes;
        float advance = 0.0f;
    };

    // The glyph of a code point: its own, or the box of an unknown one. The
    // reference stays valid for the life of the program. Thread-safe.
    const stroke_glyph &find_glyph(char32_t code_point);

    // Whether the font has a glyph of its own for the code point.
    bool has_glyph(char32_t code_point) noexcept;

    // The code points of UTF-8 text; each invalid byte becomes U+FFFD.
    std::u32string decode_utf8(std::string_view utf8);

    // The width of the text at that height: the sum of its glyphs' advances.
    float text_width(std::string_view utf8, float height);

    // Where text is laid out: the turned box from its baseline's left end
    // (`origin`), with `along` one unit in the direction it reads and `up`
    // one unit towards the top of its letters. For quarter turns 0 and 2
    // `along` is +x and `up` +y; for 1 and 3 they are +y and -x.
    struct text_frame
    {
        math::vec2f origin{};
        math::vec2f along{1.0f, 0.0f};
        math::vec2f up{0.0f, 1.0f};
    };

    text_frame layout_text(float width, math::vec2f anchor, float height, horizontal_align horizontal,
                           vertical_align vertical, int quarter_turns) noexcept;

    // The box the text covers on the sheet (descenders down to cap height,
    // and its advance across), for hit tests and bounds. Empty text gives a
    // box of no width at the anchor's alignment.
    void text_bounds(std::string_view utf8, math::vec2f anchor, float height, horizontal_align horizontal,
                     vertical_align vertical, int quarter_turns, math::vec2f &min, math::vec2f &max);

    // Appends the text's strokes to `out`, each a segment `stroke_width`
    // wide in `color`, node -1. Height and positions are in world units.
    void append_text(std::vector<segment> &out, std::string_view utf8, math::vec2f anchor, float height,
                     horizontal_align horizontal, vertical_align vertical, int quarter_turns, float stroke_width,
                     const math::vec4f &color);

} // namespace volt::schematic
