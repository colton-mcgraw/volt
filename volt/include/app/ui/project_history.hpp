#pragma once

#include <history/repository.hpp>
#include <history/workspace.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace volt::library
{
    struct project;
} // namespace volt::library

// -------------------------------------------------------------------------
// project history
// -------------------------------------------------------------------------
//
// The open project's snapshots and branches (<history/repository.hpp>,
// <history/workspace.hpp>) as the Project Manager's HISTORY area shows and
// changes them, worked out apart from any widget so that it can be tested
// without a window:
//
//   the files     what a snapshot of the project holds: its .voltproj, its
//                 sheets, its board and its project library, those of them
//                 that are on disk below the project's folder
//                 (tracked_files()). A sheet or board not written yet is
//                 skipped, which is normal, not a problem.
//   the view      the current branch's snapshots, newest first, and which
//                 one the head is at; the branches; which files differ on
//                 disk from the head's snapshot (view(), refresh())
//   the changes   a snapshot taken; a branch made (at the head, or at an
//                 older snapshot, whose files are put back), switched to,
//                 renamed or deleted; an older snapshot's files put back
//                 while staying on the branch (restore())
//
// The repository is <project folder>/.volthistory, made by the first
// snapshot: opening a project, or looking at its history, makes nothing.
//
// Nothing is ever lost and nothing is ever detached. Going back to an old
// snapshot without a new branch puts its files back and leaves the head
// where it is: the old state shows as changes, and the next snapshot
// records it on top. Whatever writes files does so before it moves the
// head, so a failure part way leaves the head where it was and the files as
// changes against it, which the view then shows.
//
// What this leaves to the application: saving the project before a
// snapshot (a snapshot holds what is on disk), asking before files are
// overwritten while some are not in a snapshot, and loading the project
// again after they were (<app/ui/windows/project_manager.hpp> does all
// three). For that it says whether anything is not captured (status(),
// fresh), whether files were written (checkout::files_touched), and which
// project file to load after (checkout::project_file).
//
// Interface thread; synchronous. Taking a snapshot hashes and copies the
// project's files, and the status hashes them: a few milliseconds for a
// project library of several megabytes, so the window does it in place.

namespace volt::ui
{

    // A snapshot as the list shows it.
    struct history_entry
    {
        std::string id;
        std::string short_id; // its first digits, as the list and the messages give it
        std::string message;  // its first line
        std::string when;     // local time, "2026-11-22 15:03"; empty if the snapshot's time is unreadable
        bool head = false;    // the snapshot the head is at
    };

    struct history_branch
    {
        std::string name;
        std::string snapshot; // empty while it has none
        bool current = false;
    };

    enum class history_state
    {
        no_project,  // none is open
        not_on_disk, // the project file is not on disk
        no_history,  // no repository yet: the first snapshot makes it
        ready,
        broken,      // the repository cannot be read: problem says why
    };

    struct history_view
    {
        history_state state = history_state::no_project;

        // Why the repository cannot be read (broken), or why the status or
        // the list could not be worked out (ready).
        std::string problem;

        std::string branch; // the current branch; empty when detached (by another tool)
        std::string head;   // the head's snapshot; empty before the first
        std::vector<history_branch> branches;

        // The current branch's snapshots, newest first, at most
        // project_history::list_limit; `more` when there are older ones.
        std::vector<history_entry> snapshots;
        bool more = false;

        // What differs on disk from the head's snapshot, as of the last
        // refresh. Valid when status_known.
        bool status_known = false;
        std::vector<history::file_change> changes;
        // The project's files a snapshot would leave out: not written yet,
        // outside the project's folder.
        std::vector<history::skipped_file> skipped;

        // Whether files differ from the head's snapshot (known, and some do).
        bool changed() const noexcept { return status_known && !changes.empty(); }
    };

    // The first eight digits of a snapshot id.
    std::string short_id(std::string_view id);

    // "2026-11-22 15:03", in the local time zone.
    std::string local_time_text(std::chrono::sys_seconds time);

    // The changes for a sentence: "Blinky.voltlib (modified)", "a.voltsch
    // (added) and b.voltsch (removed)", "a, b, c and 2 more files": at most
    // `listed` named.
    std::string describe_changes(const std::vector<history::file_change> &changes, std::size_t listed = 3);

    // The skipped files worth telling the user about: those that would be
    // in a snapshot but cannot be (outside the project's folder, a path a
    // snapshot cannot hold), not those that are simply not written yet.
    std::vector<history::skipped_file> notable_skips(const std::vector<history::skipped_file> &skipped);

    class project_history
    {
    public:
        // The snapshots listed at most.
        static constexpr std::size_t list_limit = 500;

