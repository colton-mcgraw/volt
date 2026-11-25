// volt_parts: the parts library from the command line, for people and for
// the tools that bring parts in from elsewhere.
//
//   volt_parts schema [--json]
//       The parameters and devices Volt knows: what to call things so that
//       a simulation finds them.
//
//   volt_parts schema --resolve=<name>... [--device=<id>] [--json]
//       How Volt would read a parameter name on a part of that device (or
//       of none): the parameter it is, or what it could be when it is not
//       read as anything. For a tool to ask before it writes a part,
//       rather than copy the rules. Exit status 1 when a name is not read
//       as a parameter.
//
//   volt_parts check <file>... [--json]
//       Checks the parts of .voltpart and .voltlib files against the schema.
//
//   volt_parts import <library.voltlib> <package>... [options] [--json]
//       Puts the parts of the packages (.voltpart, or other .voltlib files)
//       into the library and saves it.
//         --create              make the library if the file does not exist
//         --title=<text>        the title of a library made by --create
//         --conflict=<what>     a part whose id the library has already:
//                               skip (default), replace, rename, fail
//         --strict              leave out parts that have errors
//         --dry-run             report, but do not save
//
//   volt_parts export <library.voltlib> <part id> <file.voltpart>
//       Writes one part, with the files it names, as a package.
//
// With --json the report goes to standard output as one JSON value, for a
// program to read. The exit status is 0 when all went well, 1 when
// something could not be done or a part has errors, 2 for a command line
// that makes no sense.

#include <library/package.hpp>
#include <library/project.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace volt;

namespace
{
    struct arguments
    {
        std::vector<std::string> positional;
        bool json = false;
        bool create = false;
        bool strict = false;
        bool dry_run = false;
        std::string title;
        std::string device;
        std::vector<std::string> resolve;
        library::on_conflict conflict = library::on_conflict::skip;
        std::string error;
    };

    arguments parse_arguments(int argc, char **argv)
    {
        arguments a;
        for (int i = 2; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            if (arg == "--json")
                a.json = true;
            else if (arg == "--create")
                a.create = true;
            else if (arg == "--strict")
                a.strict = true;
            else if (arg == "--dry-run")
                a.dry_run = true;
            else if (arg.starts_with("--title="))
                a.title = std::string(arg.substr(8));
            else if (arg.starts_with("--device="))
                a.device = std::string(arg.substr(9));
            else if (arg.starts_with("--resolve="))
                a.resolve.emplace_back(arg.substr(10));
            else if (arg.starts_with("--conflict="))
            {
                const std::string_view what = arg.substr(11);
                if (what == "skip")
                    a.conflict = library::on_conflict::skip;
                else if (what == "replace")
                    a.conflict = library::on_conflict::replace;
                else if (what == "rename")
                    a.conflict = library::on_conflict::rename;
                else if (what == "fail")
                    a.conflict = library::on_conflict::fail;
                else
                    a.error = "--conflict is one of skip, replace, rename, fail";
            }
            else if (arg.starts_with("--"))
                a.error = "unknown option " + std::string(arg);
            else
                a.positional.emplace_back(arg);
        }
        return a;
    }

    int usage()
    {
        std::fprintf(stderr, "usage: volt_parts schema [--json]\n"
                             "       volt_parts schema --resolve=<name>... [--device=<id>] [--json]\n"
                             "       volt_parts check <file>... [--json]\n"
                             "       volt_parts import <library.voltlib> <package>... [--create] [--title=<text>]\n"
                             "                  [--conflict=skip|replace|rename|fail] [--strict] [--dry-run] [--json]\n"
                             "       volt_parts export <library.voltlib> <part id> <file.voltpart>\n");
        return 2;
    }

    void print(const json::value &v)
    {
        const std::string text = json::write(v);
        std::fwrite(text.data(), 1, text.size(), stdout);
    }

