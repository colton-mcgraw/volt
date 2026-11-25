#pragma once

#include <schematic/document.hpp>
#include <schematic/draw.hpp>
#include <schematic/netlist.hpp>
#include <schematic/scene.hpp>
#include <schematic/sheet.hpp>
#include <ui/signal.hpp>
#include <ui/theme.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace volt::library
{
    struct sheet;
} // namespace volt::library

// -------------------------------------------------------------------------
// schematic_document
// -------------------------------------------------------------------------
//
// The application's schematic: the sheets that are open, each an undoable
// schematic::document (<schematic/document.hpp>), one of them shown and
// edited (the active one); how the view draws it; and what the simulation
// last ran. It lives in app_services (app_services::schematic()), as long
// as the application, so every window sees the same sheets.
//
// The sheets. With no project open there is one, the scratch sheet
// "Ladder (demo)" (schematic::demo_ladder()): editable, never saved, and
// made anew when a project closes. A project opens one sheet per entry of
// its `sheets` (<library/project.hpp>), from the files beside it
// (open_project_sheets()): a file that is not there is a new, empty sheet,
// written by the next save; a file that cannot be read opens as an empty
// sheet marked not loaded, which saving never writes, so that the file is
// left for the user to mend. save_project_sheets() writes the sheets that
// changed or have no file yet. app_services does both as the project opens
// and saves (<app/ui/services.hpp>).
//
// Editing. active() is the shown sheet's document: edits go straight
// through its operations, each one an undo step already. Whatever changes a
// sheet, undo and redo included, the document reports (on_change), and this
// object then marks the view dirty and emits `changed`; `modified_changed`
// when the sheets as a whole go from saved to modified or back (an undo to
// the saved state); `sheets_changed` when other sheets open or another one
// becomes the active one.
//
// The scene. scene.segments are this object's: rebuilt from the active
// sheet (schematic::append_sheet()), then the overlay, whenever the view
// takes the scene to draw it (update_scene()) and something it is drawn
// from changed: the sheet (its revision), the active sheet, the style (the
// theme, the wire width), the selection, the overlay, or whether the
// simulation is current. The camera (scene.view), the background and the
// grid are set from outside as before.
//
// Derived data, worked out when first asked for and kept while the active
// sheet stays at the same revision: the netlist and the circuit
// (<schematic/netlist.hpp>), for the simulation, the properties and the
// list of problems.
//
// Connectivity. What connects (schematic::connectivity, <schematic/sheet.hpp>:
// whether a pin inside a wire connects to it) is the project's, the same for
// all its sheets, and is stored in the project file (library::project's
// `schematic`, <library/project.hpp>), so that a design means the same on
// every machine. open_project_sheets() takes it from there; the scratch
// sheet has the defaults. The netlist, the circuit and the drawing (junction
// dots, free-pin marks) follow it, and so should the editing tools
// (schematic_tools::set_connectivity()). set_connectivity() changes it: the
// derived data and the scene are worked out again, `changed` and
// `connectivity_changed` fire, and app_services, hearing the second, puts it
// into the open project and marks the project modified (the next save
// writes it).
//
// The simulation. record_simulation() notes what the simulation was just
// given: the circuit of the active sheet at its present revision, its
// wires' nodes and the range of its potentials. Wires carry their nodes
// (their segments' `node`, which the renderer colours by potential) only
// while that sheet is shown at that revision: after an edit the nodes may
// be numbered otherwise than in the simulation's buffer, so the
// potentials are not shown again until the next run (simulation_state()).
//
// Dirty flags. The view is rendered into its offscreen target only when
// its scene changed (mark_dirty(), take_dirty()), its viewport changed
// size, or the simulation has new values. Whoever changes the scene marks
// it dirty; `changed` says so to the window showing it, which asks for a
// frame.
//
// Interface thread only, the render loop included (it reads the scene and
// the dirty flag between laying out and painting).

namespace volt::ui
{

    class schematic_document
    {
    public:
        // What the active sheet is, as a value: which set of open sheets
        // (generation: every open_scratch() and set_sheets() makes a new
        // one), which of them, and at which revision. What is derived from
        // a sheet is kept under its key.
        struct sheet_key
        {
            std::uint64_t generation = 0;
            std::size_t index = 0;
            std::uint64_t revision = 0;

            bool operator==(const sheet_key &) const = default;
        };

        // A sheet to open: see set_sheets().
        struct new_sheet
        {
            std::string name;
            // As the project stores it, relative to the project file; empty
            // for a sheet that is never saved (the scratch sheet).
            std::string file;
            schematic::sheet sheet;
            // False when the file could not be read: the sheet is empty, and
            // saving leaves the file alone.
            bool loaded = true;
        };

