#include "../math/check.hpp"

#include <app/ui/input/window_input.hpp>

#include <atomic>
#include <chrono>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace volt;
using namespace volt::ui;
using namespace std::chrono_literals;

namespace
{
    // The setters of volt::ui::window, without the window: what attach()
    // needs, and a way to fire the callbacks as the platform would.
    struct fake_window
    {
        std::function<void(key, int, action, modifiers)> on_key;
        std::function<void(mouse_button, action, modifiers)> on_mouse;
        std::function<void(const math::vec2i &)> on_cursor;
        std::function<void(const math::vec2i &)> on_enter;
        std::function<void(const math::vec2i &)> on_leave;
        std::function<void(double, double)> on_scroll;
        std::function<void(char32_t)> on_char;
        std::function<void(std::span<const std::string>)> on_drop;
        std::function<void(const math::vec2i &)> on_resize;
        std::function<void(const math::vec2i &)> on_move;
        std::function<void(float)> on_scale;
        std::function<void(bool)> on_focus;
        std::function<void(bool)> on_maximize;
        std::function<void(bool)> on_minimize;
        std::function<void()> on_restore;
        std::function<void()> on_close;
        std::function<void()> on_capture_lost;
        bool focused = false;
        float scale = 1.0f;

        void set_key_callback(std::function<void(key, int, action, modifiers)> f) { on_key = std::move(f); }
        void set_mouse_callback(std::function<void(mouse_button, action, modifiers)> f) { on_mouse = std::move(f); }
        void set_cursor_callback(std::function<void(const math::vec2i &)> f) { on_cursor = std::move(f); }
        void set_enter_callback(std::function<void(const math::vec2i &)> f) { on_enter = std::move(f); }
        void set_leave_callback(std::function<void(const math::vec2i &)> f) { on_leave = std::move(f); }
        void set_scroll_callback(std::function<void(double, double)> f) { on_scroll = std::move(f); }
        void set_char_callback(std::function<void(char32_t)> f) { on_char = std::move(f); }
        void set_drop_callback(std::function<void(std::span<const std::string>)> f) { on_drop = std::move(f); }
        void set_resize_callback(std::function<void(const math::vec2i &)> f) { on_resize = std::move(f); }
        void set_move_callback(std::function<void(const math::vec2i &)> f) { on_move = std::move(f); }
        void set_scale_callback(std::function<void(float)> f) { on_scale = std::move(f); }
        void set_focus_callback(std::function<void(bool)> f) { on_focus = std::move(f); }
        void set_maximize_callback(std::function<void(bool)> f) { on_maximize = std::move(f); }
        void set_minimize_callback(std::function<void(bool)> f) { on_minimize = std::move(f); }
        void set_restore_callback(std::function<void()> f) { on_restore = std::move(f); }
        void set_close_callback(std::function<void()> f) { on_close = std::move(f); }
        void set_capture_lost_callback(std::function<void()> f) { on_capture_lost = std::move(f); }
        bool is_focused() const { return focused; }
        float content_scale() const { return scale; }

        bool any_callback() const
        {
            return on_key || on_mouse || on_cursor || on_enter || on_leave || on_scroll || on_char || on_drop ||
                   on_resize || on_move || on_scale || on_focus || on_maximize || on_minimize || on_restore ||
                   on_close || on_capture_lost;
        }

        bool every_callback() const
        {
            return on_key && on_mouse && on_cursor && on_enter && on_leave && on_scroll && on_char && on_drop &&
                   on_resize && on_move && on_scale && on_focus && on_maximize && on_minimize && on_restore &&
                   on_close && on_capture_lost;
        }
    };

    void attach_and_detach()
    {
        fake_window window;
        window.focused = true;
        {
            window_input input;
            input.attach(window);
            CHECK(window.every_callback());

            // An already focused window is reported as such.
            CHECK(input.state().focused());

            // Every callback arrives as its event on the immediate bus.
            int closes = 0;
            math::vec2i size{};
            events::scoped_token close_token =
                input.immediate().add_listener<close_request_event>([&](const close_request_event &) { ++closes; });
            events::scoped_token resize_token = input.immediate().add_listener<window_resize_event>(
                [&](const window_resize_event &e) { size = e.size; });

            window.on_close();
            window.on_resize({640, 480});
            CHECK(closes == 1);
            CHECK(size == math::vec2i{640, 480});

            // Detaching releases what is held: no more releases will come.
            window.on_key(key::q, 16, action::press, modifiers::none);
            CHECK(input.state().is_down(key::q));
            input.detach();
            CHECK(!window.any_callback());
            CHECK(!input.state().is_down(key::q));

            input.attach(window);
            CHECK(window.every_callback());
        }
        // The destructor detaches too.
        CHECK(!window.any_callback());
    }

