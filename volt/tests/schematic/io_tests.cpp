// The .voltsch file: a sheet written and read back is the same sheet, and
// written again is the same bytes; fields this version does not know are
// kept at every level, in file order; files that are not sheets fail with a
// message; records that cannot be read are reported and left out, fields
// that cannot be read reported and left empty; and saving and loading a
// file under a non-ASCII name.

#include "../math/check.hpp"

#include <schematic/io.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace volt;
using namespace volt::schematic;

namespace
{
    sheet rich_sheet()
    {
        sheet s;
        part r{"R", {-200, 100}, 1, true, "R1", "4k7", "resistor", "volt-resistors:r-4k7", {}};
        r.extra.set("locked", json::value(true));
        r.extra.set("fields", json::value(json::object{{"Tolerance", "1%"}}));
        s.parts.insert(3, r);
        s.parts.insert(10, part{"Conn_01x04", {1000, -300}, 3, false, "J1", "", "", "", {}});
        s.parts.insert(11, part{"Mystery", {0, 0}, 2, true, "", "", "", "", {}});

        wire w{{0, 0}, {100, 0}, {}};
        w.extra.set("width", json::value(2));
        s.wires.insert(1, w);
        s.wires.insert(7, wire{{-2'000'000'000, 5}, {300, 2'000'000'000}, {}});

        s.labels.insert(5, label{label_kind::net, {0, 0}, 0, "VCC", {}});
        s.labels.insert(6, label{label_kind::power, {100, -100}, 3, "GND", {}});
        label note{label_kind::note, {500, 500}, 2, "\xCE\xA9 \xC2\xB5 \"quoted\"\nsecond line", {}};
        note.extra.set("size", json::value(1.5));
        s.labels.insert(8, note);

        s.extra.set("title", json::value("Main"));
        s.extra.set("view", json::value(json::array{json::value(1), json::value(2)}));
        return s;
    }

    void round_trips()
    {
        for (const sheet &s : {sheet{}, rich_sheet()})
        {
            const std::string text = write_sheet(s);
            const auto loaded = parse_sheet(text);
            CHECK(loaded.has_value());
            if (!loaded)
                continue;
            CHECK(loaded->report.clean());
            CHECK(loaded->format_version == sheet_format_version);
            CHECK(loaded->sheet == s);
            CHECK(write_sheet(loaded->sheet) == text);
            CHECK(loaded->report.records_loaded == s.parts.size() + s.wires.size() + s.labels.size());
        }

        // What it looks like.
        const std::string text = write_sheet(rich_sheet());
        for (const char *expected : {"\"format\": \"volt-schematic\"", "\"format_version\": 1", "\"id\": 3",
                                     "\"position\": [-200, 100]", "\"rotation\": 90", "\"rotation\": 270",
                                     "\"mirrored\": true", "\"library_part\": \"volt-resistors:r-4k7\"",
                                     "\"kind\": \"power\"", "\"kind\": \"note\"", "\"title\": \"Main\""})
            CHECK(text.find(expected) != std::string::npos);
        // Empty device and library part are left out.
        CHECK(text.find("\"device\": \"\"") == std::string::npos && text.find("\"library_part\": \"\"") == std::string::npos);
        // Tables in order: parts, wires, labels, then unknown fields; records
        // in table order.
        CHECK(text.find("\"parts\"") < text.find("\"wires\"") && text.find("\"wires\"") < text.find("\"labels\""));
        CHECK(text.find("\"labels\"") < text.find("\"title\""));
        CHECK(text.find("\"id\": 3") < text.find("\"id\": 10") && text.find("\"id\": 10") < text.find("\"id\": 11"));
        CHECK(text.back() == '\n');
    }

