#include "harness.hpp"

#include <ui/layout/box.hpp>
#include <ui/widgets/numeric_field.hpp>
#include <ui/widgets/text_field.hpp>

#include <string>

// Editing a line of text: typing, caret movement by character and word,
// selection with Shift and with the mouse, deleting, the clipboard through
// the injected interface, scrolling to keep the caret in view; and the
// numeric field on top of it, with engineering notation.

using namespace volt::tests::ui;

namespace
{
    // A clipboard the test can look into.
    class test_clipboard final : public clipboard
    {
    public:
        std::string content;
        int writes = 0;
        std::string text() const override { return content; }
        void set_text(std::string t) override
        {
            content = std::move(t);
            ++writes;
        }
    };

    struct fixture
    {
        harness h{{400, 100}};
        test_clipboard board;
        text_field *field = nullptr;
        std::string last_change;
        int changes = 0;

        explicit fixture(std::string text = {})
        {
            h.screen->set_clipboard(&board);
            auto &root = h.screen->emplace_root<column>(0.0f, insets::uniform(10.0f));
            root.set_cross_align(align::start);
            field = &root.emplace<text_field>(std::move(text));
            field->layout().width = 200.0f;
            field->changed.connect([this](const std::string &t) {
                last_change = t;
                ++changes;
            });
            h.screen->layout();
            field->focus();
        }
    };

    void typing_and_deleting()
    {
        fixture f;
        f.h.type("hello world");
        CHECK(f.field->text() == "hello world");
        CHECK(f.field->caret() == 11);
        CHECK(f.changes == 11);
        CHECK(f.last_change == "hello world");

        f.h.key(key::backspace);
        CHECK(f.field->text() == "hello worl");
        f.h.key(key::home);
        CHECK(f.field->caret() == 0);
        f.h.key(key::del);
        CHECK(f.field->text() == "ello worl");

        // Ctrl+Backspace and Ctrl+Delete take words.
        f.h.key(key::end);
        f.h.key(key::backspace, modifiers::control);
        CHECK(f.field->text() == "ello ");
        f.h.key(key::home);
        f.h.key(key::del, modifiers::control);
        CHECK(f.field->text() == "");

        // UTF-8: one step per code point.
        f.h.type("a\xC3\xA9z"); // aéz
        CHECK(f.field->text().size() == 4);
        f.h.key(key::left);
        f.h.key(key::left);
        CHECK(f.field->caret() == 1);
        f.h.key(key::del);
        CHECK(f.field->text() == "az");
    }

    void words_and_selection()
    {
        fixture f("one two  three");
        f.h.key(key::home);
        f.h.key(key::right, modifiers::control);
        CHECK(f.field->caret() == 4); // after "one "
        f.h.key(key::right, modifiers::control);
        CHECK(f.field->caret() == 9); // after "two  "
        f.h.key(key::left, modifiers::control);
        CHECK(f.field->caret() == 4);

        // Shift extends; plain Left collapses to the selection's start.
        f.h.key(key::right, modifiers::shift | modifiers::control);
        CHECK(f.field->selected_text() == "two  ");
        f.h.key(key::left);
        CHECK(!f.field->has_selection());
        CHECK(f.field->caret() == 4);

        f.h.key(key::end, modifiers::shift);
        CHECK(f.field->selected_text() == "two  three");

        // Typing replaces the selection.
        f.h.type("2");
        CHECK(f.field->text() == "one 2");

        f.h.key(key::a, modifiers::control);
        CHECK(f.field->selected_text() == "one 2");
    }

    void clipboard_round_trip()
    {
        fixture f("copy me");
        f.h.key(key::a, modifiers::control);
        f.h.key(key::c, modifiers::control);
        CHECK(f.board.content == "copy me");

        f.h.key(key::end);
        f.h.key(key::v, modifiers::control);
        CHECK(f.field->text() == "copy mecopy me");

        f.h.key(key::home);
        f.h.key(key::right, modifiers::shift | modifiers::control); // "copy "
        f.h.key(key::x, modifiers::control);
        CHECK(f.board.content == "copy ");
        CHECK(f.field->text() == "mecopy me");

        // Pasted line breaks become spaces; a filter drops what it refuses.
        f.board.content = "a\nb";
        f.h.key(key::end);
        f.h.key(key::v, modifiers::control);
        CHECK(f.field->text() == "mecopy mea b");

        f.field->set_filter([](char32_t c) { return c >= U'0' && c <= U'9'; });
        f.h.type("x7y");
        CHECK(f.field->text() == "mecopy mea b7");

        // Copy with nothing selected leaves the clipboard alone.
        const int writes = f.board.writes;
        f.h.key(key::c, modifiers::control);
        CHECK(f.board.writes == writes);
    }

