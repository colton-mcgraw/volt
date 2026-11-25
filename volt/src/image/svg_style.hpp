#pragma once

#include <image/xml.hpp>

#include <math/matrix.hpp>
#include <math/vector.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The small languages inside SVG attributes: numbers, lengths, colors,
// transforms, and CSS declarations and style sheets. Internal to the image
// library.

namespace volt::image::svg::detail
{

    std::string_view trim(std::string_view text) noexcept;
    bool is_space(char c) noexcept;

    // Reads numbers (and arc flags) as SVG's grammars write them: separated
    // by whitespace and at most one comma, or by nothing where the next
    // number's sign or point makes the boundary clear ("1.5.5", "10-5").
    class number_reader
    {
    public:
        explicit number_reader(std::string_view text) noexcept : m_text(text) {}

        // Skips separators; false (and nothing consumed) if what follows is
        // not a number.
        bool number(float &value) noexcept;

        // A single 0 or 1, as the arc command's flags are written.
        bool flag(bool &value) noexcept;

        void skip_separators() noexcept;
        void skip_spaces() noexcept;
        bool at_end() noexcept;
        char peek() noexcept;
        void advance() noexcept { ++m_position; }
        std::size_t position() const noexcept { return m_position; }
        std::string_view rest() const noexcept { return m_text.substr(m_position); }

    private:
        std::string_view m_text;
        std::size_t m_position = 0;
    };

    // All the numbers in a list; stops at the first thing that is not one.
    std::vector<float> parse_numbers(std::string_view text);

    struct length
    {
        enum class unit : std::uint8_t
        {
            none, // user units
            px,
            pt,
            pc,
            mm,
            cm,
            in,
            em,
            ex,
            percent,
        };

        float value = 0.0f;
        unit type = unit::none;

        // In user units; percentages of `reference`.
        float resolve(float reference) const noexcept;
    };

    std::optional<length> parse_length(std::string_view text) noexcept;

    // A color as straight RGBA, sRGB-encoded. Not "currentColor", "none" or
    // url(), which the caller handles.
    std::optional<math::vec4f> parse_color(std::string_view text) noexcept;

    // The transform attribute's list; nothing if it is malformed (and then
    // ignored, as SVG 2 says).
    std::optional<math::mat3f> parse_transform(std::string_view text);

    math::mat3f translation(float x, float y) noexcept;
    math::mat3f scaling(float x, float y) noexcept;

    // ---- CSS ----------------------------------------------------------------

    using declarations = std::vector<std::pair<std::string, std::string>>;

    // "name: value; name: value", names lowercased, !important dropped.
    declarations parse_declarations(std::string_view text);

    class style_sheet
    {
    public:
        // Adds the rules of a <style> element's text. Rules with selectors
        // beyond simple compounds are skipped, as are at-rules.
        void add(std::string_view text);

        bool empty() const noexcept { return m_rules.empty(); }

        // The declarations of every rule that matches, least specific first
        // (and in document order among equals), so that later ones win.
        declarations match(const xml::element &element) const;

    private:
        struct selector
        {
            std::string tag; // empty: any
            std::string id;
            std::vector<std::string> classes;
            int specificity = 0;
        };

        struct rule
        {
            selector match;
            declarations body;
            std::size_t order = 0;
        };

        std::vector<rule> m_rules;
    };

} // namespace volt::image::svg::detail
