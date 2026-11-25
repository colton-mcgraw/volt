#include <history/workspace.hpp>

#include "files.hpp"

#include <format>
#include <set>
#include <utility>

namespace volt::history
{

    namespace
    {
        namespace fs = std::filesystem;

        fs::path absolute_normal(const fs::path &path)
        {
            std::error_code error;
            const fs::path absolute = fs::absolute(path, error);
            return (error ? path : absolute).lexically_normal();
        }

        // A path the caller named, made relative to the root and normal,
        // with the file it names; or why it cannot be in a snapshot.
        struct located_file
        {
            std::string path;
            fs::path full;
        };

        std::expected<located_file, skipped_file> locate(const fs::path &root, const fs::path &repository_directory,
                                                         const std::string &given)
        {
            const auto skip = [&](skip_reason reason, std::string message) {
                return std::unexpected(skipped_file{given, reason, std::move(message)});
            };
            if (given.empty() || !detail::valid_utf8(given))
                return skip(skip_reason::invalid_path, "the path is empty or not valid UTF-8");
            const fs::path named = detail::path_from_utf8(given);
            const fs::path full = (named.is_absolute() ? named : root / named).lexically_normal();
            const fs::path relative = full.lexically_relative(root);
            const std::string text = detail::path_to_utf8(relative);
            if (relative.empty() || text == ".." || text.starts_with("../"))
                return skip(skip_reason::outside_root, std::format("\"{}\" is outside the project directory", given));
            if (text == ".")
                return skip(skip_reason::not_a_file, std::format("\"{}\" is the project directory", given));
            const std::string first = text.substr(0, text.find('/'));
            if (detail::is_within(repository_directory, full) ||
                detail::fold_case(first) == detail::fold_case(history_directory_name))
                return skip(skip_reason::in_repository, std::format("\"{}\" is in the history's own directory", given));
            if (auto checked = check_path(text); !checked)
                return skip(skip_reason::invalid_path, checked.error());
            return located_file{text, full};
        }

        // Where a file of a snapshot goes below the root: the path checked
        // again, and the directory it goes in, links followed, below the
        // root and not in the repository. The file itself is not followed:
        // it is replaced or removed, never written through.
        std::expected<fs::path, std::string> safe_target(const fs::path &root, const fs::path &canonical_root,
                                                         const fs::path &canonical_repository, std::string_view path)
        {
            if (auto checked = check_path(path); !checked)
                return std::unexpected(std::format("refused: {}", checked.error()));
            const fs::path target = (root / detail::path_from_utf8(path)).lexically_normal();
            if (!detail::is_within(root, target) || target == root)
                return std::unexpected(std::format("refused: \"{}\" is outside the project directory", path));
            std::error_code error;
            const fs::path parent = fs::weakly_canonical(target.parent_path(), error);
            if (error)
                return std::unexpected(std::format("\"{}\": cannot resolve its directory ({})", path, error.message()));
            const fs::path resolved = parent / target.filename();
            if (!detail::is_within(canonical_root, resolved))
                return std::unexpected(
                    std::format("refused: \"{}\" leads outside the project directory through a link", path));
            if (detail::is_within(canonical_repository, resolved))
                return std::unexpected(std::format("refused: \"{}\" is in the history's own directory", path));
            return target;
        }
    } // namespace

    std::string_view describe(skip_reason reason) noexcept
    {
        switch (reason)
        {
        case skip_reason::missing:
            return "missing";
        case skip_reason::not_a_file:
            return "not a file";
        case skip_reason::outside_root:
            return "outside the project directory";
        case skip_reason::in_repository:
            return "in the history's directory";
        case skip_reason::invalid_path:
            return "not a path a snapshot can hold";
        }
        return "unknown";
    }

    std::vector<std::string> paths_of(const snapshot &s)
    {
        std::vector<std::string> paths;
        paths.reserve(s.files.size());
        for (const snapshot_file &f : s.files)
            paths.push_back(f.path);
        return paths;
    }

    std::expected<captured_files, std::string> capture_files(const repository &repo, const std::filesystem::path &root,
                                                             std::span<const std::string> paths)
    {
        const fs::path base = absolute_normal(root);
        const fs::path repository_directory = absolute_normal(repo.directory());
        captured_files captured;
        std::set<std::string> seen;
        for (const std::string &given : paths)
        {
            auto located = locate(base, repository_directory, given);
            if (!located)
            {
                captured.skipped.push_back(std::move(located.error()));
                continue;
            }
            if (!seen.insert(located->path).second)
                continue;

            std::error_code error;
            const fs::file_status status = fs::status(located->full, error);
            if (status.type() == fs::file_type::not_found)
            {
                captured.skipped.push_back({given, skip_reason::missing, std::format("\"{}\" does not exist", given)});
                continue;
            }
            if (error)
                return std::unexpected(std::format("\"{}\": cannot read the file ({})", given, error.message()));
            if (!fs::is_regular_file(status))
            {
                captured.skipped.push_back({given, skip_reason::not_a_file, std::format("\"{}\" is not a file", given)});
                continue;
            }
            const std::uintmax_t size = fs::file_size(located->full, error);
            if (!error && size > max_file_size)
                return std::unexpected(std::format("\"{}\": the file is {} bytes, more than the {} a snapshot takes",
                                                   given, size, max_file_size));
            auto bytes = json::read_text_file(located->full, max_file_size);
            if (!bytes)
                return std::unexpected(std::format("\"{}\": {}", given, bytes.error()));
            captured.files.push_back({std::move(located->path), std::move(*bytes)});
        }
        return captured;
    }

