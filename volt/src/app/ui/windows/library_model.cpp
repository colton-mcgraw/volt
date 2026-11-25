#include <app/ui/windows/library_model.hpp>

#include <library/schema.hpp>
#include <math/electrical/engineering.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <span>

namespace volt::ui
{
    namespace
    {
        using steady = std::chrono::steady_clock;

        double milliseconds_since(steady::time_point start)
        {
            return std::chrono::duration<double, std::milli>(steady::now() - start).count();
        }

        constexpr char fold(char c) noexcept
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        constexpr bool is_digit(char c) noexcept
        {
            return c >= '0' && c <= '9';
        }

        bool contains_folded(std::string_view haystack, std::string_view needle) noexcept
        {
            if (needle.empty())
                return true;
            if (needle.size() > haystack.size())
                return false;
            for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
            {
                std::size_t j = 0;
                while (j < needle.size() && fold(haystack[i + j]) == fold(needle[j]))
                    ++j;
                if (j == needle.size())
                    return true;
            }
            return false;
        }

        // A value read as a quantity for sorting: "4.7k" before "10k". Text
        // that is not a quantity sorts after every number.
        std::optional<double> quantity_of(const library::part &p)
        {
            if (p.value.empty())
                return std::nullopt;
            library::parameter value;
            value.value = p.value;
            return value.number();
        }

        void add_categories(scope_node &into, const library::category_node &node, const std::string &library)
        {
            for (const library::category_node &child : node.children)
            {
                scope_node made;
                made.label = child.name;
                made.library = library;
                made.category = child.path;
                made.parts = child.parts;
                add_categories(made, child, library);
                into.children.push_back(std::move(made));
            }
        }
    } // namespace

    std::string_view column_title(part_column column) noexcept
    {
        switch (column)
        {
        case part_column::name:
            return "Name";
        case part_column::value:
            return "Value";
        case part_column::package:
            return "Package";
        case part_column::manufacturer:
            return "Manufacturer";
        case part_column::mpn:
            return "MPN";
        case part_column::library:
            return "Library";
        }
        return "";
    }

    int natural_compare(std::string_view a, std::string_view b) noexcept
    {
        std::size_t i = 0;
        std::size_t j = 0;
        while (i < a.size() && j < b.size())
        {
            if (is_digit(a[i]) && is_digit(b[j]))
            {
                // Two runs of digits: by value, which is by length once
                // leading zeros are skipped, then digit by digit.
                std::size_t ia = i;
                std::size_t jb = j;
                while (ia < a.size() && a[ia] == '0')
                    ++ia;
                while (jb < b.size() && b[jb] == '0')
                    ++jb;
                std::size_t ea = ia;
                std::size_t eb = jb;
                while (ea < a.size() && is_digit(a[ea]))
                    ++ea;
                while (eb < b.size() && is_digit(b[eb]))
                    ++eb;
                if (ea - ia != eb - jb)
                    return ea - ia < eb - jb ? -1 : 1;
                for (std::size_t k = 0; k < ea - ia; ++k)
                {
                    if (a[ia + k] != b[jb + k])
                        return a[ia + k] < b[jb + k] ? -1 : 1;
                }
                i = ea;
                j = eb;
                continue;
            }
            const char ca = fold(a[i]);
            const char cb = fold(b[j]);
            if (ca != cb)
                return static_cast<unsigned char>(ca) < static_cast<unsigned char>(cb) ? -1 : 1;
            ++i;
            ++j;
        }
        if (i < a.size())
            return 1;
        if (j < b.size())
            return -1;
        return 0;
    }

    // ---- part_list --------------------------------------------------------------------------

    void part_list::set_catalog(std::shared_ptr<const library::catalog> catalog)
    {
        m_catalog = std::move(catalog);
        search();
    }

    bool part_list::set_filter(part_filter filter)
    {
        if (filter == m_filter)
            return false;
        m_filter = std::move(filter);
        search();
        return true;
    }

    void part_list::set_sort(std::optional<part_column> column, bool ascending)
    {
        if (column == m_sort && ascending == m_ascending)
            return;
        m_sort = column;
        m_ascending = ascending;
        sort();
    }

    std::size_t part_list::total() const noexcept
    {
        return m_catalog ? m_catalog->part_count() : 0;
    }

