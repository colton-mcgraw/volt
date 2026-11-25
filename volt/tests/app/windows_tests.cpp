#include "math/check.hpp"

#include <app/ui/scale.hpp>
#include <app/ui/shared_text.hpp>
#include <app/ui/window_book.hpp>
#include <app/ui/window_geometry.hpp>

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

// The application's window architecture where it needs no window and no
// GPU: how a window's interface scale is worked out (<app/ui/scale.hpp>),
// the book the interface keeps of its windows (<app/ui/window_book.hpp>),
// and painting several windows against one glyph atlas
// (<app/ui/shared_text.hpp>).

using namespace volt;
using ui::window_book;
using ui::window_kind;

namespace
{
    bool close_to(float a, float b)
    {
        return std::fabs(a - b) < 1e-5f;
    }

    void scale()
    {
        // The display's scale times the user's.
        CHECK(close_to(ui::effective_scale(2.5f, 1.0f), 2.5f));
        CHECK(close_to(ui::effective_scale(2.5f, 1.25f), 3.125f));
        CHECK(close_to(ui::effective_scale(1.0f, 0.75f), 0.75f));

        // Nonsense from either side is 1, or kept in bounds.
        CHECK(close_to(ui::effective_scale(0.0f, 1.0f), 1.0f));
        CHECK(close_to(ui::effective_scale(-3.0f, 1.0f), 1.0f));
        CHECK(close_to(ui::effective_scale(1.5f, 0.0f), 1.5f));
        CHECK(close_to(ui::effective_scale(1.0f, NAN), 1.0f));
        CHECK(close_to(ui::clamp_user_scale(0.1f), ui::min_user_scale));
        CHECK(close_to(ui::clamp_user_scale(9.0f), ui::max_user_scale));

        // Preferences shows the nearest choice; 100% is one.
        CHECK(close_to(ui::user_scales[ui::nearest_user_scale(1.0f)], 1.0f));
        CHECK(close_to(ui::user_scales[ui::nearest_user_scale(1.3f)], 1.25f));
        CHECK(close_to(ui::user_scales[ui::nearest_user_scale(0.1f)], 0.75f));
        CHECK(close_to(ui::user_scales[ui::nearest_user_scale(5.0f)], 2.0f));

        CHECK(ui::scale_text(1.1f) == "110%");
        CHECK(ui::describe_scale(2.5f, 1.0f) == "100% (250%)");
        CHECK(ui::describe_scale(1.5f, 1.25f) == "125% (188%)");

        // A logo 22 units square is 55 pixels at 2.5, never less than one.
        CHECK(ui::scaled_pixels(22.0f, 2.5f) == 55);
        CHECK(ui::scaled_pixels(0.1f, 1.0f) == 1);
    }

