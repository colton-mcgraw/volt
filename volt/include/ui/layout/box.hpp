#pragma once

#include <ui/geometry.hpp>
#include <ui/widget.hpp>

#include <cstdint>

// -------------------------------------------------------------------------
// box
// -------------------------------------------------------------------------
//
// Children one after the other along an axis, a row or a column: the
// container most of an interface is made of.
//
// Along the axis each child gets what it measures, and the space left over
// (the box's length less those, the spacing and the padding) goes to the
// children with a flex weight, in proportion to it (layout_params::flex).
// When no child flexes, the left-over space is placed per main_align(). A
// box too short for its children gives each what it measured anyway and
// lets the end hang out (to be clipped by whoever clips).
//
// Across the axis each child is placed by cross_align(), or by its own
// layout_params::cross: stretched to the box's full depth, or at its
// measured depth at the start, centre or end.

namespace volt::ui
{

    enum class orientation : std::uint8_t
    {
        horizontal, // a row
        vertical,   // a column
    };

    class box : public widget
    {
    public:
        explicit box(ui::orientation orientation = orientation::vertical, float spacing = 0.0f,
                     insets padding = {})
            : m_orientation(orientation), m_spacing(spacing), m_padding(padding)
        {
        }

        ui::orientation orientation() const noexcept { return m_orientation; }

        float spacing() const noexcept { return m_spacing; }
        void set_spacing(float spacing);

        const insets &padding() const noexcept { return m_padding; }
        void set_padding(const insets &padding);

        align main_align() const noexcept { return m_main; }
        void set_main_align(align a);

        align cross_align() const noexcept { return m_cross; }
        void set_cross_align(align a);

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_arrange(const rect &bounds) override;

    private:
        ui::orientation m_orientation;
        float m_spacing;
        insets m_padding;
        align m_main = align::start;
        align m_cross = align::stretch;
    };

    // A box that runs left to right.
    class row : public box
    {
    public:
        explicit row(float spacing = 0.0f, insets padding = {}) : box(orientation::horizontal, spacing, padding) {}
    };

    // A box that runs top to bottom.
    class column : public box
    {
    public:
        explicit column(float spacing = 0.0f, insets padding = {}) : box(orientation::vertical, spacing, padding) {}
    };

    // Takes up left-over space in a box, pushing what follows to the end.
    class spacer : public widget
    {
    public:
        explicit spacer(float flex = 1.0f)
        {
            layout().flex = flex;
            set_hit_testable(false);
        }
    };

} // namespace volt::ui