    void immediate_and_queued()
    {
        fake_window window;
        window_input input;

        // Without a queue, nothing waits for drain().
        input.attach(window);
        window.on_enter({1, 1});
        CHECK(input.drain() == 0);
        input.detach();

        window_input queued_input;
        int wakes = 0;
        queued_input.enable_queue([&] { ++wakes; });
        queued_input.attach(window);

        std::vector<std::string> immediate_seen;
        std::vector<std::string> queued_seen;

        // Handling an event on the immediate bus does not keep it from the queue.
        events::scoped_token handled = queued_input.immediate().add_listener<key_event>(
            [&](const key_event &) {
                immediate_seen.push_back("key");
                return true;
            },
            10);
        events::scoped_token shadowed = queued_input.immediate().add_listener<key_event>(
            [&](const key_event &) { immediate_seen.push_back("shadowed"); });

        bool down_when_dispatched = false;
        events::scoped_token q1 = queued_input.queued().add_listener<key_event>([&](const key_event &e) {
            queued_seen.push_back(e.action == action::press ? "press" : "release");
            if (e.action == action::press)
                down_when_dispatched = queued_input.queued_state().is_down(key::w);
        });
        events::scoped_token q2 =
            queued_input.queued().add_listener<text_event>([&](const text_event &) { queued_seen.push_back("text"); });

        window.on_key(key::w, 17, action::press, modifiers::none);
        window.on_char(U'w');
        window.on_key(key::w, 17, action::release, modifiers::none);

        CHECK(immediate_seen == std::vector<std::string>{"key", "key"});
        CHECK(queued_seen.empty()); // not until drained
        CHECK(wakes == 3);
        CHECK(!queued_input.queued_state().focused());

        CHECK(queued_input.drain() == 3);
        CHECK(queued_seen == (std::vector<std::string>{"press", "text", "release"}));
        CHECK(down_when_dispatched); // the state is applied before each dispatch
        CHECK(!queued_input.queued_state().is_down(key::w));
        CHECK(queued_input.drain() == 0);
    }

    void motion_is_merged_in_the_queue()
    {
        fake_window window;
        window_input input;
        input.enable_queue();
        input.attach(window);

        std::vector<mouse_move_event> moves;
        std::vector<drag_move_event> drags;
        int begins = 0;
        events::scoped_token t1 =
            input.queued().add_listener<mouse_move_event>([&](const mouse_move_event &e) { moves.push_back(e); });
        events::scoped_token t2 =
            input.queued().add_listener<drag_move_event>([&](const drag_move_event &e) { drags.push_back(e); });
        events::scoped_token t3 = input.queued().add_listener<drag_begin_event>([&](const drag_begin_event &) { ++begins; });

        window.on_enter({0, 0});
        for (int x = 1; x <= 10; ++x)
            window.on_cursor({x, 0});
        input.drain();
        CHECK(moves.size() == 1);
        CHECK(moves.size() == 1 && moves[0].position == math::vec2i{10, 0} && moves[0].delta == math::vec2i{10, 0});
        CHECK(input.queued_state().cursor() == math::vec2i{10, 0});

        // During a drag, moves and drag moves interleave and merge by kind.
        moves.clear();
        window.on_mouse(mouse_button::left, action::press, modifiers::none);
        window.on_cursor({20, 0}); // move, drag begin
        window.on_cursor({25, 0}); // move, drag move
        window.on_cursor({30, 3}); // move, drag move
        window.on_cursor({31, 3}); // move, drag move
        input.drain();
        CHECK(begins == 1);
        CHECK(moves.size() == 2); // one before the drag began, one after
        CHECK(drags.size() == 1);
        CHECK(drags.size() == 1 && drags[0].position == math::vec2i{31, 3} && drags[0].delta == math::vec2i{11, 3});
        CHECK(moves.size() == 2 && moves[1].delta == math::vec2i{11, 3});

        // Anything else in between stops the merging.
        moves.clear();
        window.on_mouse(mouse_button::left, action::release, modifiers::none);
        window.on_cursor({32, 3});
        window.on_scroll(0.0, 1.0);
        window.on_cursor({33, 3});
        input.drain();
        CHECK(moves.size() == 2);
    }

