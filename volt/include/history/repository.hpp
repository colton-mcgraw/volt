#pragma once

#include <json/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// repository
// -------------------------------------------------------------------------
//
// The history of a project's files: snapshots of all of them together,
// taken when the user asks, kept in a directory next to the project
// (<project directory>/.volthistory), with branches, so that any snapshot
// can be gone back to, or built on in a new direction while the old one is
// kept. This is storage only: bytes in, bytes out. It never reads what is
// in a file; putting files on disk and reading them is <history/workspace.hpp>.
//
//   .volthistory/
//     history.json          the branches and the head
//     blobs/3f/2a91...      a file's bytes, named by their SHA-256: two hex
//                           digits as a directory, then the other 62
//     snapshots/b7/d104...  a snapshot, named by the SHA-256 of its text
//
// Everything below blobs/ and snapshots/ is named by its content, so it is
// written once and never changed: a file that is the same in a hundred
// snapshots is stored once, and an object that has been damaged no longer
// matches its name, which reading checks. A snapshot lists its files and
// the snapshots it was taken after (its parents: one, or none for the
// first; a list so that merging can be added later):
//
//   {
//     "format": "volt-snapshot",
//     "format_version": 1,
//     "parents": ["9c41...e0"],
//     "time": "2026-11-22T14:03:07Z",
//     "author": "Ada",
//     "message": "Routed the power rails",
//     "files": [
//       {"path": "Blinky.voltpcb", "blob": "3f2a...91", "size": 48113},
//       {"path": "Blinky.voltproj", "blob": "77c0...2d", "size": 412}
//     ]
//   }
//
// The text is written deterministically (keys in this order, files sorted
// by path, bytewise), so the same snapshot always has the same id. Fields
// that are not known are kept when a snapshot is read (snapshot::extra).
//
// history.json is the only file that changes. A branch is a name pointing
// at a snapshot; the head is the branch the user is on, or a snapshot of
// its own ("detached", after going back to an old snapshot without making
// a branch). A new repository has the branch "main", with no snapshot yet,
// and is on it.
//
//   {
//     "format": "volt-history",
//     "format_version": 1,
//     "head": {"branch": "main"},           or {"snapshot": "<id>"}
//     "branches": [
//       {"name": "main", "snapshot": "b7d1...04"},
//       {"name": "two-layer", "snapshot": "e2f8...6a"}
//     ]
//   }
//
// Fields that are not known are kept and written back, there and in
// "head" and each branch.
//
// Every change is ordered so that a crash at any point leaves a valid
// repository: the blobs are written first, then the snapshot that names
// them, then history.json that names the snapshot, each through a
// temporary file and a rename. What a crash can leave behind is objects
// nothing names yet, which do no harm (and are not collected).
//
// One process uses a repository at a time; nothing here locks it. Within
// that process the const functions only read files and the small state
// held here, so they may run on several threads at once (a worker thread
// computing a status or a log), but not while a non-const function runs.
// A repository is cheap to open and to copy: it holds the directory, the
// head and the branches, nothing else.

namespace volt::history
{

    inline constexpr std::string_view history_format = "volt-history";
    inline constexpr int history_format_version = 1;
    inline constexpr std::string_view snapshot_format = "volt-snapshot";
    inline constexpr int snapshot_format_version = 1;

    // The repository's directory in a project's directory.
    inline constexpr std::string_view history_directory_name = ".volthistory";

    // The branch a new repository has.
    inline constexpr std::string_view default_branch = "main";

    // The largest file kept (as json::read_text_file's default limit). A
    // project file larger than this is a mistake, not a design.
    inline constexpr std::uintmax_t max_file_size = std::uintmax_t{256} << 20;

    // Branch names are at most this many bytes.
    inline constexpr std::size_t max_branch_name_size = 100;

    // A file to snapshot: its path in the project, and its bytes.
    struct file_content
    {
        std::string path;
        std::string bytes;
    };

    // A file in a snapshot.
    struct snapshot_file
    {
        std::string path;   // relative, UTF-8, forward slashes (check_path)
        std::string blob;   // the SHA-256 of its bytes: where they are stored
        std::uint64_t size = 0;
        json::object extra; // fields that are not known

        bool operator==(const snapshot_file &) const = default;
    };

