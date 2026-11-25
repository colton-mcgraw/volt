#include "../math/check.hpp"

#include <app/ui/input/capture.hpp>
#include <app/ui/input/processor.hpp>

#include <chrono>
#include <cstddef>
#include <limits>
#include <string>
#include <variant>
#include <vector>

using namespace volt;
using namespace volt::ui;
using namespace std::chrono_literals;

namespace
{
    // A processor fed by hand, with a clock that moves only when told to and
    // a record of everything it put out.
    struct rig
    {
        input_time now{};
        std::vector<input_event> out;
        input_processor input{[this](input_event &&e) { out.push_back(std::move(e)); }, gesture_settings{},
                              [this] { return now; }};

        void advance(std::chrono::milliseconds by) { now += by; }

        // Takes the events recorded so far.
        std::vector<input_event> take() { return std::exchange(out, {}); }

        // Moves the cursor into the window at `at`, and forgets the events.
        void enter(math::vec2i at)
        {
            input.on_enter(at);
            out.clear();
        }

        void click(mouse_button button = mouse_button::left)
        {
            input.on_mouse(button, action::press, modifiers::none);
            input.on_mouse(button, action::release, modifiers::none);
        }
    };

    template <typename Event>
    std::vector<Event> all_of(const std::vector<input_event> &events)
    {
        std::vector<Event> result;
        for (const auto &e : events)
            if (const auto *p = std::get_if<Event>(&e))
                result.push_back(*p);
        return result;
    }

    template <typename... Events>
    bool kinds_are(const std::vector<input_event> &events)
    {
        constexpr std::size_t count = sizeof...(Events);
        if (events.size() != count)
            return false;
        std::size_t i = 0;
        return (std::holds_alternative<Events>(events[i++]) && ...);
    }

    // ---- keyboard ----------------------------------------------------------

    void keys_pair_up()
    {
        rig r;

        // A release whose press went elsewhere is dropped.
        r.input.on_key(key::a, 30, action::release, modifiers::none);
        CHECK(r.take().empty());

        // A repeat with no press seen becomes the press.
        r.input.on_key(key::a, 30, action::repeat, modifiers::none);
        auto events = r.take();
        CHECK(kinds_are<key_event>(events));
        CHECK(std::get<key_event>(events[0]).action == action::press);
        CHECK(r.input.state().is_down(key::a));

        // A second press of a held key is a repeat.
        r.input.on_key(key::a, 30, action::press, modifiers::none);
        events = r.take();
        CHECK(std::get<key_event>(events[0]).action == action::repeat);

        r.input.on_key(key::a, 30, action::release, modifiers::none);
        events = r.take();
        CHECK(std::get<key_event>(events[0]).action == action::release);
        CHECK(!r.input.state().is_down(key::a));
        CHECK(!r.input.state().any_key_down());

        // Unnamed keys pass through untracked.
        r.input.on_key(key::unknown, 999, action::release, modifiers::none);
        CHECK(r.take().size() == 1);
    }

    void modifiers_include_the_key()
    {
        rig r;

        // Wayland style: the modifiers reported with a key lag behind it.
        r.input.on_key(key::left_shift, 42, action::press, modifiers::none);
        CHECK(std::get<key_event>(r.take()[0]).mods == modifiers::shift);
        CHECK(r.input.state().mods() == modifiers::shift);

        r.input.on_key(key::right_shift, 54, action::press, modifiers::shift);
        r.input.on_key(key::left_shift, 42, action::release, modifiers::shift);
        CHECK(std::get<key_event>(r.take()[1]).mods == modifiers::shift); // the other one is still down

        r.input.on_key(key::right_shift, 54, action::release, modifiers::shift);
        CHECK(std::get<key_event>(r.take()[0]).mods == modifiers::none);
        CHECK(r.input.state().mods() == modifiers::none);

        // Shift held since before the window had focus: its release is still
        // delivered, because it changes the modifiers.
        r.input.on_key(key::s, 31, action::press, modifiers::shift | modifiers::caps_lock);
        r.input.on_key(key::s, 31, action::release, modifiers::shift | modifiers::caps_lock);
        r.take();
        r.input.on_key(key::left_shift, 42, action::release, modifiers::shift | modifiers::caps_lock);
        auto events = r.take();
        CHECK(events.size() == 1);
        CHECK(r.input.state().mods() == modifiers::caps_lock);
    }

