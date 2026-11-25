#pragma once

#include <library/part.hpp>

#include <json/json.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// library
// -------------------------------------------------------------------------
//
// A named, versioned collection of parts, kept as a .voltlib file: JSON,
// pretty-printed with one field per line, so that a change to a part is a
// change to a few lines in a diff.
//
//   {
//     "format": "volt-library",
//     "format_version": 1,
//     "name": "volt-resistors",
//     "title": "Volt Resistors",
//     "description": "Thick film chip resistors, E24 and E96",
//     "version": "2026.11.0",
//     "author": "Volt",
//     "license": "CC0-1.0",
//     "parts": [
//       {
//         "id": "p-7k3f9x2m4qha",
//         "name": "10kΩ 1% 0603",
//         "category": "Passives/Resistors/Chip",
//         "manufacturer": "Yageo",
//         "mpn": "RC0603FR-0710KL",
//         "value": "10k",
//         "package": "0603",
//         "prefix": "R",
//         "symbol": "R",
//         "footprint": "Resistor_SMD:R_0603_1608Metric",
//         "keywords": ["resistor", "chip", "smd"],
//         "parameters": [
//           {"name": "Resistance", "value": "10k", "unit": "Ω"},
//           {"name": "Tolerance", "value": "1", "unit": "%"}
//         ]
//       }
//     ]
//   }
//
// Empty fields are left out. A part copied from another library also has
// "origin": {"library", "part", "version", "hash"}. Fields that are not
// known are kept (part::extra, library::extra) and written back after the
// known ones.
//
// A library can also carry the files its parts need, so that a part and
// everything that draws it are in one place and travel together:
//
//     "files": [
//       {"name": "R_0603.step", "encoding": "text", "data": "ISO-10303-21;..."}
//     ]
//
// A part names such a file where it would otherwise name one in the models
// directory ("model": "R_0603.step"); the library's own file is looked for
// first. Text files (STEP, OBJ) are stored as text, anything else as
// base64. Several parts may name the same file.
//
// Loading is forgiving per part and strict per file: a file that is not
// JSON, or not a Volt library, fails; a part that is broken (no id, an id
// seen before, not an object) is left out and reported, and the rest load.

namespace volt::library
{

    inline constexpr std::string_view library_format = "volt-library";
    inline constexpr int library_format_version = 1;
    inline constexpr std::string_view library_extension = ".voltlib";

    // A file a library carries for its parts: a 3D model today.
    struct embedded_file
    {
        // What parts call it: a relative path with forward slashes, unique
        // in its library (see valid_file_name).
        std::string name;
        std::string data; // the file's bytes

        json::object extra;

        bool operator==(const embedded_file &) const = default;
    };

    // Whether text can name an embedded file: a relative path of forward
    // slashes with no empty, "." or ".." segment, no ':' or backslash and
    // no control characters, so that it can be written below any directory.
    bool valid_file_name(std::string_view name) noexcept;

    // A 64-bit FNV-1a hash of the bytes, as 16 hex digits.
    std::string content_hash(std::string_view bytes);

    // An embedded file as the JSON object the files hold, and back. Reading
    // returns nothing, with the reason in `problem`, for an entry that is
    // not a file (no name, data that is not base64, ...).
    json::value file_to_json(const embedded_file &file);
    std::optional<embedded_file> file_from_json(const json::value &v, std::string &problem);

    struct library
    {
        // The identifier: the part before ':' in a fully qualified id. Same
        // rules as a part id (valid_id).
        std::string name;
        std::string title; // for people; name when empty
        std::string description;
        // The library's own version, chosen by whoever publishes it.
        // Recorded in the origin of parts copied from it.
        std::string version;
        std::string author;
        std::string license;
        std::string url;

        std::vector<part> parts;

        // The files the library carries for its parts, by name.
        std::vector<embedded_file> files;

        json::object extra;

        // Where it was loaded from; not part of the file.
        std::filesystem::path path;

        // The format version the file was written in; saving always writes
        // library_format_version.
        int format_version = library_format_version;

        // title, or name when there is none.
        std::string_view display_name() const noexcept;

        // A part by id: a linear search, fine for a project's tens of parts.
        // The catalog has an index for large libraries.
        const part *find(std::string_view id) const noexcept;
        part *find(std::string_view id) noexcept;

        // An embedded file by name (exact match), or nullptr.
        const embedded_file *find_file(std::string_view file_name) const noexcept;

        bool operator==(const library &other) const;
    };

    // What a load did not manage. A library that loads with problems still
    // loads; the parts that could not be read are left out, and this says
    // which and why.
    struct load_report
    {
        struct problem
        {
            static constexpr std::size_t no_part = static_cast<std::size_t>(-1);

            // The file the problem is in, for reports covering several.
            std::filesystem::path file;
            // The part's position in the file's "parts" array, from 0, or
            // no_part for a problem with the file as a whole.
            std::size_t part = no_part;
            std::string id; // the part's id when it has one
            std::string message;
            // Whether the part was left out (otherwise it loaded, with a
            // field missing or ignored).
            bool skipped = false;
        };

        std::vector<problem> problems;
        std::size_t parts_loaded = 0;
        std::size_t parts_skipped = 0;
        double seconds = 0.0;

        bool clean() const noexcept { return problems.empty(); }

        // A few lines for a log: a count, then one line per problem,
        // "volt-resistors.voltlib: part 12 (p-7k3f9x2m4qha): duplicate id".
        std::string summary() const;

        void add(problem p);
        // A problem with the file as a whole.
        void add(std::string message);
        // A problem with one part, by position (and id, when it has one).
        void add(std::size_t part, std::string id, std::string message, bool skipped = false);
        void merge(const load_report &other);
    };

    struct loaded_library
    {
        library library;
        load_report report;
    };

    // From text. name_hint names a library whose file has no name (the
    // file's stem, when loading a file).
    std::expected<loaded_library, std::string> parse_library(std::string_view text, std::string_view name_hint = {});

    // From a .voltlib file. Errors name the file, and the line and column
    // for JSON that does not parse.
    std::expected<loaded_library, std::string> load_library(const std::filesystem::path &path);

    // The .voltlib text: deterministic, so saving an unchanged library
    // writes the same bytes.
    std::string write_library(const library &lib);
    json::value library_to_json(const library &lib);

    // Saves through a temporary file and a rename, so a failed save leaves
    // the previous file as it was.
    std::expected<void, std::string> save_library(const std::filesystem::path &path, const library &lib);

} // namespace volt::library