    struct snapshot
    {
        std::string id;                   // the SHA-256 of its text
        std::vector<std::string> parents; // the first is the one it was taken after
        std::vector<snapshot_file> files; // sorted by path
        std::string message;
        std::string author;               // may be empty
        std::string time;                 // UTC, "2026-11-22T14:03:07Z" (parse_time)
        json::object extra;               // fields that are not known
        int format_version = snapshot_format_version;

        // The file at that path, or nullptr.
        const snapshot_file *find(std::string_view path) const noexcept;

        bool operator==(const snapshot &) const = default;
    };

    struct branch
    {
        std::string name;
        std::string snapshot; // empty until the first snapshot on it
        json::object extra;

        bool operator==(const branch &) const = default;
    };

    // Where the repository is.
    struct head_state
    {
        std::string branch;   // the current branch; empty when detached
        std::string snapshot; // the snapshot the head is at; empty when the branch has none yet

        bool detached() const noexcept { return branch.empty(); }

        bool operator==(const head_state &) const = default;
    };

    struct snapshot_options
    {
        std::string message;
        std::string author;
        // Take a snapshot even when the files are the same as the head's.
        bool allow_unchanged = false;
    };

    // What taking a snapshot did. Nothing having changed is not an error:
    // created is false and id is the head's snapshot (empty when there is
    // none yet).
    struct taken_snapshot
    {
        std::string id;
        bool created = false;
    };

    enum class file_status
    {
        added,
        removed,
        modified,
    };

    // "added", "removed", "modified".
    std::string_view describe(file_status status) noexcept;

    // A file that differs between two sets of files.
    struct file_change
    {
        std::string path;
        file_status status = file_status::modified;
        std::string old_blob; // empty when added
        std::string new_blob; // empty when removed

        bool operator==(const file_change &) const = default;
    };

    // Something wrong found by repository::verify.
    struct verify_problem
    {
        std::string id;       // the snapshot or blob
        bool missing = false; // otherwise there, but damaged or unreadable
        std::string message;
    };

    // The time a snapshot is taken at. The system clock by default;
    // repository::set_clock replaces it (tests use a fixed one).
    using clock_function = std::function<std::chrono::sys_seconds()>;

    // ---- names -------------------------------------------------------------------

    // Whether the text is an object id: 64 lowercase hexadecimal digits.
    bool is_object_id(std::string_view text) noexcept;

    // Checks a path as a snapshot stores it: relative, UTF-8, with forward
    // slashes; no empty, "." or ".." segment; no control character and none
    // of \ : < > " | ? * (which Windows does not allow in a name); no
    // segment ending in a dot or a space (Windows drops those, so "a." is
    // "a"); no device name (CON, NUL, COM1, ...); at most 255 bytes a
    // segment and 4096 in all. The error says what is wrong.
    std::expected<void, std::string> check_path(std::string_view path);

    // Checks a branch name: not empty, at most max_branch_name_size bytes,
    // UTF-8, no control character, no / \ or :, no space at either end.
    std::expected<void, std::string> check_branch_name(std::string_view name);

    // Whether two paths are one file on a case-insensitive file system
    // (Windows, macOS): equal once their letters are in lower case (ASCII,
    // Latin-1, Latin Extended-A, Greek and Cyrillic; other scripts are
    // compared exactly).
    bool same_path_ignoring_case(std::string_view a, std::string_view b);

    // A time as a snapshot stores it, "2026-11-22T14:03:07Z", and back;
    // nullopt for text that is not one.
    std::string format_time(std::chrono::sys_seconds time);
    std::optional<std::chrono::sys_seconds> parse_time(std::string_view text);

    // ---- the repository --------------------------------------------------------

    class repository
    {
    public:
        // Whether the directory holds a repository (a history.json).
        static bool exists(const std::filesystem::path &directory);

        // Opens a repository. Fails for a directory without one, a
        // history.json that is not a Volt history or is damaged, and one
        // written by a newer Volt (a newer format_version), which this one
        // could not be trusted to change.
        static std::expected<repository, std::string> open(const std::filesystem::path &directory);

        // Makes a new repository, creating the directory: the branch "main"
        // with no snapshot, and the head on it. Fails if there is one there
        // already.
        static std::expected<repository, std::string> create(const std::filesystem::path &directory);

        // open when there is one, create when there is not.
        static std::expected<repository, std::string> open_or_create(const std::filesystem::path &directory);

        const std::filesystem::path &directory() const noexcept { return m_directory; }

        // Replaces the clock snapshots take their time from.
        void set_clock(clock_function clock);