    void text_filters_controls()
    {
        rig r;
        r.input.on_char(U'a');
        r.input.on_char(0x01); // Ctrl+A on Windows
        r.input.on_char(U'\r');
        r.input.on_char(U'\b');
        r.input.on_char(0x7F);
        r.input.on_char(0x85);
        r.input.on_char(0xD800);
        r.input.on_char(U'é');
        r.input.on_char(U'\U0001F600');
        const auto text = all_of<text_event>(r.take());
        CHECK(text.size() == 3);
        CHECK(text.size() == 3 && text[0].codepoint == U'a' && text[1].codepoint == U'é' &&
              text[2].codepoint == U'\U0001F600');
    }

    // ---- mouse ---------------------------------------------------------------

    void mouse_events_carry_position_and_modifiers()
    {
        rig r;
        r.input.on_enter({10, 20});
        auto events = r.take();
        CHECK(kinds_are<mouse_enter_event>(events));
        CHECK(r.input.state().cursor_inside());

        r.input.on_cursor({10, 20}); // no change, no event
        CHECK(r.take().empty());

        r.input.on_cursor({15, 18});
        events = r.take();
        CHECK(kinds_are<mouse_move_event>(events));
        const auto move = std::get<mouse_move_event>(events[0]);
        CHECK(move.position == math::vec2i{15, 18});
        CHECK(move.delta == math::vec2i{5, -2});

        r.input.on_mouse(mouse_button::right, action::press, modifiers::control);
        events = r.take();
        CHECK(kinds_are<mouse_button_event>(events));
        const auto press = std::get<mouse_button_event>(events[0]);
        CHECK(press.position == math::vec2i{15, 18});
        CHECK(press.mods == modifiers::control);
        CHECK(press.buttons == mask_of(mouse_button::right));
        CHECK(press.clicks == 1);
        CHECK(r.input.state().is_down(mouse_button::right));

        // Scrolling reports where, and what is held.
        r.input.on_scroll(0.0, 1.5);
        const auto scroll = all_of<scroll_event>(r.take());
        CHECK(scroll.size() == 1 && scroll[0].offset == math::vec2d{0.0, 1.5} &&
              scroll[0].buttons == mask_of(mouse_button::right) && scroll[0].mods == modifiers::control);

        // A duplicate press and a release of a button not held are dropped.
        r.input.on_mouse(mouse_button::right, action::press, modifiers::none);
        r.input.on_mouse(mouse_button::middle, action::release, modifiers::none);
        CHECK(r.take().empty());
    }

    void enter_is_implied_by_motion()
    {
        rig r;
        r.input.on_cursor({3, 4});
        const auto events = r.take();
        CHECK(kinds_are<mouse_enter_event>(events));
        CHECK(r.input.state().cursor_inside());
        CHECK(r.input.state().cursor() == math::vec2i{3, 4});
    }

    void clicks_count_up()
    {
        rig r;
        r.enter({100, 100});

        r.click();
        auto events = r.take();
        // The click comes before the release that completes it.
        CHECK(kinds_are<mouse_button_event, click_event, mouse_button_event>(events));
        CHECK(std::get<click_event>(events[1]).clicks == 1);

        r.advance(200ms);
        r.input.on_cursor({101, 102}); // a small wobble is allowed
        r.click();
        auto clicks = all_of<click_event>(r.take());
        CHECK(clicks.size() == 1 && clicks[0].clicks == 2);

        r.advance(200ms);
        r.input.on_mouse(mouse_button::left, action::press, modifiers::none);
        events = r.take();
        CHECK(std::get<mouse_button_event>(events[0]).clicks == 3); // known at the press
        r.input.on_mouse(mouse_button::left, action::release, modifiers::none);
        events = r.take();
        CHECK(std::get<click_event>(events[0]).clicks == 3);
        CHECK(std::get<mouse_button_event>(events[1]).clicks == 3);

        // After the maximum the count starts over.
        r.advance(200ms);
        r.click();
        clicks = all_of<click_event>(r.take());
        CHECK(clicks.size() == 1 && clicks[0].clicks == 1);
    }

