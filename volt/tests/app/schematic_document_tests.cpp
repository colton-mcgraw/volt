// The application's schematic document without a window or a GPU
// (app/ui/schematic_document.hpp): the scratch sheet, a project's sheets and
// the active one; the scene rebuilt from the active sheet when it is edited,
// undone, redone, selected in, drawn over or restyled, with the overlay
// last; `changed`, `sheets_changed` and `modified_changed`; wires carrying
// the simulation's nodes only while the sheet is at the revision that was
// simulated; and a project's sheets loaded and saved: missing files, a file
// that does not load (and is never saved over), a load, edit, save and
// reload round trip, and a sheet saved somewhere else; the connectivity
// rules, from the project or the defaults, followed by the netlist and the
// scene.

#include "history/history_test_support.hpp"

#include <app/ui/schematic_document.hpp>
#include <library/project.hpp>
#include <schematic/demo.hpp>
#include <schematic/io.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

using namespace volt;

namespace
{
    namespace fs = std::filesystem;
    using schematic::item_kind;
    using schematic::item_ref;
    using ui::schematic_document;

    // Counts a signal's emissions.
    struct counter
    {
        int count = 0;
        auto slot()
        {
            return [this] { ++count; };
        }
    };

    // Segments have no ==: the same lines, widths, colours and nodes.
    bool same(const std::vector<schematic::segment> &a, const std::vector<schematic::segment> &b)
    {
        return std::ranges::equal(a, b, [](const schematic::segment &x, const schematic::segment &y) {
            return x.a == y.a && x.b == y.b && x.width == y.width && x.color == y.color && x.node == y.node;
        });
    }

    bool any_node(const std::vector<schematic::segment> &segments)
    {
        return std::ranges::any_of(segments, [](const schematic::segment &s) { return s.node >= 0; });
    }

    void scratch_sheet()
    {
        schematic_document doc;
        CHECK(doc.scratch());
        CHECK(doc.sheet_count() == 1);
        CHECK(doc.sheet_name(0) == schematic_document::scratch_name);
        CHECK(doc.sheet_file(0).empty() && doc.sheet_loaded(0));
        CHECK(doc.active_index() == 0);
        CHECK(doc.active().sheet() == schematic::demo_ladder());
        CHECK(!doc.modified());

        // Its circuit: the ladder, 5 V across it.
        CHECK(doc.circuit().network.nodes > 0);
        CHECK(!doc.netlist().nets.empty());
        schematic::point low;
        schematic::point high;
        CHECK(doc.sheet_bounds(low, high) && low.x() < 0 && high.x() > 0);

        // Nothing saves it.
        auto saved = doc.save_project_sheets(fs::temp_directory_path());
        CHECK(saved.has_value() && saved->written == 0);
    }