    const library::part &part_list::part(std::size_t row) const noexcept
    {
        return m_catalog->get(m_rows[row]);
    }

    const library::library &part_list::library_of(std::size_t row) const noexcept
    {
        return m_catalog->library_of(m_rows[row]);
    }

    std::string part_list::qualified_id(std::size_t row) const
    {
        return m_catalog->qualified_id(m_rows[row]);
    }

    std::string_view part_list::cell(std::size_t row, part_column column) const noexcept
    {
        const library::part &p = part(row);
        switch (column)
        {
        case part_column::name:
            return p.name.empty() ? std::string_view(p.id) : std::string_view(p.name);
        case part_column::value:
            return p.value;
        case part_column::package:
            return p.package;
        case part_column::manufacturer:
            return p.manufacturer;
        case part_column::mpn:
            return p.mpn;
        case part_column::library:
            return library_of(row).name;
        }
        return {};
    }

    std::string_view part_list::specs(std::size_t row) const
    {
        if (m_specs.size() != m_rows.size())
        {
            m_specs.assign(m_rows.size(), {});
            m_specs_made.assign(m_rows.size(), false);
        }
        if (!m_specs_made[row])
        {
            m_specs[row] = spec_summary(part(row));
            m_specs_made[row] = true;
        }
        return m_specs[row];
    }

    std::optional<std::size_t> part_list::find(std::string_view qualified_id) const
    {
        if (!m_catalog || qualified_id.empty())
            return std::nullopt;
        const auto ref = m_catalog->find(qualified_id);
        if (!ref)
            return std::nullopt;
        const auto found = std::ranges::find(m_rows, *ref);
        if (found == m_rows.end())
            return std::nullopt;
        return static_cast<std::size_t>(found - m_rows.begin());
    }

    std::vector<library::part_ref> part_list::run_search(const library::catalog &catalog, const part_filter &filter)
    {
        library::search_query query;
        query.text = filter.text;
        query.library = filter.library;
        query.category = filter.category;
        library::search_results results = catalog.search(query);
        std::vector<library::part_ref> ranked;
        ranked.reserve(results.hits.size());
        for (const library::search_hit &hit : results.hits)
            ranked.push_back(hit.ref);
        return ranked;
    }

    bool part_list::set_results(const std::shared_ptr<const library::catalog> &catalog, part_filter filter,
                                std::vector<library::part_ref> ranked, double milliseconds)
    {
        if (catalog != m_catalog)
            return false; // made for a catalog that has been replaced since
        m_filter = std::move(filter);
        m_ranked = std::move(ranked);
        m_search_ms = milliseconds;
        sort();
        return true;
    }

    void part_list::search()
    {
        m_ranked.clear();
        m_rows.clear();
        m_search_ms = 0.0;
        if (!m_catalog)
            return;

        const auto start = steady::now();
        m_ranked = run_search(*m_catalog, m_filter);
        m_search_ms = milliseconds_since(start);
        sort();
    }

    void part_list::sort()
    {
        const auto start = steady::now();
        m_rows = m_ranked;
        m_specs.clear();
        m_specs_made.clear();
        if (m_sort && m_catalog)
        {
            const library::catalog &c = *m_catalog;
            const part_column column = *m_sort;
            const bool ascending = m_ascending;

            if (column == part_column::value)
            {
                // By quantity, read once per row rather than per comparison;
                // values that are not quantities after, by their text.
                std::vector<std::pair<double, std::uint32_t>> keys;
                keys.reserve(m_rows.size());
                for (std::uint32_t i = 0; i < m_rows.size(); ++i)
                {
                    const auto q = quantity_of(c.get(m_rows[i]));
                    keys.emplace_back(q ? *q : std::numeric_limits<double>::infinity(), i);
                }
                std::stable_sort(keys.begin(), keys.end(), [&](const auto &a, const auto &b) {
                    if (a.first != b.first)
                        return ascending ? a.first < b.first : a.first > b.first;
                    if (std::isinf(a.first))
                    {
                        const int order = natural_compare(c.get(m_rows[a.second]).value, c.get(m_rows[b.second]).value);
                        return ascending ? order < 0 : order > 0;
                    }
                    return false;
                });
                std::vector<library::part_ref> sorted;
                sorted.reserve(m_rows.size());
                for (const auto &key : keys)
                    sorted.push_back(m_rows[key.second]);
                m_rows = std::move(sorted);
            }
            else
            {
                const auto text = [&](library::part_ref ref) -> std::string_view {
                    const library::part &p = c.get(ref);
                    switch (column)
                    {
                    case part_column::name:
                        return p.name.empty() ? std::string_view(p.id) : std::string_view(p.name);
                    case part_column::package:
                        return p.package;
                    case part_column::manufacturer:
                        return p.manufacturer;
                    case part_column::mpn:
                        return p.mpn;
                    case part_column::library:
                        return c.library_of(ref).name;
                    case part_column::value:
                        break;
                    }
                    return {};
                };
                // Empty cells last either way: a column sorted by
                // manufacturer starts with manufacturers.
                std::stable_sort(m_rows.begin(), m_rows.end(), [&](library::part_ref a, library::part_ref b) {
                    const std::string_view ta = text(a);
                    const std::string_view tb = text(b);
                    if (ta.empty() != tb.empty())
                        return tb.empty();
                    const int order = natural_compare(ta, tb);
                    return ascending ? order < 0 : order > 0;
                });
            }
        }
        m_sort_ms = milliseconds_since(start);
    }

