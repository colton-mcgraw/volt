#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// -------------------------------------------------------------------------
// utf8
// -------------------------------------------------------------------------
//
// Text arrives as UTF-8 and is not to be trusted to be valid. Decoding never
// fails: each ill-formed sequence becomes one U+FFFD REPLACEMENT CHARACTER
// per "maximal subpart", the practice Unicode recommends (and browsers
// follow), so that one bad byte never swallows the good characters after it.
// Overlong forms, surrogates (U+D800 to U+DFFF) and values above U+10FFFF
// are ill-formed.
//
// Positions in text are byte offsets into the UTF-8, which is what editing a
// std::string needs; every function here keeps them on boundaries between
// code points (or between a replacement and what follows).

namespace volt::font
{

    inline constexpr char32_t replacement_character = U'�';

    struct decoded
    {
        char32_t code_point = 0;

        // Bytes consumed, at least 1 unless at the end of the text.
        std::uint32_t length = 0;
    };

    // The code point starting at byte `offset`. At or past the end: {0, 0}.
    decoded decode_utf8(std::string_view text, std::size_t offset) noexcept;

    // Appends the UTF-8 of `code_point`; surrogates and values above
    // U+10FFFF append U+FFFD instead.
    void append_utf8(std::string &out, char32_t code_point);

    // The boundary after the code point at `offset`, and the one before it:
    // where a caret goes on Right and Left. Clamped to [0, text.size()].
    std::size_t next_boundary(std::string_view text, std::size_t offset) noexcept;
    std::size_t previous_boundary(std::string_view text, std::size_t offset) noexcept;

    // How many code points decoding produces.
    std::size_t count_code_points(std::string_view text) noexcept;

} // namespace volt::font
