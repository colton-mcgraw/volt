#pragma once

#include <schematic/sheet.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// the sheet file
// -------------------------------------------------------------------------
//
// A .voltsch file: one sheet as deterministic JSON (<json/json.hpp>), in the
// manner of the project and library files (<library/project.hpp>): the
// same sheet always writes the same bytes, so a sheet loaded and saved
// unchanged does not change, and one moved part is one changed line in a
// diff. Records are written in table order, each with its id; fields this
// version does not know, at the top and in every record, are kept (`extra`)
// and written back after the known ones, in the order they were read.
//
//   {
//     "format": "volt-schematic",
//     "format_version": 1,
//     "parts": [
//       {"id": 1, "symbol": "R", "reference": "R1", "value": "10k", "device": "resistor",
//        "position": [-200, 200], "rotation": 90, "mirrored": false}
//     ],
//     "wires": [
//       {"id": 2, "a": [-200, 300], "b": [0, 300]}
//     ],
//     "labels": [
//       {"id": 3, "kind": "power", "text": "GND", "position": [0, -200], "rotation": 0}
//     ]
//   }
//
// Positions are [x, y] in sheet units; rotations are written in degrees,
// counter-clockwise (0, 90, 180, 270), so the file reads without knowing
// what a quarter turn is. "device" and "library_part" are left out when
// empty.
//
// Reading is forgiving where it can be, as loading a library is: a field of
// the wrong type is reported and read as empty (or zero); a record that
// cannot be read at all (not an object, no usable id, an id used before by
// any record, a label of a kind not known) is reported and left out; the
// rest loads. A file that is not JSON, not a Volt sheet, or whose tables
// are not arrays, fails to load.

namespace volt::schematic
{

    inline constexpr std::string_view sheet_format = "volt-schematic";
    inline constexpr int sheet_format_version = 1;
    inline constexpr std::string_view sheet_extension = ".voltsch";

    // What a load did not manage.
    struct load_report
    {
        struct problem
        {
            static constexpr std::size_t no_record = static_cast<std::size_t>(-1);

            // The table the record is in ("parts", "wires", "labels"), and
            // its position in it, from 0; empty and no_record for a problem
            // with the file as a whole.
            std::string table;
            std::size_t record = no_record;
            item_id id = 0; // the record's id when it has one
            std::string message;
            // Whether the record was left out (otherwise it loaded, with a
            // field missing or ignored).
            bool skipped = false;
        };

        std::vector<problem> problems;
        std::size_t records_loaded = 0;
        std::size_t records_skipped = 0;

        bool clean() const noexcept { return problems.empty(); }

        // A few lines for a log: a count, then one line per problem,
        // "parts[3] (id 12): \"value\" should be a string, not a number".
        std::string summary() const;
    };

    struct loaded_sheet
    {
        schematic::sheet sheet;
        load_report report;
        // The version the file was written in; saving writes
        // sheet_format_version.
        int format_version = sheet_format_version;
    };

    // The .voltsch text of a sheet, and back.
    std::string write_sheet(const sheet &s);
    json::value sheet_to_json(const sheet &s);
    std::expected<loaded_sheet, std::string> parse_sheet(std::string_view text);

    // From a file; errors name the file, and the line and column for JSON
    // that does not parse.
    std::expected<loaded_sheet, std::string> load_sheet(const std::filesystem::path &path);

    // Saves through a temporary file and a rename (json::write_text_file),
    // so a failed save leaves the previous file as it was.
    std::expected<void, std::string> save_sheet(const std::filesystem::path &path, const sheet &s);

} // namespace volt::schematic