    // Where windows open and how small they get (<app/ui/window_geometry.hpp>,
    // fit_inside() and logical_to_window() of <app/ui/window.hpp>).
    void geometry()
    {
        using ui::screen_rect;
        const screen_rect work{{0, 0}, {3840, 2280}}; // 3840 x 2400 less a taskbar

        // Inside already: unchanged.
        const screen_rect inside{{100, 100}, {2000, 1500}};
        CHECK(ui::fit_inside(inside, work) == inside);

        // Hanging off the bottom right: moved back, size kept.
        const screen_rect off{{3000, 1800}, {2000, 1500}};
        CHECK((ui::fit_inside(off, work) == screen_rect{{1840, 780}, {2000, 1500}}));

        // Above and left of it (a monitor that has gone): moved in.
        CHECK((ui::fit_inside({{-5000, -300}, {800, 600}}, work) == screen_rect{{0, 0}, {800, 600}}));

        // Larger than the work area: shrunk to it on that axis only.
        CHECK((ui::fit_inside({{200, 300}, {3300, 2600}}, work) == screen_rect{{200, 0}, {3300, 2280}}));
        CHECK((ui::fit_inside({{-10, -10}, {5000, 5000}}, work) == work));

        // A work area that is not at the origin (a second monitor on the
        // left, a taskbar at the top).
        const screen_rect left{{-1920, 40}, {1920, 1040}};
        CHECK((ui::fit_inside({{-100, 0}, {1000, 800}}, left) == screen_rect{{-1000, 40}, {1000, 800}}));

        // An empty area says nothing.
        CHECK(ui::fit_inside(off, {}) == off);

        // Logical sizes to pixels, rounded; nothing below zero.
        CHECK((ui::logical_to_window({720, 440}, 2.5f) == math::vec2i{1800, 1100}));
        CHECK((ui::logical_to_window({101, 3}, 1.5f) == math::vec2i{152, 5}));
        CHECK((ui::logical_to_window({0, -4}, 2.0f) == math::vec2i{0, 0}));

        // The user's scale makes the interface, and so the window, bigger;
        // a minimum is rounded up so that it stays one.
        CHECK((ui::with_user_scale({720, 440}, 1.0f) == math::vec2i{720, 440}));
        CHECK((ui::with_user_scale({720, 440}, 1.5f) == math::vec2i{1080, 660}));
        CHECK((ui::with_user_scale({901, 481}, 1.1f) == math::vec2i{992, 530}));
        CHECK((ui::with_user_scale({720, 440}, 0.0f) == math::vec2i{720, 440}));

        // A new window: centred on its monitor, each kind a step aside.
        const ui::monitor_area monitor{work, 2.5f};
        const math::vec2i pm = ui::default_position(monitor, {1100, 700}, window_kind::project_manager);
        CHECK((pm == math::vec2i{(3840 - 2750) / 2, (2280 - 1750) / 2}));
        const math::vec2i board = ui::default_position(monitor, {1280, 780}, window_kind::board_editor);
        CHECK((board == math::vec2i{(3840 - 3200) / 2 + 160, (2280 - 1950) / 2 + 160}));
        // The same every time.
        CHECK(ui::default_position(monitor, {1280, 780}, window_kind::board_editor) == board);
        // Too big to centre: at the work area's top left, for the platform
        // to shrink (at --ui-scale 1.5 the board is 1920 x 1170 logical).
        const math::vec2i big = ui::default_position(monitor, ui::with_user_scale({1280, 780}, 1.5f),
                                                     window_kind::schematic_editor);
        CHECK((big == math::vec2i{0, 0}));
        // On a monitor left of the primary one.
        const math::vec2i other = ui::default_position({left, 1.0f}, {800, 600}, window_kind::project_manager);
        CHECK((other == math::vec2i{-1920 + 560, 40 + 220}));
    }

    // What a datasheet link may be before it is handed to the browser
    // (platform::is_web_url()).
    void web_addresses()
    {
        CHECK(platform::is_web_url("https://www.ti.com/lit/ds/symlink/lm358.pdf"));
        CHECK(platform::is_web_url("HTTP://example.com"));
        CHECK(platform::is_web_url("https://example.com/a?b=c#d"));
        CHECK(platform::is_web_url("https://\xE4\xBE\x8B\xE3\x81\x88.jp/datasheet")); // not only ASCII
        CHECK(!platform::is_web_url(""));
        CHECK(!platform::is_web_url("https://"));
        CHECK(!platform::is_web_url("https:///path"));
        CHECK(!platform::is_web_url("file:///C:/Windows/System32/calc.exe"));
        CHECK(!platform::is_web_url("C:\\datasheets\\lm358.pdf"));
        CHECK(!platform::is_web_url("javascript:alert(1)"));
        CHECK(!platform::is_web_url("ftp://example.com/x.pdf"));
        CHECK(!platform::is_web_url("https://example.com/a b"));
        CHECK(!platform::is_web_url("https://example.com/\"&calc"));
        CHECK(!platform::is_web_url("https://example.com/\nline"));
        CHECK(!platform::is_web_url("https://example.com/`x`"));
        CHECK(!platform::is_web_url("https://example.com/" + std::string(2100, 'a')));
    }

