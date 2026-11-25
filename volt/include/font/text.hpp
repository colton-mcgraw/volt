#pragma once

#include <font/font.hpp>
#include <font/outline.hpp>
#include <math/vector.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// text
// -------------------------------------------------------------------------
//
// From a UTF-8 string to glyphs with positions: which font and glyph shows
// each character, where its pen position is, how lines break.
//
// Shaping is deliberately simple: one glyph per code point, left to right,
// advanced by the font's advance widths plus pair kerning. That is right for
// Latin, Greek, Cyrillic and CJK text and for anything an electronics
// schematic says. Not done: complex scripts (Arabic joining, Indic
// reordering, ligatures, mark positioning; all need GSUB/GPOS contextual
// shaping), bidirectional text (right-to-left runs come out in logical
// order), and vertical text.
//
// Sizes: every function takes the em size in whatever unit the result
// should be in: pixels for the interface, millimetres for a schematic.
// Positions are pen positions on the baseline, x to the right, y down,
// unhinted and unrounded, so text scales exactly with the size.
//
// Positions in the text are byte offsets into the UTF-8 (see <font/utf8.hpp>);
// a glyph's `cluster` is the offset of the code point it shows. Carets sit
// between code points, not between grapheme clusters: an accented letter
// typed as letter plus combining mark takes two steps.

namespace volt::font
{

    // ---------------------------------------------------------------------
    // font_chain
    // ---------------------------------------------------------------------
    //
    // Fonts tried in order for each character: the first that has a glyph
    // for it shows it. The first font is the primary one, whose metrics
    // decide the height of lines; if no font has a character, the primary
    // font's .notdef box shows it.
    class font_chain
    {
    public:
        font_chain() = default;
        explicit font_chain(std::vector<font> fonts) : m_fonts(std::move(fonts)) {}

        void add(font font) { m_fonts.push_back(std::move(font)); }

        bool empty() const noexcept { return m_fonts.empty(); }
        std::span<const font> fonts() const noexcept { return m_fonts; }

        // The primary font; an empty font for an empty chain.
        const font &primary() const noexcept;

        struct resolved
        {
            std::uint16_t font = 0; // index into fonts()
            glyph_id glyph = 0;
        };
        resolved resolve(char32_t code_point) const noexcept;

    private:
        std::vector<font> m_fonts;
    };

    // ---------------------------------------------------------------------
    // One line: shaping, measuring, carets
    // ---------------------------------------------------------------------

    struct shaped_glyph
    {
        std::uint16_t font = 0; // index into the chain's fonts()
        glyph_id glyph = 0;
        std::uint32_t cluster = 0; // byte offset of the code point

        // Pen position along the line, and the distance to the next one
        // (kerning with the next glyph included).
        float x = 0.0f;
        float advance = 0.0f;
    };

    struct shaped_run
    {
        // One per code point, in text order. Control and default-ignorable
        // characters (tab excepted) get a blank glyph with no advance; a tab
        // advances as far as four spaces.
        std::vector<shaped_glyph> glyphs;
        float width = 0.0f;
        std::size_t length = 0; // bytes of text shaped
    };

    shaped_run shape(const font_chain &fonts, std::string_view text, float size);

    // The advance width of `text` on one line.
    float measure(const font_chain &fonts, std::string_view text, float size);

    struct caret_stop
    {
        std::size_t offset = 0;
        float x = 0.0f;
    };

    // Every place a caret can go in the run, from offset 0 to the end:
    // one more than there are code points.
    std::vector<caret_stop> caret_stops(const shaped_run &run);

    // Where the caret before byte `offset` is drawn. Offsets inside a code
    // point count as its start; offsets past the end as the end.
    float caret_x(const shaped_run &run, std::size_t offset) noexcept;

    // The caret position nearest to `x`: the offset a click there puts the
    // caret at.
    std::size_t hit_test(const shaped_run &run, float x) noexcept;

    // ---------------------------------------------------------------------
    // Paragraphs: line breaking and alignment
    // ---------------------------------------------------------------------

    enum class text_align : std::uint8_t
    {
        left,
        center,
        right,
    };

    struct layout_options
    {
        // The em size, in the unit of the result.
        float size = 16.0f;

        // Lines are fitted into this width; 0 or less for no limit, in
        // which case only line breaks in the text start new lines.
        float max_width = 0.0f;

        // Break lines between words (after spaces and hyphens, and around
        // CJK ideographs) to fit max_width. A word wider than the whole line
        // is broken between characters. Without wrapping, a line wider than
        // max_width is cut short.
        bool wrap = true;

        // At most this many lines, 0 for no limit. Text beyond is cut.
        std::size_t max_lines = 0;

        // Whether cut text ends in an ellipsis.
        bool ellipsis = true;

        // Within max_width, or within the widest line when there is no limit.
        text_align align = text_align::left;

        // Multiplies the primary font's line height.
        float line_spacing = 1.0f;
    };

    struct positioned_glyph
    {
        std::uint16_t font = 0;
        glyph_id glyph = 0;
        std::uint32_t cluster = 0;

        // Pen position on the baseline, relative to the layout's top-left
        // corner, y down.
        math::vec2f position{};
        float advance = 0.0f;
    };

    struct text_line
    {
        // The text the line holds: [begin, end) in bytes. Spaces at a wrap
        // belong to the line before; a line break character belongs to no
        // line (end is its offset, the next line begins after it).
        std::uint32_t begin = 0;
        std::uint32_t end = 0;

        // Its glyphs: layout.glyphs[first_glyph] onwards. Blank glyphs
        // (spaces) included, line breaks not; an ellipsis last if cut.
        std::uint32_t first_glyph = 0;
        std::uint32_t glyph_count = 0;

        float x = 0.0f;        // left end after alignment
        float width = 0.0f;    // ink extent of the advances, trailing spaces not counted
        float baseline = 0.0f; // y of the baseline
        bool truncated = false;
        bool hard_break = false; // ended by a line break in the text
    };

    struct text_layout
    {
        std::vector<positioned_glyph> glyphs;
        std::vector<text_line> lines; // at least one, even for empty text

        float size = 0.0f;
        float ascent = 0.0f;      // primary font, scaled; positive
        float descent = 0.0f;     // primary font, scaled; negative
        float line_height = 0.0f; // baseline to baseline

        // The box the lines fill: the widest line (or max_width, if set)
        // by the height of all lines.
        math::vec2f extent{};
        std::size_t length = 0;
    };

    text_layout layout(const font_chain &fonts, std::string_view text, const layout_options &options);

    struct caret
    {
        math::vec2f top{}; // y down, layout coordinates
        float height = 0.0f;
        std::size_t line = 0;
    };

    // Where to draw the caret before byte `offset`. At a wrap, the caret
    // goes to the start of the next line.
    caret caret_at(const text_layout &layout, std::size_t offset) noexcept;

    // The offset a click at `point` (layout coordinates) puts the caret at.
    std::size_t hit_test(const text_layout &layout, const math::vec2f &point) noexcept;

    // Every glyph of the layout as one outline in layout coordinates (y
    // down, the layout's size unit), for drawing text as geometry.
    outline layout_outline(const font_chain &fonts, const text_layout &layout);

} // namespace volt::font
