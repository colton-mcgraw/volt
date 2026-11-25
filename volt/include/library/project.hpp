#pragma once

#include <library/catalog.hpp>
#include <library/library.hpp>

#include <json/json.hpp>

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// project
// -------------------------------------------------------------------------
//
// A .voltproj file: what a design consists of. Its sheets and board are
// file references: a sheet is a .voltsch file (<schematic/io.hpp>), which
// the application opens and saves along with the project (app_services,
// <app/ui/services.hpp>); the board has no document model yet. What the
// project file does hold is the project library, the parts the design
// uses, copied out of the catalog so that the project is self-contained:
// it opens the same on a machine without the libraries it was drawn from,
// and does not change when those libraries do. Every copied part records
// its origin (library, part id, the library's version, and the part's hash
// when copied), so that a copy that differs from its source can be found
// and, if wanted, updated.
//
//   {
//     "format": "volt-project",
//     "format_version": 1,
//     "name": "Blinky",
//     "sheets": [
//       {"name": "Main", "file": "Blinky.voltsch"}
//     ],
//     "board": "Blinky.voltpcb",
//     "library": "Blinky.voltlib"
//   }
//
// The project library is a sibling .voltlib file, not embedded in the
// project file. It is an ordinary library, so everything that reads a
// library reads it (the browser can show it, it can be copied into another
// project or promoted to a user library, and it can be shared alone); the
// project file stays a short list of settings that diffs on its own; and a
// change to the parts is a diff of the library file only. The price is two
// files instead of one, which a project already is (sheets, board).
//
// Settings that change what the design means live here too, so that it
// means the same to everyone who opens it, whatever their preferences: the
// schematic's connectivity rules (<schematic/sheet.hpp>), under
// "schematic". Each is written only when it differs from its default, so
// that a project using the defaults has no "schematic" at all:
//
//     "schematic": {"pins_connect_along_wires": false}
//
// Paths in the file are relative to the project file's directory, UTF-8,
// with forward slashes, so a project moves between machines and systems
// as a directory. A path that cannot be made relative (another drive) is
// stored absolute, still with forward slashes.

namespace volt::library
{

    inline constexpr std::string_view project_format = "volt-project";
    inline constexpr int project_format_version = 1;
    inline constexpr std::string_view project_extension = ".voltproj";

    struct sheet
    {
        std::string name;
        std::string file; // relative to the project file
        json::object extra;

        bool operator==(const sheet &) const = default;
    };

    // The project's schematic settings (see the top of this file).
    struct schematic_settings
    {
        // A pin lying inside a wire, not at its end, connects to it
        // (schematic::connectivity::pins_inside_wires).
        bool pins_connect_along_wires = true;

        // Fields of "schematic" this version does not know, written back.
        json::object extra;

        bool operator==(const schematic_settings &) const = default;
    };

    struct project
    {
        std::string name;
        std::vector<sheet> sheets;
        schematic_settings schematic;
        std::string board;   // relative to the project file; empty for none
        std::string library_file; // the project library's file, relative to the project file

        // The project library itself, loaded from and saved to library_file.
        volt::library::library parts;

        json::object extra;

        int format_version = project_format_version;

        bool operator==(const project &other) const;
    };

    struct loaded_project
    {
        project project;
        // Problems with the project file and its library together.
        load_report report;
    };

    // A new project in memory: one sheet, a board, and an empty project
    // library, their files named after the project ("Blinky.voltsch",
    // "Blinky.voltpcb", "Blinky.voltlib"). Characters that are not allowed
    // in file names on some system become '_'. Nothing is written until
    // save_project.
    project new_project(std::string_view name);

    // Reads the project file and its library. A missing library file is a
    // problem reported, not a failure: the project opens with an empty
    // library (and saving creates the file).
    std::expected<loaded_project, std::string> load_project(const std::filesystem::path &path);

    // Writes the project library and then the project file, each through a
    // temporary file and a rename. The sheet and board files are not
    // touched here: the application writes the sheets itself, before this
    // (app_services::save_project()).
    std::expected<void, std::string> save_project(const std::filesystem::path &path, const project &p);

    // The .voltproj text alone, and back (the library is not included).
    std::string write_project(const project &p);
    std::expected<loaded_project, std::string> parse_project(std::string_view text);

    // ---- the project library ---------------------------------------------------

    // Copies a catalog part into the project library, recording its origin,
    // and returns its id there. The id is the part's own, unless the project
    // already has a different part by that id (from another library), in
    // which case "-2", "-3", ... is appended. Adding a part that is already
    // there (same origin) changes nothing and returns its id.
    std::expected<std::string, std::string> add_part(project &p, const catalog &source, part_ref ref);

    // Removes the part from the project library; false if there was none.
    bool remove_part(project &p, std::string_view id);

    enum class part_status
    {
        unlinked,         // no origin: made in the project
        source_missing,   // its library, or the part in it, is not in the catalog
        up_to_date,       // the same as its source
        source_changed,   // the source has changed since it was copied
        locally_modified, // the copy has been edited since
        diverged,         // both
    };

    std::string_view describe(part_status status) noexcept;

    // How a copied part compares with its source in the catalog.
    part_status status_of(const part &copy, const catalog &source);

    // The part of the project library by id, compared with its source.
    // nullopt when the project has no such part.
    std::optional<part_status> status_of(const project &p, std::string_view id, const catalog &source);

    // Replaces the copy with the current source part, keeping its id in the
    // project, and records the new origin. Local edits are lost; check
    // status_of first to warn about them.
    std::expected<void, std::string> update_from_source(project &p, std::string_view id, const catalog &source);

    // ---- paths -------------------------------------------------------------------

    // A path from UTF-8 text, and back as UTF-8 with forward slashes. A
    // std::filesystem::path made from a std::string reads it in the
    // system's narrow encoding, which on Windows is not UTF-8; these go
    // through std::u8string instead.
    std::filesystem::path path_from_utf8(std::string_view text);
    std::string path_to_utf8(const std::filesystem::path &path);

    // target relative to the directory, as stored in a project file:
    // "sheets/main.voltsch", "../shared/parts.voltlib". When there is no
    // relative path (another drive), the absolute one.
    std::string relative_path(const std::filesystem::path &directory, const std::filesystem::path &target);

    // A stored path made absolute against the directory (lexically
    // normalized); an absolute stored path is returned as it is.
    std::filesystem::path resolve_path(const std::filesystem::path &directory, std::string_view stored);

} // namespace volt::library
