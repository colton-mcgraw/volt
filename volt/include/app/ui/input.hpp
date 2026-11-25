#pragma once

#include <cstddef>

namespace volt::ui
{

    enum class action
    {
        release,
        press,
        repeat, // the key is being held down; keyboard only
    };

    enum class mouse_button
    {
        left,
        right,
        middle,
        back,
        forward,
    };

    inline constexpr std::size_t mouse_button_count = static_cast<std::size_t>(mouse_button::forward) + 1;

    // Bit set of mouse buttons, one bit per mouse_button: which ones are held.
    enum class mouse_buttons : unsigned
    {
        none = 0,
    };

    constexpr mouse_buttons mask_of(mouse_button button) noexcept
    {
        return static_cast<mouse_buttons>(1u << static_cast<unsigned>(button));
    }

    constexpr mouse_buttons operator|(mouse_buttons a, mouse_buttons b) noexcept
    {
        return static_cast<mouse_buttons>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
    }

    constexpr mouse_buttons operator&(mouse_buttons a, mouse_buttons b) noexcept
    {
        return static_cast<mouse_buttons>(static_cast<unsigned>(a) & static_cast<unsigned>(b));
    }

    constexpr mouse_buttons operator~(mouse_buttons a) noexcept
    {
        return static_cast<mouse_buttons>(~static_cast<unsigned>(a));
    }

    // True if `button` is in the set.
    constexpr bool has(mouse_buttons set, mouse_button button) noexcept
    {
        return (set & mask_of(button)) != mouse_buttons::none;
    }

    // Bit set of the modifiers that were active when an input event happened.
    enum class modifiers : unsigned
    {
        none = 0,
        shift = 1u << 0,
        control = 1u << 1,
        alt = 1u << 2,
        super = 1u << 3, // Windows key / Meta
        caps_lock = 1u << 4,
        num_lock = 1u << 5,
    };

    constexpr modifiers operator|(modifiers a, modifiers b) noexcept
    {
        return static_cast<modifiers>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
    }

    constexpr modifiers operator&(modifiers a, modifiers b) noexcept
    {
        return static_cast<modifiers>(static_cast<unsigned>(a) & static_cast<unsigned>(b));
    }

    constexpr modifiers operator~(modifiers a) noexcept
    {
        return static_cast<modifiers>(~static_cast<unsigned>(a));
    }

    constexpr modifiers &operator|=(modifiers &a, modifiers b) noexcept
    {
        return a = a | b;
    }

    constexpr modifiers &operator&=(modifiers &a, modifiers b) noexcept
    {
        return a = a & b;
    }

    // The modifiers that are held rather than toggled: the ones a shortcut can
    // name. Caps Lock and Num Lock are states, and a shortcut ignores them.
    inline constexpr modifiers held_modifiers =
        modifiers::shift | modifiers::control | modifiers::alt | modifiers::super;

    // True if `set` contains every modifier in `wanted`.
    constexpr bool has(modifiers set, modifiers wanted) noexcept
    {
        return (set & wanted) == wanted;
    }

    // Keys are physical: each name is the key at that position on a US layout,
    // whatever the active layout prints on it. That is what shortcuts and
    // tool bindings want; for text, use the character callback instead.
    enum class key
    {
        unknown,

        // clang-format off
        a, b, c, d, e, f, g, h, i, j, k, l, m,
        n, o, p, q, r, s, t, u, v, w, x, y, z,

        digit_0, digit_1, digit_2, digit_3, digit_4,
        digit_5, digit_6, digit_7, digit_8, digit_9,

        f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12,

        escape, enter, tab, backspace, space,
        insert, del, home, end, page_up, page_down,
        left, right, up, down,

        minus, equal, left_bracket, right_bracket, backslash,
        semicolon, apostrophe, grave, comma, period, slash,
        world, // the extra key next to left shift on ISO keyboards

        caps_lock, scroll_lock, num_lock, print_screen, pause, menu,

        left_shift, right_shift, left_control, right_control,
        left_alt, right_alt, left_super, right_super,

        kp_0, kp_1, kp_2, kp_3, kp_4, kp_5, kp_6, kp_7, kp_8, kp_9,
        kp_decimal, kp_divide, kp_multiply, kp_subtract, kp_add, kp_enter,
        // clang-format on
    };

    // Number of keys, unknown included, for tables indexed by key. kp_enter
    // has to stay the last one.
    inline constexpr std::size_t key_count = static_cast<std::size_t>(key::kp_enter) + 1;

    // The modifier a key holds down, or none for every other key.
    constexpr modifiers modifier_of(key k) noexcept
    {
        switch (k)
        {
        case key::left_shift:
        case key::right_shift:
            return modifiers::shift;
        case key::left_control:
        case key::right_control:
            return modifiers::control;
        case key::left_alt:
        case key::right_alt:
            return modifiers::alt;
        case key::left_super:
        case key::right_super:
            return modifiers::super;
        default:
            return modifiers::none;
        }
    }

} // namespace volt::ui
