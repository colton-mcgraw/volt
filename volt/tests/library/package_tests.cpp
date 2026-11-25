// Editing a library, the files a library carries, and part packages:
// reading and writing .voltpart files and importing them, with the report
// that says what was done with each part and what is wrong with it.

#include "library_test_support.hpp"

#include <library/edit.hpp>
#include <library/package.hpp>

#include <algorithm>
#include <cmath>

using namespace volt;
using tests::make_part;

namespace
{
    const library::schema &rules = library::schema::builtin();

    library::part resistor(std::string id, double ohms)
    {
        library::part p = make_part(std::move(id), "A resistor", "Passives/Resistors", "10k", "0603");
        p.device = "resistor";
        p.parameters = {library::parameter::quantity("Resistance", ohms, "\xCE\xA9")};
        return p;
    }

    bool near_equal(double a, double b)
    {
        return std::abs(a - b) <= 1e-9 * std::abs(b);
    }

    // Every byte value, which no text encoding survives.
    std::string binary_bytes()
    {
        std::string bytes;
        for (int i = 0; i < 256; ++i)
            bytes += static_cast<char>(i);
        return bytes + bytes.substr(0, 2); // a length that needs padding
    }

    void editing()
    {
        library::library lib = library::new_library("mine", "My Parts");
        CHECK(lib.name == "mine" && lib.display_name() == "My Parts");

        // Ids are generated: valid, unlike each other, and nothing of the part.
        CHECK(library::make_id("  RC0603:10K\t") == "RC0603_10K");
        CHECK(library::make_id(" \n ").empty());
        const std::string generated = library::generate_id(lib);
        CHECK(library::valid_id(generated) && generated.size() == 14 && generated.starts_with("p-"));
        CHECK(library::generate_id(lib) != generated);

        // Adding: an id given, the name from the MPN, the category tidied.
        library::part p;
        p.mpn = "RC0603FR-0710KL";
        p.category = " Passives / Resistors ";
        auto added = library::add_part(lib, p);
        CHECK(added && added->action == library::add_action::added);
        const std::string id = added ? added->id : std::string();
        CHECK(library::valid_id(id) && id.find("RC0603") == std::string::npos);
        CHECK(lib.parts.size() == 1 && lib.parts[0].id == id && lib.parts[0].name == "RC0603FR-0710KL");
        CHECK(lib.parts[0].category == "Passives/Resistors");

        // The same part again, without an id, is another part: two makers'
        // parts of one number do not collide.
        added = library::add_part(lib, p);
        CHECK(added && added->id != id && lib.parts.size() == 2);
        const std::string second = added ? added->id : std::string();

        // An id that is taken, each way.
        p.id = id;
        p.description = "another";
        CHECK(!library::add_part(lib, p).has_value());
        added = library::add_part(lib, p, library::on_conflict::skip);
        CHECK(added && added->action == library::add_action::skipped && lib.parts[0].description.empty());
        added = library::add_part(lib, p, library::on_conflict::rename);
        CHECK(added && added->id != id && added->id != second && lib.parts.size() == 3);
        added = library::add_part(lib, p, library::on_conflict::replace);
        CHECK(added && added->action == library::add_action::replaced && lib.parts.size() == 3);
        CHECK(lib.parts[0].description == "another");

        // An id from elsewhere is kept as given, when it can be one.
        library::part coded;
        coded.id = "lcsc-C25804";
        coded.name = "10k";
        added = library::add_part(lib, coded);
        CHECK(added && added->id == "lcsc-C25804");
        library::part bad;
        bad.id = "a:b";
        CHECK(!library::add_part(lib, bad).has_value());

        // Replacing: everything but the id, which does not change.
        library::part edited = lib.parts[0];
        edited.value = "10k";
        edited.name = "Renamed";
        edited.mpn = "ANOTHER-MPN";
        edited.id.clear();
        CHECK(library::replace_part(lib, id, edited).has_value());
        CHECK(lib.parts[0].id == id && lib.parts[0].value == "10k" && lib.parts[0].name == "Renamed");
        edited.id = "R_10k";
        CHECK(!library::replace_part(lib, id, edited).has_value());
        CHECK(lib.find(id) != nullptr && lib.find("R_10k") == nullptr);
        CHECK(!library::replace_part(lib, "missing", edited).has_value());

        CHECK(library::remove_part(lib, second));
        CHECK(!library::remove_part(lib, second));
        CHECK(lib.parts.size() == 3);

        // What was edited saves as a library that loads clean.
        const auto back = library::parse_library(library::write_library(lib));
        CHECK(back && back->report.clean() && back->library == lib);
    }

