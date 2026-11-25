#pragma once

#include <ui/clipboard.hpp>

#include <memory>

// -------------------------------------------------------------------------
// the system clipboard
// -------------------------------------------------------------------------
//
// The library windows copy part numbers for use elsewhere (a distributor's
// search, a spreadsheet), which an in-process clipboard cannot do. On
// Windows this is the system's clipboard, as Unicode text
// (CF_UNICODETEXT, converted from and to UTF-8). Elsewhere, until the
// platform layer has a clipboard, it is one in memory, which says so with
// shared() == false.
//
// app_services owns one (system_clipboard()); a window gives it to its
// screen with screen::set_clipboard() to make its text fields use it too.

namespace volt::ui
{

    std::unique_ptr<clipboard> make_system_clipboard();

    // Whether text copied to `board` reaches other applications.
    bool reaches_other_applications(const clipboard &board) noexcept;

} // namespace volt::ui