    void clicks_break_on_time_distance_and_button()
    {
        rig r;
        r.enter({100, 100});

        // Too slow.
        r.click();
        r.advance(501ms);
        r.click();
        auto clicks = all_of<click_event>(r.take());
        CHECK(clicks.size() == 2 && clicks[1].clicks == 1);

        // Just in time.
        r.advance(500ms);
        r.click();
        clicks = all_of<click_event>(r.take());
        CHECK(clicks.size() == 1 && clicks[0].clicks == 2);

        // Too far, though not far enough to be a drag.
        r.advance(1s);
        r.click();
        r.advance(100ms);
        r.input.on_cursor({103, 100});
        r.click();
        clicks = all_of<click_event>(r.take());
        CHECK(clicks.size() == 2 && clicks[1].clicks == 1);

        // Another button in between.
        r.advance(1s);
        r.click();
        r.click(mouse_button::right);
        r.click();
        clicks = all_of<click_event>(r.take());
        CHECK(clicks.size() == 3 && clicks[2].clicks == 1 && clicks[1].button == mouse_button::right);

        // Configurable.
        gesture_settings settings;
        settings.multi_click_time = 1000ms;
        settings.max_clicks = 0;
        r.input.set_settings(settings);
        r.advance(1s);
        for (int i = 0; i < 5; ++i)
        {
            r.advance(900ms);
            r.click();
        }
        clicks = all_of<click_event>(r.take());
        CHECK(clicks.size() == 5 && clicks[4].clicks == 5);
    }

    void drags_replace_clicks()
    {
        rig r;
        r.enter({50, 50});

        // Within the threshold: a click.
        r.input.on_mouse(mouse_button::left, action::press, modifiers::none);
        r.input.on_cursor({54, 46});
        r.input.on_mouse(mouse_button::left, action::release, modifiers::none);
        auto events = r.take();
        CHECK(all_of<click_event>(events).size() == 1);
        CHECK(all_of<drag_begin_event>(events).empty());

        // Beyond it: a drag, and no click.
        r.advance(1s);
        r.input.on_mouse(mouse_button::left, action::press, modifiers::shift);
        r.input.on_cursor({52, 50});
        events = r.take();
        CHECK(kinds_are<mouse_button_event, mouse_move_event>(events));

        r.input.on_cursor({60, 50});
        events = r.take();
        CHECK(kinds_are<mouse_move_event, drag_begin_event>(events));
        const auto begin = std::get<drag_begin_event>(events[1]);
        CHECK(begin.origin == math::vec2i{54, 46});
        CHECK(begin.position == math::vec2i{60, 50});
        CHECK(begin.button == mouse_button::left);

        r.input.on_cursor({70, 55});
        events = r.take();
        CHECK(kinds_are<mouse_move_event, drag_move_event>(events));
        const auto drag = std::get<drag_move_event>(events[1]);
        CHECK(drag.delta == math::vec2i{10, 5});
        CHECK(drag.origin == math::vec2i{54, 46});

        r.input.on_mouse(mouse_button::left, action::release, modifiers::none);
        events = r.take();
        CHECK(kinds_are<drag_end_event, mouse_button_event>(events));
        CHECK(!std::get<drag_end_event>(events[0]).cancelled);
        CHECK(std::get<drag_end_event>(events[0]).position == math::vec2i{70, 55});

        // A drag does not start a double click.
        r.advance(100ms);
        r.click();
        const auto clicks = all_of<click_event>(r.take());
        CHECK(clicks.size() == 1 && clicks[0].clicks == 1);
    }

    void only_the_first_button_drags()
    {
        rig r;
        r.enter({0, 0});
        r.input.on_mouse(mouse_button::left, action::press, modifiers::none);
        r.input.on_mouse(mouse_button::right, action::press, modifiers::none);
        r.input.on_cursor({20, 0});
        auto events = r.take();
        const auto begins = all_of<drag_begin_event>(events);
        CHECK(begins.size() == 1 && begins[0].button == mouse_button::left);

        // The right button moved too far for a click, and does not drag.
        r.input.on_mouse(mouse_button::right, action::release, modifiers::none);
        events = r.take();
        CHECK(kinds_are<mouse_button_event>(events));

        r.input.on_mouse(mouse_button::left, action::release, modifiers::none);
        events = r.take();
        CHECK(kinds_are<drag_end_event, mouse_button_event>(events));
    }

