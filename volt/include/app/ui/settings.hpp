#pragma once

#include <cstdint>
#include <string_view>

// -------------------------------------------------------------------------
// the application's settings
// -------------------------------------------------------------------------
//
// What the user chooses for the whole application, in one value: every
// window follows it, and the Preferences dialog
// (<app/ui/windows/preferences.hpp>) is where it is changed. app_services
// holds the one copy (settings(), set_settings()), on the interface thread.
//
// Adding a setting:
//
//   1. a member here, with the default that suits a first run;
//   2. where it takes effect: whoever uses it reads app_services::settings()
//      when it needs the value, or listens to settings_changed; what has to
//      be pushed into every open window goes in
//      window_manager::settings_changed();
//   3. a row on a page of the Preferences dialog (or a new page), which
//      changes it through app_services::change_settings().
//
// Nothing writes the settings to disk yet: they last for the session and
// start from the command line (--ui-scale, --hdr). Keeping them in one
// plain value, compared with ==, is what will make saving them a matter of
// writing this struct out when it changes.

namespace volt::ui
{

    // What a window sends to its display.
    enum class color_output : std::uint8_t
    {
        // 8 bit sRGB, always. Right on every display, HDR ones included:
        // the system converts it.
        sdr,

        // On a monitor that is in HDR mode, an scRGB or HDR10 swapchain,
        // with colors brighter than SDR white; sRGB on the others. Not the
        // default, because a driver offering an HDR color space is no
        // promise that it delivers one (see volt_app::set_extended_color()).
        hdr,
    };

    constexpr std::string_view color_output_name(color_output output) noexcept
    {
        return output == color_output::hdr ? "HDR" : "SDR";
    }

    struct app_settings
    {
        // ---- appearance --------------------------------------------------

        // 0 dark, 1 light.
        int theme = 0;

        // The user's multiplier on each display's scale (<app/ui/scale.hpp>).
        float user_scale = 1.0f;

        // ---- display -----------------------------------------------------

        color_output output = color_output::sdr;

        friend bool operator==(const app_settings &, const app_settings &) = default;
    };

} // namespace volt::ui
