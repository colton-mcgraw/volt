#include <history/repository.hpp>

#include <history/sha256.hpp>

#include "files.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <utility>

namespace volt::history
{

    namespace
    {
        namespace fs = std::filesystem;

        constexpr std::string_view history_file_name = "history.json";
        constexpr std::string_view blobs_directory = "blobs";
        constexpr std::string_view snapshots_directory = "snapshots";

        constexpr std::size_t max_path_size = 4096;
        constexpr std::size_t max_segment_size = 255;

        constexpr std::string_view snapshot_keys[] = {"format", "format_version", "parents", "time",
                                                      "author", "message", "files"};
        constexpr std::string_view file_keys[] = {"path", "blob", "size"};
        constexpr std::string_view history_keys[] = {"format", "format_version", "head", "branches"};
        constexpr std::string_view head_keys[] = {"branch", "snapshot"};
        constexpr std::string_view branch_keys[] = {"name", "snapshot"};

        template <std::size_t N>
        bool is_one_of(std::string_view key, const std::string_view (&keys)[N]) noexcept
        {
            return std::ranges::find(keys, key) != std::end(keys);
        }

        // An id as shown in messages: the first twelve digits are plenty to
        // tell snapshots apart, and the full path is given beside it.
        std::string_view short_id(std::string_view id) noexcept
        {
            return id.substr(0, 12);
        }

        // Text from a file or a caller, made safe to put in a message:
        // control characters and bytes that are not UTF-8 become '?'.
        std::string printable(std::string_view text)
        {
            std::string out;
            for (std::size_t at = 0; at < text.size();)
            {
                const std::size_t start = at;
                const std::int32_t c = detail::decode_utf8(text, at);
                if (c < 0)
                {
                    out += '?';
                    at = start + 1;
                }
                else if (c < 0x20 || c == 0x7F)
                    out += '?';
                else
                    out.append(text.substr(start, at - start));
            }
            return out;
        }

        bool is_control(char c) noexcept
        {
            return static_cast<unsigned char>(c) < 0x20 || c == 0x7F;
        }

        // CON, PRN, AUX, NUL, COM1-9 and LPT1-9 name devices on Windows, with
        // any extension ("nul.txt" too).
        bool is_device_name(std::string_view segment)
        {
            std::string stem(segment.substr(0, segment.find('.')));
            while (!stem.empty() && stem.back() == ' ')
                stem.pop_back();
            for (char &c : stem)
                if (c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 32);
            if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
                return true;
            return stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '1' &&
                   stem[3] <= '9';
        }

        // ---- objects ---------------------------------------------------------------

        fs::path object_file(const fs::path &directory, std::string_view kind, std::string_view id)
        {
            return directory / detail::path_from_utf8(kind) / detail::path_from_utf8(id.substr(0, 2)) /
                   detail::path_from_utf8(id.substr(2));
        }

        struct object_error
        {
            bool missing = false;
            std::string message;
        };

        // The bytes of an object, checked against its id.
        std::expected<std::string, object_error> load_object(const fs::path &file, std::string_view kind,
                                                             std::string_view id)
        {
            const std::string shown = detail::path_to_utf8(file);
            std::error_code error;
            const fs::file_status status = fs::status(file, error);
            if (status.type() == fs::file_type::not_found)
                return std::unexpected(object_error{true, std::format("{} {} is missing ({})", kind, short_id(id), shown)});
            if (!fs::is_regular_file(status))
                return std::unexpected(
                    object_error{false, std::format("{} {} cannot be read: it is not a file ({})", kind, short_id(id), shown)});
            auto bytes = json::read_text_file(file, max_file_size);
            if (!bytes)
                return std::unexpected(
                    object_error{false, std::format("{} {} cannot be read: {} ({})", kind, short_id(id), bytes.error(), shown)});
            if (sha256_hex(*bytes) != id)
                return std::unexpected(object_error{
                    false, std::format("{} {} is damaged: its bytes do not match its name ({})", kind, short_id(id), shown)});
            return std::move(*bytes);
        }

        // Writes an object unless it is there already. One that is there
        // with other bytes is damaged, and is replaced.
        std::expected<void, std::string> store_object(const fs::path &file, std::string_view bytes)
        {
            if (detail::file_holds(file, bytes))
                return {};
            std::error_code error;
            fs::create_directories(file.parent_path(), error);
            if (error)
                return std::unexpected(std::format("{}: cannot create the directory ({})",
                                                   detail::path_to_utf8(file.parent_path()), error.message()));
            return detail::write_atomically(file, bytes);
        }

        // ---- snapshots -------------------------------------------------------------

