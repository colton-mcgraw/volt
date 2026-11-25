#include "input.hpp"

#include "../scancode.hpp"

#include <app/ui/window.hpp>

#include <xkbcommon/xkbcommon.h>

#include <cstdint>
#include <cstring>

#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>

namespace platform::wayland
{
    // Input on Wayland is not addressed to a window. The seat's pointer and
    // keyboard each announce which surface they entered, and every event after
    // that belongs to it until they leave.
    struct input
    {
        wl_seat *seat = nullptr;
        wl_pointer *pointer = nullptr;
        wl_keyboard *keyboard = nullptr;

        // Windows the devices are currently over / typing into.
        native_window *pointer_focus = nullptr;
        native_window *keyboard_focus = nullptr;

        // Last pointer position, relative to pointer_focus.
        volt::math::vec2i cursor{};

        // The compositor sends raw key codes plus the keyboard layout; turning
        // the two into characters and modifiers is the client's job, done with
        // xkbcommon. `keys` tracks which modifiers and layout are active.
        xkb_context *context = nullptr;
        xkb_keymap *keymap = nullptr;
        xkb_state *keys = nullptr;
    };

    namespace
    {
        using volt::math::vec2i;
        using volt::ui::detail::emit;
        using volt::ui::detail::window_data;

        native_window *window_of(wl_surface *surface)
        {
            // Null when the surface was destroyed while the event was in flight.
            return surface != nullptr ? static_cast<native_window *>(wl_surface_get_user_data(surface)) : nullptr;
        }

        volt::ui::modifiers current_modifiers(const input &input)
        {
            using volt::ui::modifiers;

            modifiers result = modifiers::none;
            if (input.keys == nullptr)
                return result;

            const auto active = [&](const char *name) {
                return xkb_state_mod_name_is_active(input.keys, name, XKB_STATE_MODS_EFFECTIVE) > 0;
            };

            if (active(XKB_MOD_NAME_SHIFT))
                result |= modifiers::shift;
            if (active(XKB_MOD_NAME_CTRL))
                result |= modifiers::control;
            if (active(XKB_MOD_NAME_ALT))
                result |= modifiers::alt;
            if (active(XKB_MOD_NAME_LOGO))
                result |= modifiers::super;
            if (active(XKB_MOD_NAME_CAPS))
                result |= modifiers::caps_lock;
            if (active(XKB_MOD_NAME_NUM))
                result |= modifiers::num_lock;
            return result;
        }

        // ---- pointer -----------------------------------------------------

