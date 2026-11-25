#pragma once

#include <library/catalog.hpp>
#include <library/project.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// -------------------------------------------------------------------------
// library views' model
// -------------------------------------------------------------------------
//
// What the Library Browser and the parts panel show, worked out apart from
// any widget, so that it can be tested without a window:
//
//   part_list        the parts library scope: a catalog, a filter (search
//                    text, library, category) and an order, made into rows;
//                    finds a part's row again by its qualified id, also in
//                    the next catalog after a rescan
//   scope_tree       the libraries and their categories, with counts, for
//                    the tree that restricts the list
//   project_rows     the project library's parts with their status against
//                    the catalog, and the project scope's search
//   spec_summary     the few specs that tell parts of a device apart, for
//                    a list row
//   part_fields      a part's fields as name and value, for the details
//
// Interface thread; nothing here locks. A part_list keeps the catalog it
// shows alive (shared_ptr) and its rows are part_refs into it, valid until
// the next set_catalog().

namespace volt::ui
{

    // The parts list's columns, in order.
    enum class part_column : std::uint8_t
    {
        name,
        value,
        package,
        manufacturer,
        mpn,
        library,
    };

    inline constexpr std::size_t part_column_count = 6;

    // "Name", "Value", ...
    std::string_view column_title(part_column column) noexcept;

    // Compares as people read: ignoring ASCII case, and runs of digits by
    // their value ("R2" before "R10"). Returns <0, 0 or >0.
    int natural_compare(std::string_view a, std::string_view b) noexcept;

    // ---- the parts library scope ---------------------------------------------------

    struct part_filter
    {
        std::string text;     // search terms (catalog::search)
        std::string library;  // a library's name; empty for all
        std::string category; // a category path, and below; empty for all

        bool operator==(const part_filter &) const = default;
    };

    class part_list
    {
    public:
        // The catalog to show, or null. Runs the filter and the order again.
        void set_catalog(std::shared_ptr<const library::catalog> catalog);
        const library::catalog *catalog() const noexcept { return m_catalog.get(); }
        const std::shared_ptr<const library::catalog> &shared_catalog() const noexcept { return m_catalog; }

        // Runs the search again if the filter differs; returns whether it
        // did.
        bool set_filter(part_filter filter);
        const part_filter &filter() const noexcept { return m_filter; }

        // By a column, or (nullopt) as the search ranks them: by relevance
        // with search text, else in catalog order.
        void set_sort(std::optional<part_column> column, bool ascending);
        std::optional<part_column> sort_column() const noexcept { return m_sort; }
        bool sort_ascending() const noexcept { return m_ascending; }

        // A search done elsewhere (on another thread, since a catalog is
        // read-only and safe to search from any): the hits of `filter` in
        // `catalog`, ranked. set_results() takes them as set_filter() would
        // have found them; false, and nothing changes, when `catalog` is not
        // the one shown any more.
        static std::vector<library::part_ref> run_search(const library::catalog &catalog, const part_filter &filter);
        bool set_results(const std::shared_ptr<const library::catalog> &catalog, part_filter filter,
                         std::vector<library::part_ref> ranked, double milliseconds);

        // The rows, in order.
        std::size_t size() const noexcept { return m_rows.size(); }
        // Every part in the catalog.
        std::size_t total() const noexcept;

        library::part_ref ref(std::size_t row) const noexcept { return m_rows[row]; }
        const library::part &part(std::size_t row) const noexcept;
        const library::library &library_of(std::size_t row) const noexcept;
        std::string qualified_id(std::size_t row) const;

        // A cell's text, for the table; valid while the catalog is.
        std::string_view cell(std::size_t row, part_column column) const noexcept;

        // The row's spec_summary(), made when first asked for and kept
        // until the rows change: valid until the next filter, sort or
        // catalog.
        std::string_view specs(std::size_t row) const;

        // The row showing the part of that qualified id, if it is shown.
        std::optional<std::size_t> find(std::string_view qualified_id) const;

        // How long the last search and sort took, milliseconds.
        double search_milliseconds() const noexcept { return m_search_ms; }
        double sort_milliseconds() const noexcept { return m_sort_ms; }

    private:
        void search();
        void sort();

        std::shared_ptr<const library::catalog> m_catalog;
        part_filter m_filter;
        std::optional<part_column> m_sort;
        bool m_ascending = true;

        // In search order (relevance, or catalog order), and as shown.
        std::vector<library::part_ref> m_ranked;
        std::vector<library::part_ref> m_rows;
        // specs() of the rows asked for so far (only those shown are).
        mutable std::vector<std::string> m_specs;
        mutable std::vector<bool> m_specs_made;
        double m_search_ms = 0.0;
        double m_sort_ms = 0.0;
    };