    // ---- scope_tree -------------------------------------------------------------------------

    std::vector<scope_node> scope_tree(const library::catalog &catalog)
    {
        std::vector<scope_node> roots;

        scope_node all;
        all.label = "All libraries";
        const library::category_node every = catalog.categories();
        all.parts = every.parts;
        add_categories(all, every, {});
        roots.push_back(std::move(all));

        for (const library::library &lib : catalog.libraries())
        {
            scope_node node;
            node.label = std::string(lib.display_name());
            node.library = lib.name;
            const library::category_node own = catalog.categories(lib.name);
            node.parts = own.parts;
            add_categories(node, own, lib.name);
            roots.push_back(std::move(node));
        }
        return roots;
    }

    // ---- project rows -------------------------------------------------------------------------

    std::string status_text(std::optional<library::part_status> status)
    {
        if (!status)
            return "Checking\xE2\x80\xA6";
        switch (*status)
        {
        case library::part_status::unlinked:
            return "Made in project";
        case library::part_status::source_missing:
            return "Source missing";
        case library::part_status::up_to_date:
            return "Up to date";
        case library::part_status::source_changed:
            return "Source changed";
        case library::part_status::locally_modified:
            return "Locally modified";
        case library::part_status::diverged:
            return "Diverged";
        }
        return {};
    }

    bool update_loses_edits(library::part_status status) noexcept
    {
        return status == library::part_status::locally_modified || status == library::part_status::diverged;
    }

    std::vector<project_row> project_rows(const library::project &project, const library::catalog *catalog)
    {
        std::vector<project_row> rows;
        rows.reserve(project.parts.parts.size());
        for (const library::part &p : project.parts.parts)
        {
            project_row row;
            row.id = p.id;
            row.name = p.name.empty() ? p.id : p.name;
            row.value = p.value;
            row.package = p.package;
            if (p.origin)
                row.source = p.origin->library + ":" + p.origin->part;
            if (catalog != nullptr)
                row.status = library::status_of(p, *catalog);
            else if (!p.origin)
                row.status = library::part_status::unlinked;
            row.status_text = status_text(row.status);
            rows.push_back(std::move(row));
        }
        std::stable_sort(rows.begin(), rows.end(),
                         [](const project_row &a, const project_row &b) { return natural_compare(a.name, b.name) < 0; });
        return rows;
    }

    bool matches(const project_row &row, const library::part *part, std::string_view text)
    {
        std::size_t at = 0;
        while (at < text.size())
        {
            while (at < text.size() && text[at] == ' ')
                ++at;
            std::size_t end = at;
            while (end < text.size() && text[end] != ' ')
                ++end;
            const std::string_view term = text.substr(at, end - at);
            at = end;
            if (term.empty())
                continue;
            bool found = contains_folded(row.name, term) || contains_folded(row.value, term) ||
                         contains_folded(row.package, term) || contains_folded(row.id, term) ||
                         contains_folded(row.source, term);
            if (!found && part != nullptr)
                found = contains_folded(part->mpn, term) || contains_folded(part->manufacturer, term) ||
                        contains_folded(part->description, term);
            if (!found)
                return false;
        }
        return true;
    }