    void leaving_waits_for_the_release()
    {
        rig r;
        r.enter({10, 10});

        r.input.on_mouse(mouse_button::left, action::press, modifiers::none);
        r.input.on_leave({-5, 10});
        auto events = r.take();
        CHECK(all_of<mouse_leave_event>(events).empty());
        CHECK(r.input.state().cursor_inside());

        // Motion keeps arriving outside.
        r.input.on_cursor({-40, 12});
        events = r.take();
        CHECK(!all_of<mouse_move_event>(events).empty());
        CHECK(r.input.state().cursor() == math::vec2i{-40, 12});

        r.input.on_mouse(mouse_button::left, action::release, modifiers::none);
        events = r.take();
        CHECK(kinds_are<drag_end_event, mouse_button_event, mouse_leave_event>(events));
        CHECK(!r.input.state().cursor_inside());

        // Out and back in during the capture: nothing to announce.
        r.input.on_enter({10, 10});
        r.input.on_mouse(mouse_button::left, action::press, modifiers::none);
        r.input.on_leave({-1, 10});
        r.input.on_enter({1, 10});
        r.input.on_mouse(mouse_button::left, action::release, modifiers::none);
        events = r.take();
        CHECK(all_of<mouse_leave_event>(events).empty());
        CHECK(all_of<mouse_enter_event>(events).size() == 1);
        CHECK(r.input.state().cursor_inside());

        // The capture lasts until the last button is up, not the first.
        r.input.on_mouse(mouse_button::left, action::press, modifiers::none);
        r.input.on_mouse(mouse_button::right, action::press, modifiers::none);
        r.input.on_leave({-2, 10});
        r.input.on_mouse(mouse_button::left, action::release, modifiers::none);
        CHECK(all_of<mouse_leave_event>(r.take()).empty());
        CHECK(r.input.state().cursor_inside());
        r.input.on_mouse(mouse_button::right, action::release, modifiers::none);
        CHECK(all_of<mouse_leave_event>(r.take()).size() == 1);
        r.input.on_enter({1, 10});
        r.take();

        // Without a button held, leaving is immediate.
        r.input.on_leave({1, 10});
        CHECK(kinds_are<mouse_leave_event>(r.take()));
    }

    // ---- focus -----------------------------------------------------------------

    void focus_loss_releases_everything()
    {
        rig r;
        r.input.on_focus(true);
        r.enter({10, 10});

        r.input.on_key(key::left_control, 29, action::press, modifiers::control | modifiers::num_lock);
        r.input.on_key(key::c, 46, action::press, modifiers::control | modifiers::num_lock);
        r.input.on_mouse(mouse_button::left, action::press, modifiers::control | modifiers::num_lock);
        r.input.on_mouse(mouse_button::middle, action::press, modifiers::control | modifiers::num_lock);
        r.input.on_cursor({40, 10});
        r.input.on_leave({-3, 10});
        r.take();
        CHECK(r.input.state().is_down(mouse_button::left));

        r.input.on_focus(false);
        const auto events = r.take();

        // The drag is cancelled, no click is made, every release is synthetic,
        // the held leave comes out, and only then focus is announced lost.
        CHECK(all_of<click_event>(events).empty());
        const auto ends = all_of<drag_end_event>(events);
        CHECK(ends.size() == 1 && ends[0].cancelled);

        const auto buttons = all_of<mouse_button_event>(events);
        CHECK(buttons.size() == 2);
        for (const auto &b : buttons)
            CHECK(b.synthetic && b.action == action::release);

        const auto keys = all_of<key_event>(events);
        CHECK(keys.size() == 2);
        for (const auto &k : keys)
            CHECK(k.synthetic && k.action == action::release && k.mods == modifiers::num_lock);
        CHECK(keys.size() == 2 && keys[0].scancode == 46 && keys[1].scancode == 29); // key order, scancodes kept

        CHECK(all_of<mouse_leave_event>(events).size() == 1);
        CHECK(std::holds_alternative<focus_event>(events.back()));

        const input_state &s = r.input.state();
        CHECK(!s.any_key_down());
        CHECK(s.buttons() == mouse_buttons::none);
        CHECK(s.mods() == modifiers::num_lock);
        CHECK(!s.focused());
        CHECK(!s.cursor_inside());

        // The releases that come late, if any, are dropped.
        r.input.on_mouse(mouse_button::left, action::release, modifiers::none);
        r.input.on_key(key::c, 46, action::release, modifiers::num_lock);
        CHECK(r.take().empty());

        // As is a repeated focus report.
        r.input.on_focus(false);
        CHECK(r.take().empty());

        // And the next press does not continue a series from before.
        r.input.on_focus(true);
        r.input.on_enter({40, 10});
        r.click();
        const auto clicks = all_of<click_event>(r.take());
        CHECK(clicks.size() == 1 && clicks[0].clicks == 1);
    }

