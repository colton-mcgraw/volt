// Parts and .voltlib files: parameters and their quantities, the JSON form
// of a part, the content hash, and loading and saving libraries, broken
// ones included.

#include "library_test_support.hpp"

#include <cmath>

using namespace volt;
using tests::make_part;

namespace
{
    bool near(std::optional<double> value, double expected)
    {
        return value && std::abs(*value - expected) <= 1e-12 * std::abs(expected);
    }

    // A part with every field and some unknown ones.
    library::part full_part()
    {
        library::part p = make_part("R_0603_4k7", "4.7k\xCE\xA9 1% 0603", "Passives/Resistors", "4.7k", "0603");
        p.description = "Thick film resistor \xE2\x80\x94 \xC2\xB1" "1%";
        p.manufacturer = "Yageo";
        p.mpn = "RC0603FR-074K7L";
        p.prefix = "R";
        p.symbol = "R";
        p.footprint = "Resistor_SMD:R_0603_1608Metric";
        p.model = "parts/R_0603.step";
        p.datasheet = "https://example.com/rc.pdf";
        p.keywords = {"resistor", "smd"};
        p.parameters = {library::parameter::quantity("Resistance", 4700.0, "\xCE\xA9"),
                        {"Tolerance", "1", "%", {}},
                        {"Note", "matched pair", "", json::object{{"lot", 1}, {"bin", 2}}}};
        // A typical value with limits and a condition, limits alone, and
        // where to buy it.
        library::parameter drift;
        drift.name = "Temperature coefficient";
        drift.value = "50";
        drift.min = "-100";
        drift.max = "100";
        drift.unit = "ppm/K";
        drift.condition = "-55 to 155 \xC2\xB0" "C";
        p.parameters.push_back(drift);
        library::parameter rating;
        rating.name = "Voltage rating";
        rating.max = "75";
        rating.unit = "V";
        p.parameters.push_back(rating);
        p.suppliers = {{"LCSC", "C23162", "https://www.lcsc.com/product-detail/C23162.html", {}},
                       {"Somewhere", "", "", json::object{{"stock", 12}}}};
        p.origin = library::part_origin{"volt-resistors", "R_0603_4.7k_1%", "2026.11.0", "0123456789abcdef"};
        p.extra.set("symbol_graphics", json::object{{"lines", json::array{1, 2, 3}}});
        p.extra.set("future", "kept");
        return p;
    }

    void parameters()
    {
        const library::parameter r = library::parameter::quantity("Resistance", 4700.0, "\xCE\xA9");
        CHECK(r.value == "4.7k");
        CHECK(r.text() == "4.7k\xCE\xA9");
        CHECK(near(r.number(), 4700.0));

        const library::parameter c = library::parameter::quantity("Capacitance", 1e-7, "F");
        CHECK(c.value == "100n");
        CHECK(near(c.number(), 1e-7));

        const library::parameter micro = library::parameter::quantity("Capacitance", 4.7e-6, "F");
        CHECK(micro.value == "4.7\xC2\xB5");
        CHECK(near(micro.number(), 4.7e-6));

        // As people write them: RKM, a unit repeated in the value, spaces.
        CHECK(near(library::parameter{"R", "4k7", "\xCE\xA9", {}}.number(), 4700.0));
        CHECK(near(library::parameter{"C", "100 nF", "F", {}}.number(), 1e-7));
        CHECK(near(library::parameter{"C", "10u", "F", {}}.number(), 1e-5));
        CHECK(near(library::parameter{"V", "50", "V", {}}.number(), 50.0));
        CHECK(near(library::parameter{"L", "10mil", "mil", {}}.number(), 10.0));
        CHECK(!library::parameter{"Dielectric", "X7R", "", {}}.number());
        CHECK(!library::parameter{"Package", "0603 reel", "", {}}.number());

        // Limits and conditions, as shown.
        library::parameter vf{"Forward voltage", "2.0", "V", {}};
        vf.min = "1.8";
        vf.max = "2.4";
        vf.condition = "If = 20 mA";
        CHECK(vf.text() == "2.0V (1.8 to 2.4) at If = 20 mA");
        vf.value.clear();
        CHECK(vf.text() == "1.8 to 2.4V at If = 20 mA");
        vf.min.clear();
        vf.condition.clear();
        CHECK(vf.text() == "up to 2.4V");
        vf.min = "1.8";
        vf.max.clear();
        CHECK(vf.text() == "from 1.8V");

        library::part p;
        p.parameters = {r, c};
        CHECK(p.find_parameter("Capacitance") == &p.parameters[1]);
        CHECK(p.find_parameter("capacitance") == nullptr);
    }

