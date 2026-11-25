#pragma once

#include "reader.hpp"

#include <font/font.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// face
// -------------------------------------------------------------------------
//
// What a font object points at: the file's bytes and what was learned from
// them while loading. Built once by font::load() and immutable after, which
// is what makes a font safe to share between threads.
//
// The parts of the parser live in one file per concern (sfnt.cpp for the
// table directory and the small tables, cmap.cpp, glyf.cpp, cff.cpp,
// kerning.cpp) and meet here.

namespace volt::font::detail
{

    // A CFF INDEX: a count, then count + 1 offsets of `offset_size` bytes,
    // then the data they point into. Offsets are 1-based from the byte
    // before the data, as the format has it.
    struct cff_index
    {
        bytes data;               // the whole CFF table
        std::uint32_t count = 0;
        std::uint32_t offset_size = 0;
        std::size_t offsets = 0;  // where the offset array starts
        std::size_t base = 0;     // the byte before the first item
        std::size_t end = 0;      // first byte after the INDEX

        bytes item(std::uint32_t index) const noexcept;
    };

    struct cff_font
    {
        cff_index charstrings;
        cff_index global_subrs;

        // One set of local subroutines per font dict; a font that is not
        // CID-keyed has exactly one.
        std::vector<cff_index> local_subrs;

        // Which font dict each glyph uses; empty for a single one.
        std::vector<std::uint8_t> fd_select;
    };

    struct face
    {
        std::shared_ptr<const std::vector<std::uint8_t>> storage;
        bytes file;

        std::uint32_t index = 0;
        std::uint64_t id = 0;
        outline_format format = outline_format::truetype;

        // The tables, empty where the font has none.
        bytes head;
        bytes hhea;
        bytes maxp;
        bytes hmtx;
        bytes cmap;
        bytes loca;
        bytes glyf;
        bytes kern;
        bytes os2;
        bytes name;
        bytes post;
        bytes cff;
        bytes gpos;

        std::uint32_t glyph_count = 0;
        std::uint32_t long_metrics = 0; // hhea numberOfHMetrics, at most glyph_count
        bool long_loca = false;

        font_metrics metrics;
        std::string family;
        std::string style;

        // The cmap subtable in use. `symbol` marks a (3, 0) symbol subtable,
        // whose characters sit at U+F000 + their 8-bit code.
        bytes cmap_subtable;
        std::uint16_t cmap_format = 0;
        bool cmap_symbol = false;

        // Code points below 256 resolved at load time: text is mostly those.
        std::array<glyph_id, 256> latin{};

        // 'kern' format 0: `kern_count` pairs of (left, right, value), six
        // bytes each, sorted by the pair.
        bytes kern_pairs;
        std::uint32_t kern_count = 0;

        // GPOS: per lookup under the 'kern' feature, in lookup order, the
        // offsets (from the GPOS table) of its pair adjustment subtables.
        std::vector<std::vector<std::uint32_t>> gpos_pair_lookups;

        cff_font cff_data;
    };

    // ---- implemented by the parts ----------------------------------------

    // cmap.cpp
    void parse_cmap(face &face);
    glyph_id map_code_point(const face &face, char32_t code_point) noexcept;

    // kerning.cpp
    void parse_kerning(face &face);
    int kerning(const face &face, glyph_id left, glyph_id right) noexcept;

    // glyf.cpp
    std::expected<outline, std::string> glyf_outline(const face &face, glyph_id glyph);
    box glyf_bounds(const face &face, glyph_id glyph) noexcept;

    // cff.cpp
    std::expected<void, std::string> parse_cff(face &face);
    std::expected<outline, std::string> cff_outline(const face &face, glyph_id glyph);

} // namespace volt::font::detail
