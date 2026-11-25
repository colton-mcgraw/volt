#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// parts panel
// -------------------------------------------------------------------------
//
// The parts library, compact, for a dock beside an editor (the schematic
// editor shows it as its "Parts" tab), about 300 logical units wide:
//
//   top          a search field and how many it found; the scopes: Library
//                (every part in the
//                catalog), Project (the open project's parts) and, for an
//                editor that places them, Generic (placeholder parts and
//                power ports, see below)
//   middle       the hits, a virtualized list of two-line rows: what the
//                part is (spec_summary() of <app/ui/windows/
//                library_model.hpp>: "1kΩ ±1% 100mW 100ppm/K") with its
//                package at the right; then its manufacturer and MPN (or
//                its name, without them) and where it is from
//   bottom       a strip about the selected part (its name; manufacturer,
//                MPN and package; every parameter, compactly), and its
//                actions: Place (only for an editor that places, see
//                below), Add to project, Browse (which opens or raises the
//                Library Browser with the part selected)
//
// The search is the catalog's (catalog::search: "1k" is the 1 kΩ parts,
// "1k 0603 1%" narrows them), in the Project scope too (catalog::match).
//
// It reads everything from app_services (catalog(), project()) and follows
// catalog_changed and project_changed itself, so the editor that docks it
// only builds it and puts it in its tree. The scope, the search text, the
// selections and an assignment in progress survive the window closing
// (app_services::parts_panel(kind) keeps them, as parts_panel_state, one per
// kind of window); the widgets do not.
//
// Keys inside the panel: Down and Up in the search field move through the
// hits, Enter does what a double-click does, Escape clears the search, or,
// with none, cancels an assignment. Down, Up and Enter act on the hits of
// the text as typed: a search still waiting (for a pause in the typing, or
// in the background) is run first.
//
// Placing. An editor that places parts (the schematic editor) passes
// parts_panel_options::place: the panel then has a Place button, and Enter
// and a double-click on a row place the part rather than only adding it. A
// catalog part is copied into the open project's library first (as Add to
// Project does, once), and handed over as that copy, with its id there; a
// project part is handed over as it is; with no project open the catalog
// part is handed over with an empty project id. Without the option the
// panel is as it always was.
//
// Generic. With place_symbol or place_power given, the Generic scope lists
// the placeholders and power ports of <app/ui/windows/part_match.hpp>
// (generic_entries(): Resistor 10k, Capacitor 100n, ..., GND, +5V, ...),
// searched by name ("res", "gnd"); Place, Enter and a double-click call
// place_symbol(symbol, value) or place_power(name). A placeholder has no
// library part: one is assigned to it later (below).
//
// Assigning. With `assign` given, the host can put the panel into assign
// mode for some placeholders (begin_part_assignment()): a banner says what
// is being assigned ("Assign a part to R1, R2 (1kΩ resistor)") with a Cancel
// button, the scopes give way to the candidates (match_parts(): the
// project's parts first, then the catalog's, of the same device, value and
// compatible pins), the search narrows within them, and Assign (or Enter,
// or a double-click) copies a catalog part into the project's library (as
// Place does) and calls assign() with it, then leaves the mode. With no
// project open nothing is assigned: the panel says to open one. The mode
// is in parts_panel_state, so it survives the panel being built again; the
// callbacks are the options of the panel built last.

namespace volt::library
{
    struct part;
} // namespace volt::library

namespace volt::ui
{

    class widget;
    class window_host;

    // What an assignment is for: the placeholders' device, value and
    // symbol (as part_match.hpp's placeholder), a title for the banner, and
    // the host's own ids of the placed parts, handed back to it with the
    // chosen part.
    struct part_assignment
    {
        std::string title; // "Assign a part to R1, R2 (1kΩ resistor)"
        std::string device;
        std::string value;
        std::string symbol;
        std::vector<std::uint32_t> items;

        bool operator==(const part_assignment &) const = default;
    };

    struct parts_panel_options
    {
        // Places `part`: `project_id` is its id in the open project's
        // library, empty when no project is open. Unset: no placing.
        std::function<void(const library::part &part, const std::string &project_id)> place;

        // Places a placeholder of a built-in symbol with a value to start
        // from ("R", "10k"), and a power port ("GND"). Either set: the
        // Generic scope.
        std::function<void(const std::string &symbol, const std::string &value)> place_symbol;
        std::function<void(const std::string &name)> place_power;

        // The part chosen in assign mode for `what`, already in the open
        // project's library as `project_id`. Unset: no assign mode.
        std::function<void(const part_assignment &what, const library::part &part, const std::string &project_id)>
            assign;
    };

    enum class parts_scope : std::uint8_t
    {
        library, // every part in the catalog
        project, // the open project's
        generic, // placeholders and power ports
    };

    // What a parts panel keeps between builds.
    struct parts_panel_state
    {
        parts_scope scope = parts_scope::library;
        std::string search;
        std::string selected;         // qualified id of the catalog part
        std::string selected_project; // id in the project library
        std::string selected_generic; // a generic entry's name

        // Assign mode while set; its own search, the search before it (put
        // back after), and the chosen candidate: "project:<id>" or a
        // catalog part's qualified id.
        std::optional<part_assignment> assigning;
        std::string search_before;
        std::string selected_candidate;
    };

    // Made anew in every build() of the window docking it; the panel
    // disconnects from the shared signals when it is destroyed.
    std::unique_ptr<widget> make_parts_panel(window_host &host, parts_panel_options options = {});

    // What a host can ask of a panel it built: `panel` is the widget
    // make_parts_panel() returned (anything else: nothing happens, false).
    //
    // Gives the search field the keyboard focus, its text selected, in the
    // screen's next update (after the character of a key that asked).
    void focus_parts_search(widget &panel);
    // Enters assign mode for `what`; false, and nothing changes, when the
    // panel was built without options::assign.
    bool begin_part_assignment(widget &panel, part_assignment what);
    // Leaves assign mode, if in it.
    void end_part_assignment(widget &panel);
    bool assigning_part(const widget &panel);

} // namespace volt::ui