    void files()
    {
        CHECK(library::valid_file_name("R_0603.step"));
        CHECK(library::valid_file_name("models/R 0603.step"));
        for (const char *bad : {"", "/abs.step", "a//b", "../up.step", "a/../b", "./a", "C:/x.step", "a\\b", "a\nb"})
            CHECK(!library::valid_file_name(bad));

        library::library lib = library::new_library("mine");
        const std::string step = "ISO-10303-21;\nHEADER;\nENDSEC;\nEND-ISO-10303-21;\n";

        // The same file twice is one file; another of the same name gets a
        // name of its own.
        CHECK(library::add_file(lib, {"models/R.step", step, {}}) == "models/R.step");
        CHECK(library::add_file(lib, {"models/R.step", step, {}}) == "models/R.step");
        CHECK(library::add_file(lib, {"models/R.step", "other", {}}) == "models/R-2.step");
        CHECK(library::add_file(lib, {"models/R.step", "other", {}}) == "models/R-2.step");
        CHECK(library::add_file(lib, {"blob", binary_bytes(), {}}) == "blob");
        CHECK(library::add_file(lib, {"blob", "x", {}}) == "blob-2");
        CHECK(lib.files.size() == 4);
        CHECK(lib.find_file("models/R.step") != nullptr && lib.find_file("models/r.step") == nullptr);

        // Text stays text in the file, anything else is base64; both come
        // back byte for byte.
        const json::value as_text = library::file_to_json(lib.files[0]);
        CHECK(as_text.at("encoding").string_or() == "text" && as_text.at("data").string_or() == step);
        const json::value as_base64 = library::file_to_json(*lib.find_file("blob"));
        CHECK(as_base64.at("encoding").string_or() == "base64");
        lib.parts.push_back(resistor("R1", 1e4));
        lib.parts[0].model = "models/R.step";
        const auto back = library::parse_library(library::write_library(lib));
        CHECK(back && back->report.clean());
        if (back)
        {
            CHECK(back->library == lib);
            CHECK(back->library.find_file("blob") != nullptr &&
                  back->library.find_file("blob")->data == binary_bytes());
        }

        // Broken entries are left out and reported; the rest load.
        const auto messy = library::parse_library(R"({"format": "volt-library", "format_version": 1, "name": "m",
            "parts": [], "files": [
                {"name": "ok.step", "data": "fine"},
                {"name": "ok.step", "data": "again"},
                {"name": "../escape.step", "data": "x"},
                {"name": "bad.bin", "encoding": "base64", "data": "not base64!"},
                {"name": "odd.bin", "encoding": "rot13", "data": "x"},
                {"name": "empty.bin"},
                7,
                {"name": "pad.bin", "encoding": "base64", "data": "AAE="}
            ]})");
        CHECK(messy.has_value());
        if (messy)
        {
            CHECK(messy->library.files.size() == 2);
            CHECK(messy->report.problems.size() == 6);
            const library::embedded_file *pad = messy->library.find_file("pad.bin");
            CHECK(pad != nullptr && pad->data == std::string("\0\1", 2));
        }

        // Files nothing names go; the model stays.
        CHECK(library::remove_unused_files(lib) == 3);
        CHECK(lib.files.size() == 1 && lib.files[0].name == "models/R.step");