    std::expected<files_snapshot, std::string> snapshot_files(repository &repo, const std::filesystem::path &root,
                                                              std::span<const std::string> paths,
                                                              const snapshot_options &options)
    {
        auto captured = capture_files(repo, root, paths);
        if (!captured)
            return std::unexpected(captured.error());
        auto taken = repo.take_snapshot(captured->files, options);
        if (!taken)
            return std::unexpected(taken.error());
        return files_snapshot{std::move(*taken), std::move(captured->skipped)};
    }

    std::expected<files_status, std::string> compare_files(const repository &repo, const std::filesystem::path &root,
                                                           std::span<const std::string> paths,
                                                           std::string_view snapshot_id)
    {
        auto captured = capture_files(repo, root, paths);
        if (!captured)
            return std::unexpected(captured.error());
        auto changes = repo.changes_since(snapshot_id, captured->files);
        if (!changes)
            return std::unexpected(changes.error());
        return files_status{std::move(*changes), std::move(captured->skipped)};
    }

    std::expected<restored_files, std::string> restore_files(const repository &repo, const std::filesystem::path &root,
                                                             std::string_view snapshot_id,
                                                             std::span<const std::string> tracked_before)
    {
        auto s = repo.read_snapshot(snapshot_id);
        if (!s)
            return std::unexpected(s.error());

        const fs::path base = absolute_normal(root);
        std::error_code error;
        if (!fs::is_directory(base, error))
            return std::unexpected(std::format("{}: the project directory does not exist", detail::path_to_utf8(base)));
        const fs::path canonical_root = fs::weakly_canonical(base, error);
        if (error)
            return std::unexpected(std::format("{}: {}", detail::path_to_utf8(base), error.message()));
        const fs::path canonical_repository = fs::weakly_canonical(absolute_normal(repo.directory()), error);
        if (error)
            return std::unexpected(std::format("{}: {}", detail::path_to_utf8(repo.directory()), error.message()));

        // Everything is read and checked before anything is written.
        std::vector<std::string> problems;
        std::vector<std::string> contents;
        std::vector<fs::path> targets;
        contents.reserve(s->files.size());
        targets.reserve(s->files.size());
        std::set<std::string> kept; // the snapshot's paths, ignoring case
        for (const snapshot_file &f : s->files)
        {
            auto target = safe_target(base, canonical_root, canonical_repository, f.path);
            if (!target)
                problems.push_back(std::move(target.error()));
            else if (fs::is_directory(fs::symlink_status(*target, error)))
                problems.push_back(std::format("\"{}\" is a directory on disk", f.path));
            auto bytes = repo.read_blob(f.blob);
            if (!bytes)
                problems.push_back(std::format("\"{}\": {}", f.path, bytes.error()));
            else if (bytes->size() != f.size)
                problems.push_back(std::format("\"{}\": the snapshot gives {} bytes, the blob has {}", f.path, f.size,
                                               bytes->size()));
            contents.push_back(bytes ? std::move(*bytes) : std::string());
            targets.push_back(target ? std::move(*target) : fs::path());
            kept.insert(detail::fold_case(f.path));
        }

        // Tracked files the snapshot does not have. Paths outside the root,
        // or that a snapshot could not hold, are never removed.
        std::vector<std::pair<std::string, fs::path>> removals;
        const fs::path repository_directory = absolute_normal(repo.directory());
        for (const std::string &given : tracked_before)
        {
            auto located = locate(base, repository_directory, given);
            if (!located || kept.contains(detail::fold_case(located->path)))
                continue;
            if (std::ranges::any_of(removals, [&](const auto &r) { return r.first == located->path; }))
                continue;
            auto target = safe_target(base, canonical_root, canonical_repository, located->path);
            if (!target)
                problems.push_back(std::move(target.error()));
            else
                removals.emplace_back(located->path, std::move(*target));
        }

        if (!problems.empty())
        {
            std::string message = std::format("snapshot {} cannot be restored, and nothing was written: {}",
                                              snapshot_id.substr(0, 12), problems.front());
            if (problems.size() > 1)
                message += std::format(" (and {} more problems)", problems.size() - 1);
            return std::unexpected(std::move(message));
        }

        restored_files result;
        for (std::size_t i = 0; i < s->files.size(); ++i)
        {
            const std::string &path = s->files[i].path;
            const fs::path &target = targets[i];
            if (detail::file_holds(target, contents[i]))
            {
                result.unchanged.push_back(path);
                continue;
            }
            fs::create_directories(target.parent_path(), error);
            if (error)
                return std::unexpected(std::format("\"{}\": cannot create its directory ({}); {} files were restored "
                                                   "before this",
                                                   path, error.message(), result.written.size()));
            // Made now, the directories are checked again.
            if (auto still_safe = safe_target(base, canonical_root, canonical_repository, path); !still_safe)
                return std::unexpected(still_safe.error());
            if (auto written = detail::write_atomically(target, contents[i]); !written)
                return std::unexpected(std::format("{}; {} files were restored before this", written.error(),
                                                   result.written.size()));
            result.written.push_back(path);
        }

        for (const auto &[path, target] : removals)
        {
            const fs::file_status status = fs::symlink_status(target, error);
            if (status.type() == fs::file_type::not_found || fs::is_directory(status))
                continue;
            fs::remove(target, error);
            if (error)
                return std::unexpected(std::format("\"{}\": cannot remove the file ({})", path, error.message()));
            result.removed.push_back(path);
        }
        return result;
    }

} // namespace volt::history
