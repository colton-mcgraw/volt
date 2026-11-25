#include "harness.hpp"

#include <ui/layout/box.hpp>
#include <ui/widgets/button.hpp>
#include <ui/widgets/checkbox.hpp>
#include <ui/widgets/label.hpp>
#include <ui/widgets/radio_group.hpp>
#include <ui/widgets/scroll_area.hpp>
#include <ui/widgets/slider.hpp>
#include <ui/widgets/text_field.hpp>

#include <string>
#include <vector>

// Input routing: hit testing, hover, capture, bubbling, the wheel, focus
// and its traversal, shortcuts; and invalidation: nothing asks for a frame
// unless something changed.

using namespace volt::tests::ui;
using volt::math::vec2f;

namespace
{
    // Records what reaches it.
    class probe : public widget
    {
    public:
        explicit probe(std::string id, vec2f size, bool handles = true) : m_size(size), m_handles(handles)
        {
            set_name(std::move(id));
        }

        std::vector<std::string> log;

    protected:
        vec2f on_measure(const constraints &) override { return m_size; }
        bool on_pointer_down(const pointer_event &) override
        {
            log.push_back("down");
            return m_handles;
        }
        bool on_pointer_up(const pointer_event &) override
        {
            log.push_back("up");
            return m_handles;
        }
        void on_pointer_enter() override { log.push_back("enter"); }
        void on_pointer_leave() override { log.push_back("leave"); }
        bool on_click(const pointer_event &e) override
        {
            log.push_back("click" + std::to_string(e.clicks));
            return m_handles;
        }
        bool on_scroll(const scroll_input &) override
        {
            log.push_back("scroll");
            return m_handles;
        }
        bool on_key(const key_event &e) override
        {
            if (e.action == action::press)
                log.push_back("key");
            return m_handles && e.key != key::tab;
        }

    private:
        vec2f m_size;
        bool m_handles;
    };

    bool contains(const std::vector<std::string> &log, const std::string &entry)
    {
        return std::ranges::find(log, entry) != log.end();
    }

    void hit_testing_and_hover()
    {
        harness h({400, 100});
        auto &root = h.screen->emplace_root<row>();
        root.set_cross_align(align::start);
        auto &a = root.emplace<probe>("a", vec2f{100.0f, 50.0f});
        auto &b = root.emplace<probe>("b", vec2f{100.0f, 50.0f});
        auto &text = root.emplace<label>("through");
        h.screen->layout();

        CHECK(h.screen->hit_test({50.0f, 25.0f}) == &a);
        CHECK(h.screen->hit_test({150.0f, 25.0f}) == &b);
        CHECK(h.screen->hit_test({150.0f, 75.0f}) == &root); // below the probes: the row
        // A label lets the pointer through to its parent.
        CHECK(h.screen->hit_test(text.bounds().center()) == &root);

        h.move(50, 25);
        CHECK(a.hovered());
        CHECK(root.hovered()); // the whole chain
        CHECK(contains(a.log, "enter"));
        h.move(150, 25);
        CHECK(!a.hovered());
        CHECK(b.hovered());
        CHECK(contains(a.log, "leave"));
        CHECK(h.screen->hovered() == &b);
    }

    void capture_and_click()
    {
        harness h({400, 100});
        auto &root = h.screen->emplace_root<row>();
        auto &ok = root.emplace<button>("OK");
        auto &other = root.emplace<button>("Other");
        h.screen->layout();

        int clicks = 0;
        ok.clicked.connect([&] { ++clicks; });

        h.click_on(ok);
        CHECK(clicks == 1);

        // Pressed on OK, released over Other: no click, and Other never
        // saw the gesture because OK had captured it.
        int other_clicks = 0;
        other.clicked.connect([&] { ++other_clicks; });
        const auto from = h.center_of(ok);
        const auto to = h.center_of(other);
        h.move(from.x(), from.y());
        h.press();
        CHECK(h.screen->captured() == &ok);
        CHECK(ok.is_pressed());
        h.move(to.x(), to.y());
        CHECK(!ok.is_pressed()); // drawn up again while off it
        CHECK(!other.hovered()); // hover stays put during a capture
        h.release();
        CHECK(clicks == 1);
        CHECK(other_clicks == 0);
        CHECK(h.screen->captured() == nullptr);
        CHECK(other.hovered()); // and moves once the capture ends

        // Disabled: nothing.
        ok.set_enabled(false);
        h.click_on(ok);
        CHECK(clicks == 1);
    }

