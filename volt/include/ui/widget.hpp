#pragma once

#include <app/ui/input.hpp>
#include <app/ui/input/events.hpp>
#include <ui/geometry.hpp>

#include <math/vector.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// -------------------------------------------------------------------------
// widget
// -------------------------------------------------------------------------
//
// The base of everything on screen. Widgets form a tree: each owns its
// children (unique_ptr), knows its parent, and is shown once the tree's
// root is given to a screen (<ui/screen.hpp>), which lays it out, routes
// input to it and paints it.
//
// Layout is two passes, both driven from the root by the screen whenever
// something invalidated it:
//
//   measure(constraints) -> size   how big the widget would like to be,
//                                  within the bounds its parent allows;
//   arrange(rect)                  where it goes, which it passes on to
//                                  its children.
//
// Coordinates are logical units relative to the window's top-left corner
// (absolute, not relative to the parent), so a widget's bounds() are where
// it is on screen and hit testing needs no transforms. A scroll area moves
// its content by arranging it at an offset.
//
// Invalidation: whatever changes how a widget looks calls invalidate();
// whatever changes its size calls invalidate_layout(). Both ask the screen
// for a frame, and nothing else does: the application only draws when one
// of them was called, a timer it set went off, or an animation runs.
//
// Input arrives through the on_* hooks, in logical units. Hooks that return
// bool report whether the widget handled the event: an event nobody handled
// bubbles from the widget it was aimed at to its parent, and on up. See
// <ui/screen.hpp> for which widget each kind of event is aimed at.
//
// Everything here runs on the thread the interface lives on.

namespace volt::ui
{

    class screen;
    class painter;
    class text_engine;
    struct theme;

    // What the pointer looks like over a widget. The platform layer cannot
    // set the cursor yet: the screen reports changes through its cursor
    // callback, for when it can.
    enum class cursor_shape : std::uint8_t
    {
        arrow,
        ibeam,
        hand,
        resize_horizontal,
        resize_vertical,
        move,
        crosshair,
    };

    enum class focus_reason : std::uint8_t
    {
        pointer,      // clicked
        keyboard,     // Tab, or arrows in a group: shows the focus ring
        programmatic, // set_focus() from code
    };

    // A pointer event, in logical units.
    struct pointer_event
    {
        math::vec2f position{};
        math::vec2f delta{};  // motion since the previous event of its kind
        math::vec2f origin{}; // where a drag's button went down
        mouse_button button = mouse_button::left;
        mouse_buttons buttons = mouse_buttons::none; // held after the event
        modifiers mods = modifiers::none;
        int clicks = 1;
        bool cancelled = false; // a drag ended by losing focus
    };

    // The wheel, in notches: positive y scrolls up (away from the user).
    struct scroll_input
    {
        math::vec2f offset{};
        math::vec2f position{};
        modifiers mods = modifiers::none;
    };

    // What a parent may take into account when it lays the widget out.
    // Each container documents which of these it looks at.
    struct layout_params
    {
        // Share of the space left over along a box's axis; 0 takes only
        // what the widget measures.
        float flex = 0.0f;

        // Overrides the container's cross-axis alignment for this child.
        std::optional<align> cross;

        // Fixed sizes along either axis, used in place of measuring when
        // set (0 or more).
        float width = -1.0f;
        float height = -1.0f;

        // Limits on whatever was measured.
        math::vec2f min_size{0.0f, 0.0f};
        math::vec2f max_size{unbounded, unbounded};

        // Cell of a grid.
        int row = 0;
        int column = 0;
        int column_span = 1;
    };

    class widget
    {
    public:
        widget() = default;
        virtual ~widget();

        // Children point at their parent, and the screen at all of them.
        widget(const widget &) = delete;
        widget &operator=(const widget &) = delete;

        // ---- tree ------------------------------------------------------------

        widget *parent() const noexcept { return m_parent; }
        std::span<const std::unique_ptr<widget>> children() const noexcept { return m_children; }
        std::size_t child_count() const noexcept { return m_children.size(); }
        widget &child(std::size_t index) const { return *m_children[index]; }

        // Adds `child` last and returns it.
        template <typename W>
        W &add(std::unique_ptr<W> child)
        {
            W &added = *child;
            insert_child(m_children.size(), std::move(child));
            return added;
        }

        // Makes a child in place: add<button>("OK").
        template <typename W, typename... Args>
        W &emplace(Args &&...args)
        {
            return add(std::make_unique<W>(std::forward<Args>(args)...));
        }

        void insert_child(std::size_t index, std::unique_ptr<widget> child);

        // Takes `child` out of the tree and hands it back; null if it is not
        // a child of this one.
        std::unique_ptr<widget> remove(widget &child);
        void clear_children();

        // The screen the tree is shown on, null while it is not shown.
        ui::screen *owner() const noexcept { return m_screen; }

        bool is_ancestor_of(const widget &other) const noexcept;

        // ---- identity ----------------------------------------------------------

        // For finding widgets and telling them apart in tests and logs.
        const std::string &name() const noexcept { return m_name; }
        void set_name(std::string name) { m_name = std::move(name); }

        // The first widget of that name in this subtree, depth first.
        widget *find(std::string_view name) noexcept;

        template <typename W>
        W *find_as(std::string_view name) noexcept
        {
            return dynamic_cast<W *>(find(name));
        }

        // ---- state -------------------------------------------------------------

        bool visible() const noexcept { return m_visible; }
        void set_visible(bool visible);

        // This widget's own flag; is_enabled() also asks the ancestors. A
        // disabled widget is drawn greyed and gets no input.
        bool enabled() const noexcept { return m_enabled; }
        bool is_enabled() const noexcept;
        void set_enabled(bool enabled);

