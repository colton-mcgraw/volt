// The open project's history without a window (app/ui/project_history.hpp):
// the files a project tracks; the states before there is a history, and
// that looking makes none; snapshots taken and "nothing changed"; what
// differs on disk; branches made at the head and from an older snapshot,
// switched between with the files following (and files only one branch
// has removed and brought back); an old snapshot restored without the head
// moving, and recorded by the next snapshot; renaming and deleting
// branches; a failed restore leaving the head and the branches as they
// were; a project file saved under another name; and how long a snapshot
// of a project library of several megabytes takes.

#include "history/history_test_support.hpp"

#include <app/ui/project_history.hpp>
#include <history/sha256.hpp>
#include <library/project.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace volt;
using tests::contains;
using tests::ok;

namespace
{
    namespace fs = std::filesystem;

    // A project saved in `folder`: Blinky.voltproj and Blinky.voltlib (its
    // sheet and board are named but not written, as new projects are).
    library::project saved_project(const fs::path &folder, std::string_view name = "Blinky")
    {
        library::project p = library::new_project(name);
        fs::create_directories(folder);
        const fs::path file = folder / library::path_from_utf8(std::string(name) + ".voltproj");
        if (auto saved = library::save_project(file, p); !saved)
            std::fprintf(stderr, "  cannot save the test project: %s\n", saved.error().c_str());
        return p;
    }

    ui::project_history open_history(const library::project &p, const fs::path &file)
    {
        ui::project_history h;
        h.set_clock(tests::fixed_clock());
        h.set_project(file, &p);
        return h;
    }

    bool has_change(const std::vector<history::file_change> &changes, std::string_view path,
                    history::file_status status)
    {
        return std::ranges::any_of(changes, [&](const history::file_change &c) {
            return c.path == path && c.status == status;
        });
    }

    const ui::history_branch *branch_of(const ui::history_view &view, std::string_view name)
    {
        for (const ui::history_branch &b : view.branches)
            if (b.name == name)
                return &b;
        return nullptr;
    }

    // ---- what a project tracks -------------------------------------------------------

    void tracked_files()
    {
        library::project p = library::new_project("Blinky");
        const fs::path file = fs::path("C:/Projects/Blinky") / "Blinky.voltproj";
        const std::vector<std::string> files = ui::project_history::tracked_files(p, file);
        CHECK(files == (std::vector<std::string>{"Blinky.voltproj", "Blinky.voltlib", "Blinky.voltsch",
                                                 "Blinky.voltpcb"}));

        // Each once; no board, none; a sheet elsewhere as the project has it
        // (the workspace reports it as outside the folder).
        p.board.clear();
        p.sheets.push_back({"Again", "Blinky.voltsch", {}});
        p.sheets.push_back({"Shared", "../shared/power.voltsch", {}});
        const std::vector<std::string> again = ui::project_history::tracked_files(p, file);
        CHECK(again == (std::vector<std::string>{"Blinky.voltproj", "Blinky.voltlib", "Blinky.voltsch",
                                                 "../shared/power.voltsch"}));
    }

    // ---- states, snapshots and status --------------------------------------------------

