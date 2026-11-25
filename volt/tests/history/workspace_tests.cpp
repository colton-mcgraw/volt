// A project's files on disk in and out of its repository: a snapshot of the
// files named, with those that cannot be taken reported; what changed on
// disk; a snapshot restored, previously tracked files removed and nothing
// else; nothing written when a blob is missing or damaged; and snapshots
// made by hand to write outside the project, which must fail.

#include "history_test_support.hpp"

#include <history/sha256.hpp>
#include <history/workspace.hpp>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <vector>

using namespace volt;
using tests::contains;
using tests::ok;

namespace
{
    namespace fs = std::filesystem;

    std::string to_utf8(const fs::path &path)
    {
        const std::u8string text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }

    history::repository open_repository(const fs::path &root)
    {
        auto repo = history::repository::open_or_create(root / ".volthistory");
        if (!repo)
        {
            std::fprintf(stderr, "  cannot create the repository: %s\n", repo.error().c_str());
            std::exit(1);
        }
        repo->set_clock(tests::fixed_clock());
        return std::move(*repo);
    }

    history::snapshot_options message(std::string text)
    {
        history::snapshot_options options;
        options.message = std::move(text);
        return options;
    }

    const history::skipped_file *skipped(const std::vector<history::skipped_file> &list, std::string_view path)
    {
        for (const history::skipped_file &s : list)
            if (s.path == path)
                return &s;
        return nullptr;
    }

    const history::file_change *change_of(const std::vector<history::file_change> &changes, std::string_view path)
    {
        for (const history::file_change &c : changes)
            if (c.path == path)
                return &c;
        return nullptr;
    }

    // ---- snapshots and status ------------------------------------------------------