        // On disk, for what reads models from files: once per content.
        const tests::scratch_directory directory("volt-package-tests-files");
        const auto first = library::extract_file(lib.files[0], directory.path());
        CHECK(first.has_value());
        if (first)
        {
            CHECK(first->filename() == "R.step");
            CHECK(json::read_text_file(*first).value_or("") == step);
            const auto again = library::extract_file(lib.files[0], directory.path());
            CHECK(again && *again == *first);
            const auto other = library::extract_file({"models/R.step", "other", {}}, directory.path());
            CHECK(other && *other != *first);
        }
        const auto read = library::read_file(first.value_or(std::filesystem::path()));
        CHECK(read && read->name == "R.step" && read->data == step);
        CHECK(!library::read_file(directory / "missing.step").has_value());
    }

    void packages()
    {
        library::part_package package;
        package.generator = "tests 1.0";
        package.parts = {resistor("R1", 1e4), resistor({}, 4700.0)};
        package.parts[0].model = "R.step";
        package.parts[1].mpn = "RC0603FR-074K7L";
        package.files = {{"R.step", "ISO-10303-21;", {}}, {"blob", binary_bytes(), {}}};
        package.extra.set("fetched", "2026-11-22");

        // Round trip, and deterministic.
        const std::string text = library::write_package(package);
        const auto back = library::parse_package(text);
        CHECK(back && back->report.clean() && back->package == package);
        CHECK(back && library::write_package(back->package) == text);

        // Not packages.
        CHECK(!library::parse_package("[]").has_value());
        CHECK(!library::parse_package(R"({"format": "volt-project"})").has_value());
        CHECK(!library::parse_package(R"({"format": "volt-parts", "parts": 3})").has_value());
        CHECK(!library::parse_package("{").has_value());

        // Problems are reported, and what can be read is.
        const auto loose = library::parse_package(R"({"format": "volt-parts", "format_version": 1,
            "parts": [{"name": "ok"}, 7, {"name": "typed", "mpn": 12}],
            "files": [{"name": "by-path.step", "path": "somewhere.step"}]})");
        CHECK(loose.has_value());
        if (loose)
        {
            CHECK(loose->package.parts.size() == 2);
            CHECK(loose->report.problems.size() == 3); // the 7, the mpn, the path with no directory
            CHECK(loose->package.files.empty());
        }

        // A library is a package of its parts and files.
        library::library lib = library::new_library("source", "Source");
        lib.parts = {resistor("R1", 1e4)};
        lib.parts[0].model = "R.step";
        lib.files = {{"R.step", "ISO-10303-21;", {}}};
        const auto from_library = library::parse_package(library::write_library(lib));
        CHECK(from_library && from_library->package.parts == lib.parts && from_library->package.files == lib.files);

        // One part out of a library, with its file.
        const auto exported = library::export_part(lib, "R1");
        CHECK(exported && exported->parts.size() == 1 && exported->files.size() == 1);
        CHECK(!library::export_part(lib, "R2").has_value());

        // On disk: a file given by path is read, relative to the package.
        const tests::scratch_directory directory("volt-package-tests");
        CHECK(tests::write_text(directory / "downloads/R_0603.step", "ISO-10303-21; from disk"));
        const std::filesystem::path path = directory / "fetched.voltpart";
        CHECK(tests::write_text(path, R"({"format": "volt-parts", "format_version": 1, "generator": "acme-fetch 1.2",
            "parts": [{"name": "10k", "mpn": "RC0603FR-0710KL", "model": "R_0603.step"}],
            "files": [{"name": "R_0603.step", "path": "downloads/R_0603.step"},
                      {"path": "downloads/missing.step"}]})"));
        const auto loaded = library::load_package(path);
        CHECK(loaded.has_value());
        if (loaded)
        {
            CHECK(loaded->package.generator == "acme-fetch 1.2");
            CHECK(loaded->package.files.size() == 1);
            CHECK(!loaded->package.files.empty() && loaded->package.files[0].data == "ISO-10303-21; from disk");
            CHECK(loaded->report.problems.size() == 1 && loaded->report.problems[0].file == path);
        }
        const auto failed = library::load_package(directory / "missing.voltpart");
        CHECK(!failed.has_value());

        CHECK(library::save_package(directory / "saved.voltpart", package).has_value());
        const auto saved = library::load_package(directory / "saved.voltpart");
        CHECK(saved && saved->package == package);
    }

    const library::imported_part *entry_of(const library::import_report &report, std::size_t index)
    {
        return index < report.parts.size() ? &report.parts[index] : nullptr;
    }

    void importing()
    {
        library::library target = library::new_library("mine");

        library::part_package package;
        package.parts = {resistor("R1", 1e4), resistor({}, 4700.0), resistor("R3", 100.0)};
        package.parts[0].model = "R.step";
        package.parts[1].manufacturer = "Yageo";
        package.parts[1].mpn = "RC0603FR-074K7L";
        // As a tool might write it: another spelling, a prefixed unit.
        package.parts[1].parameters = {{"ohms", "4.7", "k\xCE\xA9", {}}};
        // And a mistake.
        package.parts[2].parameters = {{"Resistence", "100", "\xCE\xA9", {}}};
        package.files = {{"R.step", "ISO-10303-21;", {}}, {"unused.step", "x", {}}};

        library::import_report report = library::import_parts(target, package, rules);
        CHECK(report.added == 3 && report.changed());
        CHECK(report.parts.size() == 3);
        CHECK(target.parts.size() == 3);
        // The part without an id was given one, which says nothing of its
        // MPN; the report calls it by the MPN. The parameters are as the
        // schema writes them.
        const std::string given = entry_of(report, 1) != nullptr ? entry_of(report, 1)->id : std::string();
        CHECK(library::valid_id(given) && given.find("RC0603") == std::string::npos);
        CHECK(entry_of(report, 1) != nullptr && entry_of(report, 1)->label == "RC0603FR-074K7L");
        const library::part *named = target.find(given);
        CHECK(named != nullptr && named->parameters[0] == (library::parameter{"Resistance", "4.7k", "\xCE\xA9", {}}));
        // Only the file a part names comes along.
        CHECK(target.files.size() == 1 && target.files[0].name == "R.step");
        // The mistake is imported, and reported: a warning and an error.
        CHECK(report.errors == 1 && report.warnings == 1);
        const library::imported_part *mistaken = entry_of(report, 2);
        CHECK(mistaken != nullptr && mistaken->action == library::import_action::added &&
              mistaken->issues.size() == 2);
        const std::string summary = report.summary();
        CHECK(summary.find("3 parts: 3 added; 1 error, 1 warning") == 0);
        CHECK(summary.find("did you mean \"Resistance\"") != std::string::npos);
        CHECK(summary.find("R1") == std::string::npos); // nothing to say about it

        // Again: the same parts are unchanged, whatever the policy. The one
        // without an id is found by its manufacturer and MPN.
        report = library::import_parts(target, package, rules);
        CHECK(report.unchanged == 3 && !report.changed() && target.parts.size() == 3 && target.files.size() == 1);
        CHECK(entry_of(report, 1) != nullptr && entry_of(report, 1)->id == given);

        // The same number from another manufacturer is another part.
        library::part_package other_maker;
        other_maker.parts = {package.parts[1]};
        other_maker.parts[0].manufacturer = "Another";
        report = library::import_parts(target, other_maker, rules);
        CHECK(report.added == 1 && report.parts[0].id != given && target.parts.size() == 4);
        CHECK(library::remove_part(target, report.parts[0].id));

        // A different part under an id that is taken, each way.
        library::part_package changed;
        changed.parts = {resistor("R1", 2e4)};
        changed.parts[0].model = "R.step";
        changed.files = {{"R.step", "ISO-10303-21; another", {}}};

        report = library::import_parts(target, changed, rules); // skip
        CHECK(report.skipped == 1 && !report.changed());
        CHECK(target.files.size() == 1); // its file did not stay behind
        CHECK(near_equal(rules.number(*target.find("R1"), "Resistance").value_or(0.0), 1e4));

        library::import_options options;
        options.conflict = library::on_conflict::fail;
        report = library::import_parts(target, changed, rules, options);
        CHECK(report.rejected == 1 && !report.parts[0].reason.empty() && target.files.size() == 1);

        options.conflict = library::on_conflict::rename;
        report = library::import_parts(target, changed, rules, options);
        const std::string renamed = report.parts[0].id;
        CHECK(report.added == 1 && renamed != "R1" && library::valid_id(renamed));
        // A different file of the same name got a name of its own, and
        // the part names that one.
        CHECK(target.files.size() == 2 && target.find(renamed) != nullptr &&
              target.find(renamed)->model == "R-2.step");

        options.conflict = library::on_conflict::replace;
        report = library::import_parts(target, changed, rules, options);
        CHECK(report.replaced == 1 && target.parts.size() == 4);
        CHECK(near_equal(rules.number(*target.find("R1"), "Resistance").value_or(0.0), 2e4));

        // Strict: parts with errors stay out; warnings and notes do not.
        library::library strict_target = library::new_library("strict");
        options = {};
        options.strict = true;
        report = library::import_parts(strict_target, package, rules, options);
        CHECK(report.added == 2 && report.rejected == 1 && strict_target.find("R3") == nullptr);

        // Without normalizing, the part goes in as written.
        library::library raw = library::new_library("raw");
        options = {};
        options.normalize = false;
        report = library::import_parts(raw, package, rules, options);
        CHECK(raw.find(report.parts[1].id) != nullptr && raw.find(report.parts[1].id)->parameters[0].name == "ohms");
        CHECK(report.errors == 1); // and still reads as a resistance

        // An id that cannot be one.
        library::part_package bad;
        bad.parts = {resistor("a:b", 1.0)};
        report = library::import_parts(raw, bad, rules);
        CHECK(report.rejected == 1 && raw.find("a:b") == nullptr);

        // The report as JSON, for tools.
        report = library::import_parts(strict_target, package, rules);
        const json::value as_json = library::import_report_to_json(report);
        CHECK(as_json.at("added").as_integer() == 1 && as_json.at("unchanged").as_integer() == 2);
        CHECK(as_json.at("parts").size() == 3);
        CHECK(as_json.at("parts").at(2).at("action").string_or() == "added");
        CHECK(as_json.at("parts").at(2).at("issues").at(0).at("code").string_or() == "misspelled-parameter");

        // What was imported saves as a library that loads clean.
        const auto back = library::parse_library(library::write_library(target));
        CHECK(back && back->report.clean() && back->library == target);
    }

    void projects()
    {
        // A part copied into a project brings its file.
        library::library source = library::new_library("source");
        source.version = "1";
        source.parts = {resistor("R1", 1e4)};
        source.parts[0].model = "R.step";
        source.files = {{"R.step", "ISO-10303-21;", {}}};
        library::catalog c;
        c.add(source);

        library::project p = library::new_project("Blinky");
        const auto id = library::add_part(p, c, *c.find("source:R1"));
        CHECK(id && p.parts.files.size() == 1 && p.parts.files[0] == source.files[0]);
        CHECK(library::status_of(p, "R1", c) == library::part_status::up_to_date);

        // And the new one, when the source's changes.
        source.files[0].data = "ISO-10303-21; revised";
        source.parts[0].description = "revised";
        c.add(source);
        CHECK(library::update_from_source(p, "R1", c).has_value());
        const library::part *copy = p.parts.find("R1");
        CHECK(copy != nullptr && p.parts.find_file(copy->model) != nullptr &&
              p.parts.find_file(copy->model)->data == "ISO-10303-21; revised");
        CHECK(p.parts.files.size() == 1);
        CHECK(library::status_of(p, "R1", c) == library::part_status::up_to_date);
    }
} // namespace

int main()
{
    editing();
    files();
    packages();
    importing();
    projects();
    return volt::tests::result("package");
}