    void states_and_snapshots()
    {
        tests::scratch_directory scratch("volt_app_project_history_states");
        const fs::path root = scratch / "Blinky";
        const fs::path file = root / "Blinky.voltproj";

        ui::project_history none;
        none.set_project({}, nullptr);
        CHECK(none.state() == ui::history_state::no_project);
        CHECK(!none.can_snapshot());
        CHECK(!none.take_snapshot("x"));

        // Named but not on disk.
        library::project unsaved = library::new_project("Blinky");
        ui::project_history not_saved = open_history(unsaved, file);
        CHECK(not_saved.state() == ui::history_state::not_on_disk);
        CHECK(!not_saved.can_snapshot());
        CHECK(!not_saved.take_snapshot("x"));

        // On disk: no history until the first snapshot, and looking makes
        // none.
        library::project p = saved_project(root);
        ui::project_history h = open_history(p, file);
        CHECK(h.state() == ui::history_state::no_history);
        CHECK(h.can_snapshot() && !h.has_snapshots());
        h.refresh();
        CHECK(!tests::file_exists(root / ".volthistory"));
        CHECK(!h.status()); // nothing to compare with yet

        auto first = h.take_snapshot("Started Blinky");
        CHECK(ok(first));
        if (!first)
            return;
        CHECK(first->created && history::is_object_id(first->id));
        CHECK(tests::file_exists(root / ".volthistory/history.json"));
        // The sheet and the board are not written yet: skipped, and not
        // worth a warning.
        CHECK(first->skipped.size() == 2);
        CHECK(ui::notable_skips(first->skipped).empty());

        const ui::history_view &view = h.view();
        CHECK(h.state() == ui::history_state::ready && h.has_snapshots());
        CHECK(view.branch == "main" && view.head == first->id);
        CHECK(view.branches.size() == 1 && view.branches[0].current);
        CHECK(view.snapshots.size() == 1 && !view.more);
        if (!view.snapshots.empty())
        {
            const ui::history_entry &e = view.snapshots[0];
            CHECK(e.head && e.id == first->id && e.message == "Started Blinky");
            CHECK(e.short_id.size() == 8 && first->id.starts_with(e.short_id));
            CHECK(e.when.size() == 16 && e.when[4] == '-' && e.when[13] == ':');
            CHECK(h.find(first->id) == &e);
        }
        CHECK(view.status_known && !view.changed());

        // Nothing changed: not an error, and nothing new.
        auto again = h.take_snapshot("Again");
        CHECK(ok(again));
        CHECK(again && !again->created && again->id == first->id);
        CHECK(h.view().snapshots.size() == 1);

        // A change on disk shows, by name, after a refresh, and in status()
        // at once.
        CHECK(tests::write_text(root / "Blinky.voltlib", "{\"changed\": true}\n"));
        auto fresh = h.status();
        CHECK(ok(fresh) && fresh && fresh->changes.size() == 1);
        CHECK(!h.view().changed()); // as of the last refresh
        h.refresh();
        CHECK(h.view().changed());
        CHECK(has_change(h.view().changes, "Blinky.voltlib", history::file_status::modified));
        CHECK(ui::describe_changes(h.view().changes) == "Blinky.voltlib (modified)");

        // Only the first line of a message is listed.
        auto second = h.take_snapshot("Changed the library\nwith a longer explanation");
        CHECK(ok(second) && second && second->created);
        CHECK(h.view().snapshots.size() == 2 && h.view().snapshots[0].message == "Changed the library");
        CHECK(h.view().snapshots[0].head && !h.view().snapshots[1].head);

        // A sheet written later joins the next snapshot.
        CHECK(tests::write_text(root / "Blinky.voltsch", "sheet\n"));
        h.refresh();
        CHECK(has_change(h.view().changes, "Blinky.voltsch", history::file_status::added));

        // Following another project, and then none.
        h.set_project({}, nullptr);
        CHECK(h.state() == ui::history_state::no_project && h.view().snapshots.empty());
    }

    void broken_history()
    {
        tests::scratch_directory scratch("volt_app_project_history_broken");
        const fs::path root = scratch / "Blinky";
        const fs::path file = root / "Blinky.voltproj";
        library::project p = saved_project(root);
        CHECK(tests::write_text(root / ".volthistory/history.json", "not json"));
        ui::project_history h = open_history(p, file);
        CHECK(h.state() == ui::history_state::broken);
        CHECK(!h.view().problem.empty());
        CHECK(!h.can_snapshot());
        auto refused = h.take_snapshot("x");
        CHECK(!refused);
        CHECK(tests::read_text(root / ".volthistory/history.json") == "not json");
    }

    // ---- branches ----------------------------------------------------------------------