    void snapshot_and_status()
    {
        tests::scratch_directory scratch("volt_history_workspace_status");
        const fs::path root = scratch / "Blinky";
        CHECK(tests::write_text(root / "Blinky.voltproj", "{\"name\": \"Blinky\"}\n"));
        CHECK(tests::write_text(root / "sheets/Main.voltsch", "sheet\n"));
        CHECK(tests::write_text(root / tests::utf8_path("\xC3\x9C" "ber.voltsch"), "\xC3\xBC" "ber\n"));
        CHECK(tests::write_text(root / "notes.txt", "not part of the project\n"));
        CHECK(tests::write_text(scratch / "shared/parts.voltlib", "shared\n"));
        history::repository repo = open_repository(root);

        std::vector<std::string> paths = {
            "Blinky.voltproj",
            "sheets/Main.voltsch",
            "\xC3\x9C" "ber.voltsch",
            "./Blinky.voltproj",          // the same file again
            "../shared/parts.voltlib",    // outside the project
            "missing.voltsch",
            ".volthistory/history.json",  // the history itself
            "sheets",                     // a directory
            "a?b.voltsch",                // not a portable name
            to_utf8(root / "sheets/Main.voltsch"), // absolute, but inside
        };
        auto taken = history::snapshot_files(repo, root, paths, message("first"));
        CHECK(ok(taken));
        if (!taken)
            return;
        CHECK(taken->taken.created);
        CHECK(taken->skipped.size() == 5);
        const auto reason_of = [&](std::string_view path) {
            const history::skipped_file *s = skipped(taken->skipped, path);
            return s ? std::optional<history::skip_reason>(s->reason) : std::nullopt;
        };
        CHECK(reason_of("../shared/parts.voltlib") == history::skip_reason::outside_root);
        CHECK(reason_of("missing.voltsch") == history::skip_reason::missing);
        CHECK(reason_of(".volthistory/history.json") == history::skip_reason::in_repository);
        CHECK(reason_of("sheets") == history::skip_reason::not_a_file);
        CHECK(reason_of("a?b.voltsch") == history::skip_reason::invalid_path);
        for (const history::skipped_file &s : taken->skipped)
            CHECK(!s.message.empty());

        auto s = repo.read_snapshot(taken->taken.id);
        CHECK(ok(s));
        if (!s)
            return;
        CHECK(s->files.size() == 3);
        CHECK(history::paths_of(*s) ==
              std::vector<std::string>{"Blinky.voltproj", "sheets/Main.voltsch", "\xC3\x9C" "ber.voltsch"});
        CHECK(repo.read_file(s->id, "\xC3\x9C" "ber.voltsch") == std::string("\xC3\xBC" "ber\n"));
        // The repository's own files are never in a snapshot.
        for (const history::snapshot_file &f : s->files)
            CHECK(!f.path.starts_with(".volthistory"));

        // As on disk: clean.
        paths = {"Blinky.voltproj", "sheets/Main.voltsch", "\xC3\x9C" "ber.voltsch", "new.voltsch"};
        auto status = history::compare_files(repo, root, paths, repo.head().snapshot);
        CHECK(ok(status));
        CHECK(status && status->clean());
        CHECK(status && status->skipped.size() == 1); // new.voltsch is not there yet

        // Again: nothing changed.
        auto again = history::snapshot_files(repo, root, paths, message("again"));
        CHECK(again && !again->taken.created && again->taken.id == s->id);

        // A change, a file gone, a file new.
        CHECK(tests::write_text(root / "sheets/Main.voltsch", "sheet, changed\n"));
        std::error_code error;
        fs::remove(root / tests::utf8_path("\xC3\x9C" "ber.voltsch"), error);
        CHECK(tests::write_text(root / "new.voltsch", "new\n"));
        status = history::compare_files(repo, root, paths, repo.head().snapshot);
        CHECK(ok(status));
        if (status)
        {
            CHECK(!status->clean());
            CHECK(status->changes.size() == 3);
            const history::file_change *modified = change_of(status->changes, "sheets/Main.voltsch");
            const history::file_change *removed = change_of(status->changes, "\xC3\x9C" "ber.voltsch");
            const history::file_change *added = change_of(status->changes, "new.voltsch");
            CHECK(modified && modified->status == history::file_status::modified);
            CHECK(removed && removed->status == history::file_status::removed);
            CHECK(added && added->status == history::file_status::added);
            CHECK(skipped(status->skipped, "\xC3\x9C" "ber.voltsch") != nullptr);
        }

        // Against no snapshot: everything is new.
        auto from_nothing = history::compare_files(repo, root, paths, {});
        CHECK(from_nothing && from_nothing->changes.size() == 3 &&
              std::ranges::all_of(from_nothing->changes, [](const history::file_change &c) {
                  return c.status == history::file_status::added;
              }));

        auto second = history::snapshot_files(repo, root, paths, message("second"));
        CHECK(second && second->taken.created);
        auto log = repo.log();
        CHECK(log && log->size() == 2 && (*log)[0].message == "second");

#ifdef _WIN32
        // Backslashes are separators on Windows.
        const std::vector<std::string> windows_paths = {"sheets\\Main.voltsch"};
        auto captured = history::capture_files(repo, root, windows_paths);
        CHECK(captured && captured->files.size() == 1 && captured->files[0].path == "sheets/Main.voltsch");
#endif

        CHECK(history::describe(history::skip_reason::missing) == "missing");
        CHECK(history::describe(history::skip_reason::outside_root) == "outside the project directory");
    }

    void too_large()
    {
        tests::scratch_directory scratch("volt_history_workspace_large");
        const fs::path root = scratch / "p";
        CHECK(tests::write_text(root / "huge.bin", "x"));
        std::error_code error;
        fs::resize_file(root / "huge.bin", history::max_file_size + 1, error);
        CHECK(!error);
        history::repository repo = open_repository(root);
        const std::vector<std::string> paths = {"huge.bin"};
        auto taken = history::snapshot_files(repo, root, paths);
        CHECK(!taken);
        if (!taken)
            CHECK(contains(taken.error(), "huge.bin") && contains(taken.error(), "bytes"));
        CHECK(repo.head().snapshot.empty());
        fs::remove(root / "huge.bin", error);
    }

    // ---- restoring -----------------------------------------------------------------