        // How the simulation's last run relates to the sheet shown.
        enum class run_state : std::uint8_t
        {
            not_run,     // nothing was run since these sheets opened
            current,     // it ran this sheet as it is
            edited,      // it ran this sheet, which has changed since
            other_sheet, // it ran another of the open sheets
        };

        // What loading a project's sheets found: one entry per sheet with
        // something to say.
        struct load_problem
        {
            std::string text;
            // The file could not be read at all (the sheet is not loaded);
            // otherwise it loaded with records left out or fields ignored.
            bool failed = false;
        };

        // What saving the sheets did.
        struct save_report
        {
            std::size_t written = 0;
            // The sheets not written because they did not load, by name.
            std::vector<std::string> skipped;
        };

        static constexpr std::string_view scratch_name = "Ladder (demo)";

        // The scratch sheet.
        schematic_document();
        ~schematic_document();

        schematic_document(const schematic_document &) = delete;
        schematic_document &operator=(const schematic_document &) = delete;

        // ---- the view --------------------------------------------------------------

        // What the schematic view draws, camera included. The segments are
        // this object's (see update_scene()).
        schematic::scene scene;

        // The grid: shown or not, and its spacing in world units.
        bool show_grid = true;
        float grid = 1.0f;

        // How wide wires are drawn, world units (apply_wire_width()).
        float wire_width = 0.12f;

        // Wires are coloured by the simulation's potentials (while it is
        // current), or plain.
        bool show_potentials = true;

        // What the simulation is doing, as of the last frame drawn. Written
        // by the render loop.
        bool simulation_busy = false;
        bool simulation_results = false;

        // The scene changed and has to be drawn again.
        void mark_dirty()
        {
            m_dirty = true;
            changed.emit();
        }
        bool take_dirty() noexcept { return std::exchange(m_dirty, false); }

        // Puts show_grid and grid into the scene.
        void apply_grid();

        // Puts wire_width into the style: wires, and the junction dots on
        // them three times as wide.
        void apply_wire_width();

        // Takes the drawing's colours from the theme: the sheet, the grid,
        // wires, symbols and text from its canvas colours; the selection,
        // the ghost of what is being placed and the snap marker from its
        // accent; free pin ends from its error colour.
        void apply_theme(const ui::theme &theme);

        // What the sheet is drawn with.
        const schematic::draw_style &style() const noexcept { return m_style; }

        // Rebuilds scene.segments if anything they are drawn from changed
        // since they were last built (see the top of this file). True when
        // it did. The render loop calls it before drawing.
        bool update_scene();

        // ---- the sheets ------------------------------------------------------------

        std::size_t sheet_count() const noexcept { return m_sheets.size(); }

        // Sheet i's name, file (empty for the scratch sheet), whether it
        // loaded, and its document. Precondition: i < sheet_count().
        const std::string &sheet_name(std::size_t i) const { return m_sheets[i]->name; }
        const std::string &sheet_file(std::size_t i) const { return m_sheets[i]->file; }
        bool sheet_loaded(std::size_t i) const { return m_sheets[i]->loaded; }
        const schematic::document &sheet(std::size_t i) const { return m_sheets[i]->document; }

        // The sheet shown and edited.
        std::size_t active_index() const noexcept { return m_active; }
        schematic::document &active() noexcept { return m_sheets[m_active]->document; }
        const schematic::document &active() const noexcept { return m_sheets[m_active]->document; }
        sheet_key active_key() const noexcept;

        // Shows sheet i: the selection and the overlay go, and
        // sheets_changed fires. False (and nothing changes) when i is out of
        // range or is the active one already.
        bool set_active(std::size_t i);

        // The sheets are the scratch sheet: no project's.
        bool scratch() const noexcept { return m_scratch; }

        // Any sheet differs from its file (the scratch sheet too, though it
        // is never saved: whoever asks knows whether a project is open).
        bool modified() const noexcept;

        // Replaces the sheets with a fresh scratch sheet, read by the
        // default connectivity rules.
        void open_scratch();

        // Replaces the sheets with these, the first one active, each with no
        // history and unmodified. At least one: an empty list opens the
        // scratch sheet instead. The connectivity rules stay as they are (a
        // project's sheets are opened by open_project_sheets(), which sets
        // them).
        void set_sheets(std::vector<new_sheet> sheets);

        // ---- connectivity ------------------------------------------------------------

        // The rules the sheets are read by (see the top of this file).
        const schematic::connectivity &connectivity() const noexcept { return m_connectivity; }

        // Changes them: nothing when they are the same; otherwise the
        // netlist, the circuit and the scene are worked out afresh, and
        // `changed` and `connectivity_changed` fire.
        void set_connectivity(const schematic::connectivity &rules);

        // ---- a project's sheets ----------------------------------------------------

