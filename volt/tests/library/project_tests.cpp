// The project file and the project library: new, save, load, parts copied
// from the catalog with their origin, how a copy compares with its source,
// updating it, the schematic settings (written only when not the
// defaults), and the relative UTF-8 paths a project file stores.

#include "library_test_support.hpp"

using namespace volt;
using tests::make_part;

namespace
{
    library::library source_library(std::string version = "1.0", std::string r1_value = "10k")
    {
        library::library lib = tests::make_library("shared", std::move(version));
        auto r1 = make_part("R1", "Resistor", "Passives/Resistors", std::move(r1_value), "0603");
        r1.parameters = {library::parameter::quantity("Resistance", 10e3, "\xCE\xA9")};
        lib.parts.push_back(r1);
        lib.parts.push_back(make_part("C1", "Capacitor", "Passives/Capacitors", "100nF", "0402"));
        return lib;
    }

    library::catalog catalog_of(library::library a, library::library b = tests::make_library("other"))
    {
        library::catalog c;
        c.add(std::move(a));
        c.add(std::move(b));
        return c;
    }

    void creating()
    {
        const library::project p = library::new_project("Blinky");
        CHECK(p.name == "Blinky");
        CHECK(p.sheets.size() == 1 && p.sheets[0].file == "Blinky.voltsch");
        CHECK(p.board == "Blinky.voltpcb");
        CHECK(p.library_file == "Blinky.voltlib");
        CHECK(p.parts.name == "Blinky");
        CHECK(p.parts.parts.empty());

        // Names that are not file names everywhere.
        const library::project odd = library::new_project("a:b/c?d. ");
        CHECK(odd.library_file == "a_b_c_d.voltlib");
        CHECK(library::valid_id(odd.parts.name));
        CHECK(library::new_project("").library_file == "project.voltlib");
        CHECK(library::new_project("\xC3\x9C" "ber").library_file == "\xC3\x9C" "ber.voltlib");

        const std::string text = library::write_project(p);
        CHECK(text == "{\n"
                      "  \"format\": \"volt-project\",\n"
                      "  \"format_version\": 1,\n"
                      "  \"name\": \"Blinky\",\n"
                      "  \"sheets\": [\n"
                      "    {\"name\": \"Main\", \"file\": \"Blinky.voltsch\"}\n"
                      "  ],\n"
                      "  \"board\": \"Blinky.voltpcb\",\n"
                      "  \"library\": \"Blinky.voltlib\"\n"
                      "}\n");
    }

    void adding_and_removing()
    {
        const library::catalog c = catalog_of(source_library("2026.1"));
        library::project p = library::new_project("Demo");

        const auto r1 = c.find("shared:R1");
        CHECK(r1.has_value());
        if (!r1)
            return;
        const auto id = library::add_part(p, c, *r1);
        CHECK(id && *id == "R1");
        const library::part *copy = p.parts.find("R1");
        CHECK(copy != nullptr);
        if (copy)
        {
            CHECK(copy->origin.has_value());
            CHECK(copy->origin->library == "shared");
            CHECK(copy->origin->part == "R1");
            CHECK(copy->origin->version == "2026.1");
            CHECK(copy->origin->hash == library::part_hash(c.get(*r1)));
            CHECK(copy->parameters == c.get(*r1).parameters);
        }

        // Adding it again changes nothing.
        CHECK(library::add_part(p, c, *r1) == std::string("R1"));
        CHECK(p.parts.parts.size() == 1);

        // A different part with the same id gets a suffix.
        library::library other = tests::make_library("other");
        other.parts.push_back(make_part("R1", "Other resistor", "Passives/Resistors", "1k"));
        const library::catalog both = catalog_of(source_library("2026.1"), other);
        const auto second = library::add_part(p, both, *both.find("other:R1"));
        CHECK(second && *second == "R1-2");
        CHECK(p.parts.parts.size() == 2);
        CHECK(library::add_part(p, both, *both.find("other:R1")) == std::string("R1-2"));

        CHECK(library::remove_part(p, "R1"));
        CHECK(!library::remove_part(p, "R1"));
        CHECK(p.parts.parts.size() == 1);
        CHECK(p.parts.parts[0].id == "R1-2");
    }