    void branches()
    {
        tests::scratch_directory scratch("volt_app_project_history_branches");
        const fs::path root = scratch / "Blinky";
        const fs::path file = root / "Blinky.voltproj";
        library::project p = saved_project(root);
        ui::project_history h = open_history(p, file);

        const std::string original_library = tests::read_text(root / "Blinky.voltlib");
        auto first = h.take_snapshot("first");
        CHECK(tests::write_text(root / "Blinky.voltlib", "main 2\n"));
        auto second = h.take_snapshot("second");
        CHECK(ok(first) && ok(second));
        if (!first || !second)
            return;

        // Before the first snapshot there is nothing to branch from.
        {
            tests::scratch_directory other("volt_app_project_history_no_branch");
            library::project q = saved_project(other / "Q", "Q");
            ui::project_history empty = open_history(q, other / "Q" / "Q.voltproj");
            CHECK(!empty.create_branch("x").ok());
        }

        // At the head: the files stay, the branch is current.
        auto at_head = h.create_branch("two-layer");
        CHECK(at_head.ok());
        CHECK(!at_head.files_touched);
        CHECK(h.view().branch == "two-layer" && h.view().head == second->id);
        CHECK(branch_of(h.view(), "two-layer") && branch_of(h.view(), "two-layer")->current);
        CHECK(branch_of(h.view(), "main") && !branch_of(h.view(), "main")->current);
        CHECK(tests::read_text(root / "Blinky.voltlib") == "main 2\n");

        // Names: invalid, taken.
        CHECK(!h.create_branch("main").ok());
        CHECK(!h.create_branch("a/b").ok());
        CHECK(!h.check_branch_name("two-layer"));
        CHECK(h.check_branch_name("two-layer", "two-layer").has_value());
        CHECK(h.check_branch_name("four-layer").has_value());

        // Work on the branch, with a sheet that only it has.
        CHECK(tests::write_text(root / "Blinky.voltlib", "two-layer\n"));
        CHECK(tests::write_text(root / "Blinky.voltsch", "sheet on two-layer\n"));
        h.refresh();
        auto on_branch = h.take_snapshot("routed");
        CHECK(ok(on_branch) && on_branch && on_branch->created);
        CHECK(h.view().snapshots.size() == 3);

        // To main: its files, the sheet it does not have removed.
        auto to_main = h.switch_branch("main");
        CHECK(to_main.ok());
        if (!to_main.ok())
            std::fprintf(stderr, "  %s\n", to_main.error.c_str());
        CHECK(to_main.files_touched && to_main.project_file == file);
        CHECK(tests::read_text(root / "Blinky.voltlib") == "main 2\n");
        CHECK(!tests::file_exists(root / "Blinky.voltsch"));
        CHECK(std::ranges::find(to_main.restored.removed, "Blinky.voltsch") != to_main.restored.removed.end());
        CHECK(h.view().branch == "main" && h.view().head == second->id);
        CHECK(h.view().snapshots.size() == 2);
        CHECK(!h.view().changed());

        // The current branch, or one that is not there: refused, untouched.
        auto same = h.switch_branch("main");
        CHECK(!same.ok() && !same.files_touched);
        auto missing = h.switch_branch("nowhere");
        CHECK(!missing.ok() && !missing.files_touched);

        // And back: the sheet returns.
        auto back = h.switch_branch("two-layer");
        CHECK(back.ok());
        CHECK(tests::read_text(root / "Blinky.voltlib") == "two-layer\n");
        CHECK(tests::read_text(root / "Blinky.voltsch") == "sheet on two-layer\n");
        CHECK(h.view().branch == "two-layer" && h.view().snapshots.size() == 3);

        // A branch from an older snapshot: its files, and it is current.
        auto older = h.create_branch("from-first", first->id);
        CHECK(older.ok());
        CHECK(older.files_touched);
        CHECK(tests::read_text(root / "Blinky.voltlib") == original_library);
        CHECK(!tests::file_exists(root / "Blinky.voltsch"));
        CHECK(h.view().branch == "from-first" && h.view().head == first->id);
        CHECK(h.view().snapshots.size() == 1 && h.view().snapshots[0].head);
        CHECK(!h.view().changed());

        // What deleting would leave unlisted: two-layer's "routed" is on
        // no other branch; main's snapshots are all on two-layer too, and
        // from-first has nothing of its own yet.
        CHECK(h.snapshots_only_on("two-layer") == std::size_t{1});
        CHECK(h.snapshots_only_on("main") == std::size_t{0});
        CHECK(h.snapshots_only_on("from-first") == std::size_t{0});
        CHECK(!h.snapshots_only_on("nowhere"));

        // Renaming, the current one too; deleting, not the current one.
        CHECK(ok(h.rename_branch("from-first", "experiment")));
        CHECK(h.view().branch == "experiment");
        CHECK(!h.rename_branch("experiment", "main"));
        CHECK(!h.rename_branch("experiment", "bad:name"));
        CHECK(!h.delete_branch("experiment"));
        CHECK(ok(h.delete_branch("two-layer")));
        CHECK(branch_of(h.view(), "two-layer") == nullptr);
        CHECK(h.view().branches.size() == 2);
    }

