#pragma once

#include <ui/geometry.hpp>
#include <ui/layout/box.hpp>
#include <ui/widget.hpp>

#include <memory>
#include <optional>

// -------------------------------------------------------------------------
// scrollbar, scroll_area
// -------------------------------------------------------------------------
//
// A scrollbar is not a widget but the state and drawing of one, kept by the
// widgets that scroll: the scroll area below, and the list and tree views,
// which scroll rows they only lay out while visible. It knows how long the
// content is, how much of it the view shows and where the view is; given
// the rectangle it occupies it draws itself and turns presses and drags on
// it into a new offset. Pressing the track pages towards the press.
//
// A scroll area shows one content widget through a viewport, measuring it
// unbounded along the axes it scrolls (so it is as long as it wants to be)
// and moving it by arranging it at an offset. A scrollbar appears along an
// axis only while the content is longer than the viewport. The wheel
// scrolls it (Shift turns vertical wheel motion horizontal) and is passed
// on to the parent once the content is at its end that way. Painting is
// clipped to the viewport.

namespace volt::ui
{

    class painter;

    class scrollbar
    {
    public:
        explicit scrollbar(ui::orientation orientation = orientation::vertical) : m_orientation(orientation) {}

        // Lengths along the bar's axis; the offset is kept in range.
        void set_extent(float content, float view);
        float content() const noexcept { return m_content; }
        float view() const noexcept { return m_view; }

        float offset() const noexcept { return m_offset; }
        float max_offset() const noexcept { return m_content > m_view ? m_content - m_view : 0.0f; }

        // Returns whether it moved.
        bool set_offset(float offset) noexcept;
        bool scroll_by(float delta) noexcept { return set_offset(m_offset + delta); }

        bool needed() const noexcept { return m_content > m_view + 0.5f; }

        // The thumb, for the bar occupying `bar`.
        rect thumb(const rect &bar) const noexcept;

        // A press at `point` on the bar: grabs the thumb, or pages. Returns
        // whether the offset changed.
        bool press(const math::vec2f &point, const rect &bar) noexcept;
        bool drag(const math::vec2f &point, const rect &bar) noexcept;
        void release() noexcept { m_grab.reset(); }
        bool dragging() const noexcept { return m_grab.has_value(); }

        void paint(painter &p, const rect &bar, bool hot) const;

    private:
        float along(const math::vec2f &v) const noexcept
        {
            return m_orientation == orientation::vertical ? v.y() : v.x();
        }

        ui::orientation m_orientation;
        float m_content = 0.0f;
        float m_view = 0.0f;
        float m_offset = 0.0f;
        std::optional<float> m_grab; // where on the thumb it was grabbed
    };

    class scroll_area : public widget
    {
    public:
        explicit scroll_area(bool vertical = true, bool horizontal = false)
            : m_scroll_vertical(vertical), m_scroll_horizontal(horizontal)
        {
            set_tracks_pointer(true);
        }

        // Replaces the content and returns it.
        template <typename W>
        W &set_content(std::unique_ptr<W> content)
        {
            W &result = *content;
            clear_children();
            add(std::move(content));
            return result;
        }

        widget *content() const noexcept { return child_count() > 0 ? &child(0) : nullptr; }

        math::vec2f offset() const noexcept { return {m_horizontal.offset(), m_vertical.offset()}; }
        void set_offset(const math::vec2f &offset);
        void scroll_by(const math::vec2f &delta);

        // Scrolls as little as it takes for `area` (logical, as laid out
        // now) to be in view.
        void scroll_into_view(const rect &area);

        const rect &viewport() const noexcept { return m_viewport; }
        const scrollbar &vertical_bar() const noexcept { return m_vertical; }
        const scrollbar &horizontal_bar() const noexcept { return m_horizontal; }

        widget *hit_test(const math::vec2f &point) override;

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_arrange(const rect &bounds) override;
        void on_paint(painter &p) override;

        bool on_scroll(const scroll_input &e) override;
        bool on_pointer_down(const pointer_event &e) override;
        void on_pointer_move(const pointer_event &e) override;
        bool on_pointer_up(const pointer_event &e) override;
        void on_pointer_leave() override;

    private:
        rect vertical_rect() const noexcept;
        rect horizontal_rect() const noexcept;
        void place_content();

        bool m_scroll_vertical;
        bool m_scroll_horizontal;
        scrollbar m_vertical{orientation::vertical};
        scrollbar m_horizontal{orientation::horizontal};
        rect m_viewport;
        math::vec2f m_content_size{};
        int m_hot_bar = 0; // 1 vertical, 2 horizontal
    };

} // namespace volt::ui