    void unknown_fields()
    {
        const std::string text = R"({
  "format": "volt-schematic",
  "zeta": 1,
  "format_version": 1,
  "alpha": {"nested": [1, 2, 3]},
  "parts": [
    {"id": 1, "z_custom": "x", "symbol": "R", "a_custom": [true], "reference": "R1", "value": "1k",
     "position": [0, 0], "rotation": 0, "mirrored": false}
  ],
  "wires": [{"q": null, "id": 2, "a": [0, 0], "b": [100, 0]}],
  "labels": [{"id": 3, "kind": "net", "text": "A", "position": [100, 0], "rotation": 0, "font": "mono"}]
})";
        const auto loaded = parse_sheet(text);
        CHECK(loaded.has_value());
        if (!loaded)
            return;
        CHECK(loaded->report.clean());
        const sheet &s = loaded->sheet;

        const auto keys = [](const json::object &o) {
            std::vector<std::string> names;
            for (const json::member &m : o)
                names.push_back(m.key);
            return names;
        };
        CHECK(keys(s.extra) == (std::vector<std::string>{"zeta", "alpha"}));
        CHECK(keys(s.parts.find(1)->extra) == (std::vector<std::string>{"z_custom", "a_custom"}));
        CHECK(keys(s.wires.find(2)->extra) == std::vector<std::string>{"q"} && s.wires.find(2)->extra.find("q")->is_null());
        CHECK(keys(s.labels.find(3)->extra) == std::vector<std::string>{"font"});
        CHECK(s.parts.find(1)->value == "1k" && s.labels.find(3)->text == "A");

        // Written back after the known fields, in the order read.
        const std::string written = write_sheet(s);
        CHECK(written.find("\"z_custom\"") < written.find("\"a_custom\""));
        CHECK(written.find("\"mirrored\"") < written.find("\"z_custom\""));
        CHECK(written.find("\"zeta\"") < written.find("\"alpha\"") && written.find("\"labels\"") < written.find("\"zeta\""));
        CHECK(written.find("\"nested\": [1, 2, 3]") != std::string::npos);
        const auto again = parse_sheet(written);
        CHECK(again && again->sheet == s && write_sheet(again->sheet) == written);

        // An extra field named like a known one is not written twice.
        sheet odd = s;
        part p = *odd.parts.find(1);
        p.extra.set("symbol", json::value("ignored"));
        odd.parts.replace(1, p);
        odd.extra.set("parts", json::value(5));
        const std::string odd_text = write_sheet(odd);
        CHECK(parse_sheet(odd_text).has_value());
        CHECK(odd_text.find("ignored") == std::string::npos);
    }

    void failures()
    {
        for (const char *text : {
                 "",
                 "[1, 2]",
                 "{\"format\": \"volt-project\", \"format_version\": 1}",
                 "{\"format_version\": 1}",
                 "{\"format\": \"volt-schematic\", \"format_version\": 0}",
                 "{\"format\": \"volt-schematic\", \"format_version\": \"1\"}",
                 "{\"format\": \"volt-schematic\", \"format_version\": 1, \"parts\": {}}",
                 "{\"format\": \"volt-schematic\", \"format_version\": 1, \"wires\": 5}",
                 "{\"format\": \"volt-schematic\", \"format_version\": 1, \"labels\": \"x\"}",
                 "{\"format\": \"volt-schematic\", \"format_version\": 1,}",
                 "{\"format\": \"volt-schematic\", \"format\": \"volt-schematic\"}",
             })
        {
            const auto loaded = parse_sheet(text);
            CHECK(!loaded.has_value());
            if (!loaded)
                CHECK(!loaded.error().empty());
        }
        const auto bad_json = parse_sheet("{\n  \"format\": \"volt-schematic\",\n}");
        CHECK(!bad_json && bad_json.error().find("line 3") != std::string::npos);
        const auto not_sheet = parse_sheet("{\"format\": \"volt-library\"}");
        CHECK(!not_sheet && not_sheet.error().find("not a Volt schematic") != std::string::npos);
    }

    void problems()
    {
        const std::string text = R"({"format": "volt-schematic", "format_version": 1,
 "parts": [
   5,
   {"symbol": "R"},
   {"id": 0, "symbol": "R"},
   {"id": -3, "symbol": "R"},
   {"id": 1.5, "symbol": "R"},
   {"id": 4294967296, "symbol": "R"},
   {"id": "7", "symbol": "R"},
   {"id": 1, "symbol": 5, "reference": "R1", "value": ["x"], "position": [1.5, 0], "rotation": 45, "mirrored": "yes"},
   {"id": 2, "symbol": "C", "position": [100, 200], "rotation": -90},
   {"id": 3, "symbol": "C", "rotation": 450, "device": null}
 ],
 "wires": [
   {"id": 1, "a": [0, 0], "b": [100, 100]},
   {"id": 10, "a": [0, 0]},
   {"id": 11, "a": [0, 0], "b": [3000000000, 0]},
   {"id": 12, "a": [0, 0], "b": [100, 0]}
 ],
 "labels": [
   {"id": 20, "kind": "bus", "text": "D[0..7]", "position": [0, 0]},
   {"id": 21, "text": "N", "position": [0, 0]},
   {"id": 22, "kind": 7},
   {"id": 23, "kind": "power", "text": "GND", "position": [0, 0], "rotation": 180}
 ]})";
        const auto loaded = parse_sheet(text);
        CHECK(loaded.has_value());
        if (!loaded)
            return;
        const load_report &report = loaded->report;
        const sheet &s = loaded->sheet;

        CHECK(report.records_loaded == 6 && report.records_skipped == 12);
        const auto skipped = std::ranges::count_if(report.problems, [](const auto &p) { return p.skipped; });
        const auto kept = std::ranges::count_if(report.problems, [](const auto &p) { return !p.skipped; });
        CHECK(skipped == 12);
        CHECK(kept == 8); // part 1: six; part 3: no position; label 21: no kind

        CHECK(s.parts.size() == 3 && s.wires.size() == 1 && s.labels.size() == 2);
        const part *one = s.parts.find(1);
        CHECK(one && one->symbol.empty() && one->reference == "R1" && one->value.empty() &&
              one->position == point{0, 0} && one->rotation == 0 && !one->mirrored);
        CHECK(s.parts.find(2)->rotation == 3 && s.parts.find(2)->position == point{100, 200});
        CHECK(s.parts.find(3)->rotation == 1 && s.parts.find(3)->device.empty());
        CHECK(s.wires.find(12) && !s.wires.find(1) && !s.wires.find(10));
        CHECK(s.labels.find(21)->kind == label_kind::net && s.labels.find(23)->rotation == 2);

        // Where each problem is.
        const auto at = [&](std::string_view table, std::size_t record) {
            return std::ranges::count_if(report.problems,
                                         [&](const auto &p) { return p.table == table && p.record == record; });
        };
        CHECK(at("parts", 0) == 1 && at("parts", 7) == 6 && at("parts", 8) == 0 && at("wires", 0) == 1);
        CHECK(at("labels", 0) == 1 && at("labels", 3) == 0);
        const std::string summary = report.summary();
        CHECK(summary.find("6 record(s) loaded, 12 skipped, 20 problem(s)") != std::string::npos);
        CHECK(summary.find("parts[7] (id 1): ") != std::string::npos);
        CHECK(summary.find("wires[0] (id 1): the id is used by another record before it (left out)") !=
              std::string::npos);
        CHECK(summary.find("\"bus\"") != std::string::npos);

        // The file as a whole.
        const auto no_version = parse_sheet(R"({"format": "volt-schematic"})");
        CHECK(no_version && no_version->sheet == sheet{} && no_version->report.problems.size() == 1 &&
              no_version->report.problems[0].record == load_report::problem::no_record);
        const auto newer = parse_sheet(R"({"format": "volt-schematic", "format_version": 7, "layers": []})");
        CHECK(newer && newer->format_version == 7 && newer->report.problems.size() == 1);
        CHECK(newer && newer->sheet.extra.contains("layers"));
        CHECK(newer && write_sheet(newer->sheet).find("\"format_version\": 1") != std::string::npos);
        const auto nulls = parse_sheet(R"({"format": "volt-schematic", "format_version": 1, "parts": null})");
        CHECK(nulls && nulls->report.clean() && nulls->sheet.parts.empty());
    }

    void files()
    {
        const std::filesystem::path directory = std::filesystem::temp_directory_path() / "volt_schematic_io_tests";
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        std::filesystem::create_directories(directory, error);
        const std::u8string name = u8"Schaltplan Ω" + std::u8string(u8".voltsch");
        const std::filesystem::path path = directory / std::filesystem::path(name);

        const sheet s = rich_sheet();
        CHECK(save_sheet(path, s).has_value());
        const auto loaded = load_sheet(path);
        CHECK(loaded && loaded->sheet == s && loaded->report.clean());

        // Saving again replaces it, and leaves no temporary file behind.
        sheet changed = s;
        changed.wires.erase(1);
        CHECK(save_sheet(path, changed).has_value());
        const auto reloaded = load_sheet(path);
        CHECK(reloaded && reloaded->sheet == changed);
        std::size_t entries = 0;
        for ([[maybe_unused]] const auto &entry : std::filesystem::directory_iterator(directory))
            ++entries;
        CHECK(entries == 1);

        // A file that is not there, and one that is not a sheet, name it.
        const auto missing = load_sheet(directory / "missing.voltsch");
        CHECK(!missing.has_value());
        {
            const std::filesystem::path wrong = directory / "wrong.voltsch";
            CHECK(json::write_text_file(wrong, "{\"format\": \"volt-project\"}").has_value());
            const auto refused = load_sheet(wrong);
            CHECK(!refused && refused.error().starts_with("wrong.voltsch: "));
        }
        CHECK(sheet_extension == ".voltsch");
        std::filesystem::remove_all(directory, error);
    }

} // namespace

int main()
{
    round_trips();
    unknown_fields();
    failures();
    problems();
    files();
    return volt::tests::result("schematic io");
}
