#include <library/project.hpp>

#include <library/edit.hpp>

#include <algorithm>
#include <format>

namespace volt::library
{

    namespace
    {
        constexpr std::string_view known_keys[] = {"format", "format_version", "name",     "sheets",
                                                   "board",  "library",        "schematic"};

        constexpr std::string_view pins_along_key = "pins_connect_along_wires";

        bool is_known(std::string_view key) noexcept
        {
            for (const std::string_view k : known_keys)
                if (k == key)
                    return true;
            return false;
        }

        bool is_space(char c) noexcept
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        // A name usable as a file name everywhere: Windows forbids
        // <>:"/\|?* and control characters, and a trailing dot or space.
        // Anything else, non-ASCII letters included, stays.
        std::string file_stem(std::string_view name)
        {
            std::string stem;
            for (const char c : name)
            {
                const bool bad = static_cast<unsigned char>(c) < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' ||
                                 c == '/' || c == '\\' || c == '|' || c == '?' || c == '*';
                stem += bad ? '_' : c;
            }
            while (!stem.empty() && (stem.back() == '.' || is_space(stem.back())))
                stem.pop_back();
            while (!stem.empty() && is_space(stem.front()))
                stem.erase(stem.begin());
            return stem.empty() ? std::string("project") : stem;
        }

        // A name usable as the project library's name (valid_id).
        std::string library_name(std::string_view name)
        {
            std::string id;
            for (const char c : name)
                id += c == ':' || static_cast<unsigned char>(c) < 0x20 || c == 0x7F ? '_' : c;
            while (!id.empty() && is_space(id.back()))
                id.pop_back();
            while (!id.empty() && is_space(id.front()))
                id.erase(id.begin());
            return id.empty() ? std::string("project") : id;
        }

        std::string stem_of(const std::filesystem::path &path)
        {
            const std::u8string stem = path.stem().u8string();
            return std::string(stem.begin(), stem.end());
        }

        std::string utf8_name(const std::filesystem::path &path)
        {
            const std::u8string name = path.filename().u8string();
            return std::string(name.begin(), name.end());
        }

        json::value project_to_json(const project &p, std::string_view library_file)
        {
            json::object o;
            o.set("format", json::value(project_format));
            o.set("format_version", json::value(project_format_version));
            o.set("name", json::value(p.name));
            json::array sheets;
            for (const sheet &s : p.sheets)
            {
                json::object so;
                so.set("name", json::value(s.name));
                so.set("file", json::value(s.file));
                for (const json::member &m : s.extra)
                    if (m.key != "name" && m.key != "file")
                        so.set(m.key, m.value);
                sheets.emplace_back(std::move(so));
            }
            o.set("sheets", json::value(std::move(sheets)));
            if (!p.board.empty())
                o.set("board", json::value(p.board));
            o.set("library", json::value(library_file));
            // Only what differs from the defaults, so that a project using
            // them has no "schematic" at all.
            if (p.schematic != schematic_settings{})
            {
                json::object so;
                if (p.schematic.pins_connect_along_wires != schematic_settings{}.pins_connect_along_wires)
                    so.set(pins_along_key, json::value(p.schematic.pins_connect_along_wires));
                for (const json::member &m : p.schematic.extra)
                    if (m.key != pins_along_key)
                        so.set(m.key, m.value);
                o.set("schematic", json::value(std::move(so)));
            }
            for (const json::member &m : p.extra)
                if (!is_known(m.key))
                    o.set(m.key, m.value);
            return json::value(std::move(o));
        }

        // The project library's file, named after the project file when the
        // project does not say.
        std::string library_file_of(const project &p, const std::filesystem::path &project_path)
        {
            if (!p.library_file.empty())
                return p.library_file;
            return stem_of(project_path) + std::string(library_extension);
        }

        part_origin origin_of(const catalog &source, part_ref ref)
        {
            const library &lib = source.library_of(ref);
            const part &p = source.get(ref);
            return part_origin{lib.name, p.id, lib.version, part_hash(p)};
        }
    } // namespace

    bool project::operator==(const project &other) const
    {
        return name == other.name && sheets == other.sheets && schematic == other.schematic && board == other.board &&
               library_file == other.library_file && parts == other.parts && extra == other.extra;
    }

    project new_project(std::string_view name)
    {
        const std::string stem = file_stem(name);
        project p;
        p.name = std::string(name);
        p.sheets.push_back({"Main", stem + ".voltsch", {}});
        p.board = stem + ".voltpcb";
        p.library_file = stem + std::string(library_extension);
        p.parts.name = library_name(name);
        p.parts.title = std::string(name);
        p.parts.description = "The parts of the project " + std::string(name);
        return p;
    }

    // ---- reading and writing ---------------------------------------------------------

    std::string write_project(const project &p)
    {
        return json::write(project_to_json(p, p.library_file));
    }

