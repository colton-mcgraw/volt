#pragma once

#include <app/ui/input.hpp>
#include <platform/linux/window.hpp>

namespace platform::wayland
{

    // Starts listening to a seat (a set of input devices; there is normally
    // exactly one) and takes ownership of it. The returned object must stay at
    // its address: the listeners point at it.
    input *create_input(wl_seat *seat);

    // Accepts null.
    void destroy_input(input *input);

    // Drops any focus on `window`. Must be called before a window's surface is
    // destroyed. Accepts a null input.
    void forget_window(input *input, const native_window &window);

} // namespace platform::wayland