        std::string snapshot_text(const snapshot &s)
        {
            json::object o;
            o.set("format", json::value(snapshot_format));
            o.set("format_version", json::value(s.format_version));
            json::array parents;
            for (const std::string &parent : s.parents)
                parents.emplace_back(parent);
            o.set("parents", json::value(std::move(parents)));
            o.set("time", json::value(s.time));
            o.set("author", json::value(s.author));
            o.set("message", json::value(s.message));
            json::array files;
            for (const snapshot_file &f : s.files)
            {
                json::object fo;
                fo.set("path", json::value(f.path));
                fo.set("blob", json::value(f.blob));
                fo.set("size", json::value(f.size));
                for (const json::member &m : f.extra)
                    if (!is_one_of(m.key, file_keys))
                        fo.set(m.key, m.value);
                files.emplace_back(std::move(fo));
            }
            o.set("files", json::value(std::move(files)));
            for (const json::member &m : s.extra)
                if (!is_one_of(m.key, snapshot_keys))
                    o.set(m.key, m.value);

            // Wide enough that each file is one line.
            json::write_options options;
            options.line_width = 200;
            return json::write(json::value(std::move(o)), options);
        }

        // Checks that the paths are sorted, unique, and unique ignoring case.
        std::expected<void, std::string> check_order(const std::vector<snapshot_file> &files)
        {
            std::vector<std::pair<std::string, std::string_view>> folded;
            folded.reserve(files.size());
            for (std::size_t i = 0; i < files.size(); ++i)
            {
                if (i > 0 && files[i - 1].path == files[i].path)
                    return std::unexpected(std::format("\"{}\" is given twice", printable(files[i].path)));
                if (i > 0 && files[i - 1].path > files[i].path)
                    return std::unexpected("the files are not sorted by path");
                folded.emplace_back(detail::fold_case(files[i].path), files[i].path);
            }
            std::ranges::sort(folded);
            for (std::size_t i = 1; i < folded.size(); ++i)
                if (folded[i - 1].first == folded[i].first)
                    return std::unexpected(std::format("\"{}\" and \"{}\" differ only in case, and would be one file "
                                                       "on Windows",
                                                       printable(folded[i - 1].second), printable(folded[i].second)));
            return {};
        }

        std::expected<snapshot, std::string> parse_snapshot(std::string_view text)
        {
            auto document = json::parse(text);
            if (!document)
                return std::unexpected(document.error().message());
            const json::object *root = document->as_object();
            if (root == nullptr)
                return std::unexpected("not a snapshot: the file does not hold an object");
            if (document->at("format").as_string() != std::optional<std::string_view>(snapshot_format))
                return std::unexpected(std::format("not a snapshot: \"format\" should be \"{}\"", snapshot_format));

            snapshot s;
            const auto version = document->at("format_version").as_integer();
            if (!version || *version < 1)
                return std::unexpected("\"format_version\" should be a whole number from 1");
            if (*version > snapshot_format_version)
                return std::unexpected(std::format("written by a newer Volt (snapshot format version {}; this one "
                                                   "reads up to {})",
                                                   *version, snapshot_format_version));
            s.format_version = static_cast<int>(*version);

            const json::array *parents = document->at("parents").as_array();
            if (parents == nullptr)
                return std::unexpected("\"parents\" should be an array");
            for (const json::value &parent : *parents)
            {
                const auto id = parent.as_string();
                if (!id || !is_object_id(*id))
                    return std::unexpected("\"parents\" should hold snapshot ids");
                s.parents.emplace_back(*id);
            }

            const auto text_field = [&](std::string_view key, std::string &out) -> std::expected<void, std::string> {
                const json::value &v = document->at(key);
                if (v.is_null())
                    return {};
                const auto string = v.as_string();
                if (!string)
                    return std::unexpected(std::format("\"{}\" should be a string", key));
                out = std::string(*string);
                return {};
            };
            if (auto ok = text_field("time", s.time); !ok)
                return std::unexpected(ok.error());
            if (auto ok = text_field("author", s.author); !ok)
                return std::unexpected(ok.error());
            if (auto ok = text_field("message", s.message); !ok)
                return std::unexpected(ok.error());

            const json::array *files = document->at("files").as_array();
            if (files == nullptr)
                return std::unexpected("\"files\" should be an array");
            for (std::size_t i = 0; i < files->size(); ++i)
            {
                const json::value &f = (*files)[i];
                const json::object *fo = f.as_object();
                if (fo == nullptr)
                    return std::unexpected(std::format("file {} should be an object", i));
                snapshot_file entry;
                const auto path = f.at("path").as_string();
                if (!path)
                    return std::unexpected(std::format("file {} has no \"path\"", i));
                if (auto checked = check_path(*path); !checked)
                    return std::unexpected(std::format("file {}: {}", i, checked.error()));
                entry.path = std::string(*path);
                const auto blob = f.at("blob").as_string();
                if (!blob || !is_object_id(*blob))
                    return std::unexpected(std::format("file {} (\"{}\") should have a blob id", i, entry.path));
                entry.blob = std::string(*blob);
                const auto size = f.at("size").as_integer();
                if (!size || *size < 0)
                    return std::unexpected(std::format("file {} (\"{}\") should have a size", i, entry.path));
                entry.size = static_cast<std::uint64_t>(*size);
                for (const json::member &m : *fo)
                    if (!is_one_of(m.key, file_keys))
                        entry.extra.set(m.key, m.value);
                s.files.push_back(std::move(entry));
            }
            if (auto ordered = check_order(s.files); !ordered)
                return std::unexpected(ordered.error());

            for (const json::member &m : *root)
                if (!is_one_of(m.key, snapshot_keys))
                    s.extra.set(m.key, m.value);
            return s;
        }