    json::value problems_to_json(const library::load_report &report)
    {
        json::array problems;
        for (const library::load_report::problem &p : report.problems)
        {
            json::object o;
            if (p.part != library::load_report::problem::no_part)
                o.set("part", json::value(p.part));
            if (!p.id.empty())
                o.set("id", json::value(p.id));
            o.set("message", json::value(p.message));
            if (p.skipped)
                o.set("skipped", json::value(true));
            problems.emplace_back(std::move(o));
        }
        return json::value(std::move(problems));
    }

    void print_problems(const library::load_report &report, const std::string &file)
    {
        for (const library::load_report::problem &p : report.problems)
        {
            if (p.part != library::load_report::problem::no_part)
                std::printf("%s: part %zu%s%s: %s\n", file.c_str(), p.part, p.id.empty() ? "" : " ", p.id.c_str(),
                            p.message.c_str());
            else
                std::printf("%s: %s\n", file.c_str(), p.message.c_str());
        }
    }

    int resolve_command(const arguments &a)
    {
        const library::schema &s = library::schema::builtin();
        bool all_read = true;
        json::array results;
        for (const std::string &name : a.resolve)
        {
            const library::resolution r = s.resolve(name, a.device);
            all_read = all_read && r.definition != nullptr;
            json::object o;
            o.set("name", json::value(name));
            o.set("read_as", json::value(library::describe(r.how)));
            if (r.definition != nullptr)
            {
                o.set("parameter", json::value(r.definition->name));
                o.set("kind", json::value(r.definition->kind == library::value_kind::quantity ? "quantity" : "text"));
                if (!r.definition->unit.empty())
                    o.set("unit", json::value(r.definition->unit));
            }
            json::array candidates;
            for (const std::string &candidate : r.candidates)
                candidates.emplace_back(candidate);
            if (!candidates.empty())
                o.set("candidates", json::value(std::move(candidates)));
            results.emplace_back(std::move(o));

            if (a.json)
                continue;
            std::string could_be;
            for (const std::string &candidate : r.candidates)
                could_be += (could_be.empty() ? "" : ", ") + candidate;
            if (r.definition != nullptr)
                std::printf("%s: %s (%s)\n", name.c_str(), r.definition->name.c_str(),
                            std::string(library::describe(r.how)).c_str());
            else if (r.how == library::resolution::kind::ambiguous)
                std::printf("%s: ambiguous; could be %s\n", name.c_str(), could_be.c_str());
            else if (!could_be.empty())
                std::printf("%s: unknown; close to %s\n", name.c_str(), could_be.c_str());
            else
                std::printf("%s: unknown\n", name.c_str());
        }
        if (a.json)
        {
            json::object root;
            if (!a.device.empty())
                root.set("device", json::value(a.device));
            root.set("names", json::value(std::move(results)));
            print(json::value(std::move(root)));
        }
        return all_read ? 0 : 1;
    }

    int schema_command(const arguments &a)
    {
        if (!a.resolve.empty())
            return resolve_command(a);
        const library::schema &s = library::schema::builtin();
        if (a.json)
        {
            print(library::schema_to_json(s));
            return 0;
        }
        std::printf("Parameters\n");
        for (const library::parameter_definition &d : s.parameters())
        {
            std::string also;
            for (const std::string &alias : d.aliases)
                also += (also.empty() ? "  (also: " : ", ") + alias;
            for (const library::parameter_definition::short_name &n : d.short_names)
            {
                std::string on;
                for (const std::string &device : n.devices)
                    on += (on.empty() ? "" : "/") + device;
                also += (also.empty() ? "  (also: " : ", ") + n.name + " on a " + on;
            }
            if (!also.empty())
                also += ")";
            std::printf("  %-26s %-6s %s%s\n", d.name.c_str(),
                        d.kind == library::value_kind::text ? "text" : d.unit.empty() ? "number" : d.unit.c_str(),
                        d.description.c_str(), also.c_str());
        }
        std::printf("\nDevices\n");
        for (const library::device_definition &d : s.devices())
        {
            std::string wants;
            for (const library::device_parameter &q : d.parameters)
                wants += (wants.empty() ? "" : ", ") + q.name + (q.required ? " (required)" : "");
            std::printf("  %-12s %s%s%s\n", d.id.c_str(), d.description.c_str(), wants.empty() ? "" : ": ",
                        wants.c_str());
        }
        return 0;
    }