    void restoring()
    {
        tests::scratch_directory scratch("volt_history_workspace_restore");
        const fs::path root = scratch / "p";
        CHECK(tests::write_text(root / "a.txt", "a1\n"));
        CHECK(tests::write_text(root / "sub/b.txt", "b1\n"));
        CHECK(tests::write_text(root / "notes.txt", "mine\n"));
        CHECK(tests::write_text(scratch / "outside.txt", "outside\n"));
        history::repository repo = open_repository(root);

        auto first = history::snapshot_files(repo, root, std::vector<std::string>{"a.txt", "sub/b.txt"});
        CHECK(tests::write_text(root / "sub/b.txt", "b2\n"));
        CHECK(tests::write_text(root / "c/d/c.txt", "c\n"));
        auto second =
            history::snapshot_files(repo, root, std::vector<std::string>{"a.txt", "sub/b.txt", "c/d/c.txt"});
        CHECK(ok(first) && ok(second));
        if (!first || !second)
            return;
        const std::string one = first->taken.id;
        const std::string two = second->taken.id;
        auto s1 = repo.read_snapshot(one);
        auto s2 = repo.read_snapshot(two);
        CHECK(ok(s1) && ok(s2));
        if (!s1 || !s2)
            return;

        // Back to the first: b rewritten, c removed (it was tracked), a and
        // the untracked notes left alone.
        auto back = history::restore_files(repo, root, one, history::paths_of(*s2));
        CHECK(ok(back));
        if (back)
        {
            CHECK(back->written == std::vector<std::string>{"sub/b.txt"});
            CHECK(back->removed == std::vector<std::string>{"c/d/c.txt"});
            CHECK(back->unchanged == std::vector<std::string>{"a.txt"});
        }
        CHECK(tests::read_text(root / "sub/b.txt") == "b1\n");
        CHECK(!tests::file_exists(root / "c/d/c.txt"));
        CHECK(fs::is_directory(root / "c/d")); // directories are never removed
        CHECK(tests::read_text(root / "notes.txt") == "mine\n");
        CHECK(ok(repo.detach(one)));
        auto status = history::compare_files(repo, root, history::paths_of(*s1), repo.head().snapshot);
        CHECK(status && status->clean());

        // And forward again.
        auto forward = history::restore_files(repo, root, two, history::paths_of(*s1));
        CHECK(ok(forward));
        if (forward)
        {
            CHECK(forward->written == (std::vector<std::string>{"c/d/c.txt", "sub/b.txt"}));
            CHECK(forward->removed.empty());
            CHECK(forward->unchanged == std::vector<std::string>{"a.txt"});
        }
        CHECK(tests::read_text(root / "c/d/c.txt") == "c\n");
        CHECK(tests::read_text(root / "sub/b.txt") == "b2\n");

        // Without a list of tracked files nothing is removed.
        auto keep = history::restore_files(repo, root, one);
        CHECK(keep && keep->removed.empty());
        CHECK(tests::file_exists(root / "c/d/c.txt"));

        // Tracked paths outside the project are never removed; ones that
        // are not there are not reported.
        const std::vector<std::string> tracked = {"c/d/c.txt", "../outside.txt", to_utf8(scratch / "outside.txt"),
                                                  "gone.txt", ".volthistory/history.json"};
        auto careful = history::restore_files(repo, root, one, tracked);
        CHECK(ok(careful));
        CHECK(careful && careful->removed == std::vector<std::string>{"c/d/c.txt"});
        CHECK(tests::read_text(scratch / "outside.txt") == "outside\n");
        CHECK(history::repository::exists(repo.directory()));

        // Into an empty directory: everything written, directories made.
        const fs::path elsewhere = scratch / "copy";
        fs::create_directories(elsewhere);
        auto copy = history::restore_files(repo, elsewhere, two);
        CHECK(ok(copy));
        CHECK(copy && copy->written.size() == 3 && copy->unchanged.empty());
        CHECK(tests::read_text(elsewhere / "c/d/c.txt") == "c\n");
        CHECK(tests::count_files(elsewhere) == 3);

        // A file the snapshot has where a directory is: refused up front.
        const fs::path clash = scratch / "clash";
        fs::create_directories(clash / "a.txt");
        CHECK(tests::write_text(clash / "sub/b.txt", "untouched\n"));
        auto refused = history::restore_files(repo, clash, one);
        CHECK(!refused);
        CHECK(tests::read_text(clash / "sub/b.txt") == "untouched\n");

        // A directory that is not there, a snapshot that is not there.
        CHECK(!history::restore_files(repo, scratch / "nowhere", one));
        CHECK(!history::restore_files(repo, root, std::string(64, '0')));
        CHECK(!history::restore_files(repo, root, {}));
    }

