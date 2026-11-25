#include <app/ui/project_history.hpp>

#include <library/project.hpp>

#include <algorithm>
#include <ctime>
#include <format>
#include <set>
#include <system_error>
#include <utility>

namespace volt::ui
{
    namespace
    {
        namespace fs = std::filesystem;

        // Whether a snapshot's path names a project file.
        bool is_project_file(std::string_view path)
        {
            const std::string_view extension = library::project_extension;
            if (path.size() < extension.size())
                return false;
            const std::string_view tail = path.substr(path.size() - extension.size());
            return std::ranges::equal(tail, extension, [](char a, char b) {
                const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
                return lower(a) == lower(b);
            });
        }

        std::string first_line(std::string_view text)
        {
            return std::string(text.substr(0, text.find_first_of("\r\n")));
        }
    } // namespace

    // ---- helpers ----------------------------------------------------------------------------

    std::string short_id(std::string_view id)
    {
        return std::string(id.substr(0, 8));
    }

    std::string local_time_text(std::chrono::sys_seconds time)
    {
        const std::time_t t = std::chrono::system_clock::to_time_t(time);
        std::tm local{};
#ifdef _WIN32
        if (localtime_s(&local, &t) != 0)
            return {};
#else
        if (localtime_r(&t, &local) == nullptr)
            return {};
#endif
        return std::format("{:04}-{:02}-{:02} {:02}:{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                           local.tm_hour, local.tm_min);
    }

    std::string describe_changes(const std::vector<history::file_change> &changes, std::size_t listed)
    {
        listed = std::max<std::size_t>(listed, 1);
        const std::size_t named = std::min(listed, changes.size());
        std::vector<std::string> parts;
        for (std::size_t i = 0; i < named; ++i)
            parts.push_back(std::format("{} ({})", changes[i].path, history::describe(changes[i].status)));
        const std::size_t rest = changes.size() - named;
        if (rest > 0)
            parts.push_back(std::format("{} more file{}", rest, rest == 1 ? "" : "s"));

        std::string text;
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            if (i > 0)
                text += i + 1 == parts.size() ? " and " : ", ";
            text += parts[i];
        }
        return text;
    }

    std::vector<history::skipped_file> notable_skips(const std::vector<history::skipped_file> &skipped)
    {
        // A sheet or a board that has not been written yet is the usual
        // case for now, not something to warn about.
        std::vector<history::skipped_file> notable;
        for (const history::skipped_file &s : skipped)
            if (s.reason != history::skip_reason::missing)
                notable.push_back(s);
        return notable;
    }

    // ---- the project's files ------------------------------------------------------------------

    std::vector<std::string> project_history::tracked_files(const library::project &p,
                                                            const std::filesystem::path &project_file)
    {
        // As the project stores them: relative to its folder, which is the
        // workspace's root. One that lies elsewhere is reported by the
        // workspace as outside it, and left out of every snapshot.
        std::vector<std::string> files;
        const auto add = [&files](std::string path) {
            if (!path.empty() && std::ranges::find(files, path) == files.end())
                files.push_back(std::move(path));
        };
        add(library::path_to_utf8(project_file.filename()));
        add(p.library_file);
        for (const library::sheet &sheet : p.sheets)
            add(sheet.file);
        add(p.board);
        return files;
    }

    void project_history::set_project(const std::filesystem::path &project_file, const library::project *p)
    {
        m_project_file = p != nullptr ? project_file : std::filesystem::path();
        m_root = m_project_file.empty() ? std::filesystem::path() : m_project_file.parent_path();
        m_tracked = p != nullptr && !m_project_file.empty() ? tracked_files(*p, m_project_file)
                                                            : std::vector<std::string>{};
        refresh();
    }

    void project_history::set_clock(history::clock_function clock)
    {
        m_clock = std::move(clock);
        if (m_repository)
            m_repository->set_clock(m_clock);
    }

    bool project_history::open_repository()
    {
        // Opened again each time: cheap (history.json only), and what is on
        // disk is what counts, whoever changed it last.
        m_repository.reset();
        const fs::path directory = m_root / history::history_directory_name;
        if (!history::repository::exists(directory))
            return false;
        auto opened = history::repository::open(directory);
        if (!opened)
        {
            m_view.state = history_state::broken;
            m_view.problem = opened.error();
            return false;
        }
        m_repository = std::move(*opened);
        if (m_clock)
            m_repository->set_clock(m_clock);
        return true;
    }