        // ---- branches and the head ---------------------------------------

        // The current branch (or none) and the snapshot the head is at.
        head_state head() const;

        // The branches, in the order they were made.
        const std::vector<branch> &branches() const noexcept { return m_branches; }

        // The branch of that name (compared exactly), or nullptr.
        const branch *find_branch(std::string_view name) const noexcept;

        // Makes a branch at the snapshot (a full id), or at the head's
        // snapshot when that is empty. The head does not move: switch_branch
        // to go on it. Fails for an invalid name, a name in use, or a
        // snapshot the repository does not have.
        std::expected<void, std::string> create_branch(std::string_view name, std::string_view snapshot_id = {});

        // Puts the head on the branch. Only the head moves; the files on
        // disk are the workspace's (restore_files).
        std::expected<void, std::string> switch_branch(std::string_view name);

        // Puts the head on the snapshot itself, on no branch. Snapshots
        // taken there are on no branch either until create_branch is called
        // at the head.
        std::expected<void, std::string> detach(std::string_view snapshot_id);

        // Renames a branch; the head follows it if it is on it.
        std::expected<void, std::string> rename_branch(std::string_view name, std::string_view new_name);

        // Deletes a branch; not the current one. Its snapshots stay on disk
        // (a later version may collect what nothing reaches).
        std::expected<void, std::string> delete_branch(std::string_view name);

        // ---- snapshots ---------------------------------------------------

        // Stores the files as a snapshot after the head's snapshot, and
        // moves the current branch (or the detached head) to it. Paths must
        // pass check_path, be unique ignoring case, and their files be at
        // most max_file_size. When the files are the same as the head's
        // snapshot's (same paths, same bytes), nothing is written and
        // created is false, unless options.allow_unchanged.
        std::expected<taken_snapshot, std::string> take_snapshot(std::span<const file_content> files,
                                                                 const snapshot_options &options = {});

        // Whether the repository has that snapshot (a full id); does not
        // read it.
        bool has_snapshot(std::string_view id) const;

        // A snapshot, read and checked against its id.
        std::expected<snapshot, std::string> read_snapshot(std::string_view id) const;

        // The bytes of a blob, checked against its id.
        std::expected<std::string, std::string> read_blob(std::string_view id) const;

        // The bytes of one file of a snapshot.
        std::expected<std::string, std::string> read_file(std::string_view snapshot_id, std::string_view path) const;

        // The snapshot and those before it, newest first, following first
        // parents, at most limit of them. From the head's snapshot when
        // `from` is empty (an empty list when the head has none yet).
        std::expected<std::vector<snapshot>, std::string> log(
            std::string_view from = {}, std::size_t limit = std::numeric_limits<std::size_t>::max()) const;

        // The full id of the one snapshot whose id starts with the text (at
        // least 4 hexadecimal digits, either case), for ids typed or pasted
        // by hand. Fails when none or several do.
        std::expected<std::string, std::string> resolve(std::string_view prefix) const;

        // ---- comparing ---------------------------------------------------
        //
        // Changes sorted by path. An empty id stands for no snapshot at all
        // (no files): what a branch without a snapshot has.

        // What changed from one snapshot to another.
        std::expected<std::vector<file_change>, std::string> changes_between(std::string_view from_id,
                                                                             std::string_view to_id) const;

        // What changed from a snapshot to the given files (what a snapshot
        // of them would record): "changes since the last snapshot". The
        // files are checked as take_snapshot checks them.
        std::expected<std::vector<file_change>, std::string> changes_since(std::string_view from_id,
                                                                           std::span<const file_content> files) const;

        // ---- verification ------------------------------------------------

        // Reads every snapshot reachable from the branches and the head (all
        // parents, not only first ones) and every blob they name, and
        // reports each one that is missing or damaged. Empty when all is
        // well.
        std::vector<verify_problem> verify() const;

    private:
        repository() = default;

        std::expected<void, std::string> load();
        // Writes history.json with this head and these branches, and takes
        // them only once it is written.
        std::expected<void, std::string> commit(std::string head_branch, std::string head_snapshot,
                                                std::vector<branch> branches);

        std::filesystem::path m_directory;
        std::string m_head_branch;   // empty when detached
        std::string m_head_snapshot; // when detached
        json::object m_head_extra;
        std::vector<branch> m_branches;
        json::object m_extra;
        clock_function m_clock;
    };

} // namespace volt::history
