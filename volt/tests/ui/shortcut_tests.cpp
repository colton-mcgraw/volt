#include "../math/check.hpp"

#include <app/ui/input/shortcut.hpp>

#include <cstddef>
#include <string>
#include <string_view>

using namespace volt;
using namespace volt::ui;

namespace
{
    bool parses_to(std::string_view text, shortcut expected)
    {
        const auto parsed = parse_shortcut(text);
        return parsed && *parsed == expected;
    }

    bool fails(std::string_view text)
    {
        const auto parsed = parse_shortcut(text);
        return !parsed && !parsed.error().empty();
    }

    void parsing()
    {
        CHECK(parses_to("Ctrl+Shift+S", {modifiers::control | modifiers::shift, key::s}));
        CHECK(parses_to("ctrl + shift + s", {modifiers::control | modifiers::shift, key::s}));
        CHECK(parses_to("Shift+Control+s", {modifiers::control | modifiers::shift, key::s}));
        CHECK(parses_to("F5", {modifiers::none, key::f5}));
        CHECK(parses_to("Alt+F4", {modifiers::alt, key::f4}));
        CHECK(parses_to("Cmd+Option+Esc", {modifiers::super | modifiers::alt, key::escape}));
        CHECK(parses_to("Ctrl+=", {modifiers::control, key::equal}));
        CHECK(parses_to("Ctrl+-", {modifiers::control, key::minus}));
        CHECK(parses_to("Ctrl+Num+", {modifiers::control, key::kp_add}));
        CHECK(parses_to("Num7", {modifiers::none, key::kp_7}));
        CHECK(parses_to("Ctrl+1", {modifiers::control, key::digit_1}));
        CHECK(parses_to("Ctrl+PgDn", {modifiers::control, key::page_down}));
        CHECK(parses_to("Shift+\\", {modifiers::shift, key::backslash}));

        CHECK(fails(""));
        CHECK(fails("Ctrl+"));
        CHECK(fails("Ctrl"));
        CHECK(fails("Ctrl+Shift"));
        CHECK(fails("Ctrl+Ctrl+S"));
        CHECK(fails("Ctrl+Banana"));
        CHECK(fails("Ctrl++"));
        CHECK(fails("Ctrl+LeftShift"));
        CHECK(fails("+S"));
    }

    void formatting()
    {
        CHECK(to_string({modifiers::shift | modifiers::control, key::s}) == "Ctrl+Shift+S");
        CHECK(to_string({modifiers::super | modifiers::alt | modifiers::shift | modifiers::control, key::f12}) ==
              "Ctrl+Shift+Alt+Super+F12");
        CHECK(to_string({modifiers::none, key::page_up}) == "PageUp");
        CHECK(to_string({modifiers::control, key::kp_add}) == "Ctrl+Num+");
        CHECK(to_string({modifiers::control | modifiers::caps_lock, key::digit_0}) == "Ctrl+0");

        // Every named key survives the round trip, with every held modifier.
        for (std::size_t i = 1; i < key_count; ++i)
        {
            const auto k = static_cast<key>(i);
            if (modifier_of(k) != modifiers::none)
                continue;
            const shortcut s{held_modifiers, k};
            CHECK(!key_name(k).empty());
            CHECK(parses_to(to_string(s), s));
            CHECK(key_from_name(key_name(k)) == k);
        }
    }

    void matching()
    {
        const shortcut save{modifiers::control, key::s};
        key_event e{key::s, 31, action::press, modifiers::control | modifiers::num_lock, false, {}};
        CHECK(save.matches(e)); // locks do not count

        e.action = action::repeat;
        CHECK(save.matches(e));
        e.action = action::release;
        CHECK(!save.matches(e));

        e.action = action::press;
        e.mods = modifiers::control | modifiers::shift;
        CHECK(!save.matches(e)); // extra modifiers do

        shortcut_table table;
        table.bind(save, "file.save");
        CHECK(table.bind("Ctrl+Shift+S", "file.save_as").has_value());
        CHECK(table.bind("F2", "edit.rename").has_value());
        CHECK(table.bind("Ctrl+Shift+S", "file.save_all").has_value()); // replaces
        CHECK(table.bind("Ctrl+Nothing", "x").error().find("Nothing") != std::string::npos);
        CHECK(table.bindings().size() == 3);

        CHECK(table.match(e) == "file.save_all");
        e.mods = modifiers::control | modifiers::caps_lock;
        CHECK(table.match(e) == "file.save");
        e.action = action::release;
        CHECK(!table.match(e));
        e.action = action::press;
        e.key = key::d;
        CHECK(!table.match(e));

        CHECK(table.bind("Ctrl+S", "file.save").has_value());
        CHECK(table.bind("Ctrl+Alt+S", "file.save").has_value());
        const auto shortcuts = table.shortcuts_for("file.save");
        CHECK(shortcuts.size() == 2 && to_string(shortcuts[1]) == "Ctrl+Alt+S");

        table.unbind(save);
        CHECK(!table.find(save));
        CHECK(table.find({modifiers::control | modifiers::alt, key::s}) == "file.save");
    }

} // namespace

int main()
{
    parsing();
    formatting();
    matching();
    return volt::tests::result("ui shortcuts");
}