        // Files given to be snapshotted, checked, hashed and sorted.
        struct prepared_file
        {
            snapshot_file entry;
            std::string_view bytes;
        };

        std::expected<std::vector<prepared_file>, std::string> prepare(std::span<const file_content> files)
        {
            std::vector<prepared_file> prepared;
            prepared.reserve(files.size());
            for (const file_content &f : files)
            {
                if (auto checked = check_path(f.path); !checked)
                    return std::unexpected(checked.error());
                if (f.bytes.size() > max_file_size)
                    return std::unexpected(std::format("\"{}\": the file is {} bytes, more than the {} allowed", f.path,
                                                       f.bytes.size(), max_file_size));
                prepared.push_back({snapshot_file{f.path, sha256_hex(f.bytes), f.bytes.size(), {}}, f.bytes});
            }
            std::ranges::sort(prepared, {}, [](const prepared_file &p) -> const std::string & { return p.entry.path; });
            std::vector<snapshot_file> entries;
            entries.reserve(prepared.size());
            for (const prepared_file &p : prepared)
                entries.push_back(p.entry);
            if (auto ordered = check_order(entries); !ordered)
                return std::unexpected(ordered.error());
            return prepared;
        }

        bool same_files(const std::vector<snapshot_file> &a, const std::vector<snapshot_file> &b)
        {
            return std::ranges::equal(a, b, [](const snapshot_file &x, const snapshot_file &y) {
                return x.path == y.path && x.blob == y.blob && x.size == y.size;
            });
        }

        // Both sorted by path.
        std::vector<file_change> differences(const std::vector<snapshot_file> &from, const std::vector<snapshot_file> &to)
        {
            std::vector<file_change> changes;
            std::size_t i = 0;
            std::size_t j = 0;
            while (i < from.size() || j < to.size())
            {
                if (j == to.size() || (i < from.size() && from[i].path < to[j].path))
                {
                    changes.push_back({from[i].path, file_status::removed, from[i].blob, {}});
                    ++i;
                }
                else if (i == from.size() || to[j].path < from[i].path)
                {
                    changes.push_back({to[j].path, file_status::added, {}, to[j].blob});
                    ++j;
                }
                else
                {
                    if (from[i].blob != to[j].blob)
                        changes.push_back({to[j].path, file_status::modified, from[i].blob, to[j].blob});
                    ++i;
                    ++j;
                }
            }
            return changes;
        }

        std::chrono::sys_seconds system_now()
        {
            return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        }
    } // namespace

    // ---- names -----------------------------------------------------------------------

    std::string_view describe(file_status status) noexcept
    {
        switch (status)
        {
        case file_status::added:
            return "added";
        case file_status::removed:
            return "removed";
        case file_status::modified:
            return "modified";
        }
        return "unknown";
    }

