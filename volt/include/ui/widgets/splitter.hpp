#pragma once

#include <ui/layout/box.hpp>
#include <ui/signal.hpp>
#include <ui/widget.hpp>

#include <memory>

// -------------------------------------------------------------------------
// splitter
// -------------------------------------------------------------------------
//
// Two panes side by side (or one above the other) with a divider between
// them that the user drags to share the space differently.
//
// The divider's place is kept one of two ways. As a ratio, the default,
// both panes grow with the splitter: right for two views of equal standing.
// As a fixed size of the first or the second pane, only the other one
// grows: right for a side panel next to a document. Either way each pane
// keeps at least its minimum size while there is room for both.
//
// Either pane can be collapsed (set_collapsed()): it is hidden, the other
// takes the whole length, and the divider goes too, so there is nothing to
// drag until the pane comes back, where it was. The splitter owns the
// visibility of its panes for this: hide a pane by collapsing it, not with
// set_visible().

namespace volt::ui
{

    class splitter : public widget
    {
    public:
        enum class keep : std::uint8_t
        {
            ratio,
            first,  // the first pane's size
            second, // the second pane's size
        };

        enum class pane : std::uint8_t
        {
            none,
            first,
            second,
        };

        explicit splitter(ui::orientation orientation = orientation::horizontal);

        // The two panes; set_first() and set_second() replace them.
        template <typename W>
        W &set_first(std::unique_ptr<W> pane)
        {
            W &result = *pane;
            replace(0, std::move(pane));
            return result;
        }

        template <typename W>
        W &set_second(std::unique_ptr<W> pane)
        {
            W &result = *pane;
            replace(1, std::move(pane));
            return result;
        }

        widget *first() const noexcept { return m_first; }
        widget *second() const noexcept { return m_second; }

        ui::orientation orientation() const noexcept { return m_orientation; }

        // The divider at `ratio` of the length (0 to 1).
        void set_ratio(float ratio);

        // The divider so that the first (or second) pane is `size` long,
        // and kept that way as the splitter changes size.
        void set_fixed(keep which, float size);

        void set_minimums(float first, float second);

        // Collapses `which` pane (none: neither). The collapsed pane is
        // hidden (set_visible(false): it is not measured, painted or hit,
        // and gives up the keyboard focus); the other one fills the
        // splitter; the divider is gone. The divider's place (ratio or
        // fixed size) is kept, and the pane comes back to it. Collapsing
        // one pane brings back the other if it was collapsed.
        void set_collapsed(pane which);
        pane collapsed() const noexcept { return m_collapsed; }
        bool is_collapsed(pane which) const noexcept { return which != pane::none && m_collapsed == which; }

        // The first pane's length, as last laid out with both panes shown.
        float position() const noexcept { return m_position; }

        // Where the divider is, as laid out; empty while a pane is
        // collapsed.
        rect divider() const noexcept;

        // Emitted while the user drags, with the first pane's new length.
        signal<float> moved;

        widget *hit_test(const math::vec2f &point) override;
        cursor_shape cursor(const math::vec2f &point) const override;

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_arrange(const rect &bounds) override;
        void on_paint(painter &p) override;
        bool on_pointer_down(const pointer_event &e) override;
        void on_pointer_move(const pointer_event &e) override;
        bool on_pointer_up(const pointer_event &e) override;
        void on_pointer_leave() override
        {
            if (m_hot)
                invalidate();
            m_hot = false;
        }

    private:
        void replace(int index, std::unique_ptr<widget> pane);
        float length() const noexcept;
        float clamp_position(float position) const noexcept;
        void drag_to(float position);
        bool over_divider(const math::vec2f &point) const noexcept;

        ui::orientation m_orientation;
        widget *m_first = nullptr;
        widget *m_second = nullptr;

        keep m_keep = keep::ratio;
        float m_ratio = 0.5f;
        float m_fixed = 200.0f;
        float m_min_first = 40.0f;
        float m_min_second = 40.0f;

        pane m_collapsed = pane::none;

        float m_position = 0.0f;
        bool m_dragging = false;
        float m_grab = 0.0f;
        bool m_hot = false;
    };

} // namespace volt::ui