        // Takes the keyboard focus, by click or by Tab.
        bool focusable() const noexcept { return m_focusable; }
        void set_focusable(bool focusable) noexcept { m_focusable = focusable; }

        bool has_focus() const noexcept;

        // Takes the keyboard focus (while shown).
        void focus(focus_reason reason = focus_reason::programmatic);

        // The pointer is over this widget or one of its descendants.
        bool hovered() const noexcept { return m_hovered; }

        // Shown after the pointer rests on the widget a moment.
        const std::string &tooltip() const noexcept { return m_tooltip; }
        void set_tooltip(std::string text) { m_tooltip = std::move(text); }

        // ---- layout ------------------------------------------------------------

        layout_params &layout() noexcept { return m_layout; }
        const layout_params &layout() const noexcept { return m_layout; }

        // Measures, applying the layout params' fixed and limiting sizes on
        // top of what on_measure() says, and remembers the answer.
        math::vec2f measure(const constraints &limits);
        math::vec2f measured() const noexcept { return m_measured; }

        // Places the widget; containers place their children in turn.
        void arrange(const rect &bounds);
        const rect &bounds() const noexcept { return m_bounds; }

        void invalidate();
        void invalidate_layout();

        // ---- painting and hit testing ------------------------------------------

        // Paints the widget if it is visible and in the painter's clip.
        void paint(painter &p);

        // The deepest widget at `point` that takes pointer input, or null.
        virtual widget *hit_test(const math::vec2f &point);

        // Labels and decorations let the pointer through to what is behind
        // them (their parent, usually).
        bool hit_testable() const noexcept { return m_hit_testable; }
        void set_hit_testable(bool hit_testable) noexcept { m_hit_testable = hit_testable; }

        virtual cursor_shape cursor(const math::vec2f &point) const;

        // Gets on_pointer_move() for motion over its descendants too, not only
        // over itself: for widgets that highlight parts of themselves (a
        // scrollbar) which children may cover.
        bool tracks_pointer() const noexcept { return m_tracks_pointer; }
        void set_tracks_pointer(bool tracks) noexcept { m_tracks_pointer = tracks; }

    protected:
        friend class screen;

        // ---- for subclasses ------------------------------------------------------

        // The default measures the children as a stack: as large as the
        // largest, plus nothing.
        virtual math::vec2f on_measure(const constraints &limits);

        // The default gives every child the whole rectangle.
        virtual void on_arrange(const rect &bounds);

        // The default paints the children.
        virtual void on_paint(painter &p);
        void paint_children(painter &p);

        // Called when the widget joins or leaves a shown tree.
        virtual void on_attached() {}
        virtual void on_detached() {}

        // ---- input hooks; see <ui/screen.hpp> for the routing ---------------------

        virtual bool on_pointer_down(const pointer_event &) { return false; }
        virtual bool on_pointer_up(const pointer_event &) { return false; }
        virtual void on_pointer_move(const pointer_event &) {}
        virtual void on_pointer_enter() {}
        virtual void on_pointer_leave() {}
        virtual bool on_click(const pointer_event &) { return false; }
        virtual bool on_drag_begin(const pointer_event &) { return false; }
        virtual bool on_drag_move(const pointer_event &) { return false; }
        virtual bool on_drag_end(const pointer_event &) { return false; }
        virtual bool on_scroll(const scroll_input &) { return false; }
        virtual bool on_key(const key_event &) { return false; }
        virtual bool on_text(const text_event &) { return false; }
        virtual void on_focus_changed(bool, focus_reason) {}

        // Called every frame while the widget animates (see animate()),
        // with the seconds since the last call. Returns whether it still
        // animates.
        virtual bool on_animate(float) { return false; }

        // ---- services --------------------------------------------------------------

        // The screen's theme and text engine. Only while shown.
        const ui::theme &theme() const;
        text_engine &fonts() const;

        // Asks for on_animate() calls, one per frame, until it returns false.
        void animate();

        // While a button is held, this widget gets the pointer's events
        // wherever it goes. Pressing a button on a widget that handles the
        // press captures it already; this is for taking it over from a
        // child, or letting go early.
        void capture_pointer();
        void release_pointer();

    private:
        void attach(ui::screen *screen);
        void detach();

        ui::screen *m_screen = nullptr;
        widget *m_parent = nullptr;
        std::vector<std::unique_ptr<widget>> m_children;

        std::string m_name;
        std::string m_tooltip;

        layout_params m_layout;
        math::vec2f m_measured{};
        rect m_bounds;

        bool m_visible = true;
        bool m_enabled = true;
        bool m_focusable = false;
        bool m_hit_testable = true;
        bool m_hovered = false;
        bool m_tracks_pointer = false;
    };

    // ---------------------------------------------------------------------
    // fade
    // ---------------------------------------------------------------------
    //
    // A value easing towards a target at a steady rate: hover highlights
    // that come and go over theme::transition seconds instead of at once.
    class fade
    {
    public:
        // Starts moving towards `target`; returns whether it has to move.
        bool set(float target) noexcept
        {
            m_target = target;
            return m_value != m_target;
        }

        void jump(float value) noexcept { m_value = m_target = value; }

        // Moves on by `seconds`, over a full change in `duration`. Returns
        // whether it is still moving.
        bool step(float seconds, float duration) noexcept
        {
            const float rate = duration > 0.0f ? seconds / duration : 1.0f;
            if (m_value < m_target)
                m_value = m_value + rate < m_target ? m_value + rate : m_target;
            else if (m_value > m_target)
                m_value = m_value - rate > m_target ? m_value - rate : m_target;
            return m_value != m_target;
        }

        float value() const noexcept { return m_value; }
        float target() const noexcept { return m_target; }

    private:
        float m_value = 0.0f;
        float m_target = 0.0f;
    };

} // namespace volt::ui