    // Another window takes the mouse mid-press while the keyboard stays
    // (WM_CAPTURECHANGED): the buttons are released, cancelled, with no
    // click; the keys held stay held.
    void capture_lost_cancels_without_a_click()
    {
        rig r;
        r.input.on_focus(true);
        r.enter({10, 10});

        // A plain press: released, no click.
        r.input.on_key(key::left_shift, 42, action::press, modifiers::shift);
        r.input.on_mouse(mouse_button::left, action::press, modifiers::shift);
        r.take();
        r.input.on_capture_lost();
        auto events = r.take();
        CHECK(all_of<click_event>(events).empty());
        auto buttons = all_of<mouse_button_event>(events);
        CHECK(buttons.size() == 1 && buttons[0].synthetic && buttons[0].action == action::release);
        CHECK(buttons.size() == 1 && buttons[0].mods == modifiers::shift);
        CHECK(r.input.state().buttons() == mouse_buttons::none);
        CHECK(r.input.state().is_down(key::left_shift));
        CHECK(r.input.state().focused());
        CHECK(all_of<focus_event>(events).empty());

        // Its late release is dropped, and the next press starts a new
        // series: no double click out of a cancelled press.
        r.input.on_mouse(mouse_button::left, action::release, modifiers::shift);
        CHECK(r.take().empty());
        r.input.on_key(key::left_shift, 42, action::release, modifiers::none);
        r.click();
        auto clicks = all_of<click_event>(r.take());
        CHECK(clicks.size() == 1 && clicks[0].clicks == 1);

        // A drag in progress, two buttons, the cursor gone outside: the
        // drag ends cancelled, both buttons come up, and the leave held
        // back for the release is announced.
        r.advance(1s);
        r.input.on_mouse(mouse_button::left, action::press, modifiers::none);
        r.input.on_cursor({40, 10});
        r.input.on_mouse(mouse_button::right, action::press, modifiers::none);
        r.input.on_leave({-3, 10});
        r.take();
        r.input.on_capture_lost();
        events = r.take();
        CHECK(all_of<click_event>(events).empty());
        const auto ends = all_of<drag_end_event>(events);
        CHECK(ends.size() == 1 && ends[0].cancelled && ends[0].button == mouse_button::left);
        buttons = all_of<mouse_button_event>(events);
        CHECK(buttons.size() == 2);
        CHECK(buttons.size() == 2 && buttons[1].button == mouse_button::left); // the drag's last
        CHECK(all_of<mouse_leave_event>(events).size() == 1);
        CHECK(!r.input.state().cursor_inside());

        // Nothing held: nothing to say.
        r.input.on_enter({5, 5});
        r.take();
        r.input.on_capture_lost();
        CHECK(r.take().empty());
    }

    // ---- window ---------------------------------------------------------------

    void window_events()
    {
        rig r;
        r.input.on_resize({800, 600});
        r.input.on_move({20, 30});
        r.input.on_close();
        const std::vector<std::string> paths{"a.kicad_sch", "b.step"};
        r.input.on_drop(paths);
        auto events = r.take();
        CHECK(kinds_are<window_resize_event, window_move_event, close_request_event, drop_event>(events));
        CHECK(std::get<window_resize_event>(events[0]).size == math::vec2i{800, 600});
        CHECK(std::get<drop_event>(events[3]).paths == paths);

        // The order Windows reports minimizing a maximized window in, and back.
        r.input.on_maximize(true);
        r.input.on_minimize(true);
        r.input.on_maximize(false);
        events = r.take();
        auto states = all_of<window_state_event>(events);
        CHECK(states.size() == 2 && states[0].state == window_state::maximized &&
              states[1].state == window_state::minimized);

        r.input.on_minimize(false);
        r.input.on_restore();
        states = all_of<window_state_event>(r.take());
        CHECK(states.size() == 1 && states[0].state == window_state::normal);
        CHECK(r.input.current_window_state() == window_state::normal);
    }

