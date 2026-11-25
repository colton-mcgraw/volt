#pragma once

#include <app/ui/windows/content.hpp>

// -------------------------------------------------------------------------
// the About dialog
// -------------------------------------------------------------------------
//
// Help > About Volt (F1, the help.about command) in every window: the logo,
// the name and a line about the program, modal over the window it was
// opened from. Its logo follows the window's scale while it is open
// (window_host::track_logo()). Interface thread.

namespace volt::ui
{

    void open_about(window_host &host);

} // namespace volt::ui
