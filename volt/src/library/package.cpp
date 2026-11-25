#include <library/package.hpp>

#include <library/project.hpp>

#include <algorithm>
#include <format>

namespace volt::library
{

    namespace
    {
        void put(json::object &o, std::string_view key, const std::string &text)
        {
            if (!text.empty())
                o.set(key, json::value(text));
        }

        constexpr std::string_view known_keys[] = {"format", "format_version", "generator", "parts", "files"};

        bool is_known(std::string_view key) noexcept
        {
            return std::ranges::find(known_keys, key) != std::end(known_keys);
        }

        // One entry of "files": data as in a library, or a path to read.
        std::optional<embedded_file> read_entry(const json::value &entry, const std::filesystem::path &directory,
                                                std::string &problem)
        {
            const auto stored = entry.at("path").as_string();
            if (!stored || entry.find("data") != nullptr)
                return file_from_json(entry, problem);
            if (directory.empty())
            {
                problem = std::format("the file \"{}\" is given by a path, and there is no directory to read it from",
                                      *stored);
                return std::nullopt;
            }
            const std::filesystem::path path = resolve_path(directory, *stored);
            auto file = read_file(path, std::string(entry.at("name").string_or()));
            if (!file)
            {
                problem = std::format("the file \"{}\" could not be read: {}", *stored, file.error());
                return std::nullopt;
            }
            for (const json::member &m : *entry.as_object())
                if (m.key != "name" && m.key != "path")
                    file->extra.set(m.key, m.value);
            return std::move(*file);
        }

        std::string plural(std::size_t count, std::string_view one)
        {
            return std::format("{} {}{}", count, one, count == 1 ? "" : "s");
        }
    } // namespace

    // ---- reading -----------------------------------------------------------------------

    std::expected<loaded_package, std::string> parse_package(std::string_view text,
                                                             const std::filesystem::path &directory)
    {
        auto document = json::parse(text);
        if (!document)
            return std::unexpected(document.error().message());
        const json::object *root = document->as_object();
        if (root == nullptr)
            return std::unexpected(std::format("not a Volt part package: the file holds {}, not an object",
                                               json::describe(document->type())));
        const std::string_view format = document->at("format").string_or();

        loaded_package result;
        part_package &package = result.package;
        load_report &report = result.report;

        if (format == library_format)
        {
            // A library is a package of its parts and files.
            auto loaded = parse_library(text, "library");
            if (!loaded)
                return std::unexpected(std::move(loaded.error()));
            package.generator = std::string(loaded->library.display_name());
            package.parts = std::move(loaded->library.parts);
            package.files = std::move(loaded->library.files);
            report = std::move(loaded->report);
            return result;
        }
        if (format != package_format)
            return std::unexpected(std::format("not a Volt part package: \"format\" should be \"{}\" (or \"{}\")",
                                               package_format, library_format));

        if (const json::value *version = root->find("format_version"))
        {
            const auto number = version->as_integer();
            if (!number || *number < 1)
                return std::unexpected("\"format_version\" should be a whole number from 1");
            if (*number > package_format_version)
                report.add(std::format("written in format version {}, newer than this Volt's {}: fields it does not "
                                       "know are kept but not used",
                                       *number, package_format_version));
        }
        package.generator = std::string(document->at("generator").string_or());
        for (const json::member &m : *root)
            if (!is_known(m.key))
                package.extra.set(m.key, m.value);

        const json::value *parts = root->find("parts");
        if (parts == nullptr)
            report.add("no \"parts\"; the package is empty");
        else if (!parts->is_array())
            return std::unexpected(std::format("\"parts\" should be an array, not {}", json::describe(parts->type())));
        else
        {
            const json::array &array = *parts->as_array();
            std::vector<std::string> messages;
            for (std::size_t i = 0; i < array.size(); ++i)
            {
                const json::object *o = array[i].as_object();
                if (o == nullptr)
                {
                    report.add(i, {}, std::format("should be an object, not {}", json::describe(array[i].type())),
                               true);
                    ++report.parts_skipped;
                    continue;
                }
                messages.clear();
                part p = part_from_json(*o, messages);
                for (std::string &message : messages)
                    report.add(i, p.id, std::move(message));
                package.parts.push_back(std::move(p));
                ++report.parts_loaded;
            }
        }

        if (const json::value *files = root->find("files"))
        {
            if (const json::array *entries = files->as_array())
            {
                for (const json::value &entry : *entries)
                {
                    std::string problem;
                    auto file = read_entry(entry, directory, problem);
                    if (!file)
                        report.add(problem + "; left out");
                    else if (std::ranges::any_of(package.files,
                                                 [&](const embedded_file &f) { return f.name == file->name; }))
                        report.add(std::format("the file \"{}\" is there twice; the first is kept", file->name));
                    else
                        package.files.push_back(std::move(*file));
                }
            }
            else if (!files->is_null())
                report.add(std::format("\"files\" should be an array, not {}; ignored", json::describe(files->type())));
        }
        return result;
    }