    void nothing_is_written_when_a_blob_is_bad()
    {
        tests::scratch_directory scratch("volt_history_workspace_bad_blob");
        const fs::path root = scratch / "p";
        CHECK(tests::write_text(root / "a.txt", "a1\n"));
        CHECK(tests::write_text(root / "b.txt", "b1\n"));
        history::repository repo = open_repository(root);
        const std::vector<std::string> paths = {"a.txt", "b.txt"};
        auto first = history::snapshot_files(repo, root, paths);
        CHECK(tests::write_text(root / "a.txt", "a2\n"));
        CHECK(tests::write_text(root / "b.txt", "b2\n"));
        auto second = history::snapshot_files(repo, root, paths);
        CHECK(ok(first) && ok(second));
        if (!first || !second)
            return;

        // b's first bytes missing: a, which could be written, is not.
        const fs::path b1 = tests::object_path(repo, "blobs", history::sha256_hex("b1\n"));
        std::error_code error;
        fs::remove(b1, error);
        auto missing = history::restore_files(repo, root, first->taken.id, paths);
        CHECK(!missing);
        if (!missing)
            CHECK(contains(missing.error(), "nothing was written") && contains(missing.error(), "b.txt") &&
                  contains(missing.error(), "missing"));
        CHECK(tests::read_text(root / "a.txt") == "a2\n");
        CHECK(tests::read_text(root / "b.txt") == "b2\n");

        // Damaged rather than missing: the same.
        CHECK(tests::write_text(b1, "b1 but not quite\n"));
        auto damaged = history::restore_files(repo, root, first->taken.id, paths);
        CHECK(!damaged);
        if (!damaged)
            CHECK(contains(damaged.error(), "damaged"));
        CHECK(tests::read_text(root / "a.txt") == "a2\n");

        // Repaired, it goes through.
        CHECK(tests::write_text(b1, "b1\n"));
        auto repaired = history::restore_files(repo, root, first->taken.id, paths);
        CHECK(ok(repaired));
        CHECK(tests::read_text(root / "a.txt") == "a1\n");
        CHECK(tests::read_text(root / "b.txt") == "b1\n");
    }

    // ---- snapshots made to escape ----------------------------------------------------

    // Writes a snapshot by hand, as someone could who hands over a project
    // with its history, and returns its id.
    std::string malicious_snapshot(const history::repository &repo, std::string_view path, std::string_view bytes)
    {
        const std::string blob = history::sha256_hex(bytes);
        tests::write_text(tests::object_path(repo, "blobs", blob), bytes);
        json::object file{{"path", json::value(path)}, {"blob", json::value(blob)}, {"size", json::value(bytes.size())}};
        json::object o{{"format", "volt-snapshot"},
                       {"format_version", 1},
                       {"parents", json::array{}},
                       {"time", "2026-11-22T14:03:07Z"},
                       {"author", "Mallory"},
                       {"message", "nothing to see here"},
                       {"files", json::array{json::value(std::move(file))}}};
        const std::string text = json::write(json::value(std::move(o)));
        const std::string id = history::sha256_hex(text);
        tests::write_text(tests::object_path(repo, "snapshots", id), text);
        return id;
    }