    void bubbling()
    {
        harness h({400, 100});
        auto &outer = h.screen->emplace_root<column>();
        auto &handler = outer.emplace<probe>("handler", vec2f{200.0f, 80.0f});
        auto &inner = handler.emplace<probe>("inner", vec2f{50.0f, 20.0f}, false);
        h.screen->layout();
        inner.arrange({0.0f, 0.0f, 50.0f, 20.0f});

        // The inner widget does not handle the press; its parent does, and
        // captures.
        h.move(10, 10);
        h.press();
        CHECK(contains(inner.log, "down"));
        CHECK(contains(handler.log, "down"));
        CHECK(h.screen->captured() == &handler);
        h.release();
        CHECK(contains(handler.log, "up"));
        CHECK(contains(handler.log, "click1"));
        CHECK(!contains(inner.log, "click1")); // the capture's target gets the click

        // The wheel bubbles from what is under the pointer.
        h.scroll(10, 10, 1.0);
        CHECK(contains(inner.log, "scroll"));
        CHECK(contains(handler.log, "scroll"));
    }

    void double_click()
    {
        harness h({400, 100});
        auto &p = h.screen->emplace_root<probe>("p", vec2f{400.0f, 100.0f});
        h.screen->layout();
        h.double_click(20, 20);
        CHECK(contains(p.log, "click1"));
        CHECK(contains(p.log, "click2"));
    }

    void wheel_to_scroll_area()
    {
        harness h({200, 100});
        auto &area = h.screen->emplace_root<scroll_area>();
        auto &content = area.set_content(std::make_unique<column>());
        for (int i = 0; i < 20; ++i)
            content.emplace<probe>("row", vec2f{100.0f, 20.0f}, false);
        h.screen->layout();

        CHECK(area.vertical_bar().needed());
        CHECK(area.vertical_bar().content() == 400.0f);
        CHECK(area.offset().y() == 0.0f);

        // Down one notch: the content moves up.
        h.scroll(50, 50, -1.0);
        CHECK(area.offset().y() == 48.0f);
        CHECK(content.bounds().y == -48.0f);

        // Never past the end.
        for (int i = 0; i < 20; ++i)
            h.scroll(50, 50, -1.0);
        CHECK(area.offset().y() == 300.0f);

        // Hit testing follows the scrolled content, and stops at the
        // viewport.
        widget *hit = h.screen->hit_test({20.0f, 10.0f});
        CHECK(hit != nullptr && hit->parent() == &content);
        CHECK(hit == &content.child(15)); // 300 + 10 = row 15

        // Dragging the thumb scrolls back to the top.
        const rect thumb = area.vertical_bar().thumb(
            {area.bounds().right() - 12.0f, 0.0f, 12.0f, area.viewport().height});
        h.drag(static_cast<int>(thumb.center().x()), static_cast<int>(thumb.center().y()),
               static_cast<int>(thumb.center().x()), -200);
        CHECK(area.offset().y() == 0.0f);
    }

    void focus_traversal()
    {
        harness h({400, 300});
        auto &root = h.screen->emplace_root<column>(4.0f);
        auto &first = root.emplace<text_field>("one");
        root.emplace<label>("not focusable");
        auto &second = root.emplace<button>("two");
        auto &hidden = root.emplace<button>("hidden");
        hidden.set_visible(false);
        auto &disabled = root.emplace<checkbox>("off");
        disabled.set_enabled(false);
        auto &third = root.emplace<radio_group>(std::vector<std::string>{"a", "b"});
        h.screen->layout();

        CHECK(h.screen->focused() == nullptr);
        h.key(key::tab);
        CHECK(h.screen->focused() == &first);
        CHECK(h.screen->focus_visible());
        h.key(key::tab);
        CHECK(h.screen->focused() == &second);
        h.key(key::tab);
        CHECK(h.screen->focused() == &third); // hidden and disabled skipped
        h.key(key::tab);
        CHECK(h.screen->focused() == &first); // wraps
        h.key(key::tab, modifiers::shift);
        CHECK(h.screen->focused() == &third);

        // Arrows move within the group, not between widgets.
        h.key(key::down);
        CHECK(third.selected() == 1);
        CHECK(h.screen->focused() == &third);

        // A click focuses, without the ring.
        h.click_on(second);
        CHECK(h.screen->focused() == &second);
        CHECK(!h.screen->focus_visible());

        // Hiding the focused widget lets the focus go.
        second.set_visible(false);
        CHECK(h.screen->focused() == nullptr);
    }

    void keys_and_shortcuts()
    {
        harness h({400, 100});
        auto &root = h.screen->emplace_root<row>();
        auto &field = root.emplace<text_field>();
        auto &ok = root.emplace<button>("OK");
        h.screen->layout();

        int saves = 0;
        h.screen->add_command("file.save", [&] { ++saves; });
        CHECK(h.screen->shortcuts().bind("Ctrl+S", "file.save").has_value());
        CHECK(h.screen->shortcut_text("file.save") == "Ctrl+S");

        h.key(key::s, modifiers::control);
        CHECK(saves == 1);

        // A focused text field does not swallow Ctrl+S...
        field.focus();
        h.key(key::s, modifiers::control);
        CHECK(saves == 2);

        // ...but a plain key is text, not a shortcut.
        int deletes = 0;
        h.screen->add_command("edit.delete", [&] { ++deletes; });
        CHECK(h.screen->shortcuts().bind("D", "edit.delete").has_value());
        h.key(key::d);
        CHECK(deletes == 0);
        ok.focus();
        h.key(key::d);
        CHECK(deletes == 1);

        // Space activates a focused button.
        int clicks = 0;
        ok.clicked.connect([&] { ++clicks; });
        h.key(key::space);
        CHECK(clicks == 1);
    }

