// The repository: creating and opening one, snapshots stored and read back,
// blobs stored once, snapshot ids that depend on nothing but the content,
// "nothing changed", the log, branches and the head, comparing, verifying,
// unknown fields kept, paths and branch names checked, and what is left
// behind by a crash doing no harm.

#include "history_test_support.hpp"

#include <history/sha256.hpp>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <vector>

using namespace volt;
using history::file_content;
using tests::contains;
using tests::ok;

namespace
{
    namespace fs = std::filesystem;

    history::repository make_repository(const fs::path &directory)
    {
        auto repo = history::repository::create(directory);
        if (!repo)
        {
            std::fprintf(stderr, "  cannot create the repository: %s\n", repo.error().c_str());
            std::exit(1);
        }
        repo->set_clock(tests::fixed_clock());
        return std::move(*repo);
    }

    history::snapshot_options options(std::string message, std::string author = {}, bool allow_unchanged = false)
    {
        history::snapshot_options o;
        o.message = std::move(message);
        o.author = std::move(author);
        o.allow_unchanged = allow_unchanged;
        return o;
    }

    std::vector<file_content> project_files(std::string_view sheet = "sheet v1\n")
    {
        return {
            {"Blinky.voltproj", "{\"name\": \"Blinky\"}\n"},
            {"sheets/Main.voltsch", std::string(sheet)},
            {"\xC3\x9C" "ber.voltsch", "\xC3\xBC" "ber\n"},
        };
    }

    // ---- creating and opening ------------------------------------------------------

    void creating_and_opening()
    {
        tests::scratch_directory scratch("volt_history_repository_open");
        const fs::path directory = scratch / ".volthistory";
        CHECK(!history::repository::exists(directory));

        auto opened = history::repository::open(directory);
        CHECK(!opened);
        if (!opened)
            CHECK(contains(opened.error(), "no Volt history"));

        auto created = history::repository::create(directory);
        CHECK(ok(created));
        if (!created)
            return;
        CHECK(history::repository::exists(directory));
        CHECK(fs::is_directory(directory / "blobs"));
        CHECK(fs::is_directory(directory / "snapshots"));
        CHECK(created->branches().size() == 1 && created->branches()[0].name == "main");
        CHECK(created->branches()[0].snapshot.empty());
        CHECK(created->head() == history::head_state{"main", ""});
        CHECK(!created->head().detached());
        CHECK(tests::read_text(directory / "history.json") == "{\n"
                                                              "  \"format\": \"volt-history\",\n"
                                                              "  \"format_version\": 1,\n"
                                                              "  \"head\": {\"branch\": \"main\"},\n"
                                                              "  \"branches\": [\n"
                                                              "    {\"name\": \"main\"}\n"
                                                              "  ]\n"
                                                              "}\n");

        // Not twice.
        auto again = history::repository::create(directory);
        CHECK(!again);
        if (!again)
            CHECK(contains(again.error(), "already"));

        auto reopened = history::repository::open_or_create(directory);
        CHECK(ok(reopened));
        if (reopened)
            CHECK(reopened->head() == created->head());

        // open_or_create makes one where there is none.
        auto fresh = history::repository::open_or_create(scratch / "other");
        CHECK(ok(fresh));
        CHECK(history::repository::exists(scratch / "other"));

        // A directory is kept absolute, so a later change of the working
        // directory does not move it.
        CHECK(created->directory().is_absolute());
    }

    void rejecting_what_is_not_a_history()
    {
        tests::scratch_directory scratch("volt_history_repository_reject");
        const auto try_open = [&](std::string_view name, std::string_view text) {
            const fs::path directory = scratch / name;
            CHECK(tests::write_text(directory / "history.json", text));
            return history::repository::open(directory);
        };

        auto not_json = try_open("a", "{not json");
        CHECK(!not_json);
        if (!not_json)
            CHECK(contains(not_json.error(), "history.json") && contains(not_json.error(), "line 1"));

        auto other = try_open("b", R"({"format": "volt-project", "format_version": 1})");
        CHECK(!other);
        if (!other)
            CHECK(contains(other.error(), "not a Volt history"));

        auto newer = try_open("c", R"({"format": "volt-history", "format_version": 2, "head": {"branch": "main"},
                                       "branches": [{"name": "main"}]})");
        CHECK(!newer);
        if (!newer)
            CHECK(contains(newer.error(), "newer version of Volt") && contains(newer.error(), "version 2"));