    // ---- restoring ---------------------------------------------------------------------

    void restoring()
    {
        tests::scratch_directory scratch("volt_app_project_history_restore");
        const fs::path root = scratch / "Blinky";
        const fs::path file = root / "Blinky.voltproj";
        library::project p = saved_project(root);
        ui::project_history h = open_history(p, file);

        CHECK(tests::write_text(root / "Blinky.voltlib", "one\n"));
        auto first = h.take_snapshot("one");
        CHECK(tests::write_text(root / "Blinky.voltlib", "two\n"));
        CHECK(tests::write_text(root / "Blinky.voltpcb", "board\n"));
        auto second = h.take_snapshot("two");
        CHECK(ok(first) && ok(second));
        if (!first || !second)
            return;

        // The old files back; the head stays; the difference shows as
        // changes, and the board the old snapshot lacks is removed.
        auto restored = h.restore(first->id);
        CHECK(restored.ok() && restored.files_touched && restored.project_file == file);
        CHECK(tests::read_text(root / "Blinky.voltlib") == "one\n");
        CHECK(!tests::file_exists(root / "Blinky.voltpcb"));
        CHECK(h.view().branch == "main" && h.view().head == second->id);
        CHECK(h.view().changed());
        CHECK(has_change(h.view().changes, "Blinky.voltlib", history::file_status::modified));
        CHECK(has_change(h.view().changes, "Blinky.voltpcb", history::file_status::removed));

        // The next snapshot records it, on top: nothing is lost.
        auto third = h.take_snapshot("back to one");
        CHECK(ok(third) && third && third->created);
        CHECK(h.view().snapshots.size() == 3 && h.view().snapshots[0].message == "back to one");
        CHECK(!h.view().changed());

        // Not a snapshot of this history.
        auto unknown = h.restore(std::string(64, 'a'));
        CHECK(!unknown.ok() && !unknown.files_touched);

        // A damaged blob: nothing written, the head and the branches as
        // they were, and a branch from it not kept.
        CHECK(tests::write_text(root / "Blinky.voltlib", "three\n"));
        auto fourth = h.take_snapshot("three");
        CHECK(ok(fourth));
        auto repo = history::repository::open(root / ".volthistory");
        CHECK(ok(repo));
        if (!repo)
            return;
        CHECK(tests::write_text(tests::object_path(*repo, "blobs", history::sha256_hex("two\n")), "not two\n"));
        auto damaged = h.restore(second->id);
        CHECK(!damaged.ok() && damaged.files_touched);
        CHECK(contains(damaged.error, "nothing was written"));
        CHECK(tests::read_text(root / "Blinky.voltlib") == "three\n");
        auto branch = h.create_branch("from-two", second->id);
        CHECK(!branch.ok());
        CHECK(branch_of(h.view(), "from-two") == nullptr);
        CHECK(h.view().branch == "main" && fourth && h.view().head == fourth->id);
        CHECK(tests::read_text(root / "Blinky.voltlib") == "three\n");
    }

    // ---- a project file under another name ---------------------------------------------

