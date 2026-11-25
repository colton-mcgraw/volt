#pragma once

#include <font/outline.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// font
// -------------------------------------------------------------------------
//
// A TrueType or OpenType font, parsed from its file: what it is called, how
// big its lines are, which glyph shows which character, how far each glyph
// advances, how pairs of glyphs kern, and each glyph's outline.
//
// Supported: sfnt files with TrueType outlines ('glyf'/'loca') or CFF
// outlines ('CFF ', Type 2 charstrings, including CID-keyed fonts), and
// TrueType collections (.ttc), one face per font object. Kerning comes from
// GPOS pair adjustment (formats 1 and 2, under the 'kern' feature) when the
// font has it, otherwise from the old 'kern' table (format 0). Variable fonts
// load as their default instance. Not supported: CFF2, bitmap-only fonts,
// hinting (outlines are unhinted, which suits scalable and SDF text), and
// glyph substitution (GSUB: no ligatures, no contextual forms).
//
// Units: everything a font reports is in font units, y up, with the origin
// at the pen position on the baseline. A font unit is 1 / units_per_em() of
// the em, so at a size of `s` pixels per em, multiply by scale(s).
//
// Font files are untrusted. Every read is bounds checked; a damaged file
// fails to load, or loads with the damaged glyphs empty. Nothing in here
// crashes or loops on bad data.
//
// A font is a cheap handle to immutable data: copies share it, and every
// const member function may be called from any number of threads at once.

namespace volt::font
{

    namespace detail
    {
        struct face;
    } // namespace detail

    // TrueType and CFF both number glyphs with 16 bits. Glyph 0 is .notdef,
    // the box a font shows for characters it does not have.
    using glyph_id = std::uint16_t;

    enum class outline_format : std::uint8_t
    {
        truetype,
        cff,
    };

    // The vertical extent of a line, in font units, y up: `ascent` above the
    // baseline (positive), `descent` below it (negative, whatever sign the
    // file used), then `line_gap` of extra space before the next line.
    struct line_metrics
    {
        int ascent = 0;
        int descent = 0;
        int line_gap = 0;

        // Baseline to baseline.
        int height() const noexcept { return ascent - descent + line_gap; }

        friend bool operator==(const line_metrics &, const line_metrics &) = default;
    };

    // An underline or strikeout: y of the top of the stroke relative to the
    // baseline (y up, so an underline's is negative), and its thickness.
    struct stroke
    {
        int position = 0;
        int thickness = 0;
    };

    struct font_metrics
    {
        int units_per_em = 0;

        // The line metrics to lay text out with. Fonts carry three sets and
        // they disagree; this follows what browsers do:
        //   1. OS/2 typo metrics when the font asks for them (fsSelection
        //      bit 7, USE_TYPO_METRICS), because then they are the designed
        //      ones;
        //   2. otherwise hhea, what every platform but Windows GDI uses;
        //   3. otherwise (hhea all zero) typo, then win, then the head box.
        // The three raw sets are below for anyone who wants another rule.
        line_metrics line;

        line_metrics hhea;
        line_metrics typo; // zero without an OS/2 table
        line_metrics win;  // usWinAscent / -usWinDescent, no gap

        // From OS/2 when it has them (version 2 and later), otherwise
        // measured from the outlines of 'x' and 'H'. Zero if neither works.
        int x_height = 0;
        int cap_height = 0;

        // From 'post' and OS/2. Where a font leaves them out, they are
        // derived from the em: a twentieth thick, an underline a tenth below
        // the baseline, a strikeout at half the x height.
        stroke underline;
        stroke strikeout;

        // The box around every glyph, from 'head'.
        box bounds;
    };

    struct glyph_metrics
    {
        // How far the pen moves after this glyph, before kerning.
        int advance = 0;
        int left_side_bearing = 0;

        // The glyph's box in font units: the one stored in its 'glyf' header
        // (control points included), or for CFF the box of its outline.
        // Empty for glyphs without an outline, like the space.
        box bounds;
    };

    // ---------------------------------------------------------------------
    // font
    // ---------------------------------------------------------------------
    class font
    {
    public:
        // An empty font: valid() is false, it has no glyphs and everything
        // it reports is zero.
        font() = default;

        // Parses the face at `index` in `bytes`, a font file or a collection
        // (index 0 for a plain font file). The bytes are copied.
        static std::expected<font, std::string> load(std::span<const std::byte> bytes, std::uint32_t index = 0);
        static std::expected<font, std::string> load_file(const std::filesystem::path &path,
                                                          std::uint32_t index = 0);

        // Every face of a collection (or the one of a plain file), sharing
        // one copy of the bytes.
        static std::expected<std::vector<font>, std::string> load_collection(std::span<const std::byte> bytes);

        // How many faces `bytes` holds: 1 for a font file, the count for a
        // collection.
        static std::expected<std::uint32_t, std::string> face_count(std::span<const std::byte> bytes);

        bool valid() const noexcept { return m_face != nullptr; }

        // Unique per loaded face for the life of the process (copies share
        // it), and never 0: the key for caches of anything made from a font.
        std::uint64_t id() const noexcept;

        // Which face of its file this is.
        std::uint32_t index() const noexcept;

        // From the 'name' table: the typographic family and subfamily when
        // the font has them, else the legacy ones. "Arial", "Bold Italic".
        const std::string &family() const noexcept;
        const std::string &style() const noexcept;

        outline_format format() const noexcept;
        int units_per_em() const noexcept;
        std::uint32_t glyph_count() const noexcept;
        const font_metrics &metrics() const noexcept;

        // Font units to the unit `size` is in, for an em of `size`.
        float scale(float size) const noexcept;

        // The glyph for a Unicode code point, 0 if the font has none.
        glyph_id glyph(char32_t code_point) const noexcept;
        bool has_glyph(char32_t code_point) const noexcept { return glyph(code_point) != 0; }

        // Metrics of one glyph. Out of range ids get zeros.
        glyph_metrics metrics(glyph_id glyph) const noexcept;
        int advance(glyph_id glyph) const noexcept;

        // What to add to the advance of `left` when `right` follows it,
        // usually negative. Zero for pairs the font does not kern.
        int kerning(glyph_id left, glyph_id right) const noexcept;
        bool has_kerning() const noexcept;

        // The glyph's outline in font units. Empty for blank glyphs; an error
        // where the glyph's data is damaged or the id is out of range.
        std::expected<outline, std::string> glyph_outline(glyph_id glyph) const;

    private:
        explicit font(std::shared_ptr<const detail::face> face) : m_face(std::move(face)) {}

        std::shared_ptr<const detail::face> m_face;
    };

} // namespace volt::font