        auto no_branch = try_open("d", R"({"format": "volt-history", "format_version": 1, "head": {"branch": "x"},
                                           "branches": [{"name": "main"}]})");
        CHECK(!no_branch);
        if (!no_branch)
            CHECK(contains(no_branch.error(), "does not exist"));

        auto bad_name = try_open("e", R"({"format": "volt-history", "format_version": 1, "head": {"branch": "a/b"},
                                          "branches": [{"name": "a/b"}]})");
        CHECK(!bad_name);

        auto twice = try_open("f", R"({"format": "volt-history", "format_version": 1, "head": {"branch": "main"},
                                       "branches": [{"name": "main"}, {"name": "main"}]})");
        CHECK(!twice);

        auto bad_id = try_open("g", R"({"format": "volt-history", "format_version": 1, "head": {"branch": "main"},
                                        "branches": [{"name": "main", "snapshot": "abc"}]})");
        CHECK(!bad_id);

        auto no_head = try_open("h", R"({"format": "volt-history", "format_version": 1, "head": {},
                                         "branches": [{"name": "main"}]})");
        CHECK(!no_head);

        // A detached head is fine.
        auto detached = try_open("i", std::format(R"({{"format": "volt-history", "format_version": 1,
                                                       "head": {{"snapshot": "{}"}}, "branches": []}})",
                                                  std::string(64, 'a')));
        CHECK(ok(detached));
        if (detached)
        {
            CHECK(detached->head().detached());
            CHECK(detached->head().snapshot == std::string(64, 'a'));
        }
    }

    // ---- snapshots -----------------------------------------------------------------

    void snapshots_round_trip()
    {
        tests::scratch_directory scratch("volt_history_repository_round_trip");
        history::repository repo = make_repository(scratch / ".volthistory");

        auto taken = repo.take_snapshot(project_files(), options("First", "Ada"));
        CHECK(ok(taken));
        if (!taken)
            return;
        CHECK(taken->created);
        CHECK(history::is_object_id(taken->id));
        CHECK(repo.head() == history::head_state{"main", taken->id});
        CHECK(repo.find_branch("main") && repo.find_branch("main")->snapshot == taken->id);
        CHECK(repo.has_snapshot(taken->id));

        auto s = repo.read_snapshot(taken->id);
        CHECK(ok(s));
        if (!s)
            return;
        CHECK(s->id == taken->id);
        CHECK(s->parents.empty());
        CHECK(s->message == "First");
        CHECK(s->author == "Ada");
        CHECK(s->time == "2026-11-22T14:03:07Z");
        CHECK(s->format_version == 1);
        CHECK(s->files.size() == 3);
        if (s->files.size() == 3)
        {
            // Sorted by path, bytewise: 'B' < 's' < 0xC3.
            CHECK(s->files[0].path == "Blinky.voltproj");
            CHECK(s->files[1].path == "sheets/Main.voltsch");
            CHECK(s->files[2].path == "\xC3\x9C" "ber.voltsch");
            CHECK(s->files[1].blob == history::sha256_hex("sheet v1\n"));
            CHECK(s->files[1].size == 9);
        }
        CHECK(s->find("sheets/Main.voltsch") != nullptr);
        CHECK(s->find("nope") == nullptr);

        for (const file_content &f : project_files())
        {
            auto bytes = repo.read_file(taken->id, f.path);
            CHECK(ok(bytes));
            CHECK(bytes && *bytes == f.bytes);
        }
        CHECK(!repo.read_file(taken->id, "nope.voltsch"));
        CHECK(!repo.read_snapshot("not an id"));
        CHECK(!repo.read_snapshot(std::string(64, '0')));
        CHECK(!repo.read_blob(std::string(64, '0')));

        // Binary bytes are fine.
        std::string binary;
        for (int i = 0; i < 256; ++i)
            binary += static_cast<char>(i);
        auto second = repo.take_snapshot(std::vector<file_content>{{"model.step", binary}});
        CHECK(ok(second));
        if (second)
        {
            CHECK(repo.read_file(second->id, "model.step") == binary);
            auto s2 = repo.read_snapshot(second->id);
            CHECK(s2 && s2->parents == std::vector<std::string>{taken->id});
            CHECK(s2 && s2->message.empty() && s2->author.empty());
        }

        // An empty set of files is a snapshot too, when it is a change.
        auto empty = repo.take_snapshot({});
        CHECK(ok(empty) && empty->created);
        if (empty)
        {
            auto s3 = repo.read_snapshot(empty->id);
            CHECK(s3 && s3->files.empty());
        }
    }

    void blobs_are_stored_once()
    {
        tests::scratch_directory scratch("volt_history_repository_dedup");
        history::repository repo = make_repository(scratch / ".volthistory");
        const fs::path blobs = repo.directory() / "blobs";

        // Two files with the same bytes: one blob.
        std::vector<file_content> files = {{"a.txt", "same\n"}, {"b.txt", "same\n"}, {"c.txt", "other\n"}};
        CHECK(ok(repo.take_snapshot(files, options("one"))));
        CHECK(tests::count_files(blobs) == 2);
        CHECK(tests::count_files(repo.directory() / "snapshots") == 1);

        // One file changed: one blob more.
        files[2].bytes = "changed\n";
        CHECK(ok(repo.take_snapshot(files, options("two"))));
        CHECK(tests::count_files(blobs) == 3);

        // Back to the first bytes: nothing new to store but the snapshot.
        files[2].bytes = "other\n";
        CHECK(ok(repo.take_snapshot(files, options("three"))));
        CHECK(tests::count_files(blobs) == 3);
        CHECK(tests::count_files(repo.directory() / "snapshots") == 3);

        // No temporary file is left behind.
        std::error_code error;
        for (fs::recursive_directory_iterator it(repo.directory(), error), end; !error && it != end; it.increment(error))
            CHECK(it->path().extension() != ".partial");
    }

    void ids_depend_on_content_alone()
    {
        tests::scratch_directory scratch("volt_history_repository_determinism");
        history::repository a = make_repository(scratch / "a");
        history::repository b = make_repository(scratch / "b");

        // The same files given in another order.
        std::vector<file_content> files = project_files();
        std::vector<file_content> reversed(files.rbegin(), files.rend());
        auto in_a = a.take_snapshot(files, options("m", "Ada"));
        auto in_b = b.take_snapshot(reversed, options("m", "Ada"));
        CHECK(ok(in_a) && ok(in_b));
        if (!in_a || !in_b)
            return;
        CHECK(in_a->id == in_b->id);

        // The text, exactly.
        const std::string text = tests::read_text(tests::object_path(a, "snapshots", in_a->id));
        CHECK(history::sha256_hex(text) == in_a->id);
        const std::string expected = std::format(
            "{{\n"
            "  \"format\": \"volt-snapshot\",\n"
            "  \"format_version\": 1,\n"
            "  \"parents\": [],\n"
            "  \"time\": \"2026-11-22T14:03:07Z\",\n"
            "  \"author\": \"Ada\",\n"
            "  \"message\": \"m\",\n"
            "  \"files\": [\n"
            "    {{\"path\": \"Blinky.voltproj\", \"blob\": \"{}\", \"size\": 19}},\n"
            "    {{\"path\": \"sheets/Main.voltsch\", \"blob\": \"{}\", \"size\": 9}},\n"
            "    {{\"path\": \"\xC3\x9C" "ber.voltsch\", \"blob\": \"{}\", \"size\": 6}}\n"
            "  ]\n"
            "}}\n",
            history::sha256_hex(files[0].bytes), history::sha256_hex(files[1].bytes),
            history::sha256_hex(files[2].bytes));
        CHECK(text == expected);
        if (text != expected)
            std::fprintf(stderr, "%s\n", text.c_str());

        // Another message, author or time is another snapshot.
        history::repository c = make_repository(scratch / "c");
        auto other_message = c.take_snapshot(files, options("n", "Ada"));
        CHECK(other_message && other_message->id != in_a->id);
        history::repository d = make_repository(scratch / "d");
        d.set_clock([] { return tests::fixed_time() + std::chrono::seconds{1}; });
        auto other_time = d.take_snapshot(files, options("m", "Ada"));
        CHECK(other_time && other_time->id != in_a->id);
    }

    void nothing_changed()
    {
        tests::scratch_directory scratch("volt_history_repository_unchanged");
        history::repository repo = make_repository(scratch / ".volthistory");

        // Nothing at all, in a new repository: nothing changed either.
        auto empty = repo.take_snapshot({});
        CHECK(ok(empty));
        CHECK(empty && !empty->created && empty->id.empty());
        CHECK(repo.head().snapshot.empty());
        CHECK(tests::count_files(repo.directory() / "snapshots") == 0);

        auto first = repo.take_snapshot(project_files(), options("first"));
        CHECK(ok(first) && first->created);
        if (!first)
            return;

        // The same files, another message: still nothing changed.
        auto same = repo.take_snapshot(project_files(), options("again"));
        CHECK(ok(same));
        CHECK(same && !same->created && same->id == first->id);
        CHECK(repo.head().snapshot == first->id);
        CHECK(tests::count_files(repo.directory() / "snapshots") == 1);

        // Unless asked for.
        auto forced = repo.take_snapshot(project_files(), options("again", {}, true));
        CHECK(ok(forced));
        CHECK(forced && forced->created && forced->id != first->id);
        if (forced)
        {
            auto s = repo.read_snapshot(forced->id);
            CHECK(s && s->parents == std::vector<std::string>{first->id});
            CHECK(repo.head().snapshot == forced->id);
        }

        // A file removed is a change.
        std::vector<file_content> fewer = project_files();
        fewer.pop_back();
        auto removed = repo.take_snapshot(fewer);
        CHECK(removed && removed->created);
    }

    void the_log()
    {
        tests::scratch_directory scratch("volt_history_repository_log");
        history::repository repo = make_repository(scratch / ".volthistory");

        auto none = repo.log();
        CHECK(ok(none) && none->empty());

        std::vector<std::string> ids;
        for (int i = 0; i < 5; ++i)
        {
            auto taken = repo.take_snapshot(std::vector<file_content>{{"a.txt", std::format("version {}\n", i)}},
                                            options(std::format("snapshot {}", i)));
            CHECK(ok(taken));
            if (taken)
                ids.push_back(taken->id);
        }
        if (ids.size() != 5)
            return;

        auto all = repo.log();
        CHECK(ok(all));
        if (all)
        {
            CHECK(all->size() == 5);
            for (std::size_t i = 0; i < all->size() && i < 5; ++i)
            {
                CHECK((*all)[i].id == ids[4 - i]);
                CHECK((*all)[i].message == std::format("snapshot {}", 4 - i));
            }
        }

        auto two = repo.log({}, 2);
        CHECK(two && two->size() == 2 && (*two)[0].id == ids[4] && (*two)[1].id == ids[3]);

        auto from_middle = repo.log(ids[2]);
        CHECK(from_middle && from_middle->size() == 3 && (*from_middle)[0].id == ids[2] &&
              (*from_middle)[2].id == ids[0]);

        auto zero = repo.log({}, 0);
        CHECK(zero && zero->empty());

        CHECK(!repo.log("nonsense"));
    }

    // ---- branches ------------------------------------------------------------------

    void branches_and_the_head()
    {
        tests::scratch_directory scratch("volt_history_repository_branches");
        history::repository repo = make_repository(scratch / ".volthistory");

        // A branch made before any snapshot has none either.
        CHECK(ok(repo.create_branch("early")));
        CHECK(repo.find_branch("early") && repo.find_branch("early")->snapshot.empty());

        auto first = repo.take_snapshot(std::vector<file_content>{{"a.txt", "1\n"}});
        auto second = repo.take_snapshot(std::vector<file_content>{{"a.txt", "2\n"}});
        CHECK(ok(first) && ok(second));
        if (!first || !second)
            return;
        CHECK(repo.find_branch("early")->snapshot.empty());

        // At the head, and at a given snapshot; the head stays.
        CHECK(ok(repo.create_branch("at-head")));
        CHECK(repo.find_branch("at-head")->snapshot == second->id);
        CHECK(ok(repo.create_branch("two-layer", first->id)));
        CHECK(repo.find_branch("two-layer")->snapshot == first->id);
        CHECK(repo.head() == history::head_state{"main", second->id});
        CHECK(repo.branches().size() == 4);
        CHECK(repo.branches()[1].name == "early"); // in the order made

        // Names are compared exactly: "Main" is another branch.
        CHECK(ok(repo.create_branch("Main")));
        CHECK(ok(repo.create_branch("\xC3\x9C" "berarbeitung")));

        // Switching moves the head only; a snapshot there advances that branch.
        CHECK(ok(repo.switch_branch("two-layer")));
        CHECK(repo.head() == history::head_state{"two-layer", first->id});
        auto on_branch = repo.take_snapshot(std::vector<file_content>{{"a.txt", "1b\n"}});
        CHECK(ok(on_branch));
        if (!on_branch)
            return;
        CHECK(repo.find_branch("two-layer")->snapshot == on_branch->id);
        CHECK(repo.find_branch("main")->snapshot == second->id);
        auto branched = repo.read_snapshot(on_branch->id);
        CHECK(branched && branched->parents == std::vector<std::string>{first->id});

        // Detached: snapshots move the head, no branch.
        CHECK(ok(repo.detach(second->id)));
        CHECK(repo.head().detached());
        CHECK(repo.head().snapshot == second->id);
        auto detached = repo.take_snapshot(std::vector<file_content>{{"a.txt", "2b\n"}});
        CHECK(ok(detached));
        if (!detached)
            return;
        CHECK(repo.head() == history::head_state{"", detached->id});
        CHECK(repo.find_branch("main")->snapshot == second->id);
        // A branch made there keeps it.
        CHECK(ok(repo.create_branch("experiment")));
        CHECK(repo.find_branch("experiment")->snapshot == detached->id);
        CHECK(ok(repo.switch_branch("experiment")));
        CHECK(repo.head() == history::head_state{"experiment", detached->id});

        // Renaming the current branch: the head follows.
        CHECK(ok(repo.rename_branch("experiment", "idea")));
        CHECK(repo.find_branch("experiment") == nullptr);
        CHECK(repo.head() == history::head_state{"idea", detached->id});
        CHECK(ok(repo.rename_branch("idea", "idea"))); // to itself: nothing
        CHECK(ok(repo.rename_branch("early", "earliest")));
        CHECK(repo.head().branch == "idea");

        // Deleting: not the current one.
        auto current = repo.delete_branch("idea");
        CHECK(!current);
        if (!current)
            CHECK(contains(current.error(), "current branch"));
        CHECK(ok(repo.delete_branch("earliest")));
        CHECK(repo.find_branch("earliest") == nullptr);
        // Its snapshots stay.
        CHECK(repo.has_snapshot(first->id));

        // Errors.
        CHECK(!repo.create_branch("main"));               // in use
        CHECK(!repo.create_branch("x", std::string(64, 'f'))); // no such snapshot
        CHECK(!repo.create_branch("x", "abc"));           // not an id
        CHECK(!repo.switch_branch("nope"));
        CHECK(!repo.switch_branch("main ")); // exact names
        CHECK(!repo.detach(std::string(64, 'f')));
        CHECK(!repo.detach("main"));
        CHECK(!repo.rename_branch("nope", "x"));
        CHECK(!repo.rename_branch("main", "Main")); // taken
        CHECK(!repo.rename_branch("main", "a/b"));
        CHECK(!repo.delete_branch("nope"));
        for (const std::string_view bad : {"", "a/b", "a\\b", "a:b", " a", "a ", "a\tb", "a\nb", "\x7F"})
            CHECK(!repo.create_branch(bad));
        CHECK(!repo.create_branch(std::string(101, 'x')));
        CHECK(ok(repo.create_branch(std::string(100, 'x'))));
        CHECK(!repo.create_branch("bad\xC3"));
        CHECK(!repo.create_branch("fix: no"));
        CHECK(ok(repo.create_branch("two words")));

        // All of it from disk again.
        auto reopened = history::repository::open(repo.directory());
        CHECK(ok(reopened));
        if (reopened)
        {
            CHECK(reopened->head() == repo.head());
            CHECK(reopened->branches() == repo.branches());
        }
    }

    // ---- comparing -----------------------------------------------------------------

    void comparing()
    {
        tests::scratch_directory scratch("volt_history_repository_compare");
        history::repository repo = make_repository(scratch / ".volthistory");

        auto one = repo.take_snapshot(
            std::vector<file_content>{{"a.txt", "a\n"}, {"b.txt", "b\n"}, {"c.txt", "c\n"}});
        auto two = repo.take_snapshot(
            std::vector<file_content>{{"a.txt", "a\n"}, {"b.txt", "B\n"}, {"d/e.txt", "e\n"}});
        CHECK(ok(one) && ok(two));
        if (!one || !two)
            return;

        auto changes = repo.changes_between(one->id, two->id);
        CHECK(ok(changes));
        if (changes)
        {
            CHECK(changes->size() == 3);
            if (changes->size() == 3)
            {
                CHECK((*changes)[0] == history::file_change{"b.txt", history::file_status::modified,
                                                            history::sha256_hex("b\n"), history::sha256_hex("B\n")});
                CHECK((*changes)[1] ==
                      history::file_change{"c.txt", history::file_status::removed, history::sha256_hex("c\n"), {}});
                CHECK((*changes)[2] ==
                      history::file_change{"d/e.txt", history::file_status::added, {}, history::sha256_hex("e\n")});
            }
        }

        // The other way round.
        auto back = repo.changes_between(two->id, one->id);
        CHECK(back && back->size() == 3 && (*back)[1].status == history::file_status::added &&
              (*back)[2].status == history::file_status::removed);

        // With itself, and from nothing.
        auto itself = repo.changes_between(one->id, one->id);
        CHECK(itself && itself->empty());
        auto from_nothing = repo.changes_between({}, one->id);
        CHECK(from_nothing && from_nothing->size() == 3 &&
              std::ranges::all_of(*from_nothing,
                                  [](const history::file_change &c) { return c.status == history::file_status::added; }));

        // Files in memory against a snapshot.
        const std::vector<file_content> now = {{"a.txt", "a\n"}, {"d/e.txt", "e2\n"}, {"f.txt", "f\n"}};
        auto since = repo.changes_since(two->id, now);
        CHECK(ok(since));
        if (since)
        {
            CHECK(since->size() == 3);
            if (since->size() == 3)
            {
                CHECK((*since)[0].path == "b.txt" && (*since)[0].status == history::file_status::removed);
                CHECK((*since)[1].path == "d/e.txt" && (*since)[1].status == history::file_status::modified);
                CHECK((*since)[2].path == "f.txt" && (*since)[2].status == history::file_status::added);
            }
        }
        auto unchanged = repo.changes_since(two->id, std::vector<file_content>{
                                                         {"d/e.txt", "e\n"}, {"b.txt", "B\n"}, {"a.txt", "a\n"}});
        CHECK(unchanged && unchanged->empty());
        CHECK(!repo.changes_since(two->id, std::vector<file_content>{{"../x", "x"}}));
        CHECK(!repo.changes_between(one->id, std::string(64, '0')));

        CHECK(history::describe(history::file_status::added) == "added");
        CHECK(history::describe(history::file_status::removed) == "removed");
        CHECK(history::describe(history::file_status::modified) == "modified");
    }

    // ---- verification and damage ---------------------------------------------------

    void verification()
    {
        tests::scratch_directory scratch("volt_history_repository_verify");
        history::repository repo = make_repository(scratch / ".volthistory");

        CHECK(repo.verify().empty());
        auto one = repo.take_snapshot(std::vector<file_content>{{"a.txt", "a\n"}, {"b.txt", "b\n"}});
        auto two = repo.take_snapshot(std::vector<file_content>{{"a.txt", "a2\n"}, {"b.txt", "b\n"}});
        CHECK(ok(one) && ok(two));
        if (!one || !two)
            return;
        CHECK(ok(repo.create_branch("old", one->id)));
        CHECK(repo.verify().empty());

        // A blob deleted, another tampered with: both found.
        const std::string a1 = history::sha256_hex("a\n");
        const std::string b = history::sha256_hex("b\n");
        std::error_code error;
        fs::remove(tests::object_path(repo, "blobs", a1), error);
        CHECK(tests::write_text(tests::object_path(repo, "blobs", b), "B\n"));
        std::vector<history::verify_problem> problems = repo.verify();
        CHECK(problems.size() == 2);
        const auto find = [&](std::string_view id) -> const history::verify_problem * {
            for (const history::verify_problem &p : problems)
                if (p.id == id)
                    return &p;
            return nullptr;
        };
        const history::verify_problem *missing = find(a1);
        const history::verify_problem *damaged = find(b);
        CHECK(missing && missing->missing && contains(missing->message, "missing") &&
              contains(missing->message, "a.txt"));
        CHECK(damaged && !damaged->missing && contains(damaged->message, "damaged"));

        // Reading says so too.
        auto read = repo.read_file(two->id, "b.txt");
        CHECK(!read);
        if (!read)
            CHECK(contains(read.error(), "damaged"));
        CHECK(!repo.read_file(one->id, "a.txt"));

        // Storing the same bytes again repairs both.
        CHECK(ok(repo.take_snapshot(std::vector<file_content>{{"a.txt", "a\n"}, {"b.txt", "b\n"}})));
        CHECK(repo.verify().empty());
        CHECK(repo.read_file(two->id, "b.txt") == std::string("b\n"));

        // A snapshot that is a parent, deleted: found, and the walk goes on
        // to what else there is.
        fs::remove(tests::object_path(repo, "snapshots", one->id), error);
        CHECK(tests::write_text(tests::object_path(repo, "blobs", history::sha256_hex("a2\n")), "tampered"));
        problems = repo.verify();
        CHECK(problems.size() == 2);
        CHECK(find(one->id) && find(one->id)->missing);
        CHECK(find(history::sha256_hex("a2\n")) && !find(history::sha256_hex("a2\n"))->missing);

        // A damaged snapshot.
        CHECK(tests::write_text(tests::object_path(repo, "snapshots", two->id), "{}"));
        problems = repo.verify();
        CHECK(find(two->id) && !find(two->id)->missing);
        auto log = repo.log();
        CHECK(!log);
    }

    void reopening_keeps_unknown_fields()
    {
        tests::scratch_directory scratch("volt_history_repository_unknown");
        const fs::path directory = scratch / ".volthistory";
        std::string id;
        {
            history::repository repo = make_repository(directory);
            auto taken = repo.take_snapshot(project_files());
            CHECK(ok(taken));
            if (!taken)
                return;
            id = taken->id;
        }
        CHECK(tests::write_text(directory / "history.json",
                                std::format(R"({{
  "format": "volt-history",
  "format_version": 1,
  "head": {{"branch": "main", "since": "yesterday"}},
  "branches": [
    {{"name": "main", "snapshot": "{}", "color": "#ff8800"}}
  ],
  "remote": {{"url": "later"}}
}}
)",
                                            id)));
        auto repo = history::repository::open(directory);
        CHECK(ok(repo));
        if (!repo)
            return;
        CHECK(repo->head() == history::head_state{"main", id});
        CHECK(repo->branches()[0].extra.find("color") != nullptr);

        CHECK(ok(repo->create_branch("second")));
        auto document = json::read_file(directory / "history.json");
        CHECK(ok(document));
        if (document)
        {
            CHECK(document->at("remote").at("url").string_or() == "later");
            CHECK(document->at("head").at("since").string_or() == "yesterday");
            CHECK(document->at("branches").at(0).at("color").string_or() == "#ff8800");
            CHECK(document->at("branches").at(1).at("name").string_or() == "second");
        }

        // A snapshot with fields this version does not know reads, and
        // keeps them.
        const std::string blob = history::sha256_hex("x\n");
        CHECK(tests::write_text(tests::object_path(*repo, "blobs", blob), "x\n"));
        const std::string text = std::format(
            R"({{"format": "volt-snapshot", "format_version": 1, "parents": [], "time": "2026-01-01T00:00:00Z",
                 "author": "", "message": "hand made", "tags": ["v1"],
                 "files": [{{"path": "x.txt", "blob": "{}", "size": 2, "mode": "rw"}}]}})",
            blob);
        const std::string hand_made = history::sha256_hex(text);
        CHECK(tests::write_text(tests::object_path(*repo, "snapshots", hand_made), text));
        auto s = repo->read_snapshot(hand_made);
        CHECK(ok(s));
        if (s)
        {
            CHECK(s->extra.find("tags") != nullptr);
            CHECK(s->files.size() == 1 && s->files[0].extra.find("mode") != nullptr);
            CHECK(s->message == "hand made");
        }

        // One from a newer Volt does not.
        const std::string newer = R"({"format": "volt-snapshot", "format_version": 2, "parents": [], "files": []})";
        CHECK(tests::write_text(tests::object_path(*repo, "snapshots", history::sha256_hex(newer)), newer));
        auto too_new = repo->read_snapshot(history::sha256_hex(newer));
        CHECK(!too_new);
        if (!too_new)
            CHECK(contains(too_new.error(), "newer"));

        // Nor one whose files are not sorted, or differ only in case.
        const std::string unsorted = std::format(
            R"({{"format": "volt-snapshot", "format_version": 1, "parents": [],
                 "files": [{{"path": "b", "blob": "{0}", "size": 2}}, {{"path": "a", "blob": "{0}", "size": 2}}]}})",
            blob);
        CHECK(tests::write_text(tests::object_path(*repo, "snapshots", history::sha256_hex(unsorted)), unsorted));
        CHECK(!repo->read_snapshot(history::sha256_hex(unsorted)));
        const std::string cased = std::format(
            R"({{"format": "volt-snapshot", "format_version": 1, "parents": [],
                 "files": [{{"path": "A", "blob": "{0}", "size": 2}}, {{"path": "a", "blob": "{0}", "size": 2}}]}})",
            blob);
        CHECK(tests::write_text(tests::object_path(*repo, "snapshots", history::sha256_hex(cased)), cased));
        CHECK(!repo->read_snapshot(history::sha256_hex(cased)));
    }

    void leftovers_of_a_crash_do_no_harm()
    {
        tests::scratch_directory scratch("volt_history_repository_crash");
        const fs::path directory = scratch / ".volthistory";
        history::repository repo = make_repository(directory);
        auto first = repo.take_snapshot(project_files());
        CHECK(ok(first));

        // A crash after the objects of a second snapshot were written but
        // before history.json was: objects nothing names, and temporary
        // files.
        CHECK(tests::write_text(tests::object_path(repo, "blobs", history::sha256_hex("orphan")), "orphan"));
        CHECK(tests::write_text(directory / "history.json.partial", "{\"half\": "));
        CHECK(tests::write_text(directory / "blobs" / "ab" / "cdef.partial", "half a blob"));

        auto reopened = history::repository::open(directory);
        CHECK(ok(reopened));
        if (!reopened)
            return;
        CHECK(reopened->head().snapshot == first->id);
        CHECK(reopened->verify().empty());
        reopened->set_clock(tests::fixed_clock());
        auto second = reopened->take_snapshot(project_files("sheet v2\n"));
        CHECK(ok(second) && second->created);
        CHECK(!tests::file_exists(directory / "history.json.partial"));
    }

    // ---- names ---------------------------------------------------------------------

    void paths()
    {
        for (const std::string_view good :
             {"a", "a.txt", "sheets/Main.voltsch", "a/b/c/d.e", "\xC3\x9C" "ber.voltsch", ".hidden", "a..b",
              "with space.txt", "con-artist.txt", "COM10", "x.con"})
        {
            const auto checked = history::check_path(good);
            CHECK(checked.has_value());
            if (!checked)
                std::fprintf(stderr, "  %s\n", checked.error().c_str());
        }
        const std::string_view bad_paths[] = {"", "/abs", "a//b", "a/", "./a", "a/./b", "../a", "a/..", "a\\b", "C:/x", "C:x", "a:b", "a\tb",
              "a\nb", std::string_view("a\0b", 3), "a*b", "a?b", "a|b", "a<b", "a>b", "a\"b", "a.", "a ", "dir./x",
              "CON", "nul.txt", "Com1", "lpt9.voltsch", "aux", "bad\xC3", "\xC0\xAF", "\xED\xA0\x80"};
        for (const std::string_view bad : bad_paths)
        {
            const auto checked = history::check_path(bad);
            CHECK(!checked.has_value());
            if (checked)
                std::fprintf(stderr, "  accepted: %.*s\n", static_cast<int>(bad.size()), bad.data());
        }
        CHECK(!history::check_path(std::string(256, 'a')));
        CHECK(history::check_path(std::string(255, 'a')).has_value());
        std::string long_path;
        for (int i = 0; i < 30; ++i)
            long_path += std::string(200, 'a') + "/";
        long_path += "x";
        CHECK(!history::check_path(long_path));

        CHECK(history::same_path_ignoring_case("A/B.txt", "a/b.TXT"));
        CHECK(history::same_path_ignoring_case("\xC3\x9C" "ber", "\xC3\xBC" "ber"));   // Ü, ü
        CHECK(history::same_path_ignoring_case("\xD0\x96", "\xD0\xB6"));                // Ж, ж
        CHECK(history::same_path_ignoring_case("\xCE\xA3", "\xCF\x83"));                // Σ, σ
        CHECK(history::same_path_ignoring_case("\xC5\x81", "\xC5\x82"));                // Ł, ł
        CHECK(!history::same_path_ignoring_case("a", "b"));
        CHECK(!history::same_path_ignoring_case("\xC3\x97", "\xC3\xB7")); // × is not ÷

        // take_snapshot checks every path, and refuses case twins.
        tests::scratch_directory scratch("volt_history_repository_paths");
        history::repository repo = make_repository(scratch / ".volthistory");
        CHECK(!repo.take_snapshot(std::vector<file_content>{{"../escape", "x"}}));
        CHECK(!repo.take_snapshot(std::vector<file_content>{{"a\\b", "x"}}));
        auto twins = repo.take_snapshot(std::vector<file_content>{{"Board.voltpcb", "x"}, {"board.voltpcb", "y"}});
        CHECK(!twins);
        if (!twins)
            CHECK(contains(twins.error(), "differ only in case"));
        CHECK(!repo.take_snapshot(std::vector<file_content>{{"\xC3\x9C" "ber", "x"}, {"\xC3\xBC" "ber", "y"}}));
        auto duplicate = repo.take_snapshot(std::vector<file_content>{{"a", "x"}, {"a", "x"}});
        CHECK(!duplicate);
        if (!duplicate)
            CHECK(contains(duplicate.error(), "twice"));
        // Nothing was stored by the refusals.
        CHECK(tests::count_files(repo.directory() / "blobs") == 0);
        CHECK(repo.head().snapshot.empty());

        CHECK(history::is_object_id(std::string(64, 'a')));
        CHECK(!history::is_object_id(std::string(64, 'A')));
        CHECK(!history::is_object_id(std::string(63, 'a')));
        CHECK(!history::is_object_id(std::string(64, 'g')));
    }

    void resolving_prefixes()
    {
        tests::scratch_directory scratch("volt_history_repository_resolve");
        history::repository repo = make_repository(scratch / ".volthistory");
        auto taken = repo.take_snapshot(project_files());
        CHECK(ok(taken));
        if (!taken)
            return;
        const std::string &id = taken->id;
        CHECK(repo.resolve(id.substr(0, 4)) == id);
        CHECK(repo.resolve(id.substr(0, 12)) == id);
        CHECK(repo.resolve(id) == id);
        std::string upper = id.substr(0, 10);
        for (char &c : upper)
            if (c >= 'a' && c <= 'f')
                c = static_cast<char>(c - 32);
        CHECK(repo.resolve(upper) == id);
        CHECK(!repo.resolve(id.substr(0, 3)));  // too short
        CHECK(!repo.resolve("zzzz"));            // not hex
        CHECK(!repo.resolve(id.substr(0, 4) == "0000" ? "1111" : "0000"));

        // Two snapshots sharing a prefix: a hand-made pair of files in one
        // directory.
        const fs::path dir = repo.directory() / "snapshots" / tests::utf8_path(id.substr(0, 2));
        CHECK(tests::write_text(dir / (id.substr(2, 4) + std::string(58, '0')), "x"));
        auto ambiguous = repo.resolve(id.substr(0, 6));
        CHECK(!ambiguous);
        if (!ambiguous)
            CHECK(contains(ambiguous.error(), "more digits"));
        CHECK(repo.resolve(id.substr(0, 20)) == id);
    }

    void times()
    {
        CHECK(history::format_time(tests::fixed_time()) == "2026-11-22T14:03:07Z");
        CHECK(history::parse_time("2026-11-22T14:03:07Z") == tests::fixed_time());
        using namespace std::chrono;
        CHECK(history::format_time(sys_seconds{}) == "1970-01-01T00:00:00Z");
        CHECK(history::format_time(sys_days{year{2000} / February / 29} + hours{23} + minutes{59} + seconds{59}) ==
              "2000-02-29T23:59:59Z");
        CHECK(history::parse_time("2000-02-29T23:59:59Z").has_value());
        CHECK(!history::parse_time("2001-02-29T00:00:00Z"));
        CHECK(!history::parse_time("2026-11-22T24:00:00Z"));
        CHECK(!history::parse_time("2026-11-22 14:03:07Z"));
        CHECK(!history::parse_time("2026-11-22T14:03:07"));
        CHECK(!history::parse_time("2026-1x-22T14:03:07Z"));
        CHECK(!history::parse_time(""));
    }
} // namespace

int main()
{
    creating_and_opening();
    rejecting_what_is_not_a_history();
    snapshots_round_trip();
    blobs_are_stored_once();
    ids_depend_on_content_alone();
    nothing_changed();
    the_log();
    branches_and_the_head();
    comparing();
    verification();
    reopening_keeps_unknown_fields();
    leftovers_of_a_crash_do_no_harm();
    paths();
    resolving_prefixes();
    times();
    return volt::tests::result("history repository");
}