    bool is_object_id(std::string_view text) noexcept
    {
        return text.size() == 64 &&
               std::ranges::all_of(text, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }

    std::expected<void, std::string> check_path(std::string_view path)
    {
        const auto fail = [&](std::string_view why) {
            return std::unexpected(std::format("\"{}\": {}", printable(path), why));
        };
        if (path.empty())
            return std::unexpected("a path is empty");
        if (path.size() > max_path_size)
            return fail(std::format("the path is longer than {} bytes", max_path_size));
        if (!detail::valid_utf8(path))
            return fail("the path is not valid UTF-8");
        for (const char c : path)
        {
            if (is_control(c))
                return fail("the path has a control character");
            if (c == '\\')
                return fail("the path has a backslash; paths are written with /");
            if (c == ':')
                return fail("the path has a colon (a drive letter, or a stream on Windows)");
            if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*')
                return fail(std::format("the path has '{}', which Windows does not allow in a name", c));
        }
        if (path.front() == '/')
            return fail("the path is absolute; it should be relative to the project");

        std::size_t start = 0;
        while (start <= path.size())
        {
            const std::size_t end = std::min(path.find('/', start), path.size());
            const std::string_view segment = path.substr(start, end - start);
            if (segment.empty())
                return fail("the path has an empty segment");
            if (segment == "." || segment == "..")
                return fail("the path has a \".\" or \"..\" segment");
            if (segment.size() > max_segment_size)
                return fail(std::format("the path has a name longer than {} bytes", max_segment_size));
            if (segment.back() == '.' || segment.back() == ' ')
                return fail("the path has a name ending in a dot or a space, which Windows drops");
            if (is_device_name(segment))
                return fail(std::format("\"{}\" names a device on Windows", segment));
            start = end + 1;
        }
        return {};
    }

    std::expected<void, std::string> check_branch_name(std::string_view name)
    {
        const auto fail = [&](std::string_view why) {
            return std::unexpected(std::format("the branch name \"{}\" {}", printable(name), why));
        };
        if (name.empty())
            return std::unexpected("a branch name cannot be empty");
        if (name.size() > max_branch_name_size)
            return fail(std::format("is longer than {} bytes", max_branch_name_size));
        if (!detail::valid_utf8(name))
            return fail("is not valid UTF-8");
        for (const char c : name)
        {
            if (is_control(c))
                return fail("has a control character");
            if (c == '/' || c == '\\' || c == ':')
                return fail(std::format("has '{}'", c));
        }
        if (name.front() == ' ' || name.back() == ' ')
            return fail("starts or ends with a space");
        return {};
    }

    bool same_path_ignoring_case(std::string_view a, std::string_view b)
    {
        return detail::fold_case(a) == detail::fold_case(b);
    }

    std::string format_time(std::chrono::sys_seconds time)
    {
        const auto days = std::chrono::floor<std::chrono::days>(time);
        const std::chrono::year_month_day date{days};
        const std::chrono::hh_mm_ss time_of_day{time - days};
        return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", static_cast<int>(date.year()),
                           static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                           time_of_day.hours().count(), time_of_day.minutes().count(), time_of_day.seconds().count());
    }

