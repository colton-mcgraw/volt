#pragma once

#include <span>
#include <string>
#include <string_view>

// -------------------------------------------------------------------------
// part editor
// -------------------------------------------------------------------------
//
// Making a part, and changing one, without writing JSON: a modal dialog over
// the Library Browser, its logic in part_editor_model
// (<app/ui/windows/part_editor_model.hpp>).
//
//   header        New Part, Edit Part or Duplicate Part, and where it is
//                 saved; Undo and Redo, the step each would take in its
//                 tooltip; for a new part or a duplicate while a project is
//                 open, a choice between My Parts and the project's library
//   left          the part's fields (name, id, category, description,
//                 manufacturer, MPN, value, package, prefix; device;
//                 symbol, footprint, 3D model, datasheet, keywords), then
//                 its parameters: a row each of name, value and unit, with
//                 the schema's completions and corrections under the name.
//                 The id is shown, not typed: a part keeps the one it was
//                 given, and a new part or a duplicate is given its own when
//                 it is saved.
//   right         the checks: what stops a save (no name or MPN, a
//                 parameter without a name, a model file that is not
//                 there), then the schema's
//                 errors, warnings and notes, each named as well as
//                 coloured, updated as the user types
//   footer        why Save is disabled, Cancel, Save
//
// Every edit can be undone (Ctrl+Z) and redone (Ctrl+Y, Ctrl+Shift+Z),
// whatever has the focus in the dialog: one history for the whole part
// (part_editor_model), in which what is typed into a field, until it loses
// the focus or another field is typed into, is one step. The text fields
// have no undo of their own. The dialog is modal, so the window's menus
// and shortcuts are out of reach while it is open: the keys and buttons are
// its own.
//
// Where parts go:
//
//   a new part, a duplicate   My Parts (app_services::user_parts_library(),
//                             made by the first save) or the open project's
//                             library
//   a part of a user library  that library file, in place
//   a part of the project     the project library, in place (saved with the
//                             project, Ctrl+S)
//
// A library under the application's assets ships with Volt and is not
// changed in place: the browser offers Duplicate instead. A library file is
// read again just before it is saved, so that the part goes into the file
// as it is now, and not into the catalog's copy of it; then the catalog is
// scanned again and the browser shows the part (reveal_in_library()).
//
// Closing with changes asks first: Save, Discard, or Keep Editing; so does
// closing its window or exiting (the window's close guard, <app/ui/
// close_guards.hpp>), and, for a part that goes into the project, closing
// the project or opening another (the project guard). A 3D
// model file dropped on the window while the editor is open becomes the
// part's model, carried into its library when saved.

namespace volt::library
{
    struct library;
    struct part;
} // namespace volt::library

namespace volt::ui
{

    class window_host;

    // A new part. `project` makes the open project's library the first
    // choice of where it goes, rather than My Parts.
    void open_new_part(window_host &host, bool project = false);

    // A part of a library in the catalog, changed in its library file. The
    // library must be writable (is_writable_library()).
    void open_edit_part(window_host &host, const library::library &lib, const library::part &part);

    // A copy of a part of a library in the catalog, saved as a new part of
    // My Parts or the project, linked to its source by its origin.
    void open_duplicate_part(window_host &host, const library::library &lib, const library::part &part);

    // A part of the open project's library, by its id there, changed in
    // place.
    void open_edit_project_part(window_host &host, std::string_view id);

    // An editor is open in the host's window.
    bool part_editor_open(window_host &host);

    // Files dropped on the window while an editor is open: the first 3D
    // model among them becomes the part's model. True when an editor is
    // open, which takes the drop whatever it was (nothing behind a modal
    // dialog is to open files).
    bool part_editor_drop(window_host &host, std::span<const std::string> paths);

    // Asks whether to delete a part of a writable library in the catalog,
    // and does: the part goes from the library file, which is saved, and
    // the catalog is scanned again.
    void confirm_delete_part(window_host &host, const library::library &lib, const library::part &part);

} // namespace volt::ui
