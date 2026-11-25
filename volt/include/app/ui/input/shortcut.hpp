#pragma once

#include <app/ui/input.hpp>
#include <app/ui/input/events.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// -------------------------------------------------------------------------
// shortcuts
// -------------------------------------------------------------------------
//
// A keyboard shortcut is a key plus the modifiers held with it, and its text
// form is what menus show and settings files store: "Ctrl+Shift+S". Keys are
// physical (see <app/ui/input.hpp>), so Ctrl+Z stays where Z is on a US
// layout whatever the layout, and punctuation is named by its US character:
// "Ctrl+=", "Ctrl+-". Keypad keys are "Num0" to "Num9", "Num+", "NumEnter"
// and so on.
//
// Parsing is forgiving (any case, spaces around the +, common aliases such as
// "Esc" or "Control"); formatting always produces the canonical spelling,
// modifiers in the order Ctrl, Shift, Alt, Super.

namespace volt::ui
{

    struct shortcut
    {
        modifiers mods = modifiers::none; // only held_modifiers count
        ui::key key = ui::key::unknown;

        friend constexpr bool operator==(const shortcut &a, const shortcut &b) noexcept
        {
            return a.key == b.key && (a.mods & held_modifiers) == (b.mods & held_modifiers);
        }

        // The event presses (or repeats) this shortcut: same key, same held
        // modifiers, Caps Lock and Num Lock ignored. A caller that does not
        // want a held shortcut to fire again checks for action::repeat.
        constexpr bool matches(const key_event &e) const noexcept
        {
            return e.action != action::release && *this == shortcut{e.mods, e.key};
        }
    };

    namespace detail
    {
        struct key_name
        {
            ui::key key;
            std::string_view name;
        };

        // The first name listed for a key is its canonical one; the rest are
        // accepted when parsing. Letters and digits are handled apart.
        inline constexpr key_name key_names[] = {
            {key::f1, "F1"},
            {key::f2, "F2"},
            {key::f3, "F3"},
            {key::f4, "F4"},
            {key::f5, "F5"},
            {key::f6, "F6"},
            {key::f7, "F7"},
            {key::f8, "F8"},
            {key::f9, "F9"},
            {key::f10, "F10"},
            {key::f11, "F11"},
            {key::f12, "F12"},
            {key::escape, "Escape"},
            {key::escape, "Esc"},
            {key::enter, "Enter"},
            {key::enter, "Return"},
            {key::tab, "Tab"},
            {key::backspace, "Backspace"},
            {key::space, "Space"},
            {key::insert, "Insert"},
            {key::insert, "Ins"},
            {key::del, "Delete"},
            {key::del, "Del"},
            {key::home, "Home"},
            {key::end, "End"},
            {key::page_up, "PageUp"},
            {key::page_up, "PgUp"},
            {key::page_down, "PageDown"},
            {key::page_down, "PgDn"},
            {key::left, "Left"},
            {key::right, "Right"},
            {key::up, "Up"},
            {key::down, "Down"},
            {key::minus, "-"},
            {key::minus, "Minus"},
            {key::equal, "="},
            {key::equal, "Equal"},
            {key::left_bracket, "["},
            {key::right_bracket, "]"},
            {key::backslash, "\\"},
            {key::semicolon, ";"},
            {key::apostrophe, "'"},
            {key::grave, "`"},
            {key::comma, ","},
            {key::comma, "Comma"},
            {key::period, "."},
            {key::period, "Period"},
            {key::slash, "/"},
            {key::world, "World"},
            {key::caps_lock, "CapsLock"},
            {key::scroll_lock, "ScrollLock"},
            {key::num_lock, "NumLock"},
            {key::print_screen, "PrintScreen"},
            {key::pause, "Pause"},
            {key::menu, "Menu"},
            {key::left_shift, "LeftShift"},
            {key::right_shift, "RightShift"},
            {key::left_control, "LeftCtrl"},
            {key::right_control, "RightCtrl"},
            {key::left_alt, "LeftAlt"},
            {key::right_alt, "RightAlt"},
            {key::left_super, "LeftSuper"},
            {key::right_super, "RightSuper"},
            {key::kp_decimal, "Num."},
            {key::kp_divide, "Num/"},
            {key::kp_multiply, "Num*"},
            {key::kp_subtract, "Num-"},
            {key::kp_add, "Num+"},
            {key::kp_enter, "NumEnter"},
        };

        struct modifier_name
        {
            modifiers mod;
            std::string_view name;
        };

        // In formatting order; the first name of each is canonical.
        inline constexpr modifier_name modifier_names[] = {
            {modifiers::control, "Ctrl"},  {modifiers::control, "Control"}, {modifiers::shift, "Shift"},
            {modifiers::alt, "Alt"},       {modifiers::alt, "Option"},      {modifiers::super, "Super"},
            {modifiers::super, "Win"},     {modifiers::super, "Meta"},      {modifiers::super, "Cmd"},
        };

        constexpr char lower(char c) noexcept
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        constexpr bool same_text(std::string_view a, std::string_view b) noexcept
        {
            return std::ranges::equal(a, b, [](char x, char y) { return lower(x) == lower(y); });
        }

        constexpr std::string_view trim(std::string_view text) noexcept
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
                text.remove_prefix(1);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
                text.remove_suffix(1);
            return text;
        }

