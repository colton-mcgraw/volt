#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <string>

// -------------------------------------------------------------------------
// interface scale
// -------------------------------------------------------------------------
//
// How big the interface is drawn in a window: pixels per logical unit, the
// scale a ui::screen lays out and paints with (<ui/screen.hpp>). It is the
// product of two numbers:
//
//   * the window's content scale, which the system reports for the monitor
//     the window is on (ui::window::content_scale(), window_scale_event):
//     its DPI over 96 on Windows, so 2.5 at 250% display scaling. It changes
//     when the window moves to another monitor, and differs from window to
//     window when they are on different monitors;
//
//   * the user's scale, an application-wide setting on top (Preferences,
//     --ui-scale), 1 by default: "a little bigger than the system's" without
//     changing the system.
//
// So at the defaults the interface is exactly as big as every other
// application on that monitor, and it follows a window from monitor to
// monitor. Plain arithmetic, tested without a window.

namespace volt::ui
{

    // The user scales Preferences offers.
    inline constexpr std::array<float, 8> user_scales{0.75f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f};

    // What the user scale is kept within, whatever --ui-scale says: past
    // these the interface no longer fits a window, or is unreadably small.
    inline constexpr float min_user_scale = 0.5f;
    inline constexpr float max_user_scale = 3.0f;

    // A user scale within bounds; nonsense (zero, negative, NaN) is 1.
    constexpr float clamp_user_scale(float user) noexcept
    {
        if (!(user > 0.0f))
            return 1.0f;
        return user < min_user_scale ? min_user_scale : user > max_user_scale ? max_user_scale : user;
    }

    // The scale a window's screen gets. A content scale that is not a
    // positive number (a closed window reports 1, but be safe) counts as 1.
    constexpr float effective_scale(float content, float user) noexcept
    {
        const float display = content > 0.0f ? content : 1.0f;
        return display * clamp_user_scale(user);
    }

    // The entry of user_scales nearest to `user`: what Preferences shows
    // selected when the scale came from the command line.
    constexpr std::size_t nearest_user_scale(float user) noexcept
    {
        std::size_t best = 0;
        float best_distance = -1.0f;
        for (std::size_t i = 0; i < user_scales.size(); ++i)
        {
            const float d = user_scales[i] > user ? user_scales[i] - user : user - user_scales[i];
            if (best_distance < 0.0f || d < best_distance)
            {
                best = i;
                best_distance = d;
            }
        }
        return best;
    }

    // A scale as a whole percentage: 1.25 is 125.
    inline int scale_percent(float scale) noexcept
    {
        return static_cast<int>(std::lround(scale * 100.0f));
    }

    // "125%".
    inline std::string scale_text(float scale)
    {
        return std::format("{}%", scale_percent(scale));
    }

    // What the status bar says about a window's scale: the user's setting,
    // and what it comes to on this window's monitor. "100% (250%)": 100% of
    // the system's size, which is 250% here.
    inline std::string describe_scale(float content, float user)
    {
        return std::format("{} ({})", scale_text(clamp_user_scale(user)), scale_text(effective_scale(content, user)));
    }

    // The pixel size of something `logical` units square at `scale`, at
    // least one pixel: what a bitmap shown at that size is rasterized at.
    inline int scaled_pixels(float logical, float scale) noexcept
    {
        const long pixels = std::lround(logical * scale);
        return pixels < 1 ? 1 : static_cast<int>(pixels);
    }

} // namespace volt::ui