    void renamed_project_file()
    {
        tests::scratch_directory scratch("volt_app_project_history_renamed");
        const fs::path root = scratch / "Blinky";
        const fs::path file = root / "Blinky.voltproj";
        library::project p = saved_project(root);
        ui::project_history h = open_history(p, file);
        auto first = h.take_snapshot("as Blinky");
        CHECK(ok(first));
        CHECK(h.create_branch("renamed").ok());

        // Saved As "Blinky 2" in the same folder: another project file and
        // library, in the same history.
        library::project copy = p;
        copy.library_file = "Blinky 2.voltlib";
        const fs::path file2 = root / "Blinky 2.voltproj";
        CHECK(ok(library::save_project(file2, copy)));
        h.set_project(file2, &copy);
        auto renamed = h.take_snapshot("as Blinky 2");
        CHECK(ok(renamed) && renamed && renamed->created);

        // Back on main the project file is the old one; the new one, which
        // main does not have, is removed.
        auto to_main = h.switch_branch("main");
        CHECK(to_main.ok());
        CHECK(to_main.project_file == file);
        CHECK(!tests::file_exists(file2));
        CHECK(tests::file_exists(file));
    }

    // ---- words -------------------------------------------------------------------------

    void words()
    {
        CHECK(ui::short_id("0123456789abcdef") == "01234567");
        CHECK(ui::short_id("abc") == "abc");

        using history::file_status;
        const auto change = [](std::string path, file_status status) {
            history::file_change c;
            c.path = std::move(path);
            c.status = status;
            return c;
        };
        std::vector<history::file_change> changes = {change("a", file_status::added)};
        CHECK(ui::describe_changes(changes) == "a (added)");
        changes.push_back(change("b", file_status::removed));
        CHECK(ui::describe_changes(changes) == "a (added) and b (removed)");
        changes.push_back(change("c", file_status::modified));
        CHECK(ui::describe_changes(changes) == "a (added), b (removed) and c (modified)");
        changes.push_back(change("d", file_status::modified));
        changes.push_back(change("e", file_status::modified));
        CHECK(ui::describe_changes(changes) == "a (added), b (removed), c (modified) and 2 more files");
        CHECK(ui::describe_changes(changes, 4) == "a (added), b (removed), c (modified), d (modified) and 1 more file");
        CHECK(ui::describe_changes({}).empty());

        // Local time, whatever the zone: its shape, and a minute later is a
        // minute later.
        const std::string when = ui::local_time_text(tests::fixed_time());
        CHECK(when.size() == 16 && when[4] == '-' && when[7] == '-' && when[10] == ' ' && when[13] == ':');
        CHECK(ui::local_time_text(tests::fixed_time() + std::chrono::minutes(1)) != when);

        std::vector<history::skipped_file> skipped = {
            {"Blinky.voltsch", history::skip_reason::missing, "not there"},
            {"../shared/a.voltlib", history::skip_reason::outside_root, "outside"},
        };
        const auto notable = ui::notable_skips(skipped);
        CHECK(notable.size() == 1 && notable[0].path == "../shared/a.voltlib");
    }

    // ---- time --------------------------------------------------------------------------

    // Not a check: what a snapshot and a status cost with a project library
    // of 8 MB, which is what decides that the window does them in place.
    void timing()
    {
        tests::scratch_directory scratch("volt_app_project_history_timing");
        const fs::path root = scratch / "Big";
        const fs::path file = root / "Big.voltproj";
        library::project p = saved_project(root, "Big");
        std::string big(8u << 20, ' ');
        for (std::size_t i = 0; i < big.size(); ++i)
            big[i] = static_cast<char>('a' + (i * 7919u) % 26u);
        CHECK(tests::write_text(root / "Big.voltlib", big));
        ui::project_history h = open_history(p, file);

        using clock = std::chrono::steady_clock;
        const auto started = clock::now();
        auto taken = h.take_snapshot("big");
        const auto snapped = clock::now();
        CHECK(ok(taken));
        auto status = h.status();
        const auto compared = clock::now();
        CHECK(ok(status) && status && status->clean());
        const auto ms = [](clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
        std::printf("  8 MB project library: snapshot (with the list and status after) %.1f ms, status %.1f ms\n",
                    ms(snapped - started), ms(compared - snapped));
    }
} // namespace

int main()
{
    tracked_files();
    states_and_snapshots();
    broken_history();
    branches();
    restoring();
    renamed_project_file();
    words();
    timing();
    return volt::tests::result("project history");
}
