#pragma once

#include <ui/text.hpp>
#include <ui/widget.hpp>

#include <math/vector.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// -------------------------------------------------------------------------
// label
// -------------------------------------------------------------------------
//
// Text that is only read. One line by default, cut with an ellipsis where it
// does not fit; with wrapping on, as many lines as its width needs (up to
// max_lines). Lets the pointer through to what is behind it.

namespace volt::ui
{

    // Which of the theme's text colours a label uses.
    enum class text_role : std::uint8_t
    {
        normal,
        dim,
        accent,
        on_accent,
    };

    // Which of the theme's sizes.
    enum class text_size : std::uint8_t
    {
        body,
        small,
        title,
    };

    class label : public widget
    {
    public:
        explicit label(std::string text = {}, text_role role = text_role::normal);

        const std::string &text() const noexcept { return m_text; }
        void set_text(std::string text);

        void set_role(text_role role);
        void set_size(text_size size);

        // A colour of its own instead of the role's (working space).
        void set_color(std::optional<math::vec4f> color);

        // Horizontal alignment within the label's bounds.
        void set_align(align horizontal);

        // Break into lines to fit the width, at most `max_lines` (0: any).
        void set_wrap(bool wrap, std::size_t max_lines = 0);

        text_style style() const;

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_paint(painter &p) override;

    private:
        math::vec4f color() const;

        std::string m_text;
        text_role m_role;
        text_size m_size = text_size::body;
        std::optional<math::vec4f> m_color;
        align m_align = align::start;
        bool m_wrap = false;
        std::size_t m_max_lines = 0;
    };

} // namespace volt::ui