    void titles()
    {
        CHECK(ui::window_title(window_kind::schematic_editor, "", false) == "Volt \xE2\x80\x94 Schematic Editor");
        CHECK(ui::window_title(window_kind::board_editor, "Blinky", false) ==
              "Blinky \xE2\x80\x94 Board Editor \xE2\x80\x94 Volt");
        CHECK(ui::window_title(window_kind::project_manager, "Blinky", true) ==
              "*Blinky \xE2\x80\x94 Project Manager \xE2\x80\x94 Volt");
        // Without a project there is nothing to be modified.
        CHECK(ui::window_title(window_kind::library_browser, "", true) == "Volt \xE2\x80\x94 Library Browser");
    }

    void kinds()
    {
        for (window_kind kind : ui::all_window_kinds)
        {
            const auto back = ui::window_kind_from_key(ui::window_key(kind));
            CHECK(back.has_value() && *back == kind);
            CHECK(!ui::window_name(kind).empty());
        }
        CHECK(!ui::window_kind_from_key("schematics").has_value());
        CHECK(!ui::window_kind_from_key("").has_value());
    }

    void opening()
    {
        window_book book;
        CHECK(book.state_of(window_kind::board_editor) == window_book::state::closed);

        // The first request makes a window; asking again while it is on its
        // way does nothing more.
        const auto first = book.request_open(window_kind::board_editor);
        CHECK(first.action == window_book::open_action::create && first.serial != 0);
        CHECK(book.state_of(window_kind::board_editor) == window_book::state::opening);
        CHECK(!book.is_open(window_kind::board_editor));
        CHECK(book.request_open(window_kind::board_editor).action == window_book::open_action::none);

        // Reported made: open. Asking now raises it.
        CHECK(book.created(window_kind::board_editor, first.serial));
        CHECK(book.is_open(window_kind::board_editor));
        const auto again = book.request_open(window_kind::board_editor);
        CHECK(again.action == window_book::open_action::focus && again.serial == first.serial);

        // Other kinds are their own.
        CHECK(book.state_of(window_kind::library_browser) == window_book::state::closed);
        CHECK(book.request_open(window_kind::library_browser).action == window_book::open_action::create);

        // A report about a window the book never asked for changes nothing.
        CHECK(!book.created(window_kind::board_editor, first.serial + 100));
        CHECK(book.is_open(window_kind::board_editor));
    }

    void closing()
    {
        window_book book;
        const auto w = book.request_open(window_kind::schematic_editor);
        CHECK(book.created(window_kind::schematic_editor, w.serial));

        // The close button twice: the first takes it down, the second finds
        // it gone.
        CHECK(book.request_close(window_kind::schematic_editor, w.serial) == window_book::close_action::tear_down);
        CHECK(book.request_close(window_kind::schematic_editor, w.serial) == window_book::close_action::none);
        CHECK(book.state_of(window_kind::schematic_editor) == window_book::state::closed);

        // Opened again: a new serial. A late close request about the old
        // window is stale.
        const auto w2 = book.request_open(window_kind::schematic_editor);
        CHECK(w2.action == window_book::open_action::create && w2.serial != w.serial);
        CHECK(book.created(window_kind::schematic_editor, w2.serial));
        CHECK(book.request_close(window_kind::schematic_editor, w.serial) == window_book::close_action::none);
        CHECK(book.is_open(window_kind::schematic_editor));

        // The interface's own request (no serial) closes whichever is open.
        CHECK(book.request_close(window_kind::schematic_editor) == window_book::close_action::tear_down);
        CHECK(book.request_close(window_kind::schematic_editor) == window_book::close_action::none);
    }

    void closing_while_opening()
    {
        window_book book;
        const auto w = book.request_open(window_kind::library_browser);

        // Closed before the platform thread reported it: forgotten, and the
        // report that comes later says to retire it.
        CHECK(book.request_close(window_kind::library_browser) == window_book::close_action::forget);
        CHECK(!book.created(window_kind::library_browser, w.serial));
        CHECK(book.state_of(window_kind::library_browser) == window_book::state::closed);

        // Opened again before the old report came: the old one is retired,
        // the new one opens.
        const auto a = book.request_open(window_kind::library_browser);
        (void)book.request_close(window_kind::library_browser);
        const auto b = book.request_open(window_kind::library_browser);
        CHECK(a.serial != b.serial);
        CHECK(!book.created(window_kind::library_browser, a.serial));
        CHECK(book.created(window_kind::library_browser, b.serial));
        CHECK(book.is_open(window_kind::library_browser));
    }