    int check_command(const arguments &a)
    {
        if (a.positional.empty())
            return usage();
        const library::schema &s = library::schema::builtin();
        std::size_t errors = 0, warnings = 0, notes = 0, parts = 0;
        bool failed = false;
        json::array files;
        for (const std::string &name : a.positional)
        {
            json::object entry;
            entry.set("file", json::value(name));
            auto loaded = library::load_package(library::path_from_utf8(name));
            if (!loaded)
            {
                failed = true;
                entry.set("error", json::value(loaded.error()));
                if (!a.json)
                    std::printf("%s\n", loaded.error().c_str());
                files.emplace_back(std::move(entry));
                continue;
            }
            if (!a.json)
                print_problems(loaded->report, name);
            entry.set("problems", problems_to_json(loaded->report));
            json::array checked;
            for (std::size_t index = 0; index < loaded->package.parts.size(); ++index)
            {
                const library::part &p = loaded->package.parts[index];
                ++parts;
                const std::vector<library::issue> issues = s.check(p);
                if (issues.empty())
                    continue;
                // What to call it: a part of a package need not have an id.
                const std::string id = !p.mpn.empty()    ? p.mpn
                                       : !p.name.empty() ? p.name
                                       : !p.id.empty()   ? p.id
                                                         : "part " + std::to_string(index);
                json::array list;
                for (const library::issue &i : issues)
                {
                    errors += i.level == library::severity::error ? 1 : 0;
                    warnings += i.level == library::severity::warning ? 1 : 0;
                    notes += i.level == library::severity::note ? 1 : 0;
                    if (!a.json)
                        std::printf("%s: %s: %s: %s\n", name.c_str(), id.c_str(),
                                    std::string(library::describe(i.level)).c_str(), i.message.c_str());
                    list.push_back(library::issue_to_json(i));
                }
                // By position too: ids made from an MPN need not be unique.
                json::object o;
                o.set("index", json::value(index));
                if (!p.id.empty())
                    o.set("id", json::value(p.id));
                o.set("label", json::value(id));
                o.set("issues", json::value(std::move(list)));
                checked.emplace_back(std::move(o));
            }
            entry.set("parts", json::value(std::move(checked)));
            files.emplace_back(std::move(entry));
        }
        if (a.json)
        {
            json::object root;
            root.set("checked", json::value(parts));
            root.set("errors", json::value(errors));
            root.set("warnings", json::value(warnings));
            root.set("notes", json::value(notes));
            root.set("files", json::value(std::move(files)));
            print(json::value(std::move(root)));
        }
        else
            std::printf("%zu part(s) checked: %zu error(s), %zu warning(s), %zu note(s)\n", parts, errors, warnings,
                        notes);
        return failed || errors > 0 ? 1 : 0;
    }