    void comparing_and_updating()
    {
        const library::catalog original = catalog_of(source_library("1.0", "10k"));
        library::project p = library::new_project("Demo");
        CHECK(library::add_part(p, original, *original.find("shared:R1")).has_value());
        CHECK(library::status_of(p, "R1", original) == library::part_status::up_to_date);
        CHECK(!library::status_of(p, "nope", original));

        // The source changes (a new release of the library).
        const library::catalog released = catalog_of(source_library("1.1", "12k"));
        CHECK(library::status_of(p, "R1", released) == library::part_status::source_changed);

        // The copy changes, the source does not.
        library::project edited = p;
        edited.parts.find("R1")->description = "edited in the project";
        CHECK(library::status_of(edited, "R1", original) == library::part_status::locally_modified);

        // Both.
        CHECK(library::status_of(edited, "R1", released) == library::part_status::diverged);

        // Both, the same way: nothing to do.
        library::project converged = p;
        converged.parts.find("R1")->value = "12k";
        CHECK(library::status_of(converged, "R1", released) == library::part_status::up_to_date);

        // The source is gone.
        const library::catalog empty;
        CHECK(library::status_of(p, "R1", empty) == library::part_status::source_missing);

        // A part made in the project has no source.
        p.parts.parts.push_back(make_part("LOCAL", "Local part", "Misc"));
        CHECK(library::status_of(p, "LOCAL", original) == library::part_status::unlinked);
        CHECK(library::describe(library::part_status::diverged).find("project") != std::string_view::npos);

        // Updating takes the source as it is now and records the new origin.
        CHECK(library::update_from_source(edited, "R1", released).has_value());
        const library::part *updated = edited.parts.find("R1");
        CHECK(updated && updated->value == "12k" && updated->description.empty());
        CHECK(updated && updated->origin && updated->origin->version == "1.1");
        CHECK(library::status_of(edited, "R1", released) == library::part_status::up_to_date);

        // ... keeping the id it has in the project.
        library::library other = tests::make_library("other");
        other.parts.push_back(make_part("R1", "Other", "Passives", "1k"));
        library::project two = library::new_project("Two");
        const library::catalog both = catalog_of(source_library(), other);
        CHECK(library::add_part(two, both, *both.find("shared:R1")).has_value());
        CHECK(library::add_part(two, both, *both.find("other:R1")) == std::string("R1-2"));
        library::library other_changed = other;
        other_changed.parts[0].value = "2k";
        const library::catalog changed = catalog_of(source_library(), other_changed);
        CHECK(library::status_of(two, "R1-2", changed) == library::part_status::source_changed);
        CHECK(library::update_from_source(two, "R1-2", changed).has_value());
        CHECK(two.parts.find("R1-2") && two.parts.find("R1-2")->value == "2k");
        CHECK(two.parts.find("R1") && two.parts.find("R1")->value == "10k");

        CHECK(!library::update_from_source(p, "LOCAL", original).has_value());
        CHECK(!library::update_from_source(p, "nope", original).has_value());
        CHECK(!library::update_from_source(p, "R1", empty).has_value());
    }