    void failing()
    {
        window_book book;
        const auto w = book.request_open(window_kind::board_editor);
        book.failed(window_kind::board_editor, w.serial);
        CHECK(book.state_of(window_kind::board_editor) == window_book::state::closed);

        // A failure reported about an older window leaves the new one be.
        const auto w2 = book.request_open(window_kind::board_editor);
        book.failed(window_kind::board_editor, w.serial);
        CHECK(book.state_of(window_kind::board_editor) == window_book::state::opening);
        CHECK(book.created(window_kind::board_editor, w2.serial));
    }

    void exiting()
    {
        window_book book;
        const auto open = book.request_open(window_kind::project_manager);
        CHECK(book.created(window_kind::project_manager, open.serial));
        const auto opening = book.request_open(window_kind::board_editor);

        book.begin_exit();
        CHECK(book.exiting());

        // Nothing opens any more, not even a window already on its way: its
        // report says retire.
        CHECK(book.request_open(window_kind::library_browser).action == window_book::open_action::none);
        CHECK(!book.created(window_kind::board_editor, opening.serial));

        // What is open stays in the book until closed.
        CHECK(book.is_open(window_kind::project_manager));
        CHECK(book.request_close(window_kind::project_manager) == window_book::close_action::tear_down);
    }

    // A text engine whose atlas overflows when told to, for
    // paint_consistently().
    class fake_text final : public ui::text_engine
    {
    public:
        ui::text_metrics metrics(const ui::text_style &) override { return {}; }
        font::shaped_run shape(std::string_view, const ui::text_style &) override { return {}; }
        float measure(std::string_view, const ui::text_style &) override { return 0.0f; }
        font::text_layout layout(std::string_view, const ui::text_style &, const font::layout_options &) override
        {
            return {};
        }
        void draw(ui::draw_list &, const font::text_layout &, const ui::text_style &, const math::vec2f &, float,
                  const math::vec4f &) override
        {
        }
        void draw(ui::draw_list &, const font::shaped_run &, const ui::text_style &, const math::vec2f &, float,
                  const math::vec4f &) override
        {
        }
        void reset() override { ++resets; }
        int resets = 0;
    };

    void consistent_painting()
    {
        // No overflow: each window painted once.
        {
            fake_text engine;
            ui::shared_text text(engine);
            std::vector<int> painted(3, 0);
            CHECK(ui::paint_consistently(text, 3, [&](std::size_t i) { ++painted[i]; }));
            CHECK(painted == std::vector<int>({1, 1, 1}));
        }

        // The third window's paint resets the atlas (as screen::paint() does
        // on overflow): the two painted before it are painted again, the
        // third not.
        {
            fake_text engine;
            ui::shared_text text(engine);
            std::vector<int> painted(3, 0);
            bool once = true;
            CHECK(ui::paint_consistently(text, 3, [&](std::size_t i) {
                ++painted[i];
                if (i == 2 && once)
                {
                    once = false;
                    text.reset();
                }
            }));
            CHECK(painted == std::vector<int>({2, 2, 1}));
            CHECK(text.resets() == 1 && engine.resets == 1);
        }

        // An atlas that cannot hold a frame: gives up after its rounds.
        {
            fake_text engine;
            ui::shared_text text(engine);
            int paints = 0;
            CHECK(!ui::paint_consistently(
                text, 2,
                [&](std::size_t) {
                    ++paints;
                    text.reset();
                },
                3));
            CHECK(paints <= 2 * 4);
        }
    }
} // namespace

int main()
{
    scale();
    geometry();
    titles();
    web_addresses();
    kinds();
    opening();
    closing();
    closing_while_opening();
    failing();
    exiting();
    consistent_painting();
    return volt::tests::result("app windows");
}
