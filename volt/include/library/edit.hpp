#pragma once

#include <library/library.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

// -------------------------------------------------------------------------
// editing a library
// -------------------------------------------------------------------------
//
// Changing a library in memory: making one, adding, replacing and removing
// parts, and the files parts bring with them. These keep what loading
// checks (ids valid and unique, names present, file names unique), so that
// a library changed through them saves as a file that loads clean. Saving
// is save_library (<library/library.hpp>).
//
//   library::library mine = library::new_library("my-parts", "My Parts");
//   library::part p;
//   p.name = "10k 1% 0603";
//   p.mpn = "RC0603FR-0710KL";
//   auto added = library::add_part(mine, std::move(p));   // given an id
//   library::save_library(directory / "my-parts.voltlib", mine);
//
// Nothing here touches a catalog: a catalog is a finished value
// (<library/catalog.hpp>). Change the library, save it, and build or rescan
// the catalog.

namespace volt::library
{

    // An empty library of that name. The name must be a valid id.
    library new_library(std::string_view name, std::string_view title = {});

    // ---- ids -----------------------------------------------------------------
    //
    // A part's id is its identity: given once, when the part is made, and
    // never changed, so that whatever refers to the part (a project's copy,
    // through its origin; a schematic, later) goes on finding it when its
    // name, value or MPN is corrected. That is why an id is not made from
    // any of those: it is generated, and says nothing about the part.
    //
    // Where a part comes from somewhere that already has a permanent
    // identifier for it (a distributor's order code), that may be given as
    // its id instead: it is as stable as a generated one, and lets the same
    // part be found again when it is imported a second time.

    // A new id that the library has no part of: "p-" and twelve letters and
    // digits chosen at random ("p-7k3f9x2m4qha").
    std::string generate_id(const library &lib);

    // Text made into a valid id: ':' and control characters become '_',
    // space at the ends goes. Empty when nothing is left. For identifiers
    // that come from elsewhere; not for making ids out of names.
    std::string make_id(std::string_view text);

    // ---- parts ---------------------------------------------------------------

    // What to do with a part whose id the library already has.
    enum class on_conflict
    {
        fail,    // an error
        skip,    // keep the one there
        replace, // take the new one, in the old one's place
        rename,  // add the new one under a new id (generate_id)
    };

    enum class add_action
    {
        added,
        replaced,
        skipped,
    };

    struct added_part
    {
        std::string id; // in the library; the one already there, when skipped
        add_action action = add_action::added;
    };

    // Adds a part. One without an id is given one (generate_id); one
    // without a name is named after its MPN, or else its id; its category
    // is normalized. An id that cannot be one is an error, as is a conflict
    // under on_conflict::fail.
    std::expected<added_part, std::string> add_part(library &lib, part p, on_conflict conflict = on_conflict::fail);

    // Puts `p` in the place of the part `id`, under that id: p.id is `id`
    // or empty. Any other is an error, since a part's id does not change.
    std::expected<void, std::string> replace_part(library &lib, std::string_view id, part p);

    // Removes the part; false if there was none. Its files stay (other
    // parts may name them): see remove_unused_files.
    bool remove_part(library &lib, std::string_view id);

    // ---- files ---------------------------------------------------------------

    // Adds a file and returns the name it has in the library: its own,
    // unless the library has a different file by that name, in which case
    // "-2", "-3", ... goes before the extension. A file the library has
    // already (same name, same bytes) is not added twice. The name must be
    // valid (valid_file_name).
    std::string add_file(library &lib, embedded_file file);

    // A file from disk, named `name`, or after the file when that is empty.
    std::expected<embedded_file, std::string> read_file(const std::filesystem::path &path, std::string name = {});

    // Copies into `to` the files `p` names that are among `from` (its
    // model), and updates the names in `p` where a file had to be renamed
    // there. Names that are not among `from` are left alone: they are paths
    // in the models directory.
    void carry_files(library &to, std::span<const embedded_file> from, part &p);

    // Removes the files no part names; returns how many.
    std::size_t remove_unused_files(library &lib);

    // Writes an embedded file below a directory, for code that reads
    // models from disk: <directory>/<hash of the bytes>/<file name>, so that
    // a file is written once however often it is asked for, and two files
    // of one name do not collide. Returns the path.
    std::expected<std::filesystem::path, std::string> extract_file(const embedded_file &file,
                                                                   const std::filesystem::path &directory);

} // namespace volt::library