    void saving_and_loading()
    {
        const tests::scratch_directory directory("volt_project_tests");
        const library::catalog c = catalog_of(source_library("3.0"));

        // A project whose name and directory are not ASCII.
        const std::string name = "\xD0\x9C\xD0\xBE\xD0\xB9 \xCE\xA9 projet \xC3\xA9t\xC3\xA9";
        library::project p = library::new_project(name);
        p.sheets.push_back({"Power", "sheets/power.voltsch", json::object{{"order", 2}}});
        p.extra.set("settings", json::object{{"grid", 1.27}});
        CHECK(library::add_part(p, c, *c.find("shared:R1")).has_value());
        CHECK(library::add_part(p, c, *c.find("shared:C1")).has_value());

        const std::filesystem::path path = directory / (name + "/" + name + ".voltproj");
        std::filesystem::create_directories(path.parent_path());
        CHECK(library::save_project(path, p).has_value());
        const std::filesystem::path library_path = path.parent_path() / library::path_from_utf8(name + ".voltlib");
        CHECK(std::filesystem::exists(library_path));

        auto loaded = library::load_project(path);
        CHECK(loaded.has_value());
        if (loaded)
        {
            CHECK(loaded->report.clean());
            CHECK(loaded->project == p);
            CHECK(loaded->project.parts.path == library_path);
            CHECK(library::status_of(loaded->project, "R1", c) == library::part_status::up_to_date);
        }

        // The project library is an ordinary library.
        const auto as_library = library::load_library(library_path);
        CHECK(as_library && as_library->library.parts.size() == 2);

        // Saving again writes the same bytes.
        const std::string before = json::read_text_file(path).value_or("");
        const std::string library_before = json::read_text_file(library_path).value_or("");
        CHECK(loaded && library::save_project(path, loaded->project).has_value());
        CHECK(json::read_text_file(path).value_or("") == before);
        CHECK(json::read_text_file(library_path).value_or("") == library_before);
        CHECK(before.find("\"sheets/power.voltsch\"") != std::string::npos);

        // The library elsewhere, under a relative path with a directory.
        library::project moved = p;
        moved.library_file = "libs/parts.voltlib";
        const std::filesystem::path other_path = directory / "other/moved.voltproj";
        std::filesystem::create_directories(other_path.parent_path());
        CHECK(library::save_project(other_path, moved).has_value());
        CHECK(std::filesystem::exists(directory / "other/libs/parts.voltlib"));
        const auto moved_back = library::load_project(other_path);
        CHECK(moved_back && moved_back->project == moved);

        // A project whose library file is missing opens, empty, and says so.
        const std::filesystem::path lonely = directory / "lonely.voltproj";
        CHECK(tests::write_text(lonely, R"({"format": "volt-project", "format_version": 1, "name": "Lonely",
            "sheets": [], "library": "gone.voltlib"})"));
        const auto opened = library::load_project(lonely);
        CHECK(opened.has_value());
        if (opened)
        {
            CHECK(opened->project.parts.parts.empty());
            CHECK(opened->report.problems.size() == 1);
            CHECK(opened->report.summary().find("gone.voltlib") != std::string::npos);
            // Saving creates it.
            CHECK(library::save_project(lonely, opened->project).has_value());
            CHECK(std::filesystem::exists(directory / "gone.voltlib"));
        }

        // No library named: one is named after the project file.
        const std::filesystem::path unnamed = directory / "unnamed.voltproj";
        CHECK(tests::write_text(unnamed, R"({"format": "volt-project", "format_version": 1})"));
        const auto defaulted = library::load_project(unnamed);
        CHECK(defaulted && defaulted->project.library_file == "unnamed.voltlib" && defaulted->project.name == "unnamed");

        // Broken and foreign files fail with the file's name.
        const std::filesystem::path foreign = directory / "foreign.voltproj";
        CHECK(tests::write_text(foreign, R"({"format": "volt-library"})"));
        const auto refused = library::load_project(foreign);
        CHECK(!refused && refused.error().find("foreign.voltproj") != std::string::npos);
        CHECK(!library::load_project(directory / "missing.voltproj").has_value());

        // A project library that is not a library fails the load.
        const std::filesystem::path bad_library = directory / "bad.voltproj";
        CHECK(tests::write_text(bad_library, R"({"format": "volt-project", "format_version": 1, "library": "bad.voltlib"})"));
        CHECK(tests::write_text(directory / "bad.voltlib", "[]"));
        CHECK(!library::load_project(bad_library).has_value());
    }

    void forward_compatibility()
    {
        const auto newer = library::parse_project(R"({
  "format": "volt-project",
  "format_version": 3,
  "name": "Future",
  "sheets": [{"name": "Main", "file": "main.voltsch", "hierarchy": {"parent": null}}],
  "board": "future.voltpcb",
  "library": "future.voltlib",
  "variants": ["A", "B"]
})");
        CHECK(newer.has_value());
        if (!newer)
            return;
        CHECK(newer->project.format_version == 3);
        CHECK(newer->report.problems.size() == 1);
        CHECK(newer->project.extra.contains("variants"));
        CHECK(newer->project.sheets[0].extra.contains("hierarchy"));
        const std::string text = library::write_project(newer->project);
        CHECK(text.find("\"variants\": [\"A\", \"B\"]") != std::string::npos);
        CHECK(text.find("\"hierarchy\"") != std::string::npos);