        void on_pointer_enter(void *data, wl_pointer *, std::uint32_t, wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
        {
            auto &input = *static_cast<wayland::input *>(data);

            input.pointer_focus = window_of(surface);
            input.cursor = {wl_fixed_to_int(x), wl_fixed_to_int(y)};

            if (input.pointer_focus != nullptr)
                emit(input.pointer_focus->data->on_enter, input.cursor);
        }

        // With several windows, a leave must only end the visit it belongs
        // to. The compositor sends the leave of one surface before the enter
        // of the next, but a leave for a window closed meanwhile (its surface
        // is null by then; forget_window already cleared the focus) must not
        // clear the focus of a window entered since. Same for the keyboard.
        bool leaves(native_window *focus, wl_surface *surface)
        {
            return focus != nullptr && surface != nullptr && window_of(surface) == focus;
        }

        void on_pointer_leave(void *data, wl_pointer *, std::uint32_t, wl_surface *surface)
        {
            auto &input = *static_cast<wayland::input *>(data);
            if (!leaves(input.pointer_focus, surface))
                return;

            // Copied: the callback may close the window, and the next enter
            // overwrites the input's position.
            const vec2i position = input.cursor;
            native_window *window = std::exchange(input.pointer_focus, nullptr);
            emit(window->data->on_leave, position);
        }

        void on_pointer_motion(void *data, wl_pointer *, std::uint32_t, wl_fixed_t x, wl_fixed_t y)
        {
            auto &input = *static_cast<wayland::input *>(data);

            input.cursor = {wl_fixed_to_int(x), wl_fixed_to_int(y)};
            if (input.pointer_focus != nullptr)
                emit(input.pointer_focus->data->on_cursor, input.cursor);
        }

        void on_pointer_button(void *data, wl_pointer *, std::uint32_t, std::uint32_t, std::uint32_t button,
                               std::uint32_t state)
        {
            auto &input = *static_cast<wayland::input *>(data);
            if (input.pointer_focus == nullptr)
                return;

            // Buttons are Linux input event codes.
            volt::ui::mouse_button translated;
            switch (button)
            {
            case BTN_LEFT: translated = volt::ui::mouse_button::left; break;
            case BTN_RIGHT: translated = volt::ui::mouse_button::right; break;
            case BTN_MIDDLE: translated = volt::ui::mouse_button::middle; break;
            case BTN_SIDE: translated = volt::ui::mouse_button::back; break;
            case BTN_EXTRA: translated = volt::ui::mouse_button::forward; break;
            default: return;
            }

            const auto action = state == WL_POINTER_BUTTON_STATE_PRESSED ? volt::ui::action::press
                                                                         : volt::ui::action::release;
            emit(input.pointer_focus->data->on_mouse, translated, action, current_modifiers(input));
        }

        void on_pointer_axis(void *data, wl_pointer *, std::uint32_t, std::uint32_t axis, wl_fixed_t value)
        {
            auto &input = *static_cast<wayland::input *>(data);
            if (input.pointer_focus == nullptr)
                return;

            // The distance is in surface units, with ten of them to a wheel
            // notch by convention. Positive is down or right; the callback
            // wants up as positive.
            const double notches = wl_fixed_to_double(value) / 10.0;
            if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
                emit(input.pointer_focus->data->on_scroll, 0.0, -notches);
            else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
                emit(input.pointer_focus->data->on_scroll, notches, 0.0);
        }

        // The rest of the pointer events up to the version the seat is bound
        // at (see seat_version in platform.cpp). They refine scrolling and are
        // not needed yet, but libwayland aborts on an event without a handler.
        void on_pointer_frame(void *, wl_pointer *)
        {
        }

        void on_pointer_axis_source(void *, wl_pointer *, std::uint32_t)
        {
        }

        void on_pointer_axis_stop(void *, wl_pointer *, std::uint32_t, std::uint32_t)
        {
        }

        void on_pointer_axis_discrete(void *, wl_pointer *, std::uint32_t, std::int32_t)
        {
        }

        // Assigned member by member because the struct grows with libwayland:
        // events of later versions stay null, and are never sent to a seat
        // bound at an earlier one.
        constexpr wl_pointer_listener pointer_listener = [] {
            wl_pointer_listener listener{};
            listener.enter = on_pointer_enter;
            listener.leave = on_pointer_leave;
            listener.motion = on_pointer_motion;
            listener.button = on_pointer_button;
            listener.axis = on_pointer_axis;
            listener.frame = on_pointer_frame;
            listener.axis_source = on_pointer_axis_source;
            listener.axis_stop = on_pointer_axis_stop;
            listener.axis_discrete = on_pointer_axis_discrete;
            return listener;
        }();

        // ---- keyboard ----------------------------------------------------

        // Key codes are Linux input event codes, which continue the PC
        // scancode table with their own numbers for the keys beyond it.
        volt::ui::key translate_key(std::uint32_t code)
        {
            using enum volt::ui::key;

            switch (code)
            {
            case KEY_KPENTER: return kp_enter;
            case KEY_RIGHTCTRL: return right_control;
            case KEY_KPSLASH: return kp_divide;
            case KEY_SYSRQ: return print_screen;
            case KEY_RIGHTALT: return right_alt;
            case KEY_HOME: return home;
            case KEY_UP: return up;
            case KEY_PAGEUP: return page_up;
            case KEY_LEFT: return left;
            case KEY_RIGHT: return right;
            case KEY_END: return end;
            case KEY_DOWN: return down;
            case KEY_PAGEDOWN: return page_down;
            case KEY_INSERT: return insert;
            case KEY_DELETE: return del;
            case KEY_PAUSE: return pause;
            case KEY_LEFTMETA: return left_super;
            case KEY_RIGHTMETA: return right_super;
            case KEY_COMPOSE: return menu;
            default: return key_from_set1(code);
            }
        }

        // The keyboard layout, as a file descriptor to map and compile.
        void on_keyboard_keymap(void *data, wl_keyboard *, std::uint32_t format, std::int32_t fd, std::uint32_t size)
        {
            auto &input = *static_cast<wayland::input *>(data);

            if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && input.context != nullptr)
            {
                // Since wl_seat version 7 the mapping has to be private.
                void *text = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
                if (text != MAP_FAILED)
                {
                    const auto *characters = static_cast<const char *>(text);
                    xkb_keymap *keymap =
                        xkb_keymap_new_from_buffer(input.context, characters, strnlen(characters, size),
                                                   XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
                    munmap(text, size);

                    if (xkb_state *keys = keymap != nullptr ? xkb_state_new(keymap) : nullptr)
                    {
                        xkb_state_unref(input.keys);
                        xkb_keymap_unref(input.keymap);
                        input.keymap = keymap;
                        input.keys = keys;
                    }
                    else
                    {
                        xkb_keymap_unref(keymap);
                    }
                }
            }

            close(fd);
        }

        void on_keyboard_enter(void *data, wl_keyboard *, std::uint32_t, wl_surface *surface, wl_array *)
        {
            static_cast<wayland::input *>(data)->keyboard_focus = window_of(surface);
        }

        void on_keyboard_leave(void *data, wl_keyboard *, std::uint32_t, wl_surface *surface)
        {
            auto &input = *static_cast<wayland::input *>(data);
            if (leaves(input.keyboard_focus, surface))
                input.keyboard_focus = nullptr;
        }

        void on_keyboard_key(void *data, wl_keyboard *, std::uint32_t, std::uint32_t, std::uint32_t code,
                             std::uint32_t state)
        {
            auto &input = *static_cast<wayland::input *>(data);
            if (input.keyboard_focus == nullptr)
                return;

            // The window object outlives its callbacks even if one of them
            // closes the window; the native_window does not.
            const window_data &window = *input.keyboard_focus->data;
            const bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;

            // xkb key codes are the event codes shifted by 8, for historical
            // reasons. Zero means the key produces no character.
            char32_t codepoint = 0;
            if (pressed && input.keys != nullptr)
                codepoint = xkb_state_key_get_utf32(input.keys, code + 8);

            emit(window.on_key, translate_key(code), static_cast<int>(code),
                 pressed ? volt::ui::action::press : volt::ui::action::release, current_modifiers(input));

            // Control characters (Enter, Backspace, Ctrl+letter) are keys, not text.
            if (codepoint >= 0x20 && codepoint != 0x7F)
                emit(window.on_char, codepoint);
        }

        void on_keyboard_modifiers(void *data, wl_keyboard *, std::uint32_t, std::uint32_t depressed,
                                   std::uint32_t latched, std::uint32_t locked, std::uint32_t group)
        {
            auto &input = *static_cast<wayland::input *>(data);
            if (input.keys != nullptr)
                xkb_state_update_mask(input.keys, depressed, latched, locked, 0, 0, group);
        }

        // Rate and delay for repeating a held key. Repeating is left to the
        // client on Wayland and is not implemented yet.
        void on_keyboard_repeat_info(void *, wl_keyboard *, std::int32_t, std::int32_t)
        {
        }

        constexpr wl_keyboard_listener keyboard_listener{
            .keymap = on_keyboard_keymap,
            .enter = on_keyboard_enter,
            .leave = on_keyboard_leave,
            .key = on_keyboard_key,
            .modifiers = on_keyboard_modifiers,
            .repeat_info = on_keyboard_repeat_info,
        };

        // ---- seat --------------------------------------------------------

        // The release requests tell the compositor to stop sending events;
        // older seats only have the client-side destroy.
        void release(wl_pointer *pointer)
        {
            if (wl_pointer_get_version(pointer) >= WL_POINTER_RELEASE_SINCE_VERSION)
                wl_pointer_release(pointer);
            else
                wl_pointer_destroy(pointer);
        }

        void release(wl_keyboard *keyboard)
        {
            if (wl_keyboard_get_version(keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
                wl_keyboard_release(keyboard);
            else
                wl_keyboard_destroy(keyboard);
        }

        // Devices come and go (a mouse is plugged in, a keyboard removed); the
        // seat reports which kinds it currently has.
        void on_seat_capabilities(void *data, wl_seat *seat, std::uint32_t capabilities)
        {
            auto &input = *static_cast<wayland::input *>(data);

            const bool has_pointer = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;
            if (has_pointer && input.pointer == nullptr)
            {
                input.pointer = wl_seat_get_pointer(seat);
                wl_pointer_add_listener(input.pointer, &pointer_listener, &input);
            }
            else if (!has_pointer && input.pointer != nullptr)
            {
                release(std::exchange(input.pointer, nullptr));
                input.pointer_focus = nullptr;
            }

            const bool has_keyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
            if (has_keyboard && input.keyboard == nullptr)
            {
                input.keyboard = wl_seat_get_keyboard(seat);
                wl_keyboard_add_listener(input.keyboard, &keyboard_listener, &input);
            }
            else if (!has_keyboard && input.keyboard != nullptr)
            {
                release(std::exchange(input.keyboard, nullptr));
                input.keyboard_focus = nullptr;
            }
        }

        void on_seat_name(void *, wl_seat *, const char *)
        {
        }

        constexpr wl_seat_listener seat_listener{
            .capabilities = on_seat_capabilities,
            .name = on_seat_name,
        };
    } // namespace

    input *create_input(wl_seat *seat)
    {
        auto *result = new input;
        result->seat = seat;
        result->context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

        wl_seat_add_listener(seat, &seat_listener, result);
        return result;
    }

    void destroy_input(input *input)
    {
        if (input == nullptr)
            return;

        if (input->pointer != nullptr)
            release(input->pointer);
        if (input->keyboard != nullptr)
            release(input->keyboard);

        if (wl_seat_get_version(input->seat) >= WL_SEAT_RELEASE_SINCE_VERSION)
            wl_seat_release(input->seat);
        else
            wl_seat_destroy(input->seat);

        // The xkb functions accept null.
        xkb_state_unref(input->keys);
        xkb_keymap_unref(input->keymap);
        xkb_context_unref(input->context);

        delete input;
    }

    void forget_window(input *input, const native_window &window)
    {
        if (input == nullptr)
            return;

        if (input->pointer_focus == &window)
            input->pointer_focus = nullptr;
        if (input->keyboard_focus == &window)
            input->keyboard_focus = nullptr;
    }

} // namespace platform::wayland