    std::vector<std::size_t> project_matches(const library::project &project, const std::vector<project_row> &rows,
                                             const library::catalog *catalog, std::string_view text)
    {
        std::vector<std::pair<std::uint32_t, std::size_t>> scored;
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            const library::part *p = project.parts.find(rows[i].id);
            if (catalog == nullptr || p == nullptr)
            {
                if (matches(rows[i], p, text))
                    scored.emplace_back(1u, i);
                continue;
            }
            if (const std::uint32_t score = catalog->match(*p, text))
                scored.emplace_back(score, i);
        }
        std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        std::vector<std::size_t> shown;
        shown.reserve(scored.size());
        for (const auto &s : scored)
            shown.push_back(s.second);
        return shown;
    }

    // ---- specs ------------------------------------------------------------------------------------

    std::string quantity_text(const library::quantity &q)
    {
        std::string text = math::electrical::format_engineering(q.value);
        text += library::unit_symbol(q.unit);
        return text;
    }

    namespace
    {
        // A parameter of the part as the specs show it, or empty: a
        // quantity in engineering notation, else its own text.
        std::string spec_value(const library::part &p, std::string_view name)
        {
            const library::parameter *q = library::schema::builtin().find_parameter(p, name);
            if (q == nullptr)
                return {};
            // In the base unit, written afresh ("0.1" "W" as "100mW"); with a
            // prefix of the library's own ("0.25" "pF"), or in another unit,
            // or text, as written ("100ppm/K", "X7R").
            const auto value = library::parameter_quantity(*q);
            if (value && value->unit != library::quantity_unit::none && q->min.empty() && q->max.empty() &&
                q->unit == library::unit_symbol(value->unit))
                return quantity_text(*value);
            return q->text();
        }

        struct spec
        {
            std::string_view parameter; // looked up through the schema
            std::string_view before;    // "±", "Vf ", "Zener "
            std::string_view after;     // " LED"
        };

        // What each device shows, in order.
        constexpr spec resistor_specs[] = {{"Resistance", "", ""},
                                           {"Tolerance", "\xC2\xB1", ""},
                                           {"Power rating", "", ""},
                                           {"Temperature coefficient", "", ""}};
        constexpr spec capacitor_specs[] = {
            {"Capacitance", "", ""}, {"Voltage rating", "", ""}, {"Dielectric", "", ""}, {"Tolerance", "\xC2\xB1", ""}};
        constexpr spec inductor_specs[] = {{"Inductance", "", ""},
                                           {"Current rating", "", ""},
                                           {"Saturation current", "Isat ", ""},
                                           {"DC resistance", "DCR ", ""},
                                           {"Tolerance", "\xC2\xB1", ""}};
        constexpr spec zener_specs[] = {
            {"Zener voltage", "Zener ", ""}, {"Tolerance", "\xC2\xB1", ""}, {"Power rating", "", ""}};
        constexpr spec led_specs[] = {
            {"Color", "", " LED"}, {"Forward voltage", "Vf ", ""}, {"Forward current", "If ", ""}};
        constexpr spec diode_specs[] = {
            {"Forward voltage", "Vf ", ""}, {"Forward current", "If ", ""}, {"Reverse voltage", "Vr ", ""}};
        constexpr spec bjt_specs[] = {{"Type", "", ""},
                                      {"Collector-emitter voltage", "Vce ", ""},
                                      {"Collector current", "Ic ", ""},
                                      {"hFE", "hFE ", ""}};
        constexpr spec mosfet_specs[] = {{"Type", "", ""},
                                         {"Drain-source voltage", "Vds ", ""},
                                         {"Drain current", "Id ", ""},
                                         {"Rds(on)", "Rds(on) ", ""}};

        std::string join(const std::vector<std::string> &items)
        {
            const bool spaced = std::ranges::any_of(items, [](const std::string &s) {
                return s.find(' ') != std::string::npos;
            });
            std::string text;
            for (const std::string &item : items)
            {
                if (!text.empty())
                    text += spaced ? " \xC2\xB7 " : " ";
                text += item;
            }
            return text;
        }

        // The part's value first, when it is a part number or a name rather
        // than a quantity some spec shows anyway.
        void add_value(std::vector<std::string> &items, const library::part &p)
        {
            if (!p.value.empty())
                items.push_back(p.value);
        }
    } // namespace

    std::string spec_summary(const library::part &p)
    {
        const library::device_definition *d = library::schema::builtin().find_device(p.device);
        const std::string_view device = d != nullptr ? std::string_view(d->id) : std::string_view(p.device);

        std::span<const spec> specs;
        bool value_first = true;
        if (device == library::devices::resistor)
            specs = resistor_specs;
        else if (device == library::devices::capacitor)
            specs = capacitor_specs;
        else if (device == library::devices::inductor)
            specs = inductor_specs;
        else if (device == library::devices::zener)
            specs = zener_specs;
        else if (device == library::devices::led)
            specs = led_specs;
        else if (device == library::devices::diode)
            specs = diode_specs;
        else if (device == library::devices::bjt)
            specs = bjt_specs;
        else if (device == library::devices::mosfet)
            specs = mosfet_specs;

        std::vector<std::string> items;
        if (!specs.empty())
        {
            // Passives and zeners are their value; an LED its colour.
            value_first = device == library::devices::diode || device == library::devices::bjt ||
                          device == library::devices::mosfet;
            if (value_first)
                add_value(items, p);
            bool principal_shown = false;
            for (const spec &s : specs)
            {
                std::string value = spec_value(p, s.parameter);
                if (value.empty())
                    continue;
                if (&s == &specs.front())
                    principal_shown = true;
                // A "+" or "-" already says ±.
                const bool signed_text = !value.empty() && (value[0] == '+' || value[0] == '-');
                items.push_back((signed_text && s.before == "\xC2\xB1" ? std::string{} : std::string(s.before)) +
                                value + std::string(s.after));
            }
            // A passive that states no value parameter: its value text.
            if (!value_first && !principal_shown && !p.value.empty() && device != library::devices::led)
                items.insert(items.begin(), p.value);
            if (device == library::devices::led && !principal_shown && !p.value.empty())
                items.insert(items.begin(), p.value + " LED");
            return join(items);
        }

        // Anything else: the value, then the first parameters.
        add_value(items, p);
        std::size_t shown = 0;
        for (const library::parameter &q : p.parameters)
        {
            if (shown == 3)
                break;
            const std::string text = q.text();
            if (q.name.empty() || text.empty())
                continue;
            items.push_back(q.name + " " + text);
            ++shown;
        }
        return join(items);
    }

    std::string parameter_summary(const library::part &p)
    {
        std::string text;
        for (const library::parameter &q : p.parameters)
        {
            const std::string value = q.text();
            if (q.name.empty() && value.empty())
                continue;
            if (!text.empty())
                text += " \xC2\xB7 ";
            text += q.name;
            if (!value.empty())
                text += (q.name.empty() ? "" : " ") + value;
        }
        return text;
    }

    std::string maker_text(const library::part &p)
    {
        if (p.manufacturer.empty())
            return p.mpn;
        if (p.mpn.empty())
            return p.manufacturer;
        return p.manufacturer + " " + p.mpn;
    }

    // ---- details --------------------------------------------------------------------------------

    std::vector<std::pair<std::string, std::string>> part_fields(const library::part &p)
    {
        std::vector<std::pair<std::string, std::string>> fields;
        const auto add = [&](std::string_view name, const std::string &value) {
            if (!value.empty())
                fields.emplace_back(std::string(name), value);
        };
        add("Value", p.value);
        add("Package", p.package);
        add("Manufacturer", p.manufacturer);
        add("MPN", p.mpn);
        add("Category", p.category);
        add("Prefix", p.prefix);
        add("Device", p.device);
        add("Symbol", p.symbol);
        add("Footprint", p.footprint);
        add("ID", p.id);
        if (p.origin)
        {
            add("Copied from", p.origin->library + ":" + p.origin->part);
            add("Source version", p.origin->version);
        }
        return fields;
    }

    std::string with_break_points(std::string_view text)
    {
        std::string result;
        result.reserve(text.size() + text.size() / 4);
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            result += c;
            const bool sensible = c == ':' || c == '_' || c == '/';
            if (sensible && i + 1 < text.size() && text[i + 1] != ' ')
                result += "\xE2\x80\x8B"; // U+200B ZERO WIDTH SPACE
        }
        return result;
    }

    std::filesystem::path model_path(const library::part &part, const std::filesystem::path &models)
    {
        if (part.model.empty())
            return {};
        return models / library::path_from_utf8(part.model);
    }

} // namespace volt::ui