    void project_history::refresh()
    {
        m_view = {};
        m_repository.reset();
        if (m_project_file.empty())
            return; // no_project

        std::error_code error;
        if (!fs::is_regular_file(m_project_file, error))
        {
            m_view.state = history_state::not_on_disk;
            return;
        }
        m_view.state = history_state::no_history;
        if (!open_repository())
            return; // none yet, or broken

        m_view.state = history_state::ready;
        const history::head_state head = m_repository->head();
        m_view.branch = head.branch;
        m_view.head = head.snapshot;
        for (const history::branch &b : m_repository->branches())
            m_view.branches.push_back({b.name, b.snapshot, b.name == head.branch});

        if (auto log = m_repository->log({}, list_limit + 1))
        {
            m_view.more = log->size() > list_limit;
            if (m_view.more)
                log->pop_back();
            for (const history::snapshot &s : *log)
            {
                const auto time = history::parse_time(s.time);
                m_view.snapshots.push_back(
                    {s.id, short_id(s.id), first_line(s.message), time ? local_time_text(*time) : std::string(),
                     s.id == head.snapshot});
            }
        }
        else
        {
            m_view.problem = log.error();
        }

        if (auto status = history::compare_files(*m_repository, m_root, m_tracked, head.snapshot))
        {
            m_view.status_known = true;
            m_view.changes = std::move(status->changes);
            m_view.skipped = std::move(status->skipped);
        }
        else if (m_view.problem.empty())
        {
            m_view.problem = status.error();
        }
    }

    const history_entry *project_history::find(std::string_view id) const noexcept
    {
        for (const history_entry &e : m_view.snapshots)
            if (e.id == id)
                return &e;
        return nullptr;
    }

    std::expected<history::files_status, std::string> project_history::status() const
    {
        if (!m_repository)
            return std::unexpected(std::string("the project has no history yet"));
        return history::compare_files(*m_repository, m_root, m_tracked, m_repository->head().snapshot);
    }

    // ---- changes ------------------------------------------------------------------------------

    std::expected<project_history::taken, std::string> project_history::take_snapshot(std::string message,
                                                                                    std::string author)
    {
        switch (m_view.state)
        {
        case history_state::no_project:
            return std::unexpected(std::string("no project is open"));
        case history_state::not_on_disk:
            return std::unexpected(std::string("the project is not on disk: save it first"));
        case history_state::broken:
            return std::unexpected("the history cannot be read: " + m_view.problem);
        case history_state::no_history:
        case history_state::ready:
            break;
        }

        // The first snapshot makes the repository.
        if (!m_repository)
        {
            auto made = history::repository::open_or_create(m_root / history::history_directory_name);
            if (!made)
                return std::unexpected(made.error());
            m_repository = std::move(*made);
            if (m_clock)
                m_repository->set_clock(m_clock);
        }

        history::snapshot_options options;
        options.message = std::move(message);
        options.author = std::move(author);
        auto snapshot = history::snapshot_files(*m_repository, m_root, m_tracked, options);
        refresh();
        if (!snapshot)
            return std::unexpected(snapshot.error());
        taken result;
        result.id = snapshot->taken.id;
        result.created = snapshot->taken.created;
        result.skipped = std::move(snapshot->skipped);
        return result;
    }

    std::vector<std::string> project_history::head_paths() const
    {
        // The files the head's snapshot has: those that putting another
        // snapshot back removes when it does not have them. A file that is
        // in no snapshot is never removed.
        const std::string head = m_repository->head().snapshot;
        if (head.empty())
            return {};
        auto s = m_repository->read_snapshot(head);
        return s ? history::paths_of(*s) : std::vector<std::string>{};
    }

    std::filesystem::path project_history::project_file_in(std::string_view snapshot_id) const
    {
        auto s = m_repository->read_snapshot(snapshot_id);
        if (!s)
            return m_project_file;
        const std::string own = library::path_to_utf8(m_project_file.filename());
        for (const history::snapshot_file &f : s->files)
        {
#ifdef _WIN32
            const bool same = history::same_path_ignoring_case(f.path, own);
#else
            const bool same = f.path == own;
#endif
            if (same)
                return m_project_file;
        }
        // Saved under another name in the same folder since (Save Project
        // As): the snapshot's own, one at the top first.
        const history::snapshot_file *found = nullptr;
        for (const history::snapshot_file &f : s->files)
        {
            if (!is_project_file(f.path))
                continue;
            if (found == nullptr || (found->path.find('/') != std::string::npos && f.path.find('/') == std::string::npos))
                found = &f;
        }
        return found != nullptr ? m_root / library::path_from_utf8(found->path) : std::filesystem::path();
    }

    project_history::checkout project_history::put_back(std::string_view id)
    {
        checkout result;
        result.project_file = m_project_file;

        // The head's snapshot must be readable: it says which files to
        // remove, and without it a restore would leave files of the head's
        // state behind, unexplained.
        const std::string head = m_repository->head().snapshot;
        if (!head.empty())
        {
            if (auto s = m_repository->read_snapshot(head); !s)
            {
                result.error = "the snapshot the project is at cannot be read: " + s.error();
                return result;
            }
        }
        const std::vector<std::string> before = head_paths();

        result.project_file = project_file_in(id);
        // From here files may be written, even if it fails part way.
        result.files_touched = true;
        auto restored = history::restore_files(*m_repository, m_root, id, before);
        if (!restored)
        {
            result.error = restored.error();
            result.project_file = m_project_file;
            // Part of them may be back; whichever project file is there.
            std::error_code error;
            if (!fs::is_regular_file(m_project_file, error))
                result.project_file = project_file_in(id);
            return result;
        }
        result.restored = std::move(*restored);
        return result;
    }