        // The files a snapshot of the project takes, as the workspace names
        // them (UTF-8, relative to the project's folder, or absolute when
        // the project stores them so): the project file, its library, its
        // sheets, its board; each once.
        static std::vector<std::string> tracked_files(const library::project &p,
                                                      const std::filesystem::path &project_file);

        // Follows the open project: its file (empty for none) and what it
        // names. Opens the repository if there is one (makes none) and
        // reads the view.
        void set_project(const std::filesystem::path &project_file, const library::project *p);

        // Reads the repository, the list and the status again: after files
        // changed on disk, or to see changes made elsewhere.
        void refresh();

        const history_view &view() const noexcept { return m_view; }
        history_state state() const noexcept { return m_view.state; }

        // The project's folder, where the history is; empty with none.
        const std::filesystem::path &root() const noexcept { return m_root; }
        const std::filesystem::path &project_file() const noexcept { return m_project_file; }
        const std::vector<std::string> &tracked() const noexcept { return m_tracked; }

        // A snapshot can be taken: a project on disk whose history is
        // readable or not made yet.
        bool can_snapshot() const noexcept
        {
            return m_view.state == history_state::ready || m_view.state == history_state::no_history;
        }
        // The head is at a snapshot: there is something to branch from.
        bool has_snapshots() const noexcept { return m_view.state == history_state::ready && !m_view.head.empty(); }

        // The listed snapshot of that id, or null.
        const history_entry *find(std::string_view id) const noexcept;

        // What differs on disk from the head's snapshot, worked out now
        // rather than at the last refresh: what to check before files are
        // overwritten. Everything is "added" before the first snapshot.
        std::expected<history::files_status, std::string> status() const;

        // For tests: the clock snapshots take their time from.
        void set_clock(history::clock_function clock);

        // ---- changes -------------------------------------------------------------
        //
        // Each reads the view again after. Errors are sentences for the
        // user, without the action's name ("there is a branch named ...").

        struct taken
        {
            std::string id;
            bool created = false; // false: nothing had changed since the head's snapshot
            std::vector<history::skipped_file> skipped;
        };

        // A snapshot of the project's files as they are on disk, on the
        // current branch; makes the repository the first time.
        std::expected<taken, std::string> take_snapshot(std::string message, std::string author = {});

        // What an operation that puts files back did.
        struct checkout
        {
            std::string error; // empty when it was done
            // Files were written, or may have been (a failure part way):
            // the project is to be loaded again from disk either way.
            bool files_touched = false;
            history::restored_files restored;
            // The project file to load after: the open one, or, when the
            // snapshot put back does not have it, the one it has (empty if
            // it has none).
            std::filesystem::path project_file;

            bool ok() const noexcept { return error.empty(); }
        };

        // Puts the branch's files on disk (removing those the head's
        // snapshot has and the branch's does not), then the head on the
        // branch. A branch without a snapshot is refused.
        checkout switch_branch(std::string_view name);

        // Puts a snapshot's files on disk (removing those the head's
        // snapshot has and it does not) and stays on the branch: the head
        // does not move.
        checkout restore(std::string_view id);

        // A branch at `from` (the head when empty), which becomes the
        // current one. From an older snapshot its files are put back first;
        // if that fails, the branch is not kept and the head does not move.
        checkout create_branch(std::string_view name, std::string_view from = {});

        // Renames a branch; the current one too.
        std::expected<void, std::string> rename_branch(std::string_view name, std::string_view new_name);

        // Deletes a branch, not the current one. Its snapshots stay on disk.
        std::expected<void, std::string> delete_branch(std::string_view name);

        // How many of the branch's snapshots no other branch has: what
        // deleting it would leave unlisted (they stay on disk, but nothing
        // leads back to them), for the question before deleting.
        std::expected<std::size_t, std::string> snapshots_only_on(std::string_view name) const;

        // Why `name` cannot be a new branch's name (invalid, or taken by a
        // branch other than `renaming`), for a dialog to say as it is typed.
        std::expected<void, std::string> check_branch_name(std::string_view name,
                                                           std::string_view renaming = {}) const;

    private:
        // Opens the repository when there is one; true if there is one.
        bool open_repository();
        std::vector<std::string> head_paths() const;
        checkout put_back(std::string_view id);
        std::filesystem::path project_file_in(std::string_view snapshot_id) const;

        std::filesystem::path m_project_file;
        std::filesystem::path m_root;
        std::vector<std::string> m_tracked;
        std::optional<history::repository> m_repository;
        history::clock_function m_clock;
        history_view m_view;
    };

} // namespace volt::ui
