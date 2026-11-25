#pragma once

#include <library/library.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// -------------------------------------------------------------------------
// catalog
// -------------------------------------------------------------------------
//
// Every library the application knows, together, with what a browser needs
// on top: lookup by fully qualified id ("volt-resistors:p-7k3f9x2m4qha"),
// the category tree with counts, and ranked search.
//
// Threading. A catalog is a plain value with no locks and no hidden caches:
// every const member function only reads, so any number of threads may
// call them on one catalog at once, and none may while another modifies
// it. The intended use is to build one off the interface thread and hand it
// over finished:
//
//   // worker thread
//   library::scan_report report;
//   auto built = std::make_shared<const library::catalog>(
//       library::load_catalog(directories, report));
//   // ... post `built` (and the report) to the interface thread, which
//   // swaps it in for the catalog it was showing.
//
// The interface thread keeps a std::shared_ptr<const catalog> and reads and
// searches it freely; a rescan builds a new catalog rather than changing
// the one being shown. A part_ref is an index into one catalog: it is valid
// for that catalog object (and its unchanged copies) and is invalidated by
// add() and remove(), so keep refs together with the catalog they came
// from, and use the qualified id to find a part again in a new catalog.

namespace volt::library
{

    // A part in a catalog: library and part by position.
    struct part_ref
    {
        std::uint32_t library = 0;
        std::uint32_t part = 0;

        bool operator==(const part_ref &) const = default;
        auto operator<=>(const part_ref &) const = default;
    };

    // ---- quantities ------------------------------------------------------------
    //
    // What search reads a value as. The units it knows are the electrical
    // ones a query names; a number in any other unit ("100" "ppm/K", "3mm")
    // is text to it.

    enum class quantity_unit : std::uint8_t
    {
        none, // a bare number: "1k", "2"
        ohm,
        farad,
        henry,
        volt,
        ampere,
        watt,
        hertz,
        percent,
    };

    struct quantity
    {
        double value = 0.0; // in plain SI units: 1000 for "1k" Ω, 1e-7 for "100n" F, 1 for "1" %
        quantity_unit unit = quantity_unit::none;

        bool operator==(const quantity &) const = default;
    };

    // "Ω", "F", "H", "V", "A", "W", "Hz", "%"; empty for none.
    std::string_view unit_symbol(quantity_unit unit) noexcept;

    // The same value, to a part in a million: "4k7" and "4.7k", "100n" and
    // "0.1u".
    bool same_value(double a, double b) noexcept;

    // A parameter's value as a quantity: "62.5m" "W" is 0.0625 W, "0.25"
    // "pF" is 2.5e-13 F, "1" "%" is 1 %, "100nF" with no unit is 1e-7 F. The
    // typical value, or the middle of the limits when only they are given.
    // Empty for text ("X7R") and for a unit search does not know ("ppm/K").
    std::optional<quantity> parameter_quantity(const parameter &q);

    // What a value typed into a search means: "1k", "4k7", "4r7", "100nF",
    // "10uf", "1kohm", "50V", "20mA", "0.1W", "1/10W", "1%", "±1%". Units
    // are read without regard to case, prefixes with it ("10m" is milli,
    // "10M" mega). Empty for anything else, and for a number in a unit
    // search does not know ("3mm", "0710RL").
    std::optional<quantity> query_quantity(std::string_view text);

    // What a part of a device is, by value: its principal parameter and
    // that parameter's unit. A resistor's Resistance, a capacitor's
    // Capacitance, an inductor's Inductance, a zener's Zener voltage. Other
    // devices (a diode, a transistor, an LED: their value is a part number
    // or a colour) and parts of no device have none: an empty name.
    struct principal_parameter
    {
        std::string_view name;
        quantity_unit unit = quantity_unit::none;
    };
    principal_parameter principal_of(std::string_view device);

    // The part's principal value: that parameter (however the part spells
    // it, see <library/schema.hpp>), or else its `value` read as a quantity
    // in that unit ("1k", "100nF", "5.1V"). Empty when the device has none,
    // or the part says neither.
    std::optional<quantity> principal_value(const part &p);

    // ---- search -----------------------------------------------------------------

    struct search_query
    {
        // Terms separated by spaces, all of which must match (AND),
        // case-insensitively; what each term is and where it matches is
        // catalog::search's to say. Empty matches every part, in catalog
        // order, which is how a category is browsed.
        std::string text;
        // Only parts of the library of this name; empty for all.
        std::string library;
        // Only parts in this category or below it ("Passives" includes
        // "Passives/Resistors"); empty for all.
        std::string category;
        // At most this many hits, the best ones; 0 for all of them.
        std::size_t limit = 0;
    };

