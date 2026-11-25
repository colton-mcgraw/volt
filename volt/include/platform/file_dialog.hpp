#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// file dialogs
// -------------------------------------------------------------------------
//
// The system's own dialogs for choosing files: open one, open several, save
// one, choose a folder. Asynchronous: show_file_dialog() returns at once and
// the answer comes to a callback, so that no thread of the application
// waits on the user.
//
//   Windows   the Common Item Dialog (IFileOpenDialog, IFileSaveDialog),
//             run on the platform thread inside the owner's window
//             procedure, modal to the owner: Windows disables it while the
//             dialog is up. The dialog's own message loop goes on
//             dispatching to every window, as Windows' loops for dragging
//             and menus do, and the render thread is not involved, so the
//             windows keep painting. What the platform thread does after
//             poll_events() (the window manager's tasks: opening, raising,
//             closing windows) waits until the dialog is closed.
//   Linux     zenity, or kdialog on KDE (or when zenity is missing), run as
//             a child process from a thread of its own. Not parented: the
//             Wayland backend has no way to name its window to another
//             process (that needs the desktop portal, over D-Bus, which the
//             backend does not speak). Neither installed: unavailable.
//
// Strings are UTF-8 and paths are std::filesystem::path, built from the
// system's own representation (UTF-16 on Windows), so no name is lost to a
// code page.
//
// From a window of the application, go through app_services
// (<app/ui/services.hpp>: choose_file(), show_file_dialog()), which finds
// the window's native handle, keeps to one dialog at a time, remembers the
// folder per kind of file and brings the answer back to the interface
// thread:
//
//     services().choose_file(kind(), ui::file_purpose::model,
//                            [this](const std::filesystem::path &file) { use(file); });
//
// The platform call itself, for code that has a native window at hand:
//
//     platform::file_dialog dialog;
//     dialog.mode = platform::file_dialog_mode::save_file;
//     dialog.title = "Save Project As";
//     dialog.filters = {{"Volt project", {"voltproj"}}};
//     dialog.default_extension = "voltproj";
//     dialog.file_name = "Blinky.voltproj";
//     platform::show_file_dialog(*window.native(), std::move(dialog),
//                                [](platform::file_dialog_result result) {
//                                    // Not the interface thread: hand it on.
//                                });

namespace platform
{

    // The backend's side of a window (<platform/windows/window.hpp>,
    // <platform/linux/window.hpp>).
    struct native_window;

    enum class file_dialog_mode : std::uint8_t
    {
        open_file,     // one existing file
        open_files,    // one or more existing files
        save_file,     // a file name to write, existing or not
        choose_folder, // one existing folder
    };

    // One entry of the dialog's file type list: "Volt project" and
    // {"voltproj"} shows as "Volt project (*.voltproj)" and lists the files
    // ending in .voltproj. Extensions are given without the dot ("step"; a
    // leading "." or "*." is tolerated), any case; "*" alone is every file.
    struct file_filter
    {
        std::string name;
        std::vector<std::string> extensions;
    };

    struct file_dialog
    {
        file_dialog_mode mode = file_dialog_mode::open_file;

        // The dialog's title; empty for the system's ("Open", "Save As").
        std::string title;

        // The file types to choose from, the first one selected. Empty
        // lists every file. Ignored when choosing a folder.
        std::vector<file_filter> filters;

        // Saving: added to a name typed without an extension ("Blinky"
        // becomes "Blinky.voltproj"); empty adds nothing. No dot.
        std::string default_extension;

        // Where the dialog starts. Empty, or a folder that does not exist:
        // where the system likes (on Windows, the last folder this
        // application used).
        std::filesystem::path directory;

        // Saving: the name the dialog suggests ("Blinky.voltproj").
        std::string file_name;
    };

    struct file_dialog_error
    {
        // There is no file dialog on this system (Linux without zenity or
        // kdialog): the caller can fall back to something of its own, such
        // as a typed path. False when there is one but it failed.
        bool unavailable = false;

        // What to tell the user.
        std::string message;
    };

    // The paths chosen: one, except for open_files; none when the user
    // cancelled. Absolute paths to what exists, except for save_file,
    // whose file may not exist yet (the user confirmed replacing it if it
    // does).
    using file_dialog_result = std::expected<std::vector<std::filesystem::path>, file_dialog_error>;

