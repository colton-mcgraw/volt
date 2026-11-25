#pragma once

#include <app/ui/input.hpp>

#include <iterator>

namespace platform
{

    // Maps a PC "set 1" scancode to a key. Both backends start from it: Win32
    // reports these codes directly, and Linux evdev key codes are the same
    // numbers for this range. Keys outside it (the navigation cluster, the
    // right-hand modifiers, ...) are numbered differently on each platform and
    // are handled by the backends.
    constexpr volt::ui::key key_from_set1(unsigned code) noexcept
    {
        using enum volt::ui::key;

        // clang-format off
        constexpr volt::ui::key table[] = {
            /* 0x00 */ unknown, escape, digit_1, digit_2, digit_3, digit_4, digit_5, digit_6,
            /* 0x08 */ digit_7, digit_8, digit_9, digit_0, minus, equal, backspace, tab,
            /* 0x10 */ q, w, e, r, t, y, u, i,
            /* 0x18 */ o, p, left_bracket, right_bracket, enter, left_control, a, s,
            /* 0x20 */ d, f, g, h, j, k, l, semicolon,
            /* 0x28 */ apostrophe, grave, left_shift, backslash, z, x, c, v,
            /* 0x30 */ b, n, m, comma, period, slash, right_shift, kp_multiply,
            /* 0x38 */ left_alt, space, caps_lock, f1, f2, f3, f4, f5,
            /* 0x40 */ f6, f7, f8, f9, f10, num_lock, scroll_lock, kp_7,
            /* 0x48 */ kp_8, kp_9, kp_subtract, kp_4, kp_5, kp_6, kp_add, kp_1,
            /* 0x50 */ kp_2, kp_3, kp_0, kp_decimal, unknown, unknown, world, f11,
            /* 0x58 */ f12,
        };
        // clang-format on

        return code < std::size(table) ? table[code] : unknown;
    }

} // namespace platform