        // Opens the project's sheets (set_sheets()), each from its file,
        // `file` resolved against `folder` (the project file's directory,
        // as <library/project.hpp> stores paths), read by the project's
        // connectivity rules, which become connectivity() (without
        // connectivity_changed: they are the project's already). A file
        // that is not there is a new empty sheet; one that cannot be read
        // is an empty sheet, not loaded; one that loads with problems loads
        // what it could. Returns what to report, a sheet at a time.
        std::vector<load_problem> open_project_sheets(const std::filesystem::path &folder,
                                                      std::span<const library::sheet> sheets,
                                                      const schematic::connectivity &rules = {});

        // Writes the sheets that are modified, or whose file does not exist
        // yet, to their files under `folder` (creating directories as
        // needed), and marks each one written as saved. Sheets that did not
        // load are never written (they are listed as skipped), nor is the
        // scratch sheet. Fails at the first sheet that cannot be written,
        // the ones before it written and marked.
        std::expected<save_report, std::string> save_project_sheets(const std::filesystem::path &folder);

        // Writes sheet i as it is to `path`, marking nothing: for saving a
        // project somewhere else, which then calls sheet_saved_as().
        std::expected<void, std::string> write_sheet(std::size_t i, const std::filesystem::path &path) const;

        // Sheet i is now in `file` (as the project stores it), as it is:
        // marked saved.
        void sheet_saved_as(std::size_t i, std::string file);

        // Sheet i is referred to as `file` now (the project moved), and
        // stays as modified as it was.
        void set_sheet_file(std::size_t i, std::string file) { m_sheets[i]->file = std::move(file); }

        // ---- derived from the active sheet ---------------------------------------------

        const schematic::netlist &netlist() const;
        const schematic::circuit &circuit() const;

        // The box the active sheet is drawn in, sheet units; false for an
        // empty sheet. For Fit.
        bool sheet_bounds(schematic::point &min, schematic::point &max) const;

        // ---- selection and overlay --------------------------------------------------
        //
        // For the editing tools. Both belong to the active sheet, and go
        // when another sheet is shown.

        std::span<const schematic::item_ref> selection() const noexcept { return m_selection; }
        void set_selection(std::span<const schematic::item_ref> items);

        // What a tool draws over the sheet (a ghost, a wire being drawn, a
        // rubber band; <schematic/draw.hpp>'s overlays), last in the scene.
        const std::vector<schematic::segment> &overlay() const noexcept { return m_overlay; }
        void set_overlay(std::vector<schematic::segment> segments);

        // ---- the simulation -----------------------------------------------------------

        // The simulation was just given circuit() of the active sheet as it
        // is: remember what it was made from.
        void record_simulation();

        run_state simulation_state() const noexcept;
        bool simulation_current() const noexcept { return simulation_state() == run_state::current; }

        // The colour ramp of the last run: its lowest and highest held
        // potentials, never the same (a run with nothing held, or one value
        // only, gets a ramp one volt wide).
        float simulation_low() const noexcept;
        float simulation_high() const noexcept;

        // ---- signals ---------------------------------------------------------------------

        // After every change of the scene (mark_dirty()): an edit, an undo,
        // the camera, the style, the selection.
        signal<> changed;

        // Other sheets opened, or another one became the active one. When
        // other sheets opened, the ones they replace are still alive while
        // this runs and go right after it: whoever holds a pointer to one of
        // their documents (the editor's tools) moves to active() here.
        signal<> sheets_changed;

        // modified() changed.
        signal<> modified_changed;

        // set_connectivity() changed the rules.
        signal<> connectivity_changed;

    private:
        struct open_sheet
        {
            std::string name;
            std::string file;
            schematic::document document;
            bool loaded = true;
        };

        // What the last run was made from.
        struct simulated
        {
            sheet_key key;
            std::unordered_map<schematic::item_id, std::int32_t> wire_nodes;
            float low = 0.0f;
            float high = 0.0f;
        };

        // What is worked out from the active sheet, under its key.
        struct derived
        {
            sheet_key key;
            schematic::netlist nets;
            schematic::circuit circuit;
        };

        void replace(std::vector<new_sheet> sheets, bool scratch);
        void sheet_changed(const open_sheet *changed_sheet);
        void style_changed();
        const derived &derive() const;

        std::vector<std::unique_ptr<open_sheet>> m_sheets;
        std::size_t m_active = 0;
        std::uint64_t m_generation = 0;
        bool m_scratch = true;
        bool m_was_modified = false;

        schematic::draw_style m_style;
        schematic::connectivity m_connectivity;
        std::vector<schematic::item_ref> m_selection;
        std::vector<schematic::segment> m_overlay;

        // What scene.segments were last built from, and whether something
        // without a revision of its own (style, selection, overlay) changed
        // since.
        std::optional<sheet_key> m_built;
        bool m_built_current = false;
        bool m_scene_stale = true;

        std::optional<simulated> m_simulated;
        mutable std::optional<derived> m_derived;

        bool m_dirty = true;
    };

} // namespace volt::ui