    void ids_and_categories()
    {
        CHECK(library::valid_id("R_0603_10k_1%"));
        CHECK(library::valid_id("\xC2\xB5" "C 1"));
        CHECK(!library::valid_id(""));
        CHECK(!library::valid_id("lib:part"));
        CHECK(!library::valid_id(" R1"));
        CHECK(!library::valid_id("R1 "));
        CHECK(!library::valid_id("R\n1"));

        CHECK(library::normalize_category(" Passives / Resistors/ ") == "Passives/Resistors");
        CHECK(library::normalize_category("a//b///c") == "a/b/c");
        CHECK(library::normalize_category("/") == "");
        CHECK(library::normalize_category("") == "");
        CHECK(library::normalize_category("Single") == "Single");
    }

    void part_json()
    {
        const library::part p = full_part();
        const json::value v = library::part_to_json(p);

        // Known fields first in a fixed order, then the unknown ones.
        const json::object &o = *v.as_object();
        CHECK(o.begin()->key == "id");
        CHECK((o.end() - 1)->key == "future");
        CHECK(v["parameters"][2]["lot"].as_integer() == 1);

        std::vector<std::string> problems;
        const library::part back = library::part_from_json(o, problems);
        CHECK(problems.empty());
        CHECK(back == p);

        // Empty fields are not written.
        const json::value bare = library::part_to_json(make_part("X", "X", ""));
        CHECK(bare.size() == 2);

        // Hand-written forms are read too; wrong types are reported.
        problems.clear();
        const auto loose = json::parse(R"({"id": "x", "name": 5, "value": 25, "keywords": "a, b ,,c",
            "category": " A / B ", "parameters": [{"name": "V", "value": 50}, {"value": "1"}, 7],
            "origin": {"library": "l"}, "package": null})");
        CHECK(loose.has_value());
        if (loose)
        {
            const library::part q = library::part_from_json(*loose->as_object(), problems);
            CHECK(q.name.empty());
            CHECK(q.value == "25");
            CHECK(q.category == "A/B");
            CHECK((q.keywords == std::vector<std::string>{"a", "b", "c"}));
            CHECK(q.parameters.size() == 1 && q.parameters[0].value == "50");
            CHECK(!q.origin);
            CHECK(q.package.empty());
            // name, the nameless parameter, the 7, the origin.
            CHECK(problems.size() == 4);
        }
    }

    void hashes()
    {
        const library::part p = full_part();
        const std::string hash = library::part_hash(p);
        CHECK(hash.size() == 16);
        CHECK(hash.find_first_not_of("0123456789abcdef") == std::string::npos);

        // The id and origin are not content; every other field is.
        library::part renamed = p;
        renamed.id = "R_0603_4k7-2";
        renamed.origin.reset();
        CHECK(library::part_hash(renamed) == hash);
        library::part changed = p;
        changed.parameters[1].value = "5";
        CHECK(library::part_hash(changed) != hash);
        changed = p;
        changed.extra.set("future", "changed");
        CHECK(library::part_hash(changed) != hash);

        // Stable across runs and platforms: a hash stored in a project file
        // must still match next year. Changing this value breaks every
        // project's "differs from its source" check.
        const library::part fixed = make_part("R1", "10k", "Passives/Resistors", "10k", "0603");
        CHECK(library::part_hash(fixed) == "76e61a52202f5227");
        if (library::part_hash(fixed) != "76e61a52202f5227")
            std::fprintf(stderr, "  part_hash(fixed) is %s\n", library::part_hash(fixed).c_str());
    }

    library::library sample_library()
    {
        library::library lib = tests::make_library("sample", "3.1.4");
        lib.title = "Sample \xE2\x80\x94 \xC3\xA9t\xC3\xA9";
        lib.description = "For the tests";
        lib.author = "Volt";
        lib.license = "CC0-1.0";
        lib.url = "https://example.com";
        lib.extra.set("maintainers", json::array{"a", "b"});
        lib.parts.push_back(full_part());
        lib.parts.push_back(make_part("C1", "100nF", "Passives/Capacitors", "100nF", "0402"));
        lib.parts.push_back(make_part("\xCE\xA9-part", "\xD0\xA0\xD0\xB5\xD0\xB7\xD0\xB8\xD1\x81\xD1\x82\xD0\xBE\xD1\x80",
                                      "\xD0\x9F\xD0\xB0\xD1\x81\xD1\x81\xD0\xB8\xD0\xB2/R"));
        return lib;
    }

    void library_round_trip()
    {
        const library::library lib = sample_library();
        const std::string text = library::write_library(lib);
        CHECK(text.starts_with("{\n  \"format\": \"volt-library\",\n  \"format_version\": 1,\n  \"name\": \"sample\","));
        CHECK(text.ends_with("}\n"));

        const auto back = library::parse_library(text);
        CHECK(back.has_value());
        if (!back)
            return;
        CHECK(back->report.clean());
        CHECK(back->report.parts_loaded == 3);
        CHECK(back->library == lib);
        CHECK(back->library.display_name() == lib.title);
        CHECK(library::write_library(back->library) == text);

        CHECK(back->library.find("C1") != nullptr);
        CHECK(back->library.find("c1") == nullptr);

        library::library untitled = lib;
        untitled.title.clear();
        CHECK(untitled.display_name() == "sample");
    }

    void bad_files()
    {
        const auto fails = [](std::string_view text, std::string_view expected) {
            const auto result = library::parse_library(text, "hint");
            CHECK(!result.has_value());
            if (!result)
            {
                if (result.error().find(expected) == std::string::npos)
                    std::fprintf(stderr, "  '%s' does not mention '%.*s'\n", result.error().c_str(),
                                 static_cast<int>(expected.size()), expected.data());
                CHECK(result.error().find(expected) != std::string::npos);
            }
        };
        fails("{\"format\": \"volt-library\",", "line 1");
        fails("[]", "not a Volt library");
        fails("{}", "not a Volt library");
        fails(R"({"format": "kicad"})", "not a Volt library");
        fails(R"({"format": "volt-library", "format_version": "1", "name": "x"})", "format_version");
        fails(R"({"format": "volt-library", "format_version": 0, "name": "x"})", "format_version");
        fails(R"({"format": "volt-library", "format_version": 1, "name": "x", "parts": {}})", "\"parts\"");

        // A missing or unusable name is taken from the file, if it can be.
        const auto hinted = library::parse_library(R"({"format": "volt-library", "format_version": 1, "parts": []})",
                                                   "from-file");
        CHECK(hinted.has_value() && hinted->library.name == "from-file" && hinted->report.problems.size() == 1);
        const auto colon = library::parse_library(R"({"format": "volt-library", "format_version": 1, "name": "a:b"})",
                                                  "ok");
        CHECK(colon.has_value() && colon->library.name == "ok");
        CHECK(!library::parse_library(R"({"format": "volt-library", "format_version": 1})").has_value());
    }

    void per_part_problems()
    {
        const auto result = library::parse_library(R"({
  "format": "volt-library",
  "format_version": 1,
  "name": "messy",
  "parts": [
    {"id": "A", "name": "first"},
    "not an object",
    {"name": "no id"},
    {"id": "A", "name": "duplicate"},
    {"id": "has:colon", "name": "bad id"},
    {"id": "B"},
    {"id": "C", "name": "C", "value": true, "keywords": [1, "ok"]},
    {"id": " D", "name": "space"},
    {"id": "E", "name": "last"}
  ]
})");
        CHECK(result.has_value());
        if (!result)
            return;
        const library::load_report &report = result->report;
        const library::library &lib = result->library;
        CHECK(lib.parts.size() == 4); // A, B, C, E
        CHECK(report.parts_loaded == 4);
        CHECK(report.parts_skipped == 5);
        CHECK(!report.clean());
        CHECK(lib.find("A") && lib.find("A")->name == "first");
        CHECK(lib.find("B") && lib.find("B")->name == "B"); // named after its id
        CHECK(lib.find("C") && lib.find("C")->value.empty() && lib.find("C")->keywords.size() == 1);
        CHECK(lib.parts.back().id == "E");

        std::size_t skipped = 0;
        bool duplicate = false;
        for (const library::load_report::problem &p : report.problems)
        {
            skipped += p.skipped;
            if (p.part == 3)
            {
                duplicate = p.id == "A" && p.skipped && p.message.find("duplicate") != std::string::npos &&
                            p.message.find("part 0") != std::string::npos;
            }
        }
        CHECK(skipped == 5);
        CHECK(duplicate);

        const std::string summary = report.summary();
        CHECK(summary.find("part 3 (A): duplicate id") != std::string::npos);
        CHECK(summary.find("(left out)") != std::string::npos);
    }

    void versions_and_unknown_fields()
    {
        // A file from a newer Volt: read as far as understood, the rest kept.
        const auto newer = library::parse_library(R"({
  "format": "volt-library",
  "format_version": 7,
  "name": "future",
  "symbols": [{"name": "R", "lines": []}],
  "parts": [{"id": "R1", "name": "R1", "symbol": "R", "pins": [{"number": 1}, {"number": 2}]}]
})");
        CHECK(newer.has_value());
        if (!newer)
            return;
        CHECK(newer->library.format_version == 7);
        CHECK(newer->report.problems.size() == 1);
        CHECK(newer->report.problems[0].message.find("newer") != std::string::npos);
        CHECK(newer->library.extra.contains("symbols"));
        CHECK(newer->library.parts[0].extra.contains("pins"));

        const std::string rewritten = library::write_library(newer->library);
        CHECK(rewritten.find("\"symbols\"") != std::string::npos);
        CHECK(rewritten.find("\"pins\"") != std::string::npos);
        const auto again = library::parse_library(rewritten);
        CHECK(again.has_value() && again->library == newer->library);

        // A file without a version is read as version 1, with a note.
        const auto unversioned = library::parse_library(R"({"format": "volt-library", "name": "old", "parts": []})");
        CHECK(unversioned.has_value() && unversioned->report.problems.size() == 1);
    }

    void files_on_disk()
    {
        const tests::scratch_directory directory("volt_library_tests");
        const library::library lib = sample_library();

        // A name and a directory outside ASCII.
        const std::filesystem::path path = directory / "\xD0\xB1\xD0\xB8\xD0\xB1\xD0\xBB\xD0\xB8\xD0\xBE/\xCE\xA9 parts.voltlib";
        std::filesystem::create_directories(path.parent_path());
        CHECK(library::save_library(path, lib).has_value());
        CHECK(std::filesystem::exists(path));
        auto loaded = library::load_library(path);
        CHECK(loaded.has_value());
        if (loaded)
        {
            CHECK(loaded->library == lib);
            CHECK(loaded->library.path == path);
            CHECK(loaded->report.clean());
        }

        // Saving over it leaves no temporary file behind.
        library::library changed = lib;
        changed.parts.pop_back();
        CHECK(library::save_library(path, changed).has_value());
        std::size_t files = 0;
        for (const auto &entry : std::filesystem::directory_iterator(path.parent_path()))
        {
            (void)entry;
            ++files;
        }
        CHECK(files == 1);
        loaded = library::load_library(path);
        CHECK(loaded && loaded->library.parts.size() == 2);

        // Problems name the file; errors too.
        const std::filesystem::path messy = directory / "messy.voltlib";
        CHECK(tests::write_text(messy, R"({"format": "volt-library", "format_version": 1, "parts": [{"id": "A"}]})"));
        const auto named = library::load_library(messy);
        CHECK(named.has_value());
        if (named)
        {
            CHECK(named->library.name == "messy");
            CHECK(!named->report.problems.empty() && named->report.problems[0].file == messy);
            CHECK(named->report.summary().find("messy.voltlib") != std::string::npos);
        }
        const std::filesystem::path broken = directory / "broken.voltlib";
        CHECK(tests::write_text(broken, "{\n\"format\": \"volt-library\",\n\"name\": x\n}"));
        const auto failed = library::load_library(broken);
        CHECK(!failed.has_value());
        if (!failed)
        {
            CHECK(failed.error().find("broken.voltlib") != std::string::npos);
            CHECK(failed.error().find("line 3") != std::string::npos);
        }
        CHECK(!library::load_library(directory / "missing.voltlib").has_value());
    }
} // namespace

int main()
{
    parameters();
    ids_and_categories();
    part_json();
    hashes();
    library_round_trip();
    bad_files();
    per_part_problems();
    versions_and_unknown_fields();
    files_on_disk();
    return volt::tests::result("library");
}