    // The scale reaches both buses in the order the window reported it, the
    // resize it causes after it; and attaching reports what the window's
    // scale already is, as it does the focus.
    void scale_is_reported()
    {
        fake_window window;
        window.scale = 1.5f;

        window_input input;
        input.enable_queue();

        std::vector<std::string> immediate_seen;
        std::vector<std::string> queued_seen;
        float scale_at_resize = 0.0f;
        events::scoped_token t1 = input.immediate().add_listener<window_scale_event>(
            [&](const window_scale_event &e) { immediate_seen.push_back(std::format("scale {}", e.scale)); });
        events::scoped_token t2 = input.immediate().add_listener<window_resize_event>(
            [&](const window_resize_event &e) { immediate_seen.push_back(std::format("resize {}", e.size.x())); });
        events::scoped_token t3 = input.queued().add_listener<window_scale_event>(
            [&](const window_scale_event &e) { queued_seen.push_back(std::format("scale {}", e.scale)); });
        events::scoped_token t4 =
            input.queued().add_listener<window_resize_event>([&](const window_resize_event &e) {
                queued_seen.push_back(std::format("resize {}", e.size.x()));
                scale_at_resize = input.queued_state().scale();
            });

        input.attach(window);
        CHECK(immediate_seen == std::vector<std::string>{"scale 1.5"});
        CHECK(input.state().scale() == 1.5f);

        // Moving to a 200% monitor: the scale, then the resize it brings.
        window.on_scale(2.0f);
        window.on_resize({2560, 1440});

        // The same scale again is no change.
        window.on_scale(2.0f);

        const std::vector<std::string> expected{"scale 1.5", "scale 2", "resize 2560"};
        CHECK(immediate_seen == expected);
        CHECK(input.queued_state().scale() == 1.0f); // not drained yet
        CHECK(input.drain() == 3);
        CHECK(queued_seen == expected);
        CHECK(scale_at_resize == 2.0f); // the resize was laid out with the new scale
        CHECK(input.queued_state().scale() == 2.0f);

        // Detaching takes the callback out; attaching again to a window at
        // the scale already known reports nothing.
        input.detach();
        CHECK(!window.on_scale);
        window.scale = 2.0f;
        input.attach(window);
        CHECK(input.drain() == 0);

        // A window at 1 after one at 2 is a change, reported.
        fake_window other;
        input.attach(other);
        CHECK(!window.any_callback());
        CHECK(input.drain() == 1);
        CHECK(input.queued_state().scale() == 1.0f);
    }

    // The platform thread produces while the consuming thread drains; every
    // event arrives, in order, and the consumer's state ends where the
    // producer's did.
    void drained_from_another_thread()
    {
        fake_window window;
        window_input input;

        std::atomic<int> wakes{0};
        input.enable_queue([&] {
            wakes.fetch_add(1, std::memory_order_relaxed);
            wakes.notify_one();
        });
        input.attach(window);

        constexpr int rounds = 20000;
        int presses = 0;
        int releases = 0;
        bool alternates = true;
        bool expect_press = true;
        std::thread::id listener_thread;
        events::scoped_token t = input.queued().add_listener<key_event>([&](const key_event &e) {
            listener_thread = std::this_thread::get_id();
            const bool press = e.action == action::press;
            alternates = alternates && press == expect_press;
            expect_press = !press;
            (press ? presses : releases)++;
        });

        std::atomic<bool> done{false};
        std::thread consumer([&] {
            while (!done.load())
                input.drain();
            input.drain();
        });

        for (int i = 0; i < rounds; ++i)
        {
            window.on_key(key::k, 37, action::press, modifiers::none);
            window.on_key(key::k, 37, action::release, modifiers::none);
        }
        window.on_key(key::j, 36, action::press, modifiers::none);
        done.store(true);
        consumer.join();

        CHECK(presses == rounds + 1);
        CHECK(releases == rounds);
        CHECK(alternates);
        CHECK(listener_thread != std::this_thread::get_id());
        CHECK(input.queued_state().is_down(key::j));
        CHECK(!input.queued_state().is_down(key::k));
        CHECK(wakes.load() == 2 * rounds + 1);
    }

} // namespace

int main()
{
    attach_and_detach();
    immediate_and_queued();
    motion_is_merged_in_the_queue();
    scale_is_reported();
    drained_from_another_thread();
    return volt::tests::result("ui window input");
}
