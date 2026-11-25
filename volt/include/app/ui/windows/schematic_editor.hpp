#pragma once

#include <app/ui/windows/content.hpp>
#include <app/ui/windows/parts_panel.hpp>
#include <app/ui/windows/schematic_properties.hpp>
#include <app/ui/windows/schematic_tools.hpp>
#include <app/ui/windows/schematic_view.hpp>
#include <ui/controllers.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

// -------------------------------------------------------------------------
// schematic_editor
// -------------------------------------------------------------------------
//
// The schematic, in a window of its own, arranged as schematic editors are:
//
//   menu bar     File (Save Project), Edit, View, Place, Simulate, Window,
//                Help
//   toolbar      brand; undo, redo; zoom in, zoom out, fit; the grid; Run
//                simulation, potentials on the wires; the Board Editor
//   tool strip   down the left of the canvas: the editing tools (select,
//                wire, net label, power port, part, text), one active, each
//                with a one-key shortcut; the power port's button opens the
//                power pick beside it (below)
//   sheets       a strip above the canvas naming the open sheets, the shown
//                one chosen; clicking another shows it (the sheet navigator)
//   canvas       the schematic view (offscreen_view::schematic): the tools
//                edit with the left button, the middle and right buttons
//                pan, the wheel zooms; a context menu; under it one line
//                saying what the tool does now (schematic_tools::hint())
//   dock         on the right, tabs:
//                  Properties   what the armed placing tool places next
//                               (its Tool section), the selection's
//                               (both <app/ui/windows/
//                               schematic_properties.hpp>), then the
//                               sheet's (name, file, what is on it, its
//                               nets, its unassigned parts with Select,
//                               and Pins connect along wires) and how it
//                               is drawn (grid, wires)
//                  Parts        the parts library (<app/ui/windows/
//                               parts_panel.hpp>), with Place
//                  Simulation   the circuit (nodes, branches), the problems
//                               the netlist and the circuit found (a click
//                               selects a problem's items and shows them),
//                               the wire colouring, Run automatically,
//                               whether the potentials shown are the
//                               sheet's as it is, and Run
//   status       the cursor in grid units (snapped while placing or
//                wiring), the grid, the zoom, the active tool, the
//                simulation
//
// What is there. The sheets are the schematic document's (app_services::
// schematic(), <app/ui/schematic_document.hpp>): the project's, or with none
// open the scratch demo sheet, editable and never saved (said once, at the
// first edit, and by the sheet strip). The editing is the tools'
// (<app/ui/windows/schematic_tools.hpp>, whose top comment is the manual of
// every gesture and key), driven through the view's controller
// (<app/ui/windows/schematic_view.hpp>) and from the menus:
//
//   Edit       Undo, Redo (Ctrl+Z; Ctrl+Y, Ctrl+Shift+Z), Cut (Ctrl+X),
//              Copy (Ctrl+C), Paste (Ctrl+V), Duplicate (Ctrl+D), Delete
//              (Delete), Select All (Ctrl+A), Rotate (R), Rotate Clockwise
//              (Shift+R), Mirror Horizontally (X), Mirror Vertically (Y),
//              Select Tool. Copy and Cut put the clipboard on the system's
//              clipboard as .voltsch text; Paste takes a sheet from there
//              when it holds one (so sheets copy between sessions), else
//              the tools' own clipboard.
//   Place      Wire (W), Net Label (L), Text (T), Part (A: the Parts tab,
//              its search focused, the part armed before still armed),
//              Power Port (P) and a submenu of named ports (GND, +3V3, +5V,
//              +12V, -12V, VCC)
//   Edit       also Assign Part… (Change Part… for one assigned part), for
//              placeholders (below), and Pins Connect Along Wires: the
//              project's rule of connection (schematic_document::
//              set_connectivity(), kept in the project file; the demo
//              sheet's for the session), checked by default: on, a wire
//              that passes over a pin connects to it; off, only a wire
//              that ends on a pin does. The Properties page's Sheet section
//              has the same switch. The tools follow the rule
//              (schematic_tools::set_connectivity()) from the start, on
//              connectivity_changed and when other sheets open.
//   keys       the tools' keys while the canvas has the focus (Escape,
//              Enter, Backspace, Delete, R, X, Y, Space, the arrows); R, X,
//              Y, Delete, Space, the arrows and Escape are also the
//              window's shortcuts, for when the focus is elsewhere, and a
//              text field being typed in keeps them (it claims plain keys)
//
// Power ports. P (or Place > Power Port, or the strip's button) arms the
// power tool with the port last used this session (GND the first time)
// and opens the power pick: at the pointer when it is over the canvas,
// else beside the strip's button. It lists the usual ports, then the other
// power ports on the sheet, then Other…, which shows the Tool section with
// its Name field focused (Enter arms that name and gives the canvas the
// keyboard). Choosing arms the port, its ghost under the pointer;
// dismissing the pick leaves the one armed. P again, armed or not, opens
// it again.
//
// The keyboard after placing. A net label or note placed hands its Text
// field the keyboard, all selected; Enter or Escape there keeps what was
// typed and gives the keyboard back to the canvas with the tool still armed
// (and nothing cancelled), so the next click places the next one, its
// number counted on from the name typed ("D0", then "D1"). The same for
// the Value or Reference field a double-click focused. Escape in any of the
// page's text fields only leaves the field.
//
// The Parts tab's Place (or Enter, or a double-click on a part) copies the
// part into the project's library when a project is open, arms the part
// tool with it and gives the canvas the focus. Its Generic scope places
// built-in symbols with no library part (placeholders: a resistor 10k, a
// capacitor 100n, ...) and power ports. Assign Part… (Edit menu, context
// menu, the Properties page of a placeholder), with placeholders selected,
// puts the Parts tab into assign mode (<app/ui/windows/parts_panel.hpp>)
// for the largest group of them of one device and value (the others stay
// selected for the next Assign Part…, which the status bar says); the part
// chosen there is assigned to each of them (<app/ui/windows/part_match.hpp>,
// assigned()) as one step, "Assign part to R1" ("... to 3 parts"). With one
// assigned part selected alone it is Change Part…, the same with the parts
// of its device and value ("Change part of R1"). With no project open it
// says to open one. The Properties page's Sheet section counts the
// unassigned parts and selects them all. A double-click on an item, and
// placing a net label or a note, shows Properties with the item's main text
// field focused (a part's Value, a label's Text).
//
// The simulation runs the shown sheet's netlist: on F5, and, with Simulate >
// Run Automatically (on by default), a moment after the sheet settles (no
// gesture open), when another sheet is shown and when a project opens. A
// run says what it found in the status bar when that changed, or when asked
// for with F5. Undo and Redo work on the shown sheet; Save Project writes the
// project's sheets.
//
// The sheets, the grid and the simulation's state are app_services::
// schematic(), so they are the same in every window; the camera, the tools
// (their tool, selection and clipboard), the dock's width and its tab are
// this object's, and survive the window closing. The tools always edit the
// active sheet: they move to the new one in sheets_changed, while the old
// ones still exist, whether the window is open or not; a gesture open when
// the window closes is cancelled (a wire being drawn keeps what was fixed).
// The camera fits the sheet the first time the view has a size, and again
// whenever another sheet is shown (or other sheets open, with a project).
//
// Extension points. A tool: a row in the tool table at the top of the .cpp
// (its icon, name and key) and its case in schematic_tools. A dock page:
// build_dock() adds the tabs (m_dock_tabs); a page made by another file is
// built there as make_parts_panel() is. Commands go in add_commands(), menus
// in build().