    void scene_follows_the_sheet()
    {
        schematic_document doc;
        counter changed;
        doc.changed.connect(changed.slot());

        CHECK(doc.update_scene());
        CHECK(!doc.update_scene()); // nothing changed since
        const std::vector<schematic::segment> original = doc.scene.segments;
        CHECK(!original.empty());

        // An edit, as the tools make one: the scene is rebuilt, and the view
        // was told.
        const std::array<item_ref, 1> r1{item_ref{item_kind::part, 2}};
        CHECK(doc.active().move(r1, {100, 0}));
        CHECK(changed.count == 1);
        CHECK(doc.take_dirty());
        CHECK(doc.update_scene());
        CHECK(!same(doc.scene.segments, original));

        CHECK(doc.active().undo());
        CHECK(changed.count == 2);
        CHECK(doc.update_scene());
        CHECK(same(doc.scene.segments, original));

        CHECK(doc.active().redo());
        CHECK(changed.count == 3);
        CHECK(doc.update_scene());
        CHECK(!same(doc.scene.segments, original));
        CHECK(doc.active().undo());
        CHECK(doc.update_scene());

        // The selection: the same segments, recoloured.
        doc.set_selection(r1);
        CHECK(changed.count == 5);
        CHECK(doc.selection().size() == 1);
        CHECK(doc.update_scene());
        CHECK(doc.scene.segments.size() == original.size());
        CHECK(!same(doc.scene.segments, original));
        doc.set_selection(r1); // the same again: nothing
        CHECK(changed.count == 5);
        CHECK(!doc.update_scene());
        doc.set_selection({});
        CHECK(doc.update_scene());
        CHECK(same(doc.scene.segments, original));

        // The overlay goes last.
        const schematic::segment ghost{{1.0f, 2.0f}, {3.0f, 4.0f}, 0.5f, {1.0f, 0.0f, 1.0f, 1.0f}, -1};
        doc.set_overlay({ghost});
        CHECK(doc.update_scene());
        CHECK(doc.scene.segments.size() == original.size() + 1);
        CHECK(doc.scene.segments.back().a == ghost.a && doc.scene.segments.back().b == ghost.b);
        CHECK(doc.scene.segments.back().width == ghost.width);
        // An edit under the overlay keeps it on top.
        CHECK(doc.active().move(r1, {0, 100}));
        CHECK(doc.update_scene());
        CHECK(doc.scene.segments.back().a == ghost.a);
        CHECK(doc.active().undo());
        doc.set_overlay({});
        CHECK(doc.update_scene());
        CHECK(same(doc.scene.segments, original));

        // The theme: wires in its wire colour, the sheet behind them.
        ui::theme theme = ui::theme::dark_theme();
        theme.canvas_wire = {0.25f, 0.5f, 0.75f, 1.0f};
        theme.canvas = {0.1f, 0.2f, 0.3f, 1.0f};
        doc.apply_theme(theme);
        CHECK(doc.style().wire == theme.canvas_wire);
        CHECK(doc.style().selection[0] == theme.accent[0] && doc.style().selection[3] == 1.0f);
        CHECK(doc.scene.background == math::vec3f{0.1f, 0.2f, 0.3f});
        CHECK(doc.update_scene());
        CHECK(std::ranges::any_of(doc.scene.segments,
                                  [&](const schematic::segment &s) { return s.color == theme.canvas_wire; }));

        // The wire width.
        doc.wire_width = 0.3f;
        doc.apply_wire_width();
        CHECK(doc.style().wire_width == 0.3f);
        CHECK(doc.update_scene());
        CHECK(std::ranges::any_of(doc.scene.segments, [](const schematic::segment &s) { return s.width == 0.3f; }));
    }

    void potentials_only_while_current()
    {
        schematic_document doc;
        CHECK(doc.simulation_state() == schematic_document::run_state::not_run);
        CHECK(doc.update_scene());
        CHECK(!any_node(doc.scene.segments));

        doc.record_simulation();
        CHECK(doc.simulation_current());
        CHECK(doc.simulation_low() == 0.0f && doc.simulation_high() == 5.0f);
        CHECK(doc.update_scene());
        CHECK(any_node(doc.scene.segments));

        // An edit: the numbering may differ from the simulation's now.
        const std::array<item_ref, 1> r1{item_ref{item_kind::part, 2}};
        CHECK(doc.active().move(r1, {0, 100}));
        CHECK(doc.simulation_state() == schematic_document::run_state::edited);
        CHECK(doc.update_scene());
        CHECK(!any_node(doc.scene.segments));

        // Undone, the sheet is as it was but at a later revision: still not
        // shown until the next run.
        CHECK(doc.active().undo());
        CHECK(doc.simulation_state() == schematic_document::run_state::edited);
        doc.record_simulation();
        CHECK(doc.simulation_current());
        CHECK(doc.update_scene());
        CHECK(any_node(doc.scene.segments));

        // The selection still shows over the potentials: a selected wire has
        // no node.
        const schematic::item_id wire = doc.active().sheet().wires[0].id;
        const std::array<item_ref, 1> w{item_ref{item_kind::wire, wire}};
        doc.set_selection(w);
        CHECK(doc.update_scene());
        CHECK(any_node(doc.scene.segments));

        // A fresh set of sheets was never run.
        doc.open_scratch();
        CHECK(doc.simulation_state() == schematic_document::run_state::not_run);
        CHECK(doc.update_scene());
        CHECK(!any_node(doc.scene.segments));
    }

    void modified_follows_the_history()
    {
        schematic_document doc;
        counter modified;
        doc.modified_changed.connect(modified.slot());

        CHECK(doc.active().add_wire({0, 1000}, {500, 1000}) != 0);
        CHECK(doc.modified() && modified.count == 1);
        CHECK(doc.active().add_wire({0, 1200}, {500, 1200}) != 0);
        CHECK(modified.count == 1); // modified already: no news

        CHECK(doc.active().undo());
        CHECK(doc.modified() && modified.count == 1);
        CHECK(doc.active().undo()); // back where it was saved
        CHECK(!doc.modified() && modified.count == 2);
        CHECK(doc.active().redo());
        CHECK(doc.modified() && modified.count == 3);

        // Saved as it is.
        CHECK(doc.active().mark_saved());
        CHECK(!doc.modified() && modified.count == 4);
        CHECK(doc.active().undo());
        CHECK(doc.modified() && modified.count == 5);

        // New sheets are unmodified.
        doc.open_scratch();
        CHECK(!doc.modified() && modified.count == 6);
    }