        constexpr std::optional<modifiers> modifier_from_name(std::string_view name) noexcept
        {
            for (const auto &m : modifier_names)
                if (same_text(m.name, name))
                    return m.mod;
            return std::nullopt;
        }
    } // namespace detail

    // The canonical name of a key ("A", "F5", "PageUp", "="), or "" for
    // key::unknown.
    inline std::string key_name(key k)
    {
        const auto i = static_cast<int>(k);
        if (k >= key::a && k <= key::z)
            return std::string(1, static_cast<char>('A' + (i - static_cast<int>(key::a))));
        if (k >= key::digit_0 && k <= key::digit_9)
            return std::string(1, static_cast<char>('0' + (i - static_cast<int>(key::digit_0))));
        if (k >= key::kp_0 && k <= key::kp_9)
            return "Num" + std::string(1, static_cast<char>('0' + (i - static_cast<int>(key::kp_0))));

        for (const auto &entry : detail::key_names)
            if (entry.key == k)
                return std::string(entry.name);
        return {};
    }

    // The key a name stands for, in any case and with any alias.
    inline std::optional<key> key_from_name(std::string_view name) noexcept
    {
        if (name.size() == 1)
        {
            const char c = detail::lower(name[0]);
            if (c >= 'a' && c <= 'z')
                return static_cast<key>(static_cast<int>(key::a) + (c - 'a'));
            if (c >= '0' && c <= '9')
                return static_cast<key>(static_cast<int>(key::digit_0) + (c - '0'));
        }
        if (name.size() == 4 && detail::same_text(name.substr(0, 3), "Num") && name[3] >= '0' && name[3] <= '9')
            return static_cast<key>(static_cast<int>(key::kp_0) + (name[3] - '0'));

        for (const auto &entry : detail::key_names)
            if (detail::same_text(entry.name, name))
                return entry.key;
        return std::nullopt;
    }

    // "Ctrl+Shift+S". Modifiers are taken off the front for as long as the
    // text before the next + names one, and what remains is the key, which
    // is how "Ctrl+Num+" works.
    inline std::expected<shortcut, std::string> parse_shortcut(std::string_view text)
    {
        shortcut result;
        std::string_view rest = detail::trim(text);

        for (std::size_t plus = rest.find('+'); plus != std::string_view::npos && plus > 0; plus = rest.find('+'))
        {
            const auto mod = detail::modifier_from_name(detail::trim(rest.substr(0, plus)));
            if (!mod)
                break;
            if (has(result.mods, *mod))
                return std::unexpected("shortcut \"" + std::string(text) + "\" names a modifier twice");
            result.mods |= *mod;
            rest = detail::trim(rest.substr(plus + 1));
        }

        if (rest.empty())
            return std::unexpected("shortcut \"" + std::string(text) + "\" has no key");
        if (detail::modifier_from_name(rest))
            return std::unexpected("shortcut \"" + std::string(text) + "\" has only modifiers, no key");

        const auto k = key_from_name(rest);
        if (!k)
            return std::unexpected("shortcut \"" + std::string(text) + "\": unknown key \"" + std::string(rest) + "\"");
        if (modifier_of(*k) != modifiers::none)
            return std::unexpected("shortcut \"" + std::string(text) + "\": a modifier key cannot be the key");

        result.key = *k;
        return result;
    }

    // The canonical text of a shortcut, which parse_shortcut() reads back.
    inline std::string to_string(const shortcut &s)
    {
        std::string text;
        modifiers written = modifiers::none;
        for (const auto &m : detail::modifier_names)
        {
            if (has(s.mods, m.mod) && !has(written, m.mod))
            {
                text += m.name;
                text += '+';
                written |= m.mod;
            }
        }
        return text + key_name(s.key);
    }

    // ---------------------------------------------------------------------
    // shortcut_table
    // ---------------------------------------------------------------------
    //
    // Which command each shortcut runs. Commands are named by id ("file.save");
    // what an id does is up to whoever looks it up. A shortcut runs one
    // command; a command may have several shortcuts.

    class shortcut_table
    {
    public:
        struct binding
        {
            ui::shortcut shortcut;
            std::string command;
        };

        // Binds `s` to `command`, replacing what it was bound to before.
        void bind(const shortcut &s, std::string command)
        {
            for (auto &b : m_bindings)
            {
                if (b.shortcut == s)
                {
                    b.command = std::move(command);
                    return;
                }
            }
            m_bindings.push_back({s, std::move(command)});
        }

        // The same, from text; fails if the text is not a shortcut.
        std::expected<void, std::string> bind(std::string_view text, std::string command)
        {
            auto s = parse_shortcut(text);
            if (!s)
                return std::unexpected(std::move(s).error());
            bind(*s, std::move(command));
            return {};
        }

        void unbind(const shortcut &s)
        {
            std::erase_if(m_bindings, [&](const binding &b) { return b.shortcut == s; });
        }

        // The command bound to `s`, if any.
        std::optional<std::string_view> find(const shortcut &s) const noexcept
        {
            for (const auto &b : m_bindings)
                if (b.shortcut == s)
                    return b.command;
            return std::nullopt;
        }

        // The command a key event runs: a press or repeat of a bound shortcut.
        std::optional<std::string_view> match(const key_event &e) const noexcept
        {
            if (e.action == action::release)
                return std::nullopt;
            return find(shortcut{e.mods, e.key});
        }

        // Every shortcut bound to `command`, in the order bound, for showing
        // next to it in a menu.
        std::vector<shortcut> shortcuts_for(std::string_view command) const
        {
            std::vector<shortcut> result;
            for (const auto &b : m_bindings)
                if (b.command == command)
                    result.push_back(b.shortcut);
            return result;
        }

        const std::vector<binding> &bindings() const noexcept { return m_bindings; }

    private:
        std::vector<binding> m_bindings;
    };

} // namespace volt::ui