namespace volt::ui
{

    class box;
    class button;
    class checkbox;
    class label;
    class numeric_field;
    class radio_group;
    class splitter;
    class tab_view;
    class tool_strip;
    class viewport;
    class widget;

    class schematic_editor final : public window_content
    {
    public:
        // The editing tools, in the strip's order (the tools' own).
        using tool = schematic_tools::tool;

        // The dock's pages, in their order.
        enum class page : std::uint8_t
        {
            properties,
            parts,
            simulation,
        };

        explicit schematic_editor(app_services &services);
        ~schematic_editor() override;

        window_kind kind() const noexcept override { return window_kind::schematic_editor; }
        math::vec2i default_size() const noexcept override { return {1200, 720}; }

        // The sheet's colours follow the theme.
        void theme_changed() override;

        // The pick distance follows the window's scale.
        void scale_changed(float scale) override;

        // As wide as the status bar's own segments and the frame
        // information need side by side.
        math::vec2i min_size() const noexcept override { return {900, 480}; }

        window_layout build(window_host &host) override;
        void detach() override;
        void before_frame() override;
        viewport *view(offscreen_view view) noexcept override;

    private:
        window_layout::menu edit_menu();
        window_layout::menu view_menu();
        window_layout::menu place_menu();
        window_layout::menu simulate_menu();