    void several_sheets()
    {
        schematic_document doc;
        counter sheets;
        counter changed;
        doc.sheets_changed.connect(sheets.slot());
        doc.changed.connect(changed.slot());

        std::vector<schematic_document::new_sheet> opened;
        opened.push_back({"Main", "Main.voltsch", schematic::demo_ladder(), true});
        opened.push_back({"Power", "power.voltsch", {}, true});
        doc.set_sheets(std::move(opened));
        CHECK(!doc.scratch());
        CHECK(doc.sheet_count() == 2 && doc.active_index() == 0);
        CHECK(doc.sheet_name(1) == "Power" && doc.sheet_file(1) == "power.voltsch");
        CHECK(sheets.count == 1 && changed.count >= 1);

        doc.record_simulation();
        const std::array<item_ref, 1> r1{item_ref{item_kind::part, 2}};
        doc.set_selection(r1);
        doc.set_overlay({schematic::segment{}});
        CHECK(doc.update_scene());
        const std::size_t main_segments = doc.scene.segments.size();

        CHECK(doc.set_active(1));
        CHECK(doc.active_index() == 1 && sheets.count == 2);
        CHECK(doc.selection().empty() && doc.overlay().empty());
        CHECK(doc.simulation_state() == schematic_document::run_state::other_sheet);
        CHECK(doc.update_scene());
        CHECK(doc.scene.segments.empty()); // an empty sheet, and no overlay
        CHECK(doc.circuit().network.nodes == 0);
        schematic::point low;
        schematic::point high;
        CHECK(!doc.sheet_bounds(low, high));

        CHECK(!doc.set_active(1)); // shown already
        CHECK(!doc.set_active(2)); // no such sheet
        CHECK(sheets.count == 2);

        // An edit of the other sheet is its own: undo there, not here.
        CHECK(doc.active().add_wire({0, 0}, {300, 0}) != 0);
        CHECK(doc.set_active(0));
        CHECK(!doc.active().can_undo());
        CHECK(doc.modified()); // one sheet is
        CHECK(doc.simulation_current());
        CHECK(doc.update_scene());
        CHECK(doc.scene.segments.size() == main_segments - 1); // the overlay went with the switch

        // No sheets at all is the scratch sheet.
        doc.set_sheets({});
        CHECK(doc.scratch() && doc.sheet_count() == 1);
    }

    // Whoever edits a sheet through a pointer (the editor's tools) lets go of
    // it in sheets_changed: the sheets replaced are still there then, a
    // drag's transaction can still be cancelled in one, and that changes
    // nothing of the new sheets.
    void replaced_sheets_outlive_the_signal()
    {
        schematic_document doc;
        schematic::document *held = &doc.active();
        const std::array<item_ref, 1> r1{item_ref{item_kind::part, 2}};
        held->begin("Move R1");
        CHECK(held->move(r1, {100, 0}));

        counter changed;
        int heard = 0;
        doc.sheets_changed.connect([&] {
            ++heard;
            // The old sheet is alive, its drag still open; cancel it, as
            // the tools do when they move to the new sheet.
            CHECK(held != &doc.active());
            CHECK(held->in_transaction());
            held->cancel();
            CHECK(!held->in_transaction());
            CHECK(held->sheet() == schematic::demo_ladder());
            held = &doc.active();
        });
        doc.changed.connect(changed.slot());

        std::vector<schematic_document::new_sheet> opened;
        opened.push_back({"Main", "Main.voltsch", {}, true});
        doc.set_sheets(std::move(opened));
        CHECK(heard == 1);
        CHECK(held == &doc.active() && held->sheet().parts.empty());
        // The cancel in the retired sheet reached no one: `changed` once,
        // for the new sheets.
        CHECK(changed.count == 1);
        CHECK(!doc.modified());
    }

    std::vector<library::sheet> project_sheets()
    {
        return {library::sheet{"Main", "Main.voltsch", {}}, library::sheet{"Power", "sheets/power.voltsch", {}}};
    }