    std::expected<loaded_project, std::string> parse_project(std::string_view text)
    {
        auto document = json::parse(text);
        if (!document)
            return std::unexpected(document.error().message());
        const json::object *root = document->as_object();
        if (root == nullptr)
            return std::unexpected(std::format("not a Volt project: the file holds {}, not an object",
                                               json::describe(document->type())));
        const auto format = document->at("format").as_string();
        if (!format || *format != project_format)
            return std::unexpected(std::format("not a Volt project: \"format\" should be \"{}\"", project_format));

        loaded_project result;
        project &p = result.project;
        load_report &report = result.report;
        const auto problem = [&](std::string message) { report.add(std::move(message)); };

        if (const json::value *version = root->find("format_version"))
        {
            const auto number = version->as_integer();
            if (!number || *number < 1)
                return std::unexpected("\"format_version\" should be a whole number from 1");
            p.format_version = static_cast<int>(std::min<std::int64_t>(*number, 1'000'000));
            if (p.format_version > project_format_version)
                problem(std::format("written in format version {}, newer than this Volt's {}: fields it does not "
                                    "know are kept but not used",
                                    p.format_version, project_format_version));
        }
        else
            problem(std::format("no \"format_version\"; read as version {}", project_format_version));

        const auto read = [&](const json::value &v, std::string_view key, std::string &out) {
            if (const auto s = v.as_string())
                out = std::string(*s);
            else if (!v.is_null())
                problem(std::format("\"{}\" should be a string, not {}; ignored", key, json::describe(v.type())));
        };
        read(document->at("name"), "name", p.name);
        read(document->at("board"), "board", p.board);
        read(document->at("library"), "library", p.library_file);

        const json::value &sheets = document->at("sheets");
        if (const json::array *array = sheets.as_array())
        {
            for (std::size_t i = 0; i < array->size(); ++i)
            {
                const json::object *so = (*array)[i].as_object();
                if (so == nullptr)
                {
                    problem(std::format("sheet {} should be an object; ignored", i));
                    continue;
                }
                sheet s;
                read((*array)[i].at("name"), "sheets[].name", s.name);
                read((*array)[i].at("file"), "sheets[].file", s.file);
                for (const json::member &m : *so)
                    if (m.key != "name" && m.key != "file")
                        s.extra.set(m.key, m.value);
                p.sheets.push_back(std::move(s));
            }
        }
        else if (!sheets.is_null())
            problem(std::format("\"sheets\" should be an array, not {}; ignored", json::describe(sheets.type())));

        const json::value &settings = document->at("schematic");
        if (const json::object *so = settings.as_object())
        {
            for (const json::member &m : *so)
            {
                if (m.key != pins_along_key)
                {
                    p.schematic.extra.set(m.key, m.value);
                    continue;
                }
                if (const auto on = m.value.as_bool())
                    p.schematic.pins_connect_along_wires = *on;
                else
                    problem(std::format("\"schematic.{}\" should be true or false, not {}; ignored", pins_along_key,
                                        json::describe(m.value.type())));
            }
        }
        else if (!settings.is_null())
            problem(std::format("\"schematic\" should be an object, not {}; ignored", json::describe(settings.type())));

        for (const json::member &m : *root)
            if (!is_known(m.key))
                p.extra.set(m.key, m.value);
        return result;
    }

    std::expected<loaded_project, std::string> load_project(const std::filesystem::path &path)
    {
        auto bytes = json::read_text_file(path);
        if (!bytes)
            return std::unexpected(std::move(bytes.error()));
        auto result = parse_project(*bytes);
        if (!result)
            return std::unexpected(utf8_name(path) + ": " + result.error());
        for (load_report::problem &problem : result->report.problems)
            problem.file = path;

        project &p = result->project;
        if (p.name.empty())
            p.name = stem_of(path);
        if (p.library_file.empty())
        {
            p.library_file = library_file_of(p, path);
            result->report.add(std::format("no \"library\"; using {}", p.library_file));
            result->report.problems.back().file = path;
        }

        const std::filesystem::path directory = path.parent_path();
        const std::filesystem::path library_path = resolve_path(directory, p.library_file);
        std::error_code error;
        if (std::filesystem::exists(library_path, error))
        {
            auto loaded = load_library(library_path);
            if (!loaded)
                return std::unexpected(loaded.error());
            p.parts = std::move(loaded->library);
            result->report.merge(loaded->report);
        }
        else
        {
            result->report.add(std::format("the project library {} does not exist; starting with an empty one",
                                           p.library_file));
            result->report.problems.back().file = path;
            p.parts = new_project(p.name).parts;
            p.parts.path = library_path;
        }
        return result;
    }

    std::expected<void, std::string> save_project(const std::filesystem::path &path, const project &p)
    {
        const std::string library_file = library_file_of(p, path);
        const std::filesystem::path library_path = resolve_path(path.parent_path(), library_file);
        std::error_code error;
        if (library_path.has_parent_path())
            std::filesystem::create_directories(library_path.parent_path(), error);
        if (auto saved = save_library(library_path, p.parts); !saved)
            return saved;
        return json::write_text_file(path, json::write(project_to_json(p, library_file)));
    }