    struct search_hit
    {
        part_ref ref;
        // Higher is better; only meaningful relative to other hits of the
        // same query.
        std::uint32_t score = 0;

        bool operator==(const search_hit &) const = default;
    };

    struct search_results
    {
        // Best first; equal scores keep shorter names first, then catalog
        // order (the order of the libraries, and of the parts in each file).
        std::vector<search_hit> hits;
        // Every part that matched, which is more than hits.size() when a
        // limit cut the list short.
        std::size_t matched = 0;
    };

    struct category_node
    {
        std::string name; // the last segment: "Resistors"
        std::string path; // the whole path: "Passives/Resistors"; empty at the root
        // Parts in this category and every category below it.
        std::size_t parts = 0;
        // Parts in exactly this category (at the root: parts with none).
        std::size_t own_parts = 0;
        std::vector<category_node> children; // by name

        // The node at a path below this one, or nullptr.
        const category_node *find(std::string_view path) const noexcept;
    };

    class catalog
    {
    public:
        catalog() = default;

        // ---- contents ------------------------------------------------------

        // Adds a library, replacing one of the same name. Its parts are
        // indexed here, so this costs about as much as loading it did.
        // Invalidates part refs.
        void add(library lib);

        // Removes the library of that name; false if there is none.
        // Invalidates part refs.
        bool remove(std::string_view name);

        void clear() noexcept;

        // In the order they were added.
        std::span<const library> libraries() const noexcept { return m_libraries; }
        const library *find_library(std::string_view name) const noexcept;
        std::size_t part_count() const noexcept { return m_part_count; }

        // ---- parts ---------------------------------------------------------

        // ref must come from this catalog.
        const part &get(part_ref ref) const noexcept;
        const library &library_of(part_ref ref) const noexcept;

        // "library:part"
        std::string qualified_id(part_ref ref) const;

        // By fully qualified id. The part id may itself contain no ':', so
        // the first ':' separates the two.
        std::optional<part_ref> find(std::string_view qualified_id) const noexcept;
        std::optional<part_ref> find(std::string_view library_name, std::string_view part_id) const noexcept;

        // ---- browsing ------------------------------------------------------

        // The category tree of every part, or of one library's. The root
        // has an empty name and counts every part.
        category_node categories(std::string_view library_name = {}) const;

        // Ranked search. Every term must match; each is one of three kinds,
        // decided once for the query:
        //
        //   package  a word of some part's package in the catalog ("0603",
        //            "SOT-23", "DIP-8", "1x04"): matches a part whose package
        //            has it as a whole word ("SOT-23" is not in "SOT-23-5"),
        //            else whose footprint has it between '_' or ':' and '_'
        //            ("Resistor_SMD:R_0603_1608Metric"). A word with a digit in
        //            it is a package code, and a part that names another
        //            package or footprint does not match it (though its text
        //            may say "SOT-23", a SOT-23-5 part is not one); otherwise
        //            it is matched as text, like any other word, and never as
        //            a number ("0603" is not 603 Ω).
        //   value    a quantity (query_quantity(): "1k", "4k7", "100nF",
        //            "50V", "0.1W", "1/10W", "1%"). On a part of a device that
        //            has a principal value (principal_value(): a resistor's
        //            resistance, ...) a value with no unit, or in that one's
        //            unit, matches only that, numerically: "1k" finds the
        //            1 kΩ resistors and not 11k, 5.1k or an MPN with "1K" in
        //            it; "100n" and "0.1u" find the same capacitors. The one
        //            exception is a number of four digits or more with nothing
        //            after it, which may also start a word of the MPN. A value
        //            in another unit matches a parameter in that unit: "50V"
        //            a capacitor's voltage rating, "1%" a tolerance, "0.1W" a
        //            power rating, "20mA" an LED's forward current; a part
        //            that states parameters in that unit and none of that
        //            value does not match. A part that states nothing in the
        //            unit, or has no principal value (a diode, an IC, a part
        //            of no device), is matched as text, and its `value` as a
        //            quantity written differently ("4k7" finds "4.7k" there).
        //   text     anything else: matched in the text fields, as below.
        //
        // Text ranks by where and how it occurs:
        //
        //   fields, best first: name and value, mpn, package, keywords,
        //   manufacturer, description;
        //   how, best first: the whole field ("10k" is the value), a whole
        //   word, the start of a word ("lm3" in "LM358"), anywhere inside (terms of
        //   three characters or more only: "lm" does not match inside "film").
        //
        // A term's score, best first: a principal value; a parameter's
        // value or the package; the part's `value` as a quantity (which reads
        // SI prefixes with their case, so "10m" puts 10 mΩ ahead of 10 MΩ);
        // then text. A part's score is the sum over the terms.
        // Case is ignored for ASCII letters, and µ and μ are read as u.
        // Over tens of thousands of parts this takes milliseconds: fast
        // enough to run on every keystroke.
        search_results search(const search_query &query) const;