        CHECK(!library::parse_project("[]").has_value());
        CHECK(!library::parse_project(R"({"format": "volt-project", "format_version": -1})").has_value());
        const auto loose = library::parse_project(R"({"format": "volt-project", "format_version": 1, "name": 1,
            "sheets": [3, {"name": "S"}], "board": false})");
        CHECK(loose && loose->project.sheets.size() == 1 && loose->report.problems.size() == 3);
    }

    // The schematic settings: absent from a file using the defaults (so
    // existing files read and write the same), written when they differ,
    // read back, unknown fields in them kept, wrong kinds reported.
    void schematic_settings()
    {
        library::project p = library::new_project("Blinky");
        CHECK(p.schematic.pins_connect_along_wires && p.schematic == library::schematic_settings{});
        CHECK(library::write_project(p).find("schematic") == std::string::npos);

        p.schematic.pins_connect_along_wires = false;
        const std::string text = library::write_project(p);
        CHECK(text.find("\"schematic\": {\"pins_connect_along_wires\": false}") != std::string::npos);
        const auto read = library::parse_project(text);
        CHECK(read && read->report.clean() && read->project.schematic == p.schematic);
        CHECK(read && !read->project.extra.contains("schematic"));
        CHECK(read && library::write_project(read->project) == text);
        CHECK(!(read && read->project == library::new_project("Blinky")));

        // Back to the default: the field goes again.
        p.schematic.pins_connect_along_wires = true;
        CHECK(library::write_project(p) == library::write_project(library::new_project("Blinky")));

        // Saved and loaded with its library.
        const tests::scratch_directory directory("volt_project_schematic_tests");
        library::project saved = library::new_project("Settled");
        saved.schematic.pins_connect_along_wires = false;
        const std::filesystem::path path = directory / "settled.voltproj";
        CHECK(library::save_project(path, saved).has_value());
        const auto loaded = library::load_project(path);
        CHECK(loaded && !loaded->project.schematic.pins_connect_along_wires && loaded->project == saved);

        // An explicit default reads as the default; fields of a newer
        // version are kept and written back; wrong kinds are reported.
        const auto explicit_default = library::parse_project(
            R"({"format": "volt-project", "format_version": 1, "schematic": {"pins_connect_along_wires": true}})");
        CHECK(explicit_default && explicit_default->project.schematic == library::schematic_settings{});
        const auto newer = library::parse_project(R"({"format": "volt-project", "format_version": 1,
            "schematic": {"pins_connect_along_wires": false, "bus_rules": "strict"}})");
        CHECK(newer && newer->report.clean() && !newer->project.schematic.pins_connect_along_wires);
        CHECK(newer && newer->project.schematic.extra.contains("bus_rules"));
        CHECK(newer && library::write_project(newer->project).find("\"bus_rules\": \"strict\"") != std::string::npos);
        const auto wrong = library::parse_project(R"({"format": "volt-project", "format_version": 1,
            "schematic": {"pins_connect_along_wires": "no"}})");
        CHECK(wrong && wrong->report.problems.size() == 1 && wrong->project.schematic.pins_connect_along_wires);
        const auto not_object = library::parse_project(R"({"format": "volt-project", "format_version": 1,
            "schematic": 7})");
        CHECK(not_object && not_object->report.problems.size() == 1 &&
              not_object->project.schematic == library::schematic_settings{});
    }

    void paths()
    {
        const tests::scratch_directory directory("volt_project_path_tests");
        const std::filesystem::path project = directory / "proj";

        CHECK(library::relative_path(project, project / "sheets" / "a.voltsch") == "sheets/a.voltsch");
        CHECK(library::relative_path(project, directory / "shared/lib.voltlib") == "../shared/lib.voltlib");
        CHECK(library::relative_path(project, project / "x/../y.voltlib") == "y.voltlib");

        // UTF-8 in and out, whatever the system's narrow encoding is.
        const std::string utf8 = "\xD0\xB4\xD0\xB5\xD1\x82\xD0\xB0\xD0\xBB\xD0\xB8/\xCE\xA9 \xC2\xB5" "F.voltlib";
        const std::filesystem::path target = project / library::path_from_utf8(utf8);
        CHECK(library::relative_path(project, target) == utf8);
        CHECK(library::resolve_path(project, utf8) == target.lexically_normal());
        CHECK(library::path_to_utf8(library::path_from_utf8(utf8)) == utf8);
        CHECK(library::path_from_utf8(utf8).filename().u8string() == u8"\u03a9 \u00b5F.voltlib");

        // Back and forth through resolve.
        for (const char *stored : {"a.voltlib", "sub/dir/b.voltsch", "../up.voltpcb"})
            CHECK(library::relative_path(project, library::resolve_path(project, stored)) == stored);

        // An absolute stored path is used as it is.
        const std::filesystem::path absolute = std::filesystem::absolute(directory / "abs.voltlib");
        CHECK(library::resolve_path(project, library::path_to_utf8(absolute)) == absolute);

        // Stored paths always use forward slashes.
        CHECK(library::path_to_utf8(std::filesystem::path("a") / "b" / "c") == "a/b/c");

#ifdef _WIN32
        // Another drive has no relative path: the absolute one, with '/'.
        const std::string other_drive = library::relative_path("C:\\projects\\p", "Q:\\libs\\x.voltlib");
        CHECK(other_drive == "Q:/libs/x.voltlib");
        CHECK(library::resolve_path("C:\\projects\\p", "Q:/libs/x.voltlib") == std::filesystem::path("Q:/libs/x.voltlib"));
#endif
    }
} // namespace

int main()
{
    creating();
    adding_and_removing();
    comparing_and_updating();
    saving_and_loading();
    forward_compatibility();
    schematic_settings();
    paths();
    return volt::tests::result("library project");
}