    std::expected<loaded_package, std::string> load_package(const std::filesystem::path &path)
    {
        auto bytes = json::read_text_file(path);
        if (!bytes)
            return std::unexpected(std::move(bytes.error()));
        std::error_code error;
        std::filesystem::path directory = std::filesystem::absolute(path, error).parent_path();
        if (error)
            directory = path.parent_path();
        auto result = parse_package(*bytes, directory);
        if (!result)
            return std::unexpected(path_to_utf8(path.filename()) + ": " + result.error());
        for (load_report::problem &p : result->report.problems)
            p.file = path;
        return result;
    }

    // ---- writing -----------------------------------------------------------------------

    std::string write_package(const part_package &package)
    {
        json::object o;
        o.set("format", json::value(package_format));
        o.set("format_version", json::value(package_format_version));
        put(o, "generator", package.generator);
        for (const json::member &m : package.extra)
            if (!is_known(m.key))
                o.set(m.key, m.value);
        json::array parts;
        parts.reserve(package.parts.size());
        for (const part &p : package.parts)
            parts.push_back(part_to_json(p));
        o.set("parts", json::value(std::move(parts)));
        if (!package.files.empty())
        {
            json::array files;
            files.reserve(package.files.size());
            for (const embedded_file &f : package.files)
                files.push_back(file_to_json(f));
            o.set("files", json::value(std::move(files)));
        }
        return json::write(json::value(std::move(o)));
    }

    std::expected<void, std::string> save_package(const std::filesystem::path &path, const part_package &package)
    {
        return json::write_text_file(path, write_package(package));
    }

    std::expected<part_package, std::string> export_part(const library &lib, std::string_view id)
    {
        const part *p = lib.find(id);
        if (p == nullptr)
            return std::unexpected(std::format("the library {} has no part \"{}\"", lib.name, id));
        part_package package;
        package.generator = "Volt";
        package.parts.push_back(*p);
        if (const embedded_file *model = lib.find_file(p->model))
            package.files.push_back(*model);
        return package;
    }

    // ---- importing ---------------------------------------------------------------------

    std::string_view describe(import_action action) noexcept
    {
        switch (action)
        {
        case import_action::added:
            return "added";
        case import_action::replaced:
            return "replaced";
        case import_action::unchanged:
            return "unchanged";
        case import_action::skipped:
            return "skipped";
        case import_action::rejected:
            return "rejected";
        }
        return "unknown";
    }