        std::unique_ptr<widget> build_toolbar(window_host &host);
        std::unique_ptr<widget> build_tools();
        std::unique_ptr<widget> build_sheets();
        std::unique_ptr<widget> build_canvas(window_host &host);
        std::unique_ptr<widget> build_dock(window_host &host);
        std::unique_ptr<widget> build_properties();
        std::unique_ptr<widget> build_simulation();
        void add_commands(window_host &host);
        void open_context_menu_at(window_host &host, math::vec2f at);

        // The tool strip, the Place menu and the shortcuts choose a tool.
        void set_tool(tool t);
        // P and the Power Port command: the power tool armed with the
        // port last used, and the power pick opened at the pointer (or by
        // the tool strip's button: `anchor`, the button).
        void power_tool(const button *anchor = nullptr);
        void open_power_pick(const button *anchor);
        // "Other…" in the power pick: the Tool section's name field.
        void name_power_port();
        // Enter or Escape in a field of the Properties page that the
        // keyboard was handed to: back to the canvas, its tool still armed.
        void return_to_canvas();
        void set_connectivity(bool pins_inside_wires);
        // The tools follow the document's connectivity rules.
        void connectivity_changed();
        // Shows the dock's page `p`, and the dock if it is hidden.
        void show_page(page p);
        void set_dock_shown(bool shown);
        void set_grid_shown(bool shown);
        void set_grid_spacing(double spacing);
        void set_potentials(bool shown);
        void set_auto_run(bool on);
        void zoom_by(float factor);

        // ---- the tools ----------------------------------------------------------------

        // The tools edit the active sheet; called when the sheets change,
        // window or not.
        void attach_tools();
        // What the tools show goes into the scene (the selection and the
        // overlay), and the strip, the hint and the status follow the
        // tool: after every event and command, and before every frame.
        // Nothing when nothing changed since.
        void tools_changed();
        // After a command: the view up to date, and the canvas focused when
        // something now follows the pointer.
        void after_command(bool focus_canvas = false);
        void undo();
        void redo();
        void copy();
        void cut();
        void paste();
        // A built-in symbol with no library part, and a power port.
        void place_generic(std::string symbol, std::string value);
        void place_power(std::string name);
        // From the Parts tab: `project_id` empty with no project open.
        void place_library_part(const library::part &part, const std::string &project_id);
        // Assign Part…: what the selection's placeholders are: the largest
        // group of them of one device and value (the first selected among
        // equals), or, with no placeholder selected, one assigned part
        // selected alone (Change Part…); the Parts tab put into assign mode
        // for them; the part chosen there assigned to them. `others` is
        // how many selected placeholders the group leaves out.
        std::optional<part_assignment> selection_assignment(std::size_t *others = nullptr) const;
        void assign_part();
        void apply_assignment(const part_assignment &what, const library::part &part, const std::string &project_id);
        // A double-click on an item, a label placed: its properties.
        void edit_item(schematic::item_ref item);
        // A problem's items selected and shown.
        void show_items(std::span<const schematic::item_ref> items);
        bool has_selection() const;

