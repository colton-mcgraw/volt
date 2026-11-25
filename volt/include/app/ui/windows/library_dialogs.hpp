#pragma once

#include <ui/widgets/button.hpp>

#include <functional>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// library dialogs
// -------------------------------------------------------------------------
//
// The small modal questions the library and project code asks: "Save the
// changes to Blinky before exiting?", "Update R1 from its source? Its local
// edits will be lost." A title, a few lines of text and a row of buttons;
// each button runs its action after the dialog has closed. Escape cancels,
// as does closing the window; a press outside the dialog does nothing (the
// question stays until answered).
//
// Also the problems report: a scrolling list of lines under a title, with a
// Close button.

namespace volt::ui
{

    class window_host;

    struct dialog_choice
    {
        std::string text;
        button_kind kind = button_kind::normal;
        std::function<void()> action;
    };

    // Opens the dialog in the host's window. The first primary choice has
    // the focus (Enter takes it), or else the last one. `cancelled` runs
    // when the dialog closes without a choice.
    void open_choice_dialog(window_host &host, std::string title, std::string text,
                            std::vector<dialog_choice> choices, std::function<void()> cancelled = {});

    // A title, a summary line and the lines below it, scrolling.
    void open_report_dialog(window_host &host, std::string title, std::string summary,
                            std::vector<std::string> lines);

} // namespace volt::ui