    // ---- the libraries and categories ------------------------------------------------

    struct scope_node
    {
        std::string label;    // "Volt Resistors", "Chip"
        std::string library;  // the library's name; empty for every library
        std::string category; // the category path; empty for the whole library
        std::size_t parts = 0;
        std::vector<scope_node> children;
    };

    // "All libraries" with every category below it, then each library with
    // its own. Categories are in name order, libraries in catalog order.
    std::vector<scope_node> scope_tree(const library::catalog &catalog);

    // ---- the project library ---------------------------------------------------------

    struct project_row
    {
        std::string id; // in the project library
        std::string name;
        std::string value;
        std::string package;
        std::string source; // "volt-resistors:p-7k3f9x2m4qha", or "" when made in the project
        // nullopt while there is no catalog to compare with.
        std::optional<library::part_status> status;
        std::string status_text;
    };

    // The project's parts, by name (natural order), compared with `catalog`
    // (null while it loads).
    std::vector<project_row> project_rows(const library::project &project, const library::catalog *catalog);

    // "Up to date", "Source changed", ... as the browser says it; "Checking…"
    // without a status.
    std::string status_text(std::optional<library::part_status> status);

    // Whether updating from the source would lose edits made in the
    // project.
    bool update_loses_edits(library::part_status status) noexcept;

    // The parts whose text (name, value, package, MPN, manufacturer, id)
    // has every term of `text`, ignoring ASCII case: the project scope's
    // search without a catalog, which is small enough to need no index.
    bool matches(const project_row &row, const library::part *part, std::string_view text);

    // The project scope's search: the positions in `rows` of the parts that
    // match `text`, best first (ties in the rows' order). With a catalog,
    // as catalog::match() scores them, so values, packages and ratings
    // mean what they do in the catalog's search; without one, matches().
    std::vector<std::size_t> project_matches(const library::project &project, const std::vector<project_row> &rows,
                                             const library::catalog *catalog, std::string_view text);

    // ---- specs ---------------------------------------------------------------------------

    // What tells parts of the part's device apart, compact and in a fixed
    // order, for a list row:
    //
    //   resistor    resistance, tolerance, power, temperature coefficient
    //               "1kΩ ±1% 100mW 100ppm/K"
    //   capacitor   capacitance, voltage, dielectric, tolerance
    //               "100nF 50V X7R ±10%"
    //   inductor    inductance, current, saturation current, DCR,
    //               tolerance: "10µH 1.2A DCR 50mΩ ±20%"
    //   zener       "Zener 5.1V ±5% 250mW"
    //   LED         "Red LED · Vf 2V · If 20mA"
    //   diode       the part number, Vf, If, Vr: "1N4148 · If 200mA · Vr 100V"
    //   BJT         "BC547B · NPN · Vce 45V · Ic 100mA" (and hFE)
    //   MOSFET      "2N7002 · N-channel · Vds 60V · Id 115mA" (and Rds(on))
    //   other       the value, then the first three parameters as "name
    //               value": "1x4 · Positions 4 · Rows 1 · Pitch 2.54mm"
    //
    // Only what the part states; quantities in engineering notation with
    // their unit and no space ("62.5mW"). Items are separated by a space
    // when none has one inside, else by " · ". Empty when nothing is known.
    // The package, manufacturer and MPN are not in it: see maker_text().
    std::string spec_summary(const library::part &part);

    // Every parameter as "name value", in order, joined by " · ": the
    // details strip's line. "Resistance 1kΩ · Tolerance 1% · ...".
    std::string parameter_summary(const library::part &part);

    // "Yageo RC0603FR-071KL"; either alone; empty without both.
    std::string maker_text(const library::part &part);

    // A quantity as the specs write it: "1kΩ", "62.5mW", "1%", "4.7µF".
    std::string quantity_text(const library::quantity &q);

    // ---- details -----------------------------------------------------------------------

    // The part's fields that have a value, as name and value, in the order
    // the details show them. Parameters, keywords, the datasheet and the
    // model are left to their own sections.
    std::vector<std::pair<std::string, std::string>> part_fields(const library::part &part);

    // `text` with places to break a line added where a long value without
    // spaces can be broken sensibly: after ':', '_' and '/' (a footprint
    // "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", a path), as a zero width
    // space, which the text layout breaks after and draws as nothing. Not
    // before a space or at the end, where a line breaks anyway.
    std::string with_break_points(std::string_view text);

    // The 3D model file a part names, under the models directory; empty
    // when it names none.
    std::filesystem::path model_path(const library::part &part, const std::filesystem::path &models);

} // namespace volt::ui