    void project_round_trip()
    {
        tests::scratch_directory scratch("volt_app_schematic_document_round_trip");
        const fs::path folder = scratch.path();
        const std::vector<library::sheet> entries = project_sheets();

        // Missing files: new, empty sheets, nothing to report.
        schematic_document doc;
        const auto problems = doc.open_project_sheets(folder, entries);
        CHECK(problems.empty());
        CHECK(doc.sheet_count() == 2 && !doc.scratch());
        CHECK(doc.sheet_loaded(0) && doc.sheet_loaded(1));
        CHECK(doc.active().sheet().parts.size() == 0 && !doc.modified());

        // Edited and saved: the edited sheet and the one with no file yet.
        CHECK(doc.active().add_wire({0, 0}, {500, 0}) != 0);
        CHECK(doc.active().add_label({schematic::label_kind::net, {0, 0}, 0, "IN", {}}) != 0);
        auto saved = doc.save_project_sheets(folder);
        CHECK(saved.has_value());
        if (saved)
            CHECK(saved->written == 2 && saved->skipped.empty());
        CHECK(!doc.modified());
        CHECK(tests::file_exists(scratch / "Main.voltsch"));
        CHECK(tests::file_exists(scratch / "sheets/power.voltsch"));
        const std::string main_text = tests::read_text(scratch / "Main.voltsch");

        // Nothing changed: nothing written.
        saved = doc.save_project_sheets(folder);
        CHECK(saved.has_value() && saved->written == 0);

        // Opened again: as saved, unmodified, with no history.
        schematic_document again;
        CHECK(again.open_project_sheets(folder, entries).empty());
        CHECK(again.sheet(0).sheet() == doc.sheet(0).sheet());
        CHECK(again.sheet(0).sheet().wires.size() == 1 && again.sheet(0).sheet().labels.size() == 1);
        CHECK(!again.modified() && !again.active().can_undo());

        // An edit undone is no change: nothing to write.
        CHECK(again.active().add_wire({0, 100}, {500, 100}) != 0);
        CHECK(again.active().undo());
        saved = again.save_project_sheets(folder);
        CHECK(saved.has_value() && saved->written == 0);
        CHECK(tests::read_text(scratch / "Main.voltsch") == main_text);

        // Saved somewhere else: the file follows the sheet.
        const fs::path elsewhere = scratch / "copy/Other.voltsch";
        CHECK(again.active().add_wire({0, 200}, {500, 200}) != 0);
        CHECK(again.write_sheet(0, elsewhere).has_value());
        CHECK(again.modified()); // written, but not this sheet's file yet
        again.sheet_saved_as(0, "copy/Other.voltsch");
        CHECK(!again.modified() && again.sheet_file(0) == "copy/Other.voltsch");
        auto copied = schematic::load_sheet(elsewhere);
        CHECK(copied.has_value() && copied->sheet == again.sheet(0).sheet());
        CHECK(tests::read_text(scratch / "Main.voltsch") == main_text); // the old file stays as it was
    }