    import_report import_parts(library &target, const part_package &package, const schema &rules,
                               const import_options &options)
    {
        import_report report;
        report.parts.reserve(package.parts.size());
        for (std::size_t i = 0; i < package.parts.size(); ++i)
        {
            part p = package.parts[i];
            imported_part entry;
            entry.index = i;
            // A part that comes without an id is the one the library has
            // of the same manufacturer and MPN, if it has one; otherwise
            // it is new, and is given an id when it goes in.
            if (p.id.empty() && !p.manufacturer.empty() && !p.mpn.empty())
            {
                const auto same = std::ranges::find_if(target.parts, [&](const part &other) {
                    return other.manufacturer == p.manufacturer && other.mpn == p.mpn;
                });
                if (same != target.parts.end())
                    p.id = same->id;
            }
            entry.id = p.id;
            entry.label = !p.mpn.empty()    ? p.mpn
                          : !p.name.empty() ? p.name
                          : !p.id.empty()   ? p.id
                                            : std::format("part {}", i);

            if (options.normalize)
                rules.normalize(p);
            entry.issues = rules.check(p);
            for (const issue &found : entry.issues)
            {
                report.errors += found.level == severity::error ? 1 : 0;
                report.warnings += found.level == severity::warning ? 1 : 0;
            }

            // The files go in first, since they decide the names the part
            // uses; they come out again if the part does not go in.
            const std::size_t files_before = target.files.size();
            const auto reject = [&](import_action action, std::string reason) {
                target.files.erase(target.files.begin() + static_cast<std::ptrdiff_t>(files_before),
                                   target.files.end());
                entry.action = action;
                entry.reason = std::move(reason);
            };

            if (options.strict && worst(entry.issues) == severity::error)
                reject(import_action::rejected, "it has errors, and the import is strict");
            else
            {
                carry_files(target, package.files, p);
                const part *existing = p.id.empty() ? nullptr : target.find(p.id);
                if (existing != nullptr && part_hash(*existing) == part_hash(p) && existing->origin == p.origin)
                    reject(import_action::unchanged, {});
                else if (existing != nullptr && options.conflict == on_conflict::skip)
                    reject(import_action::skipped,
                           "the library already has this part with other contents, which are kept");
                else if (auto added = add_part(target, std::move(p), options.conflict); !added)
                    reject(import_action::rejected, std::move(added.error()));
                else
                {
                    entry.id = added->id;
                    entry.action =
                        added->action == add_action::replaced ? import_action::replaced : import_action::added;
                }
            }

            switch (entry.action)
            {
            case import_action::added:
                ++report.added;
                break;
            case import_action::replaced:
                ++report.replaced;
                break;
            case import_action::unchanged:
                ++report.unchanged;
                break;
            case import_action::skipped:
                ++report.skipped;
                break;
            case import_action::rejected:
                ++report.rejected;
                break;
            }
            report.parts.push_back(std::move(entry));
        }
        return report;
    }

    std::string import_report::summary() const
    {
        std::string text = plural(parts.size(), "part") + ":";
        auto count = [&, first = true](std::size_t n, std::string_view what) mutable {
            if (n == 0)
                return;
            text += std::format("{} {} {}", first ? "" : ",", n, what);
            first = false;
        };
        count(added, "added");
        count(replaced, "replaced");
        count(unchanged, "unchanged");
        count(skipped, "skipped");
        count(rejected, "rejected");
        if (parts.empty())
            text += " nothing to import";
        if (errors + warnings > 0)
            text += std::format("; {}, {}", plural(errors, "error"), plural(warnings, "warning"));
        for (const imported_part &p : parts)
        {
            if (p.action == import_action::added && p.issues.empty())
                continue;
            text += std::format("\n  {}: {}", p.label, describe(p.action));
            if (!p.reason.empty())
                text += ": " + p.reason;
            for (const issue &found : p.issues)
                text += std::format("\n    {}: {}", describe(found.level), found.message);
        }
        return text;
    }

    json::value import_report_to_json(const import_report &report)
    {
        json::object o;
        o.set("added", json::value(report.added));
        o.set("replaced", json::value(report.replaced));
        o.set("unchanged", json::value(report.unchanged));
        o.set("skipped", json::value(report.skipped));
        o.set("rejected", json::value(report.rejected));
        o.set("errors", json::value(report.errors));
        o.set("warnings", json::value(report.warnings));
        json::array parts;
        for (const imported_part &p : report.parts)
        {
            json::object entry;
            entry.set("index", json::value(p.index));
            entry.set("id", json::value(p.id));
            entry.set("label", json::value(p.label));
            entry.set("action", json::value(describe(p.action)));
            put(entry, "reason", p.reason);
            json::array issues;
            for (const issue &found : p.issues)
                issues.push_back(issue_to_json(found));
            entry.set("issues", json::value(std::move(issues)));
            parts.emplace_back(std::move(entry));
        }
        o.set("parts", json::value(std::move(parts)));
        return json::value(std::move(o));
    }

} // namespace volt::library
