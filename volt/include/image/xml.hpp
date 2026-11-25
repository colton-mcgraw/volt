#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// xml
// -------------------------------------------------------------------------
//
// A small non-validating XML parser that builds the whole tree: enough for
// SVG, and for the XML that electronics files come in (EAGLE, IPC-2581,
// KiCad's lib tables are not, but many vendors' are). Lives in the image
// library because SVG needed it first; nothing in it is about images.
//
// What it does: elements and attributes (single or double quoted), the five
// predefined entities and numeric character references, entities declared
// in the document's internal DTD subset (Illustrator puts its namespace URIs
// there), CDATA sections, and it skips comments, processing instructions and
// the rest of the DOCTYPE. Names are kept as written, prefixes included
// ("xlink:href"); namespaces are not resolved. Text is UTF-8 in and out;
// other encodings are not converted.
//
// It is iterative, so deep nesting cannot overflow the stack, and nesting
// deeper than parse_options::max_depth is an error. Entity expansion is
// one level deep and the expanded document size is capped, so the "billion
// laughs" pattern cannot blow up memory.

namespace volt::xml
{

    struct attribute
    {
        std::string name;
        std::string value; // entities decoded
    };

    struct element
    {
        std::string name;
        std::vector<attribute> attributes;
        std::vector<element> children;

        // The character data directly inside this element (not inside its
        // children), concatenated, entities decoded, CDATA included.
        // Whitespace is kept as it is.
        std::string text;

        // Where the start tag is, from 1, for messages.
        std::size_t line = 0;

        // The attribute's value, or nothing if it is absent.
        std::optional<std::string_view> find(std::string_view attribute_name) const noexcept
        {
            for (const attribute &a : attributes)
                if (a.name == attribute_name)
                    return std::string_view(a.value);
            return std::nullopt;
        }

        // The name without its namespace prefix: "rect" for "svg:rect".
        std::string_view local_name() const noexcept
        {
            const std::string_view full(name);
            const std::size_t colon = full.find(':');
            return colon == std::string_view::npos ? full : full.substr(colon + 1);
        }
    };

    struct parse_options
    {
        std::size_t max_depth = 512;

        // The most text that entity references may add to the document.
        std::size_t max_entity_expansion = std::size_t{1} << 20;
    };

    // The root element of the document. A byte order mark at the start is
    // skipped. Errors name the line where things went wrong.
    std::expected<element, std::string> parse(std::string_view text, const parse_options &options = {});

} // namespace volt::xml