    void corrupt_sheet_is_not_overwritten()
    {
        tests::scratch_directory scratch("volt_app_schematic_document_corrupt");
        const fs::path folder = scratch.path();
        const std::vector<library::sheet> entries = project_sheets();

        const std::string broken = "{ this is not a sheet";
        CHECK(tests::write_text(scratch / "sheets/power.voltsch", broken));
        CHECK(schematic::save_sheet(scratch / "Main.voltsch", schematic::demo_ladder()).has_value());

        schematic_document doc;
        const auto problems = doc.open_project_sheets(folder, entries);
        CHECK(problems.size() == 1);
        if (!problems.empty())
        {
            CHECK(problems[0].failed);
            CHECK(problems[0].text.find("Power") != std::string::npos);
        }
        CHECK(doc.sheet_loaded(0) && !doc.sheet_loaded(1));
        CHECK(doc.sheet(0).sheet() == schematic::demo_ladder());
        CHECK(doc.sheet(1).sheet().parts.size() == 0);

        // Edited (it can be), and saved: skipped, and said so; the file is
        // as it was.
        CHECK(doc.set_active(1));
        CHECK(doc.active().add_wire({0, 0}, {100, 0}) != 0);
        auto saved = doc.save_project_sheets(folder);
        CHECK(saved.has_value());
        if (saved)
        {
            CHECK(saved->written == 0);
            CHECK(saved->skipped == std::vector<std::string>{"Power"});
        }
        CHECK(tests::read_text(scratch / "sheets/power.voltsch") == broken);
        CHECK(doc.modified()); // still not saved

        // A file that loads in part: reported, not failed.
        const std::string partial = R"({"format": "volt-schematic", "format_version": 1,
            "parts": [{"id": 1, "symbol": "R", "reference": "R1", "value": "1k", "position": [0, 0], "rotation": 0,
                       "mirrored": false}, 7],
            "wires": [], "labels": []})";
        CHECK(tests::write_text(scratch / "sheets/power.voltsch", partial));
        schematic_document again;
        const auto partial_problems = again.open_project_sheets(folder, entries);
        CHECK(partial_problems.size() == 1);
        if (!partial_problems.empty())
            CHECK(!partial_problems[0].failed);
        CHECK(again.sheet_loaded(1) && again.sheet(1).sheet().parts.size() == 1);

        // A sheet with no file at all opens empty and is never saved.
        schematic_document nameless;
        const std::array<library::sheet, 1> no_file{library::sheet{"Lost", "", {}}};
        const auto lost = nameless.open_project_sheets(folder, no_file);
        CHECK(lost.size() == 1 && !nameless.sheet_loaded(0));
        saved = nameless.save_project_sheets(folder);
        CHECK(saved.has_value() && saved->written == 0 && saved->skipped.size() == 1);
    }

    // The connectivity rules: the defaults for the scratch sheet, the
    // project's when its sheets open, and a change seen by the netlist, the
    // circuit and the scene at once.
    void connectivity_rules()
    {
        schematic_document doc;
        CHECK(doc.connectivity() == schematic::connectivity{});

        // A resistor lying along a wire: shorted by it under the default
        // rules, not when pins connect at wire ends only.
        schematic::sheet s;
        (void)s.parts.insert(1, schematic::part{"R", {0, 0}, 0, false, "R1", "1k", "resistor", {}, {}});
        (void)s.wires.insert(2, schematic::wire{{-300, 0}, {300, 0}, {}});
        std::vector<schematic_document::new_sheet> sheets;
        sheets.push_back({"Main", "main.voltsch", s, true});
        doc.set_sheets(std::move(sheets));
        const auto shorted = [&] { return doc.netlist().net_of_pin(1, "1") == doc.netlist().net_of_pin(1, "2"); };
        CHECK(shorted());
        CHECK(doc.update_scene());
        const std::vector<schematic::segment> drawn = doc.scene.segments;
        (void)doc.take_dirty();

        counter changed;
        counter rules_changed;
        counter modified;
        doc.changed.connect(changed.slot());
        doc.connectivity_changed.connect(rules_changed.slot());
        doc.modified_changed.connect(modified.slot());
        const schematic::connectivity ends_only{.pins_inside_wires = false};
        doc.set_connectivity(ends_only);
        CHECK(doc.connectivity() == ends_only);
        CHECK(changed.count >= 1 && rules_changed.count == 1 && doc.take_dirty());
        CHECK(!shorted() && doc.circuit().network.nodes > 0);
        CHECK(doc.update_scene() && !same(doc.scene.segments, drawn)); // free-pin marks now
        // The same again: nothing.
        doc.set_connectivity(ends_only);
        CHECK(rules_changed.count == 1 && !doc.update_scene());
        // The sheets are not modified by it: the project is (app_services).
        CHECK(!doc.modified() && modified.count == 0);

        // Other sheets keep the rules; the scratch sheet has the defaults.
        std::vector<schematic_document::new_sheet> more;
        more.push_back({"Main", "main.voltsch", s, true});
        doc.set_sheets(std::move(more));
        CHECK(doc.connectivity() == ends_only && !shorted());
        doc.open_scratch();
        CHECK(doc.connectivity() == schematic::connectivity{});
        CHECK(rules_changed.count == 1);

        // A project's sheets open by its rules; a project of no sheets
        // shows the scratch sheet, by its rules all the same.
        tests::scratch_directory scratch("volt_app_schematic_document_rules");
        CHECK(schematic::save_sheet(scratch / "Main.voltsch", s).has_value());
        const std::array<library::sheet, 1> entries{library::sheet{"Main", "Main.voltsch", {}}};
        schematic_document project;
        CHECK(project.open_project_sheets(scratch.path(), entries, ends_only).empty());
        CHECK(project.connectivity() == ends_only && !project.scratch());
        CHECK(project.netlist().net_of_pin(1, "1") != project.netlist().net_of_pin(1, "2"));
        CHECK(project.open_project_sheets(scratch.path(), {}, ends_only).empty());
        CHECK(project.scratch() && project.connectivity() == ends_only);
        CHECK(project.open_project_sheets(scratch.path(), entries).empty());
        CHECK(project.connectivity() == schematic::connectivity{});
    }
} // namespace

int main()
{
    scratch_sheet();
    scene_follows_the_sheet();
    potentials_only_while_current();
    modified_follows_the_history();
    several_sheets();
    replaced_sheets_outlive_the_signal();
    project_round_trip();
    corrupt_sheet_is_not_overwritten();
    connectivity_rules();
    return volt::tests::result("app schematic document");
}