    void paths_cannot_escape_the_root()
    {
        tests::scratch_directory scratch("volt_history_workspace_escape");
        const fs::path root = scratch / "project";
        CHECK(tests::write_text(root / "a.txt", "a\n"));
        history::repository repo = open_repository(root);
        const std::vector<std::string> paths = {"a.txt"};
        CHECK(ok(history::snapshot_files(repo, root, paths)));
        const std::string history_before = tests::read_text(repo.directory() / "history.json");
        const std::size_t files_before = tests::count_files(scratch.path());

        std::vector<std::string> attacks = {
            "../evil.txt",
            "../../evil.txt",
            "a/../../evil.txt",
            "sub/../../evil.txt",
            "a\\..\\..\\evil.txt",
            "..\\evil.txt",
            "/evil.txt",
            "//server/share/evil.txt",
            "C:/evil.txt",
            "C:evil.txt",
            "C:\\evil.txt",
            to_utf8(scratch / "evil.txt"),
            ".volthistory/history.json",
            ".volthistory/blobs/aa/evil",
            "./evil.txt",
            "evil.txt/",
            "evil.txt\n",
        };
#ifdef _WIN32
        attacks.push_back(".VOLTHISTORY/history.json"); // one directory on Windows
        attacks.push_back(".volthistory./history.json");
#endif
        for (const std::string &attack : attacks)
        {
            const std::string id = malicious_snapshot(repo, attack, "pwned\n");
            auto restored = history::restore_files(repo, root, id, paths);
            CHECK(!restored);
            if (restored)
                std::fprintf(stderr, "  restored a snapshot with the path \"%s\"\n", attack.c_str());
            CHECK(!tests::file_exists(scratch / "evil.txt"));
            CHECK(!tests::file_exists(root.parent_path().parent_path() / "evil.txt"));
            CHECK(tests::read_text(repo.directory() / "history.json") == history_before);
            CHECK(tests::read_text(root / "a.txt") == "a\n"); // not removed either
            // Nor with nothing to remove.
            CHECK(!history::restore_files(repo, root, id));
        }

        // A path a snapshot may hold, but that leads into the history: it
        // is reading that accepts it, and restoring that refuses it.
        const std::string into_history = malicious_snapshot(repo, ".volthistory/history.json", "{}");
        CHECK(repo.read_snapshot(into_history).has_value());
        auto overwrite = history::restore_files(repo, root, into_history);
        CHECK(!overwrite);
        if (!overwrite)
            CHECK(contains(overwrite.error(), "history's own directory"));
        CHECK(tests::read_text(repo.directory() / "history.json") == history_before);
        // Nothing was written anywhere but the hand-made objects themselves.
        CHECK(tests::count_files(root) - tests::count_files(repo.directory()) == 1);
        CHECK(tests::count_files(scratch.path()) >= files_before);
        CHECK(!tests::file_exists(repo.directory() / "blobs/aa/evil"));

        // A link inside the project to a directory outside it: a snapshot's
        // file below the link would land outside. Making links needs a
        // privilege on Windows, so this part runs only where it can.
        const fs::path outside = scratch / "outside";
        fs::create_directories(outside);
        std::error_code error;
        fs::create_directory_symlink(outside, root / "link", error);
        if (error)
            std::printf("  (a directory link cannot be made here, so that case is not tested: %s)\n",
                        error.message().c_str());
        else
        {
            const std::string id = malicious_snapshot(repo, "link/evil.txt", "pwned\n");
            auto restored = history::restore_files(repo, root, id);
            CHECK(!restored);
            if (!restored)
                CHECK(contains(restored.error(), "link"));
            CHECK(!tests::file_exists(outside / "evil.txt"));

            // Removing through a link is refused the same way.
            CHECK(tests::write_text(outside / "victim.txt", "keep me\n"));
            const std::vector<std::string> tracked = {"link/victim.txt"};
            auto removing = history::restore_files(repo, root, repo.head().snapshot, tracked);
            CHECK(!removing);
            CHECK(tests::read_text(outside / "victim.txt") == "keep me\n");
            fs::remove(root / "link", error);
        }
    }
} // namespace

int main()
{
    snapshot_and_status();
    too_large();
    restoring();
    nothing_is_written_when_a_blob_is_bad();
    paths_cannot_escape_the_root();
    return volt::tests::result("history workspace");
}