    project_history::checkout project_history::switch_branch(std::string_view name)
    {
        checkout result;
        result.project_file = m_project_file;
        if (!m_repository)
        {
            result.error = "the project has no history yet";
            return result;
        }
        const history::branch *b = m_repository->find_branch(name);
        if (b == nullptr)
        {
            result.error = std::format("there is no branch named \"{}\"", name);
            return result;
        }
        const history::head_state head = m_repository->head();
        if (b->name == head.branch)
        {
            result.error = std::format("\"{}\" is the current branch already", name);
            return result;
        }
        if (b->snapshot.empty())
        {
            result.error = std::format("\"{}\" has no snapshot yet", name);
            return result;
        }

        // The files first: if they cannot be put back, the head stays.
        const std::string target = b->snapshot;
        result = put_back(target);
        if (result.ok())
        {
            if (auto switched = m_repository->switch_branch(name); !switched)
                result.error = std::format("the files are as on \"{}\", but the history could not be moved to it ({}); "
                                           "they show as changes on \"{}\"",
                                           name, switched.error(), head.branch.empty() ? "the snapshot" : head.branch);
        }
        refresh();
        return result;
    }

    project_history::checkout project_history::restore(std::string_view id)
    {
        checkout result;
        result.project_file = m_project_file;
        if (!m_repository)
        {
            result.error = "the project has no history yet";
            return result;
        }
        if (!m_repository->has_snapshot(id))
        {
            result.error = std::format("there is no snapshot {}", short_id(id));
            return result;
        }
        result = put_back(id);
        refresh();
        return result;
    }

    project_history::checkout project_history::create_branch(std::string_view name, std::string_view from)
    {
        checkout result;
        result.project_file = m_project_file;
        if (!m_repository)
        {
            result.error = "take a snapshot first: a branch starts at one";
            return result;
        }
        if (auto checked = check_branch_name(name); !checked)
        {
            result.error = checked.error();
            return result;
        }
        const history::head_state head = m_repository->head();
        const std::string at = from.empty() ? head.snapshot : std::string(from);
        if (at.empty())
        {
            result.error = "take a snapshot first: a branch starts at one";
            return result;
        }
        if (auto made = m_repository->create_branch(name, at); !made)
        {
            result.error = made.error();
            return result;
        }

        // From an older snapshot: its files first; the branch goes again
        // if they cannot be put back, and the head stays.
        if (at != head.snapshot)
        {
            result = put_back(at);
            if (!result.ok())
            {
                (void)m_repository->delete_branch(name);
                refresh();
                return result;
            }
        }
        if (auto switched = m_repository->switch_branch(name); !switched)
            result.error = std::format("the branch \"{}\" was made, but the history could not be moved to it ({})",
                                       name, switched.error());
        refresh();
        return result;
    }

    std::expected<void, std::string> project_history::rename_branch(std::string_view name, std::string_view new_name)
    {
        if (!m_repository)
            return std::unexpected(std::string("the project has no history yet"));
        if (auto checked = check_branch_name(new_name, name); !checked)
            return checked;
        auto renamed = m_repository->rename_branch(name, new_name);
        refresh();
        return renamed;
    }

    std::expected<void, std::string> project_history::delete_branch(std::string_view name)
    {
        if (!m_repository)
            return std::unexpected(std::string("the project has no history yet"));
        auto deleted = m_repository->delete_branch(name);
        refresh();
        return deleted;
    }

    std::expected<std::size_t, std::string> project_history::snapshots_only_on(std::string_view name) const
    {
        if (!m_repository)
            return std::unexpected(std::string("the project has no history yet"));
        const history::branch *b = m_repository->find_branch(name);
        if (b == nullptr)
            return std::unexpected(std::format("there is no branch named \"{}\"", name));
        if (b->snapshot.empty())
            return 0;

        // What the other branches (and a detached head) lead back to; the
        // branch's own snapshots are counted until it joins one of them.
        std::set<std::string> elsewhere;
        std::vector<std::string> starts;
        for (const history::branch &other : m_repository->branches())
            if (other.name != b->name && !other.snapshot.empty())
                starts.push_back(other.snapshot);
        if (const history::head_state head = m_repository->head(); head.detached() && !head.snapshot.empty())
            starts.push_back(head.snapshot);
        for (const std::string &start : starts)
        {
            auto log = m_repository->log(start);
            if (!log)
                return std::unexpected(log.error());
            for (const history::snapshot &s : *log)
                elsewhere.insert(s.id);
        }
        auto own = m_repository->log(b->snapshot);
        if (!own)
            return std::unexpected(own.error());
        std::size_t only = 0;
        for (const history::snapshot &s : *own)
        {
            if (elsewhere.contains(s.id))
                break;
            ++only;
        }
        return only;
    }

    std::expected<void, std::string> project_history::check_branch_name(std::string_view name,
                                                                        std::string_view renaming) const
    {
        if (auto checked = history::check_branch_name(name); !checked)
            return checked;
        if (m_repository && name != renaming && m_repository->find_branch(name) != nullptr)
            return std::unexpected(std::format("there is a branch named \"{}\" already", name));
        return {};
    }

} // namespace volt::ui