    std::optional<std::chrono::sys_seconds> parse_time(std::string_view text)
    {
        // YYYY-MM-DDTHH:MM:SSZ
        if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
            text[16] != ':' || text[19] != 'Z')
            return std::nullopt;
        const auto number = [&](std::size_t at, std::size_t digits) -> std::optional<int> {
            int n = 0;
            for (std::size_t i = at; i < at + digits; ++i)
            {
                if (text[i] < '0' || text[i] > '9')
                    return std::nullopt;
                n = n * 10 + (text[i] - '0');
            }
            return n;
        };
        const auto year = number(0, 4), month = number(5, 2), day = number(8, 2);
        const auto hour = number(11, 2), minute = number(14, 2), second = number(17, 2);
        if (!year || !month || !day || !hour || !minute || !second)
            return std::nullopt;
        const std::chrono::year_month_day date{std::chrono::year{*year},
                                               std::chrono::month{static_cast<unsigned>(*month)},
                                               std::chrono::day{static_cast<unsigned>(*day)}};
        if (!date.ok() || *hour > 23 || *minute > 59 || *second > 59)
            return std::nullopt;
        return std::chrono::sys_days{date} + std::chrono::hours{*hour} + std::chrono::minutes{*minute} +
               std::chrono::seconds{*second};
    }

    const snapshot_file *snapshot::find(std::string_view path) const noexcept
    {
        const auto it = std::ranges::lower_bound(files, path, {}, [](const snapshot_file &f) -> std::string_view {
            return f.path;
        });
        return it != files.end() && it->path == path ? &*it : nullptr;
    }

    // ---- opening ---------------------------------------------------------------------

    bool repository::exists(const std::filesystem::path &directory)
    {
        std::error_code error;
        return fs::is_regular_file(directory / detail::path_from_utf8(history_file_name), error);
    }

    std::expected<repository, std::string> repository::open(const std::filesystem::path &directory)
    {
        repository r;
        std::error_code error;
        r.m_directory = fs::absolute(directory, error).lexically_normal();
        if (error)
            r.m_directory = directory;
        if (auto loaded = r.load(); !loaded)
            return std::unexpected(loaded.error());
        return r;
    }

    std::expected<repository, std::string> repository::create(const std::filesystem::path &directory)
    {
        repository r;
        std::error_code error;
        r.m_directory = fs::absolute(directory, error).lexically_normal();
        if (error)
            r.m_directory = directory;
        const std::string shown = detail::path_to_utf8(r.m_directory);
        if (exists(r.m_directory))
            return std::unexpected(std::format("{}: there is a Volt history here already", shown));
        for (const std::string_view sub : {blobs_directory, snapshots_directory})
        {
            fs::create_directories(r.m_directory / detail::path_from_utf8(sub), error);
            if (error)
                return std::unexpected(std::format("{}: cannot create the directory ({})", shown, error.message()));
        }
        std::vector<branch> branches;
        branches.push_back(branch{std::string(default_branch), {}, {}});
        if (auto written = r.commit(std::string(default_branch), {}, std::move(branches)); !written)
            return std::unexpected(written.error());
        return r;
    }

    std::expected<repository, std::string> repository::open_or_create(const std::filesystem::path &directory)
    {
        return exists(directory) ? open(directory) : create(directory);
    }

    void repository::set_clock(clock_function clock)
    {
        m_clock = std::move(clock);
    }

    std::expected<void, std::string> repository::load()
    {
        const fs::path file = m_directory / detail::path_from_utf8(history_file_name);
        const std::string shown = detail::path_to_utf8(file);
        if (!exists(m_directory))
            return std::unexpected(std::format("{}: there is no Volt history here (no {})",
                                               detail::path_to_utf8(m_directory), history_file_name));
        auto bytes = json::read_text_file(file);
        if (!bytes)
            return std::unexpected(std::format("{}: {}", shown, bytes.error()));
        auto document = json::parse(*bytes);
        if (!document)
            return std::unexpected(std::format("{}: {}", shown, document.error().message()));
        const auto fail = [&](std::string_view why) { return std::unexpected(std::format("{}: {}", shown, why)); };

        const json::object *root = document->as_object();
        if (root == nullptr || document->at("format").as_string() != std::optional<std::string_view>(history_format))
            return fail(std::format("not a Volt history (\"format\" should be \"{}\")", history_format));
        const auto version = document->at("format_version").as_integer();
        if (!version || *version < 1)
            return fail("\"format_version\" should be a whole number from 1");
        if (*version > history_format_version)
            return fail(std::format("written by a newer version of Volt (history format version {}; this one reads up "
                                    "to {}). Open the project with a newer Volt.",
                                    *version, history_format_version));

        std::vector<branch> branches;
        const json::array *array = document->at("branches").as_array();
        if (array == nullptr)
            return fail("\"branches\" should be an array");
        for (const json::value &v : *array)
        {
            const json::object *bo = v.as_object();
            if (bo == nullptr)
                return fail("a branch should be an object");
            branch b;
            const auto name = v.at("name").as_string();
            if (!name)
                return fail("a branch has no \"name\"");
            if (auto checked = check_branch_name(*name); !checked)
                return fail(checked.error());
            b.name = std::string(*name);
            if (std::ranges::any_of(branches, [&](const branch &other) { return other.name == b.name; }))
                return fail(std::format("there are two branches named \"{}\"", b.name));
            const json::value &snapshot_id = v.at("snapshot");
            if (!snapshot_id.is_null())
            {
                const auto id = snapshot_id.as_string();
                if (!id || !is_object_id(*id))
                    return fail(std::format("the branch \"{}\" should name a snapshot by its id", b.name));
                b.snapshot = std::string(*id);
            }
            for (const json::member &m : *bo)
                if (!is_one_of(m.key, branch_keys))
                    b.extra.set(m.key, m.value);
            branches.push_back(std::move(b));
        }

        const json::value &head_value = document->at("head");
        if (!head_value.is_object())
            return fail("\"head\" should be an object");
        std::string head_branch;
        std::string head_snapshot;
        if (const auto name = head_value.at("branch").as_string())
        {
            head_branch = std::string(*name);
            if (std::ranges::none_of(branches, [&](const branch &b) { return b.name == head_branch; }))
                return fail(std::format("the head is on the branch \"{}\", which does not exist", printable(head_branch)));
        }
        else if (const auto id = head_value.at("snapshot").as_string(); id && is_object_id(*id))
            head_snapshot = std::string(*id);
        else
            return fail("\"head\" should name a branch or a snapshot");

        m_head_branch = std::move(head_branch);
        m_head_snapshot = std::move(head_snapshot);
        m_head_extra.clear();
        for (const json::member &m : *head_value.as_object())
            if (!is_one_of(m.key, head_keys))
                m_head_extra.set(m.key, m.value);
        m_branches = std::move(branches);
        m_extra.clear();
        for (const json::member &m : *root)
            if (!is_one_of(m.key, history_keys))
                m_extra.set(m.key, m.value);
        return {};
    }

    std::expected<void, std::string> repository::commit(std::string head_branch, std::string head_snapshot,
                                                         std::vector<branch> branches)
    {
        json::object o;
        o.set("format", json::value(history_format));
        o.set("format_version", json::value(history_format_version));
        json::object head_object;
        if (!head_branch.empty())
            head_object.set("branch", json::value(head_branch));
        else
            head_object.set("snapshot", json::value(head_snapshot));
        for (const json::member &m : m_head_extra)
            if (!is_one_of(m.key, head_keys))
                head_object.set(m.key, m.value);
        o.set("head", json::value(std::move(head_object)));
        json::array array;
        for (const branch &b : branches)
        {
            json::object bo;
            bo.set("name", json::value(b.name));
            if (!b.snapshot.empty())
                bo.set("snapshot", json::value(b.snapshot));
            for (const json::member &m : b.extra)
                if (!is_one_of(m.key, branch_keys))
                    bo.set(m.key, m.value);
            array.emplace_back(std::move(bo));
        }
        o.set("branches", json::value(std::move(array)));
        for (const json::member &m : m_extra)
            if (!is_one_of(m.key, history_keys))
                o.set(m.key, m.value);

        const fs::path file = m_directory / detail::path_from_utf8(history_file_name);
        if (auto written = detail::write_atomically(file, json::write(json::value(std::move(o)))); !written)
            return written;
        m_head_branch = std::move(head_branch);
        m_head_snapshot = m_head_branch.empty() ? std::move(head_snapshot) : std::string();
        m_branches = std::move(branches);
        return {};
    }

    // ---- branches and the head -------------------------------------------------------

    head_state repository::head() const
    {
        if (m_head_branch.empty())
            return {{}, m_head_snapshot};
        const branch *b = find_branch(m_head_branch);
        return {m_head_branch, b != nullptr ? b->snapshot : std::string()};
    }

    const branch *repository::find_branch(std::string_view name) const noexcept
    {
        for (const branch &b : m_branches)
            if (b.name == name)
                return &b;
        return nullptr;
    }

    std::expected<void, std::string> repository::create_branch(std::string_view name, std::string_view snapshot_id)
    {
        if (auto checked = check_branch_name(name); !checked)
            return checked;
        if (find_branch(name) != nullptr)
            return std::unexpected(std::format("there is a branch named \"{}\" already", name));
        std::string at = snapshot_id.empty() ? head().snapshot : std::string(snapshot_id);
        if (!at.empty() && !has_snapshot(at))
            return std::unexpected(std::format("there is no snapshot {}", printable(at)));
        std::vector<branch> branches = m_branches;
        branches.push_back(branch{std::string(name), std::move(at), {}});
        return commit(m_head_branch, m_head_snapshot, std::move(branches));
    }

    std::expected<void, std::string> repository::switch_branch(std::string_view name)
    {
        if (find_branch(name) == nullptr)
            return std::unexpected(std::format("there is no branch named \"{}\"", printable(name)));
        return commit(std::string(name), {}, m_branches);
    }

    std::expected<void, std::string> repository::detach(std::string_view snapshot_id)
    {
        if (!has_snapshot(snapshot_id))
            return std::unexpected(std::format("there is no snapshot {}", printable(snapshot_id)));
        return commit({}, std::string(snapshot_id), m_branches);
    }

    std::expected<void, std::string> repository::rename_branch(std::string_view name, std::string_view new_name)
    {
        if (find_branch(name) == nullptr)
            return std::unexpected(std::format("there is no branch named \"{}\"", printable(name)));
        if (auto checked = check_branch_name(new_name); !checked)
            return checked;
        if (name == new_name)
            return {};
        if (find_branch(new_name) != nullptr)
            return std::unexpected(std::format("there is a branch named \"{}\" already", new_name));
        std::vector<branch> branches = m_branches;
        for (branch &b : branches)
            if (b.name == name)
                b.name = std::string(new_name);
        const std::string head_branch = m_head_branch == name ? std::string(new_name) : m_head_branch;
        return commit(head_branch, m_head_snapshot, std::move(branches));
    }

    std::expected<void, std::string> repository::delete_branch(std::string_view name)
    {
        if (find_branch(name) == nullptr)
            return std::unexpected(std::format("there is no branch named \"{}\"", printable(name)));
        if (m_head_branch == name)
            return std::unexpected(
                std::format("\"{}\" is the current branch; switch to another before deleting it", name));
        std::vector<branch> branches = m_branches;
        std::erase_if(branches, [&](const branch &b) { return b.name == name; });
        return commit(m_head_branch, m_head_snapshot, std::move(branches));
    }

    // ---- snapshots -------------------------------------------------------------------

    std::expected<taken_snapshot, std::string> repository::take_snapshot(std::span<const file_content> files,
                                                                         const snapshot_options &options)
    {
        auto prepared = prepare(files);
        if (!prepared)
            return std::unexpected(prepared.error());

        snapshot s;
        s.files.reserve(prepared->size());
        for (const prepared_file &p : *prepared)
            s.files.push_back(p.entry);

        const head_state current = head();
        std::vector<snapshot_file> parent_files;
        if (!current.snapshot.empty())
        {
            auto parent = read_snapshot(current.snapshot);
            if (!parent)
                return std::unexpected(std::format("the head's snapshot cannot be read: {}", parent.error()));
            parent_files = std::move(parent->files);
            s.parents.push_back(current.snapshot);
        }
        if (same_files(parent_files, s.files) && !options.allow_unchanged)
            return taken_snapshot{current.snapshot, false};

        s.message = options.message;
        s.author = options.author;
        s.time = format_time(m_clock ? m_clock() : system_now());

        // The blobs, then the snapshot, then history.json: until the last
        // is written, what is new is named by nothing.
        std::set<std::string_view> stored;
        for (const prepared_file &p : *prepared)
        {
            if (!stored.insert(p.entry.blob).second)
                continue;
            if (auto written = store_object(object_file(m_directory, blobs_directory, p.entry.blob), p.bytes); !written)
                return std::unexpected(written.error());
        }
        const std::string text = snapshot_text(s);
        const std::string id = sha256_hex(text);
        if (auto written = store_object(object_file(m_directory, snapshots_directory, id), text); !written)
            return std::unexpected(written.error());

        if (current.detached())
        {
            if (auto committed = commit({}, id, m_branches); !committed)
                return std::unexpected(committed.error());
        }
        else
        {
            std::vector<branch> branches = m_branches;
            for (branch &b : branches)
                if (b.name == current.branch)
                    b.snapshot = id;
            if (auto committed = commit(current.branch, {}, std::move(branches)); !committed)
                return std::unexpected(committed.error());
        }
        return taken_snapshot{id, true};
    }

    bool repository::has_snapshot(std::string_view id) const
    {
        std::error_code error;
        return is_object_id(id) && fs::is_regular_file(object_file(m_directory, snapshots_directory, id), error);
    }

    std::expected<snapshot, std::string> repository::read_snapshot(std::string_view id) const
    {
        if (!is_object_id(id))
            return std::unexpected(std::format("\"{}\" is not a snapshot id", printable(id)));
        const fs::path file = object_file(m_directory, snapshots_directory, id);
        auto text = load_object(file, "snapshot", id);
        if (!text)
            return std::unexpected(std::move(text.error().message));
        auto s = parse_snapshot(*text);
        if (!s)
            return std::unexpected(std::format("snapshot {} is damaged: {} ({})", short_id(id), s.error(),
                                               detail::path_to_utf8(file)));
        s->id = std::string(id);
        return s;
    }

    std::expected<std::string, std::string> repository::read_blob(std::string_view id) const
    {
        if (!is_object_id(id))
            return std::unexpected(std::format("\"{}\" is not a blob id", printable(id)));
        auto bytes = load_object(object_file(m_directory, blobs_directory, id), "blob", id);
        if (!bytes)
            return std::unexpected(std::move(bytes.error().message));
        return std::move(*bytes);
    }

    std::expected<std::string, std::string> repository::read_file(std::string_view snapshot_id,
                                                                  std::string_view path) const
    {
        auto s = read_snapshot(snapshot_id);
        if (!s)
            return std::unexpected(s.error());
        const snapshot_file *f = s->find(path);
        if (f == nullptr)
            return std::unexpected(
                std::format("snapshot {} has no file \"{}\"", short_id(snapshot_id), printable(path)));
        return read_blob(f->blob);
    }

    std::expected<std::vector<snapshot>, std::string> repository::log(std::string_view from, std::size_t limit) const
    {
        std::vector<snapshot> snapshots;
        std::string id = from.empty() ? head().snapshot : std::string(from);
        while (!id.empty() && snapshots.size() < limit)
        {
            auto s = read_snapshot(id);
            if (!s)
                return std::unexpected(s.error());
            id = s->parents.empty() ? std::string() : s->parents.front();
            snapshots.push_back(std::move(*s));
        }
        return snapshots;
    }

    std::expected<std::string, std::string> repository::resolve(std::string_view prefix) const
    {
        std::string wanted;
        for (const char c : prefix)
            wanted += c >= 'A' && c <= 'F' ? static_cast<char>(c + 32) : c;
        const bool hex = std::ranges::all_of(wanted, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
        if (!hex || wanted.size() < 4 || wanted.size() > 64)
            return std::unexpected(
                std::format("\"{}\" is not a snapshot id (4 to 64 hexadecimal digits)", printable(prefix)));
        if (wanted.size() == 64)
        {
            if (!has_snapshot(wanted))
                return std::unexpected(std::format("there is no snapshot {}", wanted));
            return wanted;
        }

        const fs::path directory =
            m_directory / detail::path_from_utf8(snapshots_directory) / detail::path_from_utf8(wanted.substr(0, 2));
        std::vector<std::string> found;
        std::error_code error;
        for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
        {
            const std::string id = wanted.substr(0, 2) + detail::path_to_utf8(it->path().filename());
            if (is_object_id(id) && id.starts_with(wanted))
                found.push_back(id);
        }
        if (found.empty())
            return std::unexpected(std::format("no snapshot's id starts with {}", wanted));
        if (found.size() > 1)
            return std::unexpected(std::format("{} snapshots have ids starting with {}; give more digits", found.size(), wanted));
        return found.front();
    }

    // ---- comparing -------------------------------------------------------------------

    std::expected<std::vector<file_change>, std::string> repository::changes_between(std::string_view from_id,
                                                                                     std::string_view to_id) const
    {
        std::vector<snapshot_file> from;
        std::vector<snapshot_file> to;
        if (!from_id.empty())
        {
            auto s = read_snapshot(from_id);
            if (!s)
                return std::unexpected(s.error());
            from = std::move(s->files);
        }
        if (!to_id.empty())
        {
            auto s = read_snapshot(to_id);
            if (!s)
                return std::unexpected(s.error());
            to = std::move(s->files);
        }
        return differences(from, to);
    }

    std::expected<std::vector<file_change>, std::string> repository::changes_since(std::string_view from_id,
                                                                                   std::span<const file_content> files) const
    {
        auto prepared = prepare(files);
        if (!prepared)
            return std::unexpected(prepared.error());
        std::vector<snapshot_file> to;
        to.reserve(prepared->size());
        for (prepared_file &p : *prepared)
            to.push_back(std::move(p.entry));
        std::vector<snapshot_file> from;
        if (!from_id.empty())
        {
            auto s = read_snapshot(from_id);
            if (!s)
                return std::unexpected(s.error());
            from = std::move(s->files);
        }
        return differences(from, to);
    }

    // ---- verification ----------------------------------------------------------------

    std::vector<verify_problem> repository::verify() const
    {
        std::vector<verify_problem> problems;

        // Snapshots to read, with what names them for the message.
        std::vector<std::pair<std::string, std::string>> pending;
        for (const branch &b : m_branches)
            if (!b.snapshot.empty())
                pending.emplace_back(b.snapshot, std::format("the branch \"{}\" points to it", b.name));
        if (m_head_branch.empty())
            pending.emplace_back(m_head_snapshot, "the head is at it");

        std::set<std::string> seen;
        // A blob's size once read; nullopt when it is missing or damaged
        // (and was reported).
        std::map<std::string, std::optional<std::uint64_t>> blobs;
        while (!pending.empty())
        {
            auto [id, named_by] = std::move(pending.back());
            pending.pop_back();
            if (!seen.insert(id).second)
                continue;

            const fs::path file = object_file(m_directory, snapshots_directory, id);
            auto text = load_object(file, "snapshot", id);
            if (!text)
            {
                problems.push_back({id, text.error().missing, std::format("{}; {}", text.error().message, named_by)});
                continue;
            }
            auto s = parse_snapshot(*text);
            if (!s)
            {
                problems.push_back({id, false,
                                    std::format("snapshot {} is damaged: {} ({}); {}", short_id(id), s.error(),
                                                detail::path_to_utf8(file), named_by)});
                continue;
            }

            for (const std::string &parent : s->parents)
                pending.emplace_back(parent, std::format("it is a parent of snapshot {}", short_id(id)));

            for (const snapshot_file &f : s->files)
            {
                auto known = blobs.find(f.blob);
                if (known == blobs.end())
                {
                    auto bytes = load_object(object_file(m_directory, blobs_directory, f.blob), "blob", f.blob);
                    if (!bytes)
                    {
                        problems.push_back({f.blob, bytes.error().missing,
                                            std::format("{}; it is \"{}\" in snapshot {}", bytes.error().message,
                                                        f.path, short_id(id))});
                        known = blobs.emplace(f.blob, std::nullopt).first;
                    }
                    else
                        known = blobs.emplace(f.blob, bytes->size()).first;
                }
                if (known->second && *known->second != f.size)
                    problems.push_back({id, false,
                                        std::format("snapshot {} gives \"{}\" as {} bytes, but its blob {} has {}",
                                                    short_id(id), f.path, f.size, short_id(f.blob), *known->second)});
            }
        }
        return problems;
    }

} // namespace volt::history