        // How well one part, which need not be in the catalog (a project
        // library's part, a candidate list's), matches `text` as search()
        // would score it; 0 when it does not. The package terms are this
        // catalog's packages and the part's own.
        std::uint32_t match(const part &p, std::string_view text) const;

    private:
        // What search looks at for one part, prepared when its library is
        // added: the searchable fields folded to lower case, one after the
        // other in the library's text buffer, then the footprint (matched
        // only by package terms); the part's principal value, and its
        // parameters that are quantities (in the library's `quantities`).
        struct entry
        {
            std::uint32_t text = 0;              // offset into the library's buffer
            std::uint16_t field_end[7] = {};     // end of each field, relative to text
            std::uint16_t footprint_end = 0;     // relative to text; starts after field_end[6] + 1
            std::uint32_t category = 0;          // into m_categories
            std::uint16_t name_length = 0;
            bool has_number = false;
            double number = 0.0;                 // the value as a quantity
            char unit[6] = {};                   // its unit, folded; empty for none
            bool has_principal = false;
            quantity principal;
            std::uint32_t quantities = 0;        // the first of them in the library's `quantities`
            std::uint16_t quantity_count = 0;
        };

        struct string_hash
        {
            using is_transparent = void;
            std::size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
        };

        struct index
        {
            std::string text;
            std::vector<entry> entries;
            std::vector<quantity> quantities;
            std::unordered_map<std::string, std::uint32_t, string_hash, std::equal_to<>> ids;
            // The words of its parts' packages, folded ("0603", "sot-23",
            // "1x04", "p2.54mm"), for telling a package term.
            std::unordered_map<std::string, std::uint32_t, string_hash, std::equal_to<>> package_words;
        };

        index build_index(const library &lib);
        static void describe(const part &p, std::string &text, entry &e, std::vector<quantity> &quantities);
        bool is_package_word(std::string_view folded) const;
        std::uint32_t intern_category(std::string_view category);

        std::vector<library> m_libraries;
        std::vector<index> m_indexes; // parallel to m_libraries
        // Every category path seen, so an entry stores a number. Never
        // shrinks; a removed library's categories just have no parts.
        std::vector<std::string> m_categories;
        std::unordered_map<std::string, std::uint32_t, string_hash, std::equal_to<>> m_category_ids;
        std::size_t m_part_count = 0;
    };

    // ---- loading ---------------------------------------------------------------

    // Every .voltlib file under the directories, recursively, each
    // directory's files sorted by path. Missing or unreadable directories
    // are skipped.
    std::vector<std::filesystem::path> discover_libraries(std::span<const std::filesystem::path> directories);

    struct scan_report
    {
        struct file
        {
            std::filesystem::path path;
            bool loaded = false;
            std::string error; // why it was not loaded

            // Opened by the user (scan_options::files) rather than found in
            // a directory.
            bool opened = false;

            // Not loaded because an opened file has a library of the same
            // name, which takes its place: by choice, so not a problem
            // (error says which file). On that opened file's entry, `shadows`
            // is the path of the one it took the place of.
            bool shadowed = false;
            std::filesystem::path shadows;

            load_report report;
        };

        std::vector<file> files;
        std::size_t libraries = 0; // loaded
        std::size_t parts = 0;
        double seconds = 0.0;
        bool cancelled = false;

        // Every file loaded (or shadowed), and without problems.
        bool clean() const noexcept;
        std::string summary() const;
    };

    struct scan_options
    {
        // Called before each file with how many are done and how many there
        // are; returning false stops the scan (what was loaded is kept and
        // report.cancelled is set). Called on the scanning thread.
        std::function<bool(std::size_t done, std::size_t total, const std::filesystem::path &next)> progress;

        // Library files the user opened, in order of priority: loaded
        // before anything the directories hold, so that one of them wins
        // over a library of the same name there, which is reported as
        // shadowed rather than as a failure. Found again in a directory, a
        // file is loaded once, as opened.
        std::vector<std::filesystem::path> files;
    };

    // Discovers and loads every library under the directories, given in
    // order of priority, after the files opened (options.files): when two
    // files have a library of the same name the first one is kept and the
    // other reported (as shadowed, when the first was opened). Touches
    // nothing but the files and its own result, so it can run on any
    // thread.
    catalog load_catalog(std::span<const std::filesystem::path> directories, scan_report &report,
                         const scan_options &options = {});

} // namespace volt::library
