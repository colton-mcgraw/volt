#pragma once

#include "../math/check.hpp"

#include <app/ui/input/processor.hpp>
#include <font/font.hpp>
#include <font/text.hpp>
#include <font/utf8.hpp>
#include <ui/draw_list.hpp>
#include <ui/screen.hpp>
#include <ui/text.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

// Shared by the framework's tests: a screen with the repository's font and
// no GPU, driven by input that goes through the real input_processor, so
// clicks, double clicks and drags arrive in the order and with the timing a
// window would produce. Time is a hand-driven clock shared by the processor
// and the screen's timers.

#ifndef VOLT_ASSETS_DIR
#error "VOLT_ASSETS_DIR must point at the repository's assets directory"
#endif

namespace volt::tests::ui
{
    using namespace volt::ui;

    inline font::font default_font()
    {
        static const font::font loaded = [] {
            auto f = font::font::load_file(std::string(VOLT_ASSETS_DIR) + "/fonts/DefaultFont.ttf");
            if (!f)
            {
                std::fprintf(stderr, "cannot load DefaultFont.ttf: %s\n", f.error().c_str());
                std::exit(1);
            }
            return *f;
        }();
        return loaded;
    }

    class harness
    {
    public:
        explicit harness(math::vec2i size = {800, 600}, float scale = 1.0f)
            : m_processor([this](input_event &&e) { deliver(std::move(e)); }, {}, [this] { return m_now; })
        {
            text.add(font::font_chain({default_font()}));
            screen = std::make_unique<volt::ui::screen>(text);
            screen->set_scale(scale);
            screen->set_size(size);
            screen->set_redraw_callback([this] { ++redraw_requests; });
            screen->update(m_now);
            m_processor.on_focus(true);
            m_processor.on_enter({0, 0});
        }

        font_text_engine text;
        std::unique_ptr<volt::ui::screen> screen;
        draw_list list;
        int redraw_requests = 0;

        // ---- time ------------------------------------------------------------

        input_time now() const noexcept { return m_now; }

        // Lets time pass and runs what came due, as a frame would.
        void wait(std::chrono::milliseconds ms)
        {
            m_now += ms;
            screen->update(m_now);
        }

        // Lays out and paints, like a frame; returns the list.
        const draw_list &frame()
        {
            screen->update(m_now);
            screen->paint(list);
            return list;
        }

        // ---- pointer, in the window's pixels --------------------------------------

        void move(int x, int y) { m_processor.on_cursor({x, y}); }

        void press(mouse_button button = mouse_button::left, modifiers mods = modifiers::none)
        {
            m_processor.on_mouse(button, action::press, mods);
        }

        void release(mouse_button button = mouse_button::left, modifiers mods = modifiers::none)
        {
            m_processor.on_mouse(button, action::release, mods);
        }

        // A click, then enough time that the next one is not a double click.
        void click(int x, int y, mouse_button button = mouse_button::left, modifiers mods = modifiers::none)
        {
            move(x, y);
            press(button, mods);
            release(button, mods);
            wait(std::chrono::milliseconds(700));
        }

        void double_click(int x, int y)
        {
            move(x, y);
            press();
            release();
            m_now += std::chrono::milliseconds(80);
            press();
            release();
            wait(std::chrono::milliseconds(700));
        }

        void drag(int x0, int y0, int x1, int y1, mouse_button button = mouse_button::left)
        {
            move(x0, y0);
            press(button);
            const int steps = 4;
            for (int i = 1; i <= steps; ++i)
                move(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
            release(button);
            wait(std::chrono::milliseconds(700));
        }

        void scroll(int x, int y, double notches)
        {
            move(x, y);
            m_processor.on_scroll(0.0, notches);
        }

        // ---- keyboard ----------------------------------------------------------------

        void key(volt::ui::key k, modifiers mods = modifiers::none)
        {
            // Modifier keys down first, as a keyboard sends them.
            const auto hold = [&](modifiers m, volt::ui::key which, action a) {
                if (has(mods, m))
                    m_processor.on_key(which, 0, a, a == action::press ? (mods & held_modifiers) : modifiers::none);
            };
            hold(modifiers::control, key::left_control, action::press);
            hold(modifiers::shift, key::left_shift, action::press);
            hold(modifiers::alt, key::left_alt, action::press);
            m_processor.on_key(k, 0, action::press, mods);
            m_processor.on_key(k, 0, action::release, mods);
            hold(modifiers::alt, key::left_alt, action::release);
            hold(modifiers::shift, key::left_shift, action::release);
            hold(modifiers::control, key::left_control, action::release);
        }

        void type(std::string_view utf8)
        {
            for (std::size_t at = 0; at < utf8.size();)
            {
                const font::decoded d = font::decode_utf8(utf8, at);
                at += d.length;
                m_processor.on_char(d.code_point);
            }
        }

        // The centre of a widget, in pixels.
        math::vec2i center_of(const widget &w) const
        {
            const math::vec2f c = w.bounds().center() * screen->scale();
            return {static_cast<int>(c.x()), static_cast<int>(c.y())};
        }

        void click_on(const widget &w, mouse_button button = mouse_button::left)
        {
            const math::vec2i c = center_of(w);
            click(c.x(), c.y(), button);
        }

    private:
        void deliver(input_event &&event)
        {
            std::visit(
                [this](const auto &e) {
                    if constexpr (requires { screen->handle(e); })
                        screen->handle(e);
                },
                event);
        }

        input_time m_now = input_clock::time_point{} + std::chrono::hours(1);
        input_processor m_processor;
    };

    // How many items of a kind a list holds.
    inline std::size_t count(const draw_list &list, shape_kind kind)
    {
        std::size_t n = 0;
        for (const draw_item &item : list.items())
            n += item.kind == kind ? 1 : 0;
        return n;
    }

    inline bool near(float a, float b, float tolerance = 0.01f)
    {
        return a - b <= tolerance && b - a <= tolerance;
    }
} // namespace volt::tests::ui