    void invalidation()
    {
        harness h({400, 100});
        auto &root = h.screen->emplace_root<row>(0.0f, insets::uniform(10.0f));
        auto &box = root.emplace<checkbox>("Grid");
        auto &plain = root.emplace<probe>("plain", vec2f{50.0f, 50.0f});
        h.frame();
        CHECK(!h.screen->needs_redraw());

        // Moving over something that does not change looks: no frame.
        const int before = h.redraw_requests;
        const auto c = h.center_of(plain);
        h.move(c.x(), c.y());
        h.move(c.x() + 1, c.y() + 1);
        CHECK(!h.screen->needs_redraw());
        CHECK(h.redraw_requests == before);

        // Nothing happening and no timers: nowhere to wake up for.
        CHECK(!h.screen->next_wakeup().has_value());

        // A hover highlight is a change.
        const auto b = h.center_of(box);
        h.move(b.x(), b.y());
        CHECK(h.screen->needs_redraw());
        h.frame();
        CHECK(!h.screen->needs_redraw());

        // So is a click, and a layout change.
        h.click_on(box);
        CHECK(box.checked());
        CHECK(h.screen->needs_redraw());
        h.frame();
        box.set_text("Grid lines");
        CHECK(h.screen->layout_dirty());
        h.frame();
        CHECK(!h.screen->layout_dirty());

        // Setting what is already there changes nothing.
        box.set_checked(true);
        CHECK(!h.screen->needs_redraw());
    }

    void caret_blink_asks_for_frames_on_a_timer()
    {
        harness h({400, 100});
        auto &field = h.screen->emplace_root<text_field>("abc");
        h.frame();
        CHECK(!h.screen->next_wakeup().has_value());

        field.focus();
        h.frame();
        // The blink is a timer, half a second or so out; not a frame now.
        const auto wake = h.screen->next_wakeup();
        CHECK(wake.has_value());
        CHECK(*wake > h.now() + std::chrono::milliseconds(300));
        CHECK(!h.screen->needs_redraw());

        h.wait(std::chrono::milliseconds(600));
        CHECK(h.screen->needs_redraw()); // the blink
        h.frame();

        // Unfocused: no more timers.
        h.screen->set_focus(nullptr);
        h.frame();
        CHECK(!h.screen->next_wakeup().has_value());
    }

    void hover_animation_ends()
    {
        harness h({400, 100});
        auto &root = h.screen->emplace_root<row>();
        auto &ok = root.emplace<button>("OK");
        h.frame();
        const auto c = h.center_of(ok);
        h.move(c.x(), c.y());

        // The fade asks for frames while it runs...
        CHECK(h.screen->next_wakeup().has_value());
        for (int i = 0; i < 20; ++i)
        {
            h.wait(std::chrono::milliseconds(16));
            h.frame();
        }
        // ...and stops asking when done.
        CHECK(!h.screen->next_wakeup().has_value());
    }

    void post_from_another_thread()
    {
        harness h({100, 100});
        int ran = 0;
        const int before = h.redraw_requests;
        std::thread other([&] { h.screen->post([&] { ++ran; }); });
        other.join();
        CHECK(h.redraw_requests > before); // woken
        CHECK(ran == 0);                   // not until the interface thread updates
        h.wait(std::chrono::milliseconds(1));
        CHECK(ran == 1);
    }

    void slider_and_checkbox()
    {
        harness h({400, 100});
        auto &root = h.screen->emplace_root<column>();
        root.set_cross_align(align::start);
        auto &s = root.emplace<slider>(0.0, 10.0, 5.0, 1.0);
        auto &c = root.emplace<checkbox>("on");
        h.screen->layout();

        double seen = -1.0;
        s.changed.connect([&](double v) { seen = v; });
        // Pressing at the track's start, dragging past its end.
        const int y = h.center_of(s).y();
        h.drag(static_cast<int>(s.track_left()), y, 2000, y);
        CHECK(s.value() == 10.0);
        CHECK(seen == 10.0);
        s.focus();
        h.key(key::left);
        CHECK(s.value() == 9.0);
        h.key(key::home);
        CHECK(s.value() == 0.0);

        bool state = false;
        c.changed.connect([&](bool on) { state = on; });
        h.click_on(c);
        CHECK(c.checked() && state);
        h.key(key::space);
        CHECK(!c.checked() && !state);
    }
} // namespace

int main()
{
    hit_testing_and_hover();
    capture_and_click();
    bubbling();
    double_click();
    wheel_to_scroll_area();
    focus_traversal();
    keys_and_shortcuts();
    invalidation();
    caret_blink_asks_for_frames_on_a_timer();
    hover_animation_ends();
    post_from_another_thread();
    slider_and_checkbox();
    return volt::tests::result("ui routing");
}
