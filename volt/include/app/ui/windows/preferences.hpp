#pragma once

#include <app/ui/windows/content.hpp>

// -------------------------------------------------------------------------
// the Preferences dialog
// -------------------------------------------------------------------------
//
// File > Preferences (Ctrl+,, the file.preferences command) in every
// window: the settings that are application wide, so a change made in one
// window shows in all of them at once (<app/ui/settings.hpp>,
// app_services::change_settings()). One tab per group of settings:
//
//   Appearance
//     Theme             Dark or Light
//     Interface scale   the user's multiplier on the display's scale, one
//                       of user_scales (<app/ui/scale.hpp>), with what that
//                       comes to on this window's monitor
//   Display
//     Color output      SDR, or HDR where the display is in HDR mode, with
//                       what this window's monitor is doing and what the
//                       window sends it
//
// A new setting is a row on one of the pages, a new group a page of its
// own: see the table of pages in preferences.cpp, and the steps in
// <app/ui/settings.hpp>.
//
// Modal over the window it was opened from; it follows changes made
// elsewhere while open (app_services::settings_changed), the window moving
// to a monitor of another scale (window_host::scale_changed) and its
// monitor changing mode (window_host::display_changed). Interface thread.

namespace volt::ui
{

    void open_preferences(window_host &host);

} // namespace volt::ui
