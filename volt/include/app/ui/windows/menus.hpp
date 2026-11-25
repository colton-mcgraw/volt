#pragma once

#include <app/ui/window_book.hpp>
#include <app/ui/windows/content.hpp>
#include <ui/widgets/bars.hpp>
#include <ui/widgets/menu.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// menus, and the commands every window has
// -------------------------------------------------------------------------
//
// Helpers for the windows' menus and toolbars, and the frame's own menus.
//
// Commands every window has. The window manager registers them on every
// window's screen before the content's build() runs; a content must not
// register these ids itself, except file.open, which a content may replace
// with one that does more (the project manager's falls back to a typed path
// where the system has no file dialog):
//
//   file.open          Ctrl+O      the system's file dialog; the files open as
//                                  if dropped on the window (app_services::
//                                  open_with_dialog()). First in File, unless
//                                  the content lists it itself
//   file.preferences   Ctrl+,      the Preferences dialog (theme, scale)
//   file.close         Ctrl+W      closes this window (the project manager's
//                                  closes the application)
//   file.exit          Ctrl+Q      closes the application
//   window.project     Ctrl+1      opens or raises the Project Manager
//   window.schematic   Ctrl+2      ... the Schematic Editor
//   window.board       Ctrl+3      ... the Board Editor
//   window.library     Ctrl+4      ... the Library Browser
//   help.about         F1          the About dialog
//   view.menu          F10         opens the menu bar
//
// Everything else is the window's own: give its ids a prefix that says
// what they act on ("schematic.", "board.", "library.") or the menu they
// sit in ("edit.", "view."); ids are per screen, so two windows may use
// the same id for their own versions of a command.

namespace volt::ui
{

    // "window.schematic": the command that opens or raises a window.
    std::string window_command(window_kind kind);

    // An item that runs a screen command, showing its shortcut.
    menu_item command_item(std::string text, std::string command, ui::icon icon = icon::none);

    // An item that runs a function; `checked`, if given, puts a check mark
    // by it while true.
    menu_item action_item(std::string text, std::function<void()> action, std::function<bool()> checked = {});

    // An item that opens a submenu.
    menu_item submenu_item(std::string text, std::vector<menu_item> items);

    // The Window menu: every kind of window, checked while open.
    std::vector<menu_item> window_menu(app_services &services);

    // ---- for the window manager ------------------------------------------------------

    // Registers the commands above on the host's screen, with their
    // shortcuts.
    void add_standard_commands(window_host &host);

    // The menu bar strip: File (Open..., layout.file_items, Preferences,
    // Close Window, Exit), layout.menus, Window, Help. Tells the host where the
    // bar is (for F10).
    std::unique_ptr<widget> build_menu_bar(window_host &host, window_layout &layout);

    // ---- for toolbars --------------------------------------------------------------------

    // The brand at the start of a toolbar: the logo, kept sharp at the
    // window's scale, and the name.
    void add_brand(toolbar &bar, window_host &host);

    // A toolbar button that runs a screen command of the host's.
    button &add_command_tool(toolbar &bar, window_host &host, ui::icon icon, std::string tooltip,
                             std::string command, std::string text = {});

} // namespace volt::ui