        void sync_camera();
        void scene_changed();
        void sheets_changed();
        // F5 (manual: always says what it found), or the automatic run
        // (says it only when that changed).
        void run_simulation(bool manual);
        // The automatic run, a moment after the sheet settled.
        void schedule_simulation();
        void run_when_settled();
        // Fits the shown sheet into the view, if the view has a size.
        void fit_sheet();
        void update_cursor_status();
        // The sheet's rows of the properties, the circuit and its problems
        // of the simulation page, the undo buttons: brought up to date in
        // before_frame().
        void update_sheet_info();
        void update_problems();

        // Declared in this order: the view's controller and the properties
        // page hold on to the tools and the camera.
        schematic_tools m_tools;
        pan_zoom_controller m_controller;
        schematic_view_controller m_view_controller{m_tools, m_controller};
        selection_properties m_properties;
        tool_options m_tool_options;

        // What tools_changed() last pushed into the scene: the tools'
        // revision and the sheet's key.
        std::optional<std::pair<std::uint64_t, schematic_document::sheet_key>> m_pushed;

        // The sheets (generation and index) the camera was last fitted to.
        std::optional<std::pair<std::uint64_t, std::size_t>> m_fitted;

        // The simulation: whether it runs by itself, the timer waiting for
        // the sheet to settle, what the last run said, the sheet as last
        // seen (for telling an edit from a camera move).
        bool m_auto_run = true;
        screen::timer_id m_run_timer = 0;
        std::string m_last_run;
        std::optional<schematic_document::sheet_key> m_seen_key;

        // The scratch sheet's first edit was told that nothing is saved;
        // until when that message keeps the automatic run's off the bar.
        bool m_scratch_told = false;
        time_point m_quiet_until{};

        // The dock's width, logical units, as the user left it; which of
        // its pages was showing; whether it is shown at all (View > Dock,
        // Ctrl+Shift+D: the splitter collapses it).
        float m_dock_width = 300.0f;
        int m_dock_page = 0;
        bool m_dock_shown = true;

        // The sheet the problems list was made for, and the sheets
        // (generation) the sheet strip lists.
        std::optional<schematic_document::sheet_key> m_problems_key;
        std::uint64_t m_strip_generation = 0;

        // `changed` while the window is open; `sheets_changed` for as long
        // as this object lives (the tools follow the sheets).
        std::size_t m_changed_connection = 0;
        std::size_t m_sheets_connection = 0;
        std::size_t m_connectivity_connection = 0;

        // Widgets talked to after building them; owned by the screen's tree,
        // null while the window is closed.
        splitter *m_split = nullptr;
        widget *m_dock = nullptr;
        tab_view *m_dock_tabs = nullptr;
        widget *m_parts_panel = nullptr;
        tool_strip *m_tool_strip = nullptr;
        box *m_sheet_holder = nullptr;
        tool_strip *m_sheet_strip = nullptr;
        label *m_sheet_note = nullptr;
        viewport *m_view = nullptr;
        label *m_hint = nullptr;
        checkbox *m_auto_box = nullptr;
        button *m_undo_tool = nullptr;
        button *m_redo_tool = nullptr;
        label *m_sheet_name = nullptr;
        label *m_sheet_file = nullptr;
        label *m_sheet_parts = nullptr;
        label *m_sheet_wires = nullptr;
        label *m_sheet_labels = nullptr;
        label *m_sheet_nets = nullptr;
        label *m_sheet_unassigned = nullptr;
        button *m_select_unassigned = nullptr;
        checkbox *m_connect_box = nullptr;
        label *m_circuit_nodes = nullptr;
        label *m_circuit_branches = nullptr;
        label *m_circuit_summary = nullptr;
        box *m_problems = nullptr;
        checkbox *m_grid_box = nullptr;
        button *m_grid_tool = nullptr;
        button *m_potentials_tool = nullptr;
        numeric_field *m_grid_field = nullptr;
        radio_group *m_colours = nullptr;
        label *m_run_state = nullptr;
        label *m_cursor_status = nullptr;
        label *m_grid_status = nullptr;
        label *m_zoom_status = nullptr;
        label *m_tool_status = nullptr;
        label *m_simulation_status = nullptr;
    };

} // namespace volt::ui
