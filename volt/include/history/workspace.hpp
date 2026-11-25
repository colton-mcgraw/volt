#pragma once

#include <history/repository.hpp>

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// workspace
// -------------------------------------------------------------------------
//
// A project's files on disk, in and out of its repository: a snapshot of
// the files below a directory (the project's), what has changed there
// since a snapshot, and a snapshot put back.
//
//   auto repo = history::repository::open_or_create(root / history::history_directory_name);
//   const std::vector<std::string> files = {"Blinky.voltproj", "Blinky.voltsch",
//                                           "Blinky.voltpcb", "Blinky.voltlib"};
//   history::snapshot_options options;
//   options.message = "Placed the LEDs";
//   auto taken = history::snapshot_files(*repo, root, files, options);
//   ...
//   auto status = history::compare_files(*repo, root, files, repo->head().snapshot);
//   ...
//   // Back to an old snapshot: the files first, then the head.
//   auto previous = repo->read_snapshot(repo->head().snapshot);
//   auto restored = history::restore_files(*repo, root, old_id, paths_of(*previous));
//   repo->detach(old_id);
//
// The files are named by the caller (the project knows its files), by
// paths relative to the root, in UTF-8. Nothing else in the directory is
// read, and the repository's own directory never is.
//
// Restoring writes only below the root, whatever the snapshot says: a
// repository can arrive with a project from someone else, so every path
// is checked again (check_path) and the file it names, links followed,
// must be below the root and not in the repository. Each file is written
// through a temporary file and a rename. Files are written one by one, so
// a failure part way (a full disk) can leave some files restored and
// others not; run it again. Restoring replaces what is on disk: check
// compare_files first to warn about changes that are not in a snapshot.

namespace volt::history
{

    // Why a file the caller named is not in a snapshot.
    enum class skip_reason
    {
        missing,        // there is no such file
        not_a_file,     // a directory, or something else that is not a file
        outside_root,   // "../shared/parts.voltlib", or an absolute path elsewhere
        in_repository,  // inside the repository's own directory
        invalid_path,   // not a path a snapshot can hold (check_path)
    };

    // "missing", "not a file", ...
    std::string_view describe(skip_reason reason) noexcept;

    struct skipped_file
    {
        std::string path; // as the caller gave it
        skip_reason reason = skip_reason::missing;
        std::string message;
    };

    // The files named, read from disk: what a snapshot of them would hold.
    struct captured_files
    {
        std::vector<file_content> files; // with paths as a snapshot stores them ("./a" is "a")
        std::vector<skipped_file> skipped;
    };

    struct files_snapshot
    {
        taken_snapshot taken;
        std::vector<skipped_file> skipped;
    };

    struct files_status
    {
        std::vector<file_change> changes; // sorted by path
        std::vector<skipped_file> skipped;

        // Whether the files are as in the snapshot.
        bool clean() const noexcept { return changes.empty(); }
    };

    struct restored_files
    {
        std::vector<std::string> written;   // the files written
        std::vector<std::string> removed;   // tracked before, not in the snapshot, removed
        std::vector<std::string> unchanged; // already as in the snapshot, left alone
    };

    // The paths of a snapshot's files, as restore_files takes them.
    std::vector<std::string> paths_of(const snapshot &s);

    // Reads the named files below the root. Paths are made normal ("./a",
    // "sub/../a" and, on Windows, "sub\a" are fine). A file that is not
    // there, lies outside the root or in the repository, or has a path a
    // snapshot cannot hold, is skipped and reported, not an error. A file
    // larger than max_file_size, or one that cannot be read, is an error.
    std::expected<captured_files, std::string> capture_files(const repository &repo, const std::filesystem::path &root,
                                                             std::span<const std::string> paths);

    // capture_files, then repository::take_snapshot.
    std::expected<files_snapshot, std::string> snapshot_files(repository &repo, const std::filesystem::path &root,
                                                              std::span<const std::string> paths,
                                                              const snapshot_options &options = {});

    // capture_files, then repository::changes_since: what changed on disk
    // since the snapshot (an empty id: since nothing). A file named that is
    // not on disk but in the snapshot is removed.
    std::expected<files_status, std::string> compare_files(const repository &repo, const std::filesystem::path &root,
                                                           std::span<const std::string> paths,
                                                           std::string_view snapshot_id);

    // Puts the snapshot's files below the root, which must exist. First
    // every blob is read and checked and every path checked: if any is
    // missing, damaged or unsafe, nothing is written. Then each file whose
    // bytes differ is written, its directories made; then each file in
    // tracked_before (paths relative to the root, such as the files of
    // the snapshot the head was at) that the snapshot does not have is
    // removed. Nothing else is deleted, directories included. The head
    // does not move: switch_branch or detach after.
    std::expected<restored_files, std::string> restore_files(const repository &repo, const std::filesystem::path &root,
                                                             std::string_view snapshot_id,
                                                             std::span<const std::string> tracked_before = {});

} // namespace volt::history