    void mouse_selection()
    {
        fixture f("alpha beta gamma");
        const rect b = f.field->bounds();
        const int y = static_cast<int>(b.center().y());
        const int left = static_cast<int>(b.x + 6.0f);

        // A click at the far left puts the caret at the start.
        f.h.click(left, y);
        CHECK(f.field->caret() == 0);

        // Dragging to the right end selects everything.
        f.h.drag(left, y, static_cast<int>(b.right()) - 2, y);
        CHECK(f.field->selection().first == 0);
        CHECK(f.field->selection().second == f.field->text().size());

        // A double click selects a word.
        const float beta_x = f.h.text.measure("alpha be", f.h.screen->theme().body_style());
        f.h.double_click(static_cast<int>(b.x + f.h.screen->theme().control_padding * 0.7f + beta_x), y);
        CHECK(f.field->selected_text() == "beta");

        // Shift+click extends from the caret.
        f.h.key(key::home);
        f.h.move(static_cast<int>(b.right()) - 2, y);
        f.h.press(mouse_button::left, modifiers::shift);
        f.h.release(mouse_button::left, modifiers::shift);
        CHECK(f.field->selected_text() == "alpha beta gamma");
    }

    void scrolling_keeps_the_caret_visible()
    {
        fixture f;
        std::string long_text;
        for (int i = 0; i < 12; ++i)
            long_text += "word" + std::to_string(i) + " ";
        f.h.type(long_text);
        // The caret is at the end, so the text has scrolled left.
        CHECK(f.field->scroll() > 0.0f);
        f.h.key(key::home);
        CHECK(f.field->scroll() == 0.0f);
        f.h.key(key::end);
        CHECK(f.field->scroll() > 0.0f);

        // And it draws clipped to the field: no glyph outside it.
        const draw_list &list = f.h.frame();
        const rect b = f.field->bounds();
        for (const draw_item &item : list.items())
        {
            if (item.kind != shape_kind::glyph)
                continue;
            const rect clip = list.clips()[item.clip];
            CHECK(clip.x >= b.x - 1.0f);
            CHECK(clip.right() <= b.right() + 1.0f);
        }
    }

    void submit_and_cancel()
    {
        fixture f("x");
        std::string submitted;
        int cancels = 0;
        f.field->submitted.connect([&](const std::string &t) { submitted = t; });
        f.field->cancelled.connect([&] { ++cancels; });
        f.h.key(key::enter);
        CHECK(submitted == "x");
        f.h.key(key::escape);
        CHECK(cancels == 1);
    }

    void numeric_engineering()
    {
        harness h({400, 100});
        auto &root = h.screen->emplace_root<column>(0.0f, insets::uniform(10.0f));
        root.set_cross_align(align::start);
        auto &r = root.emplace<numeric_field>(4700.0, 1.0, 1.0e7, 0.0);
        r.set_engineering("\xCE\xA9"); // Ω
        r.layout().width = 160.0f;
        auto &plain = root.emplace<numeric_field>(1.0, 0.25, 10.0, 0.25);
        plain.set_decimals(2);
        h.screen->layout();

        CHECK(r.text() == "4.7k\xCE\xA9");

        double seen = 0.0;
        r.value_changed.connect([&](double v) { seen = v; });

        // Typed the RKM way, read on Enter.
        r.focus(focus_reason::keyboard); // selects all
        h.type("2k2");
        h.key(key::enter);
        CHECK(r.value() == 2200.0);
        CHECK(seen == 2200.0);
        CHECK(r.text() == "2.2k\xCE\xA9");

        // A prefix and a unit, with a space.
        r.select_all();
        h.type("100 \xCE\xA9");
        h.key(key::enter);
        CHECK(r.value() == 100.0);

        // Nonsense puts the value back.
        r.select_all();
        h.type("lots");
        h.key(key::enter);
        CHECK(r.value() == 100.0);
        CHECK(r.text() == "100\xCE\xA9");

        // Up steps through the E12 values.
        h.key(key::up);
        CHECK(r.value() == 120.0);
        h.key(key::down);
        h.key(key::down);
        CHECK(r.value() == 82.0);

        // Leaving the field reads it too, and clamps.
        r.select_all();
        h.type("99M");
        plain.focus();
        CHECK(r.value() == 1.0e7);

        // Plain steps, clamped to the range.
        h.key(key::up);
        CHECK(plain.value() == 1.25);
        for (int i = 0; i < 100; ++i)
            h.key(key::up);
        CHECK(plain.value() == 10.0);
        CHECK(plain.text() == "10.00");

        // The arrows step too.
        const rect b = plain.bounds();
        h.click(static_cast<int>(b.right()) - 5, static_cast<int>(b.bottom()) - 4);
        CHECK(plain.value() == 9.75);
    }
} // namespace

int main()
{
    typing_and_deleting();
    words_and_selection();
    clipboard_round_trip();
    mouse_selection();
    scrolling_keeps_the_caret_visible();
    submit_and_cancel();
    numeric_engineering();
    return volt::tests::result("ui text field");
}