    int import_command(const arguments &a)
    {
        if (a.positional.size() < 2)
            return usage();
        const std::filesystem::path target_path = library::path_from_utf8(a.positional[0]);
        json::object root;
        const auto fail = [&](const std::string &message) {
            if (a.json)
            {
                root.set("error", json::value(message));
                print(json::value(root));
            }
            else
                std::fprintf(stderr, "%s\n", message.c_str());
            return 1;
        };

        library::library target;
        std::error_code error;
        if (std::filesystem::exists(target_path, error))
        {
            auto loaded = library::load_library(target_path);
            if (!loaded)
                return fail(loaded.error());
            target = std::move(loaded->library);
        }
        else if (a.create)
        {
            const std::string name = library::path_to_utf8(target_path.stem());
            if (!library::valid_id(name))
                return fail("\"" + name + "\" cannot be a library's name");
            target = library::new_library(name, a.title);
            target.path = target_path;
        }
        else
            return fail(a.positional[0] + " does not exist; --create makes it");

        library::import_options options;
        options.conflict = a.conflict;
        options.strict = a.strict;
        bool changed = false, failed = false;
        std::size_t errors = 0;
        json::array packages;
        for (std::size_t i = 1; i < a.positional.size(); ++i)
        {
            const std::string &name = a.positional[i];
            json::object entry;
            entry.set("file", json::value(name));
            auto loaded = library::load_package(library::path_from_utf8(name));
            if (!loaded)
            {
                failed = true;
                entry.set("error", json::value(loaded.error()));
                if (!a.json)
                    std::printf("%s\n", loaded.error().c_str());
                packages.emplace_back(std::move(entry));
                continue;
            }
            const library::import_report report =
                library::import_parts(target, loaded->package, library::schema::builtin(), options);
            changed = changed || report.changed();
            errors += report.errors;
            failed = failed || report.rejected > 0;
            if (!a.json)
            {
                print_problems(loaded->report, name);
                std::printf("%s: %s\n", name.c_str(), report.summary().c_str());
            }
            entry.set("problems", problems_to_json(loaded->report));
            entry.set("report", library::import_report_to_json(report));
            packages.emplace_back(std::move(entry));
        }

        bool saved = false;
        if (changed && !a.dry_run)
        {
            if (auto written = library::save_library(target_path, target); !written)
                return fail(written.error());
            saved = true;
        }
        if (a.json)
        {
            root.set("library", json::value(target.name));
            root.set("saved", json::value(saved));
            root.set("packages", json::value(std::move(packages)));
            print(json::value(std::move(root)));
        }
        else
            std::printf("%s %s (%zu parts)\n",
                        saved ? "saved" : a.dry_run && changed ? "dry run: not saved" : "nothing to save in",
                        a.positional[0].c_str(), target.parts.size());
        return failed || errors > 0 ? 1 : 0;
    }

    int export_command(const arguments &a)
    {
        if (a.positional.size() != 3)
            return usage();
        auto loaded = library::load_library(library::path_from_utf8(a.positional[0]));
        if (!loaded)
        {
            std::fprintf(stderr, "%s\n", loaded.error().c_str());
            return 1;
        }
        auto package = library::export_part(loaded->library, a.positional[1]);
        if (!package)
        {
            std::fprintf(stderr, "%s\n", package.error().c_str());
            return 1;
        }
        if (auto saved = library::save_package(library::path_from_utf8(a.positional[2]), *package); !saved)
        {
            std::fprintf(stderr, "%s\n", saved.error().c_str());
            return 1;
        }
        std::printf("wrote %s (%zu file(s))\n", a.positional[2].c_str(), package->files.size());
        return 0;
    }
} // namespace

namespace
{
    // The command line in UTF-8, which is what every path here is read as.
    int run(int argc, char **argv);
} // namespace

#ifdef _WIN32
// On Windows main()'s arguments are in the system's ANSI code page, which
// cannot hold every file name and is not UTF-8; the wide ones are converted
// instead.
int wmain(int argc, wchar_t **wide)
{
    std::vector<std::string> arguments_utf8;
    for (int i = 0; i < argc; ++i)
    {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string text(static_cast<std::size_t>(size > 0 ? size - 1 : 0), char(0));
        if (size > 1)
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), size, nullptr, nullptr);
        arguments_utf8.push_back(std::move(text));
    }
    std::vector<char *> argv;
    for (std::string &text : arguments_utf8)
        argv.push_back(text.data());
    argv.push_back(nullptr);
    // Messages name files in UTF-8 too.
    SetConsoleOutputCP(CP_UTF8);
    return run(argc, argv.data());
}
#else
int main(int argc, char **argv)
{
    return run(argc, argv);
}
#endif

namespace
{
int run(int argc, char **argv)
{
    if (argc < 2)
        return usage();
    const arguments a = parse_arguments(argc, argv);
    if (!a.error.empty())
    {
        std::fprintf(stderr, "%s\n", a.error.c_str());
        return usage();
    }
    const std::string_view command = argv[1];
    if (command == "schema")
        return schema_command(a);
    if (command == "check")
        return check_command(a);
    if (command == "import")
        return import_command(a);
    if (command == "export")
        return export_command(a);
    return usage();
}
} // namespace