    void scale_changes_only()
    {
        rig r;
        CHECK(r.input.state().scale() == 1.0f);

        // Starting at 1, a report of 1 is no change.
        r.input.on_scale(1.0f);
        CHECK(r.take().empty());

        // The order Windows reports a move to a 150% monitor in: the scale,
        // then the resize it causes.
        r.input.on_scale(1.5f);
        r.input.on_resize({1920, 1080});
        auto events = r.take();
        CHECK(kinds_are<window_scale_event, window_resize_event>(events));
        CHECK(std::get<window_scale_event>(events[0]).scale == 1.5f);
        CHECK(r.input.state().scale() == 1.5f);

        // Repeats and nonsense are dropped.
        r.input.on_scale(1.5f);
        r.input.on_scale(0.0f);
        r.input.on_scale(-2.0f);
        r.input.on_scale(std::numeric_limits<float>::quiet_NaN());
        CHECK(r.take().empty());
        CHECK(r.input.state().scale() == 1.5f);

        // Losing focus releases what is held, but leaves the scale alone.
        r.input.on_focus(true);
        r.input.on_focus(false);
        CHECK(r.input.state().scale() == 1.5f);

        r.input.on_scale(1.0f);
        events = r.take();
        CHECK(all_of<window_scale_event>(events).size() == 1);
        CHECK(r.input.state().scale() == 1.0f);
    }

    void timestamps_come_from_the_clock()
    {
        rig r;
        r.advance(1234ms);
        r.input.on_close();
        const auto events = r.take();
        CHECK(time_of(events[0]) == input_time{} + 1234ms);
    }

    // ---- capture ---------------------------------------------------------------

    void capture_routes_to_the_press_target()
    {
        rig r;
        r.enter({5, 5});

        // Two targets: "a" left of x = 50, "b" right of it.
        int hit_tests = 0;
        const auto hit = [&] {
            ++hit_tests;
            return r.input.state().cursor().x() < 50 ? std::string("a") : std::string("b");
        };

        pointer_capture<std::string> capture;
        std::vector<std::string> routed;

        const auto route = [&](const std::vector<input_event> &events) {
            for (const auto &e : events)
            {
                if (const auto *b = std::get_if<mouse_button_event>(&e))
                    routed.push_back(capture.button(*b, hit));
                else if (std::holds_alternative<mouse_move_event>(e) || std::holds_alternative<click_event>(e) ||
                         std::holds_alternative<drag_begin_event>(e) || std::holds_alternative<drag_move_event>(e) ||
                         std::holds_alternative<drag_end_event>(e))
                    routed.push_back(capture.pointer(hit));
            }
        };

        r.input.on_mouse(mouse_button::left, action::press, modifiers::none);
        route(r.take());
        CHECK(capture.captured() && *capture.captured() == "a");

        const int before = hit_tests;
        r.input.on_cursor({80, 5}); // over "b"
        r.input.on_mouse(mouse_button::right, action::press, modifiers::none);
        r.input.on_mouse(mouse_button::right, action::release, modifiers::none);
        r.input.on_mouse(mouse_button::left, action::release, modifiers::none);
        route(r.take());
        CHECK(hit_tests == before); // no hit test while captured
        CHECK(!capture.captured());
        for (const auto &target : routed)
            CHECK(target == "a");

        // Uncaptured motion goes by hit test.
        routed.clear();
        r.input.on_cursor({90, 5});
        route(r.take());
        CHECK(routed.size() == 1 && routed[0] == "b");
    }

} // namespace

int main()
{
    keys_pair_up();
    modifiers_include_the_key();
    text_filters_controls();
    mouse_events_carry_position_and_modifiers();
    enter_is_implied_by_motion();
    clicks_count_up();
    clicks_break_on_time_distance_and_button();
    drags_replace_clicks();
    only_the_first_button_drags();
    leaving_waits_for_the_release();
    focus_loss_releases_everything();
    capture_lost_cancels_without_a_click();
    window_events();
    scale_changes_only();
    timestamps_come_from_the_clock();
    capture_routes_to_the_press_target();
    return volt::tests::result("ui input processor");
}