    // ---- the project library ---------------------------------------------------------

    std::expected<std::string, std::string> add_part(project &p, const catalog &source, part_ref ref)
    {
        const part_origin origin = origin_of(source, ref);
        for (const part &existing : p.parts.parts)
            if (existing.origin && existing.origin->library == origin.library && existing.origin->part == origin.part)
                return existing.id;

        part copy = source.get(ref);
        copy.origin = origin;
        // With the files it names, so that the project stays whole.
        carry_files(p.parts, source.library_of(ref).files, copy);
        if (p.parts.find(copy.id) != nullptr)
        {
            const std::string base = copy.id;
            for (int n = 2;; ++n)
            {
                copy.id = std::format("{}-{}", base, n);
                if (p.parts.find(copy.id) == nullptr)
                    break;
            }
        }
        p.parts.parts.push_back(std::move(copy));
        return p.parts.parts.back().id;
    }

    bool remove_part(project &p, std::string_view id)
    {
        std::vector<part> &parts = p.parts.parts;
        for (auto it = parts.begin(); it != parts.end(); ++it)
        {
            if (it->id == id)
            {
                parts.erase(it);
                return true;
            }
        }
        return false;
    }

    std::string_view describe(part_status status) noexcept
    {
        switch (status)
        {
        case part_status::unlinked:
            return "not from a library";
        case part_status::source_missing:
            return "its source is not in the catalog";
        case part_status::up_to_date:
            return "the same as its source";
        case part_status::source_changed:
            return "its source has changed";
        case part_status::locally_modified:
            return "changed in the project";
        case part_status::diverged:
            return "changed in the project and in its source";
        }
        return "unknown";
    }

    part_status status_of(const part &copy, const catalog &source)
    {
        if (!copy.origin)
            return part_status::unlinked;
        const auto ref = source.find(copy.origin->library, copy.origin->part);
        if (!ref)
            return part_status::source_missing;
        const std::string source_hash = part_hash(source.get(*ref));
        const std::string copy_hash = part_hash(copy);
        if (copy_hash == source_hash)
            return part_status::up_to_date;
        const bool source_changed = source_hash != copy.origin->hash;
        const bool copy_changed = copy_hash != copy.origin->hash;
        if (source_changed && copy_changed)
            return part_status::diverged;
        return source_changed ? part_status::source_changed : part_status::locally_modified;
    }

    std::optional<part_status> status_of(const project &p, std::string_view id, const catalog &source)
    {
        const part *copy = p.parts.find(id);
        if (copy == nullptr)
            return std::nullopt;
        return status_of(*copy, source);
    }

    std::expected<void, std::string> update_from_source(project &p, std::string_view id, const catalog &source)
    {
        part *copy = p.parts.find(id);
        if (copy == nullptr)
            return std::unexpected(std::format("the project has no part \"{}\"", id));
        if (!copy->origin)
            return std::unexpected(std::format("\"{}\" was not copied from a library", id));
        const auto ref = source.find(copy->origin->library, copy->origin->part);
        if (!ref)
            return std::unexpected(std::format("\"{}\" comes from {}:{}, which is not in the catalog", id,
                                               copy->origin->library, copy->origin->part));
        std::string local_id = std::move(copy->id);
        const std::string old_model = std::move(copy->model);
        *copy = source.get(*ref);
        copy->id = std::move(local_id);
        copy->origin = origin_of(source, *ref);
        // The file the old copy brought goes, unless another part names
        // it, so that the source's file takes its place under its own name
        // (and the copy is the same as its source again).
        const bool shared = std::ranges::any_of(
            p.parts.parts, [&](const part &other) { return &other != copy && other.model == old_model; });
        if (!shared)
            std::erase_if(p.parts.files, [&](const embedded_file &f) { return f.name == old_model; });
        carry_files(p.parts, source.library_of(*ref).files, *copy);
        return {};
    }

    // ---- paths -------------------------------------------------------------------------

    std::filesystem::path path_from_utf8(std::string_view text)
    {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    std::string path_to_utf8(const std::filesystem::path &path)
    {
        const std::u8string text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }

    std::string relative_path(const std::filesystem::path &directory, const std::filesystem::path &target)
    {
        std::error_code error;
        const std::filesystem::path base = std::filesystem::absolute(directory, error).lexically_normal();
        const std::filesystem::path full = std::filesystem::absolute(target, error).lexically_normal();
        const std::filesystem::path relative = full.lexically_relative(base);
        if (relative.empty())
            return path_to_utf8(full);
        return path_to_utf8(relative);
    }

    std::filesystem::path resolve_path(const std::filesystem::path &directory, std::string_view stored)
    {
        const std::filesystem::path path = path_from_utf8(stored);
        if (path.is_absolute())
            return path;
        return (directory / path).lexically_normal();
    }

} // namespace volt::library