    // Runs exactly once per show_file_dialog(): on the platform thread
    // (Windows), on the dialog's own thread (Linux), or, when the dialog
    // cannot be shown at all, before show_file_dialog() returns, on the
    // calling thread. Do no more in it than hand the result on (to the
    // interface thread, say): on Windows the owner's window procedure is
    // still on the stack.
    using file_dialog_callback = std::function<void(file_dialog_result result)>;

    // Shows the dialog, modal to `owner` where the system can tell (see
    // above). Returns at once; `done` gets the answer. Any thread, while
    // `owner` is open: a window destroyed before its dialog came up answers
    // as cancelled. One dialog per window at a time: a second one is
    // refused (an error, not unavailable).
    void show_file_dialog(native_window &owner, file_dialog dialog, file_dialog_callback done);

    // Closes the dialog showing for `owner`, if any, as if cancelled; its
    // callback still runs (with no paths). Returns without waiting for
    // that. Any thread, while `owner` is open. For exiting while a dialog
    // is up.
    void close_file_dialog(native_window &owner);

    // ---------------------------------------------------------------------
    // The portable part: what the backends build their dialogs from. In
    // the header (and volt/src/platform/file_dialog.cpp) so that it is
    // tested on every system, Linux's command lines included.
    // ---------------------------------------------------------------------

    // "step" from "step", ".step", "*.step" or "*.STEP"; "*" for every file
    // ("*" or "*.*"); empty for nothing usable: empty, or holding a further
    // dot ("tar.gz"), a wildcard, a space, a separator or a control
    // character, which no backend could pass on as it stands.
    std::string normalize_extension(std::string_view extension);

    // The filter's extensions, normalized, without the unusable ones and
    // repeats.
    std::vector<std::string> filter_extensions(const file_filter &filter);

    // The glob patterns: "*.step;*.stp" with `separator` ";" (Windows),
    // "*.step *.stp" with " " (zenity, kdialog). "*" for every file.
    std::string filter_patterns(const file_filter &filter, std::string_view separator = ";");

    // What the type list shows: "3D model (*.step;*.stp)". A name that
    // already ends in a parenthesis is taken as it is; an empty one shows
    // the patterns alone.
    std::string filter_label(const file_filter &filter, std::string_view separator = ";");

    // Whether the path's extension is one of the filter's (any case); every
    // path for a filter that has "*".
    bool filter_matches(const file_filter &filter, const std::filesystem::path &path);

    // `path` with ".`extension`" appended when its file name has no
    // extension: "Blinky" gives "Blinky.voltproj". A name ending in a dot
    // has none either, and its dot is reused ("Blinky." gives the same).
    // Unchanged when it has one, when `extension` is empty, and for a
    // path with no file name.
    std::filesystem::path with_default_extension(const std::filesystem::path &path, std::string_view extension);

    // A path from UTF-8 text, and back. std::filesystem::path reads a
    // std::string in the system's narrow encoding, which on Windows is a
    // code page, not UTF-8; these go through char8_t.
    std::filesystem::path path_from_utf8(std::string_view text);
    std::string path_to_utf8(const std::filesystem::path &path);

    // The command line (program first) that shows the dialog with zenity
    // or kdialog. Paths in it are UTF-8. Multiple files come back one per
    // line (zenity --separator, kdialog --separate-output).
    enum class chooser_program : std::uint8_t
    {
        zenity,
        kdialog,
    };
    std::vector<std::string> chooser_command(chooser_program program, const file_dialog &dialog);

    // What the program printed, back into paths: one per line, the final
    // newline dropped, empty lines skipped. For save_file the default
    // extension is added (zenity and kdialog do not add one).
    std::vector<std::filesystem::path> parse_chooser_output(std::string_view output, const file_dialog &dialog);

    // Saving: the path chosen when the default extension parse_chooser_output()
    // adds makes it the name of a file that exists. The program asked
    // before replacing the name the user typed ("Blinky"), if that existed,
    // but never about this one ("Blinky.voltproj"), which saving would
    // replace unasked; the backend shows the dialog again with it filled in,
    // so that the program asks. Empty otherwise.
    std::filesystem::path unconfirmed_replacement(std::string_view output, const file_dialog &dialog);

} // namespace platform
