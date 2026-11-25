#pragma once

#include <library/edit.hpp>
#include <library/library.hpp>
#include <library/schema.hpp>

#include <json/json.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// part packages
// -------------------------------------------------------------------------
//
// A .voltpart file: parts handed to Volt, whole. It is how a part gets
// from anywhere into a library: from another person, from one library to
// another, and from a tool that fetches a manufacturer's catalog and
// translates it. One file holds the part and everything it needs, and
// importing it puts all of that into one library file:
//
//   {
//     "format": "volt-parts",
//     "format_version": 1,
//     "generator": "acme-fetch 1.2",
//     "parts": [
//       {
//         "name": "10kΩ 1% 0603",
//         "manufacturer": "Yageo",
//         "mpn": "RC0603FR-0710KL",
//         "device": "resistor",
//         "model": "R_0603.step",
//         "parameters": [
//           {"name": "Resistance", "value": "10k", "unit": "Ω"}
//         ]
//       }
//     ],
//     "files": [
//       {"name": "R_0603.step", "path": "downloads/R_0603.step"}
//     ]
//   }
//
// A part is written as in a library (<library/library.hpp>), except that
// it needs no id. A part's id is its permanent identity (<library/edit.hpp>)
// and is not made from its name or MPN: a tool whose source has a permanent
// identifier for the part (a distributor's order code) gives that as the
// id, e.g. "lcsc-C25804", and importing the part again then finds it; a
// part without an id is given a generated one, and is found again by its
// manufacturer and MPN together, when it has both. A file is given by its
// data, as in a library ("encoding" and "data"), or by a "path" relative to
// the package file, which is read when the package is loaded, so that a
// tool need not encode what it downloaded. A .voltlib file is read as a
// package too: its parts and files.
//
// Importing checks every part against the schema (<library/schema.hpp>)
// and reports, per part, what was done with it and what is wrong with it:
// the parameter that is misspelled, the one a simulation needs and will not
// find. A tool gets the same report as JSON from `volt_parts import --json`.

namespace volt::library
{

    inline constexpr std::string_view package_format = "volt-parts";
    inline constexpr int package_format_version = 1;
    inline constexpr std::string_view package_extension = ".voltpart";

    struct part_package
    {
        // Who wrote it, for messages: "acme-fetch 1.2". May be empty.
        std::string generator;
        std::vector<part> parts; // ids may be empty
        std::vector<embedded_file> files;
        json::object extra;

        bool operator==(const part_package &) const = default;
    };

    struct loaded_package
    {
        part_package package;
        // Problems with the file; a part's position is its position in
        // the file's "parts".
        load_report report;
    };

    // From text. A file given by a "path" is read relative to `directory`;
    // with no directory it is a problem reported and the file is left out.
    std::expected<loaded_package, std::string> parse_package(std::string_view text,
                                                             const std::filesystem::path &directory = {});

    // From a .voltpart (or .voltlib) file.
    std::expected<loaded_package, std::string> load_package(const std::filesystem::path &path);

    // The .voltpart text, deterministic; every file is written as data.
    std::string write_package(const part_package &package);
    std::expected<void, std::string> save_package(const std::filesystem::path &path, const part_package &package);

    // A package of one part of a library, with the files it names.
    std::expected<part_package, std::string> export_part(const library &lib, std::string_view id);

    // ---- importing -------------------------------------------------------------

    struct import_options
    {
        // A part whose id the library has already. One that is the same
        // as the part there is never a conflict: it is reported unchanged.
        on_conflict conflict = on_conflict::skip;
        // Rewrite known parameters as the schema writes them
        // (schema::normalize) before the part goes in.
        bool normalize = true;
        // Leave out parts the schema finds errors in, rather than import
        // them with the errors reported.
        bool strict = false;
    };

    enum class import_action
    {
        added,
        replaced,
        unchanged, // the library has this very part already
        skipped,   // the library has another part of this id, and keeps it
        rejected,  // not imported: see reason
    };

    std::string_view describe(import_action action) noexcept; // "added", ...

    struct imported_part
    {
        std::size_t index = 0; // in the package
        // Its id in the library; for a part not imported, the id it came
        // with, which may be none.
        std::string id;
        // What to call it in a message: its MPN, else its name, else its
        // id, else "part 3".
        std::string label;
        import_action action = import_action::added;
        std::string reason; // why it was skipped or rejected
        std::vector<issue> issues;
    };

    struct import_report
    {
        std::vector<imported_part> parts; // one per part of the package, in order
        std::size_t added = 0;
        std::size_t replaced = 0;
        std::size_t unchanged = 0;
        std::size_t skipped = 0;
        std::size_t rejected = 0;
        std::size_t errors = 0;   // issues, over every part
        std::size_t warnings = 0;

        // Whether the library is different now.
        bool changed() const noexcept { return added + replaced > 0; }

        // A few lines for a log: the counts, then one line per part that
        // was not simply added, and per issue.
        std::string summary() const;
    };

    // Puts the package's parts, and the files they name, into the library.
    // Nothing is saved: that is save_library, when changed().
    import_report import_parts(library &target, const part_package &package, const schema &rules,
                               const import_options &options = {});

    // {"added": 1, ..., "parts": [{"index", "id", "label", "action",
    //  "reason", "issues": [...]}]}
    json::value import_report_to_json(const import_report &report);

} // namespace volt::library
