#include <library/catalog.hpp>

#include <library/schema.hpp>
#include <math/electrical/engineering.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <set>

namespace volt::library
{

    namespace
    {
        // The searchable fields, in the order they sit in an entry's text,
        // and what a match in each is worth. Name and value are what people
        // type; the description is long and matches by accident.
        enum field : std::size_t
        {
            field_name,
            field_value,
            field_mpn,
            field_package,
            field_keywords,
            field_manufacturer,
            field_description,
            field_count,
        };
        constexpr std::uint32_t field_weight[field_count] = {8, 8, 7, 6, 5, 4, 2};

        // And how: the whole field, a whole word, a word's start, inside.
        constexpr std::uint32_t match_exact = 8;
        constexpr std::uint32_t match_word = 5;
        constexpr std::uint32_t match_prefix = 3;
        constexpr std::uint32_t match_inside = 1;
        constexpr std::uint32_t best_score = match_exact * 8;

        // Between fields in an entry's text. Folding turns control
        // characters into spaces, so no term contains it and no match spans
        // two fields.
        constexpr char separator = '\x1f';

        // Each field is cut to this many bytes, so an entry's offsets fit in
        // 16 bits. A description longer than that is not searched past it.
        constexpr std::size_t max_field_bytes = 4096;

        // Lower case for ASCII, the micro sign and Greek mu as 'u' (so
        // "10uF" finds "10µF"), control characters as spaces.
        void fold_append(std::string &out, std::string_view text, std::size_t limit = std::string_view::npos)
        {
            const std::size_t end = std::min(text.size(), limit);
            for (std::size_t i = 0; i < end; ++i)
            {
                const char c = text[i];
                if (c >= 'A' && c <= 'Z')
                    out += static_cast<char>(c - 'A' + 'a');
                else if (i + 1 < text.size() &&
                         ((c == '\xC2' && text[i + 1] == '\xB5') || (c == '\xCE' && text[i + 1] == '\xBC')))
                {
                    out += 'u';
                    ++i;
                }
                else if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
                    out += ' ';
                else
                    out += c;
            }
        }

        std::string fold(std::string_view text)
        {
            std::string out;
            out.reserve(text.size());
            fold_append(out, text);
            return out;
        }

        bool is_word_char(char c) noexcept
        {
            const auto u = static_cast<unsigned char>(c);
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || u >= 0x80;
        }

        bool same_quantity(double a, double b) noexcept
        {
            return std::abs(a - b) <= 1e-9 * std::max(std::abs(a), std::abs(b));
        }

        // A unit folded into a fixed buffer; false when it does not fit.
        bool store_unit(std::string_view unit, char (&out)[6])
        {
            const std::string folded = fold(unit);
            if (folded.size() >= sizeof out)
                return false;
            std::copy(folded.begin(), folded.end(), out);
            out[folded.size()] = '\0';
            return true;
        }

        bool ascii_less(std::string_view a, std::string_view b) noexcept
        {
            return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
                const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
                return static_cast<unsigned char>(lower(x)) < static_cast<unsigned char>(lower(y));
            });
        }

        std::size_t total_parts(category_node &node)
        {
            std::sort(node.children.begin(), node.children.end(),
                      [](const category_node &a, const category_node &b) { return ascii_less(a.name, b.name); });
            node.parts = node.own_parts;
            for (category_node &child : node.children)
                node.parts += total_parts(child);
            return node.parts;
        }

        std::string utf8(const std::filesystem::path &path)
        {
            const std::u8string text = path.generic_u8string();
            return std::string(text.begin(), text.end());
        }

        bool has_library_extension(const std::filesystem::path &path)
        {
            const std::u8string extension = path.extension().u8string();
            if (extension.size() != library_extension.size())
                return false;
            for (std::size_t i = 0; i < extension.size(); ++i)
            {
                char c = static_cast<char>(extension[i]);
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
                if (c != library_extension[i])
                    return false;
            }
            return true;
        }

        // ---- quantities ---------------------------------------------------------------

        // A unit as libraries write it, exactly: "Ω", "F", "Hz", "%".
        std::optional<quantity_unit> exact_unit(std::string_view unit) noexcept
        {
            if (unit.empty())
                return quantity_unit::none;
            if (unit == "\xCE\xA9" || unit == "\xE2\x84\xA6" || unit == "ohm" || unit == "Ohm" || unit == "ohms" ||
                unit == "Ohms")
                return quantity_unit::ohm;
            if (unit == "F")
                return quantity_unit::farad;
            if (unit == "H")
                return quantity_unit::henry;
            if (unit == "V")
                return quantity_unit::volt;
            if (unit == "A")
                return quantity_unit::ampere;
            if (unit == "W")
                return quantity_unit::watt;
            if (unit == "Hz")
                return quantity_unit::hertz;
            if (unit == "%")
                return quantity_unit::percent;
            return std::nullopt;
        }

        // A unit as people type it: any case, "ohm", "R" and "Ω" for ohms.
        std::optional<quantity_unit> loose_unit(std::string_view unit)
        {
            if (const auto exact = exact_unit(unit))
                return exact;
            if (unit == "\xCF\x89") // ω, a lower-case Ω
                return quantity_unit::ohm;
            const std::string folded = fold(unit);
            if (folded == "r" || folded == "ohm" || folded == "ohms")
                return quantity_unit::ohm;
            if (folded == "f")
                return quantity_unit::farad;
            if (folded == "h")
                return quantity_unit::henry;
            if (folded == "v")
                return quantity_unit::volt;
            if (folded == "a")
                return quantity_unit::ampere;
            if (folded == "w")
                return quantity_unit::watt;
            if (folded == "hz")
                return quantity_unit::hertz;
            return std::nullopt;
        }

        // A parameter's unit, which may carry a prefix of its own ("pF",
        // "mW"): the unit, and what the prefix multiplies by.
        std::optional<std::pair<quantity_unit, double>> prefixed_unit(std::string_view unit)
        {
            if (const auto exact = exact_unit(unit))
                return std::pair{*exact, 1.0};
            int exponent = 0;
            const std::size_t length = math::electrical::detail::read_prefix(unit, exponent);
            if (length == 0 || length == unit.size())
                return std::nullopt;
            const auto rest = exact_unit(unit.substr(length));
            if (!rest || *rest == quantity_unit::none || *rest == quantity_unit::percent)
                return std::nullopt;
            return std::pair{*rest, std::pow(10.0, exponent)};
        }

        // A parameter's text as a number in its unit, or nullopt.
        std::optional<quantity> read_value(std::string_view text, std::string_view unit)
        {
            const auto parsed = math::electrical::parse_engineering(text);
            if (!parsed || !std::isfinite(parsed->value))
                return std::nullopt;
            // The unit written after the number ("100nF"), or beside it.
            const std::string_view written = !parsed->unit.empty() ? parsed->unit : unit;
            const auto u = prefixed_unit(written);
            if (!u)
                return std::nullopt;
            return quantity{parsed->value * u->second, u->first};
        }

        // ---- package words --------------------------------------------------------------

        bool package_separator(char c) noexcept
        {
            return c == ' ' || c == '(' || c == ')';
        }

        // Calls `each` with every word of a folded package worth telling a
        // package term by: one with a letter in it ("sot-23", "1x04"), or of
        // four characters or more ("0603"), so that "1" of "MKDS 1,5/2-5,08"
        // is still a number.
        template <typename F>
        void package_words(std::string_view package, F &&each)
        {
            std::size_t at = 0;
            while (at < package.size())
            {
                while (at < package.size() && package_separator(package[at]))
                    ++at;
                std::size_t end = at;
                while (end < package.size() && !package_separator(package[end]))
                    ++end;
                const std::string_view word = package.substr(at, end - at);
                at = end;
                if (word.empty())
                    continue;
                const bool letter = std::ranges::any_of(word, [](char c) { return c >= 'a' && c <= 'z'; });
                if (letter || word.size() >= 4)
                    each(word);
            }
        }

        // Whether `word` is in `field` with nothing of a word on either side:
        // `before` and `after` say what may be there besides the ends.
        template <typename Before, typename After>
        bool has_word(std::string_view field, std::string_view word, Before &&before, After &&after)
        {
            if (word.empty())
                return false;
            for (std::size_t at = field.find(word); at != std::string_view::npos; at = field.find(word, at + 1))
            {
                const bool start = at == 0 || before(field[at - 1]);
                const bool end = at + word.size() == field.size() || after(field[at + word.size()]);
                if (start && end)
                    return true;
            }
            return false;
        }

        // ---- query terms ------------------------------------------------------------------

        // Scores of the structured matches, against a text match's at most
        // best_score: the part's principal value, a parameter of the unit,
        // the package (and its footprint), the value as a quantity.
        constexpr std::uint32_t principal_score = best_score + 32;
        constexpr std::uint32_t parameter_score = best_score + 8;
        constexpr std::uint32_t package_score = best_score + 8;
        constexpr std::uint32_t footprint_score = best_score + 4;
        constexpr std::uint32_t number_score = best_score + 1;

        enum class term_kind : std::uint8_t
        {
            text,
            value,
            package,
        };

        // One term of a query, prepared once for every part.
        struct term
        {
            std::string text; // folded
            term_kind kind = term_kind::text;
            // As a value term: the quantity it says, and whether it is only
            // digits, four or more (which an MPN may hold). As a package
            // term: whether it has a digit, so names a package itself.
            quantity value;
            bool digits = false;
            // The term as a quantity in any unit, for the part's `value`
            // written differently.
            bool has_number = false;
            double number = 0.0;
            char unit[6] = {};
        };

        // The terms of a query, longest first: the longest is the rarest,
        // so a part that does not match is usually turned away by the
        // first. `is_package` tells a package word, folded.
        template <typename IsPackage>
        std::vector<term> parse_terms(std::string_view text, IsPackage &&is_package)
        {
            std::vector<term> terms;
            std::string_view rest = text;
            while (!rest.empty())
            {
                const std::size_t space = rest.find_first_of(" \t\r\n");
                std::string_view word = rest.substr(0, space);
                if (space == std::string_view::npos)
                    rest = {};
                else
                    rest.remove_prefix(space + 1);
                // "±1%" is "1%".
                if (word.starts_with("\xC2\xB1"))
                    word.remove_prefix(2);
                else if (word.starts_with("+-") || word.starts_with("+/-"))
                    word.remove_prefix(word[1] == '-' ? 2 : 3);
                if (word.empty())
                    continue;
                term t;
                t.text = fold(word);
                if (is_package(std::string_view(t.text)))
                {
                    t.kind = term_kind::package;
                    t.digits = std::ranges::any_of(t.text, [](char c) { return c >= '0' && c <= '9'; });
                }
                else if (const auto q = query_quantity(word))
                {
                    t.kind = term_kind::value;
                    t.value = *q;
                    t.digits = word.size() >= 4 &&
                               std::ranges::all_of(word, [](char c) { return c >= '0' && c <= '9'; });
                }
                if (t.kind != term_kind::package)
                {
                    if (const auto quantity = math::electrical::parse_engineering(word))
                    {
                        if (std::isfinite(quantity->value) && store_unit(quantity->unit, t.unit))
                        {
                            t.has_number = true;
                            t.number = quantity->value;
                        }
                    }
                }
                terms.push_back(std::move(t));
            }
            std::stable_sort(terms.begin(), terms.end(),
                             [](const term &a, const term &b) { return a.text.size() > b.text.size(); });
            return terms;
        }

        // The best text match of a term in an entry's fields, 0 for none;
        // in one field only when `only` is given (field_count for all), and
        // there at the start of a word at least when `word_start` is set.
        template <typename Entry>
        std::uint32_t text_score(const Entry &e, std::string_view text, const term &t, std::size_t only = field_count,
                                 bool word_start_only = false)
        {
            std::uint32_t best = 0;
            const std::size_t length = t.text.size();
            std::size_t at = text.find(t.text);
            while (at != std::string_view::npos)
            {
                std::size_t f = 0;
                while (at >= e.field_end[f])
                    ++f;
                const std::size_t start = f == 0 ? 0 : e.field_end[f - 1] + std::size_t{1};
                const std::size_t end = e.field_end[f];
                const bool word_start = at == start || !is_word_char(text[at - 1]);
                const bool word_end = at + length == end || !is_word_char(text[at + length]);
                const std::uint32_t how = at == start && at + length == end ? match_exact
                                          : word_start && word_end           ? match_word
                                          : word_start                       ? match_prefix
                                                                             : match_inside;
                // One or two characters inside a word are noise ("lm" in
                // "film"), so short terms only match where words start.
                const bool wanted = (only == field_count || f == only) && (!word_start_only || how != match_inside);
                if (wanted && (how != match_inside || length >= 3))
                    best = std::max(best, how * field_weight[f]);
                if (best == best_score)
                    break;
                at = text.find(t.text, at + 1);
            }
            return best;
        }

        // A term as text, or the part's value as the same quantity written
        // differently.
        template <typename Entry>
        std::uint32_t text_or_number(const Entry &e, std::string_view text, const term &t)
        {
            // The value as a quantity ranks just above any text match: it
            // reads prefixes with their case, so "10m" puts 10 milliohms
            // ahead of 10 megohms, which match as text just as well.
            if (t.has_number && e.has_number && same_quantity(t.number, e.number) &&
                (t.unit[0] == '\0' || e.unit[0] == '\0' || std::string_view(t.unit) == e.unit))
                return number_score;
            return text_score(e, text, t);
        }

        // How well one term matches one entry (see catalog::search); 0 for
        // not at all. `all` is the entry's whole text, footprint included.
        template <typename Entry>
        std::uint32_t term_score(const Entry &e, std::string_view all, const quantity *quantities, const term &t)
        {
            const std::string_view text = all.substr(0, e.field_end[field_count - 1]);
            switch (t.kind)
            {
            case term_kind::text:
                return text_or_number(e, text, t);
            case term_kind::package: {
                const std::size_t start = e.field_end[field_package - 1] + std::size_t{1};
                const std::string_view package = text.substr(start, e.field_end[field_package] - start);
                if (has_word(package, t.text, package_separator, package_separator))
                    return package_score;
                const std::size_t footprint_start = e.field_end[field_count - 1] + std::size_t{1};
                const std::string_view footprint =
                    all.substr(footprint_start, e.footprint_end - std::min<std::size_t>(footprint_start, e.footprint_end));
                if (has_word(footprint, t.text, [](char c) { return c == ':' || c == '_'; },
                             [](char c) { return c == '_'; }))
                    return footprint_score;
                // A package code ("0603", "SOT-23") is not looked for in the
                // text of a part that names another package: the text of a
                // SOT-23-5 part says "SOT-23" too. A word without a digit
                // ("JST", "radial") may be meant as any word.
                if (t.digits && (!package.empty() || !footprint.empty()))
                    return 0;
                return text_score(e, text, t);
            }
            case term_kind::value:
                break;
            }

            const quantity_unit unit = t.value.unit;
            if (e.has_principal && (unit == quantity_unit::none || unit == e.principal.unit))
            {
                if (same_value(t.value.value, e.principal.value))
                    return principal_score;
                // Another value: no text match of it counts ("1k" is not in
                // "11k" or "5.1k"), except a long number that may begin a
                // word of the MPN.
                return t.digits ? text_score(e, text, t, field_mpn, true) : 0;
            }
            if (unit != quantity_unit::none)
            {
                bool stated = false;
                for (std::size_t i = 0; i < e.quantity_count; ++i)
                {
                    const quantity &q = quantities[e.quantities + i];
                    if (q.unit != unit)
                        continue;
                    if (same_value(t.value.value, q.value))
                        return parameter_score;
                    stated = true;
                }
                if (stated)
                    return 0;
            }
            // A part with a principal value is not also its `value` in
            // another unit ("1kF" is not a 1k resistor).
            return e.has_principal ? text_score(e, text, t) : text_or_number(e, text, t);
        }

        template <typename Entry>
        std::uint32_t entry_score(const Entry &e, std::string_view all, const quantity *quantities,
                                  const std::vector<term> &terms)
        {
            std::uint32_t total = 0;
            for (const term &t : terms)
            {
                const std::uint32_t s = term_score(e, all, quantities, t);
                if (s == 0)
                    return 0;
                total += s;
            }
            return total;
        }
    } // namespace

    // ---- quantities ----------------------------------------------------------------------

    std::string_view unit_symbol(quantity_unit unit) noexcept
    {
        switch (unit)
        {
        case quantity_unit::none:
            return {};
        case quantity_unit::ohm:
            return "\xCE\xA9";
        case quantity_unit::farad:
            return "F";
        case quantity_unit::henry:
            return "H";
        case quantity_unit::volt:
            return "V";
        case quantity_unit::ampere:
            return "A";
        case quantity_unit::watt:
            return "W";
        case quantity_unit::hertz:
            return "Hz";
        case quantity_unit::percent:
            return "%";
        }
        return {};
    }

    bool same_value(double a, double b) noexcept
    {
        return std::abs(a - b) <= 1e-6 * std::max(std::abs(a), std::abs(b));
    }

    std::optional<quantity> parameter_quantity(const parameter &q)
    {
        if (!q.value.empty())
            return read_value(q.value, q.unit);
        // Limits alone: their middle.
        const auto low = read_value(q.min, q.unit);
        const auto high = read_value(q.max, q.unit);
        if (!low || !high || low->unit != high->unit)
            return std::nullopt;
        return quantity{(low->value + high->value) / 2.0, low->unit};
    }

    std::optional<quantity> query_quantity(std::string_view text)
    {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
            text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
            text.remove_suffix(1);
        if (text.starts_with("\xC2\xB1"))
            text.remove_prefix(2);
        if (text.empty())
            return std::nullopt;

        // A fraction of whole numbers, as power ratings are written: "1/10W".
        if (const std::size_t slash = text.find('/'); slash != std::string_view::npos)
        {
            const auto whole = [](std::string_view digits) -> std::optional<double> {
                if (digits.empty() || digits.size() > 6)
                    return std::nullopt;
                double v = 0.0;
                for (const char c : digits)
                {
                    if (c < '0' || c > '9')
                        return std::nullopt;
                    v = v * 10.0 + (c - '0');
                }
                return v;
            };
            std::size_t end = slash + 1;
            while (end < text.size() && text[end] >= '0' && text[end] <= '9')
                ++end;
            const auto top = whole(text.substr(0, slash));
            const auto bottom = whole(text.substr(slash + 1, end - slash - 1));
            const auto unit = loose_unit(text.substr(end));
            if (!top || !bottom || *bottom == 0.0 || !unit)
                return std::nullopt;
            return quantity{*top / *bottom, *unit};
        }

        // "4r7", as typed, is the RKM "4R7".
        std::string written(text);
        for (std::size_t i = 1; i + 1 < written.size(); ++i)
        {
            if (written[i] == 'r' && written[i - 1] >= '0' && written[i - 1] <= '9' && written[i + 1] >= '0' &&
                written[i + 1] <= '9')
                written[i] = 'R';
        }
        const auto parsed = math::electrical::parse_engineering(written);
        if (!parsed || !std::isfinite(parsed->value))
            return std::nullopt;
        const auto unit = loose_unit(parsed->unit);
        if (!unit)
            return std::nullopt;
        return quantity{parsed->value, *unit};
    }

    principal_parameter principal_of(std::string_view device)
    {
        const device_definition *d = schema::builtin().find_device(device);
        const std::string_view id = d != nullptr ? std::string_view(d->id) : device;
        if (id == devices::resistor)
            return {names::resistance, quantity_unit::ohm};
        if (id == devices::capacitor)
            return {names::capacitance, quantity_unit::farad};
        if (id == devices::inductor)
            return {names::inductance, quantity_unit::henry};
        if (id == devices::zener)
            return {names::zener_voltage, quantity_unit::volt};
        return {};
    }

    std::optional<quantity> principal_value(const part &p)
    {
        if (p.device.empty())
            return std::nullopt;
        const principal_parameter principal = principal_of(p.device);
        if (principal.name.empty())
            return std::nullopt;
        const auto in_unit = [&](std::optional<quantity> q) -> std::optional<quantity> {
            if (!q || (q->unit != principal.unit && q->unit != quantity_unit::none))
                return std::nullopt;
            return quantity{q->value, principal.unit};
        };
        if (const parameter *q = schema::builtin().find_parameter(p, principal.name))
        {
            if (const auto v = in_unit(parameter_quantity(*q)))
                return v;
        }
        return in_unit(query_quantity(p.value));
    }

    // ---- category tree --------------------------------------------------------------

    const category_node *category_node::find(std::string_view search_path) const noexcept
    {
        const category_node *node = this;
        while (!search_path.empty())
        {
            const std::size_t slash = search_path.find('/');
            const std::string_view segment = search_path.substr(0, slash);
            const category_node *next = nullptr;
            for (const category_node &child : node->children)
            {
                if (child.name == segment)
                {
                    next = &child;
                    break;
                }
            }
            if (next == nullptr)
                return nullptr;
            node = next;
            if (slash == std::string_view::npos)
                break;
            search_path.remove_prefix(slash + 1);
        }
        return node;
    }

    // ---- contents ---------------------------------------------------------------------

    std::uint32_t catalog::intern_category(std::string_view category)
    {
        if (const auto it = m_category_ids.find(category); it != m_category_ids.end())
            return it->second;
        const auto id = static_cast<std::uint32_t>(m_categories.size());
        m_categories.emplace_back(category);
        m_category_ids.emplace(std::string(category), id);
        return id;
    }

    void catalog::describe(const part &p, std::string &text, entry &e, std::vector<quantity> &quantities)
    {
        const std::size_t start = text.size();
        e.text = static_cast<std::uint32_t>(start);
        const auto end_field = [&](std::size_t f) {
            e.field_end[f] = static_cast<std::uint16_t>(text.size() - start);
            text += separator;
        };
        fold_append(text, p.name, max_field_bytes);
        end_field(field_name);
        fold_append(text, p.value, max_field_bytes);
        end_field(field_value);
        fold_append(text, p.mpn, max_field_bytes);
        end_field(field_mpn);
        fold_append(text, p.package, max_field_bytes);
        end_field(field_package);
        const std::size_t keywords_start = text.size();
        for (const std::string &k : p.keywords)
        {
            if (text.size() - keywords_start + k.size() + 1 > max_field_bytes)
                break;
            if (text.size() != keywords_start)
                text += ' ';
            fold_append(text, k);
        }
        end_field(field_keywords);
        fold_append(text, p.manufacturer, max_field_bytes);
        end_field(field_manufacturer);
        fold_append(text, p.description, max_field_bytes);
        end_field(field_description);
        // After the searched text: only package terms look at it.
        fold_append(text, p.footprint, max_field_bytes);
        e.footprint_end = static_cast<std::uint16_t>(text.size() - start);

        e.name_length = static_cast<std::uint16_t>(std::min<std::size_t>(p.name.size(), 0xFFFF));
        if (const auto number = math::electrical::parse_engineering(p.value))
        {
            if (std::isfinite(number->value) && store_unit(number->unit, e.unit))
            {
                e.has_number = true;
                e.number = number->value;
            }
        }
        if (const auto principal = principal_value(p))
        {
            e.has_principal = true;
            e.principal = *principal;
        }
        e.quantities = static_cast<std::uint32_t>(quantities.size());
        for (const parameter &q : p.parameters)
        {
            if (e.quantity_count == 0xFFFF)
                break;
            const auto value = parameter_quantity(q);
            if (!value || value->unit == quantity_unit::none)
                continue;
            quantities.push_back(*value);
            ++e.quantity_count;
        }
    }

    catalog::index catalog::build_index(const library &lib)
    {
        index ix;
        ix.entries.reserve(lib.parts.size());
        ix.ids.reserve(lib.parts.size());
        std::size_t bytes = 0;
        for (const part &p : lib.parts)
            bytes += p.name.size() + p.value.size() + p.mpn.size() + p.package.size() + p.manufacturer.size() +
                     std::min(p.description.size(), max_field_bytes) + 16 * p.keywords.size() + p.footprint.size() +
                     8;
        ix.text.reserve(bytes);

        for (std::size_t i = 0; i < lib.parts.size(); ++i)
        {
            const part &p = lib.parts[i];
            entry e;
            describe(p, ix.text, e, ix.quantities);
            e.category = intern_category(p.category);
            const std::string_view package(ix.text.data() + e.text + e.field_end[field_package - 1] + 1,
                                           e.field_end[field_package] - e.field_end[field_package - 1] - 1u);
            package_words(package, [&](std::string_view word) {
                const auto found = ix.package_words.find(word);
                if (found == ix.package_words.end())
                    ix.package_words.emplace(std::string(word), 1u);
                else
                    ++found->second;
            });
            ix.entries.push_back(e);
            ix.ids.emplace(p.id, static_cast<std::uint32_t>(i));
        }
        return ix;
    }

    bool catalog::is_package_word(std::string_view folded) const
    {
        for (const index &ix : m_indexes)
            if (ix.package_words.contains(folded))
                return true;
        return false;
    }

    void catalog::add(library lib)
    {
        index ix = build_index(lib);
        for (std::size_t i = 0; i < m_libraries.size(); ++i)
        {
            if (m_libraries[i].name == lib.name)
            {
                m_part_count -= m_libraries[i].parts.size();
                m_part_count += lib.parts.size();
                m_libraries[i] = std::move(lib);
                m_indexes[i] = std::move(ix);
                return;
            }
        }
        m_part_count += lib.parts.size();
        m_libraries.push_back(std::move(lib));
        m_indexes.push_back(std::move(ix));
    }

    bool catalog::remove(std::string_view name)
    {
        for (std::size_t i = 0; i < m_libraries.size(); ++i)
        {
            if (m_libraries[i].name == name)
            {
                m_part_count -= m_libraries[i].parts.size();
                m_libraries.erase(m_libraries.begin() + static_cast<std::ptrdiff_t>(i));
                m_indexes.erase(m_indexes.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        }
        return false;
    }

    void catalog::clear() noexcept
    {
        m_libraries.clear();
        m_indexes.clear();
        m_categories.clear();
        m_category_ids.clear();
        m_part_count = 0;
    }

    const library *catalog::find_library(std::string_view name) const noexcept
    {
        for (const library &lib : m_libraries)
            if (lib.name == name)
                return &lib;
        return nullptr;
    }

    // ---- parts ------------------------------------------------------------------------

    const part &catalog::get(part_ref ref) const noexcept
    {
        return m_libraries[ref.library].parts[ref.part];
    }

    const library &catalog::library_of(part_ref ref) const noexcept
    {
        return m_libraries[ref.library];
    }

    std::string catalog::qualified_id(part_ref ref) const
    {
        return m_libraries[ref.library].name + ":" + get(ref).id;
    }

    std::optional<part_ref> catalog::find(std::string_view qualified) const noexcept
    {
        const std::size_t colon = qualified.find(':');
        if (colon == std::string_view::npos)
            return std::nullopt;
        return find(qualified.substr(0, colon), qualified.substr(colon + 1));
    }

    std::optional<part_ref> catalog::find(std::string_view library_name, std::string_view part_id) const noexcept
    {
        for (std::size_t i = 0; i < m_libraries.size(); ++i)
        {
            if (m_libraries[i].name != library_name)
                continue;
            const auto it = m_indexes[i].ids.find(part_id);
            if (it == m_indexes[i].ids.end())
                return std::nullopt;
            return part_ref{static_cast<std::uint32_t>(i), it->second};
        }
        return std::nullopt;
    }

    // ---- browsing ---------------------------------------------------------------------

    category_node catalog::categories(std::string_view library_name) const
    {
        std::vector<std::size_t> counts(m_categories.size(), 0);
        for (std::size_t i = 0; i < m_libraries.size(); ++i)
        {
            if (!library_name.empty() && m_libraries[i].name != library_name)
                continue;
            for (const entry &e : m_indexes[i].entries)
                ++counts[e.category];
        }

        category_node root;
        for (std::size_t id = 0; id < counts.size(); ++id)
        {
            if (counts[id] == 0)
                continue;
            std::string_view rest = m_categories[id];
            category_node *node = &root;
            std::size_t consumed = 0;
            while (!rest.empty())
            {
                const std::size_t slash = rest.find('/');
                const std::string_view segment = rest.substr(0, slash);
                consumed += segment.size();
                category_node *next = nullptr;
                for (category_node &child : node->children)
                {
                    if (child.name == segment)
                    {
                        next = &child;
                        break;
                    }
                }
                if (next == nullptr)
                {
                    category_node child;
                    child.name = std::string(segment);
                    child.path = m_categories[id].substr(0, consumed);
                    node->children.push_back(std::move(child));
                    next = &node->children.back();
                }
                node = next;
                if (slash == std::string_view::npos)
                    break;
                rest.remove_prefix(slash + 1);
                ++consumed;
            }
            node->own_parts += counts[id];
        }
        total_parts(root);
        return root;
    }

    search_results catalog::search(const search_query &query) const
    {
        search_results results;

        const std::vector<term> terms =
            parse_terms(query.text, [this](std::string_view word) { return is_package_word(word); });

        // Which categories pass the filter, decided once per category.
        std::vector<char> allowed;
        const std::string category = normalize_category(query.category);
        if (!category.empty())
        {
            allowed.resize(m_categories.size(), 0);
            for (std::size_t id = 0; id < m_categories.size(); ++id)
            {
                const std::string &c = m_categories[id];
                allowed[id] = c == category || (c.size() > category.size() && c.starts_with(category) &&
                                                c[category.size()] == '/');
            }
        }

        std::vector<std::uint16_t> name_lengths; // for ordering ties, parallel to hits
        for (std::size_t l = 0; l < m_libraries.size(); ++l)
        {
            if (!query.library.empty() && m_libraries[l].name != query.library)
                continue;
            const index &ix = m_indexes[l];
            const char *base = ix.text.data();
            for (std::size_t p = 0; p < ix.entries.size(); ++p)
            {
                const entry &e = ix.entries[p];
                if (!allowed.empty() && !allowed[e.category])
                    continue;

                std::uint32_t total = 0;
                if (!terms.empty())
                {
                    total = entry_score(e, std::string_view(base + e.text, e.footprint_end), ix.quantities.data(),
                                        terms);
                    if (total == 0)
                        continue;
                }
                results.hits.push_back({part_ref{static_cast<std::uint32_t>(l), static_cast<std::uint32_t>(p)}, total});
                name_lengths.push_back(e.name_length);
            }
        }
        results.matched = results.hits.size();

        // An empty query lists in catalog order, which is how they came.
        if (terms.empty())
        {
            if (query.limit != 0 && results.hits.size() > query.limit)
                results.hits.resize(query.limit);
            return results;
        }

        // Ranked: sort positions, so the name lengths travel along.
        std::vector<std::uint32_t> order(results.hits.size());
        for (std::size_t i = 0; i < order.size(); ++i)
            order[i] = static_cast<std::uint32_t>(i);
        const auto better = [&](std::uint32_t a, std::uint32_t b) {
            const search_hit &x = results.hits[a];
            const search_hit &y = results.hits[b];
            if (x.score != y.score)
                return x.score > y.score;
            if (name_lengths[a] != name_lengths[b])
                return name_lengths[a] < name_lengths[b];
            return x.ref < y.ref;
        };
        const std::size_t keep = query.limit == 0 ? order.size() : std::min(query.limit, order.size());
        if (keep < order.size())
            std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(keep), order.end(), better);
        else
            std::sort(order.begin(), order.end(), better);

        std::vector<search_hit> sorted;
        sorted.reserve(keep);
        for (std::size_t i = 0; i < keep; ++i)
            sorted.push_back(results.hits[order[i]]);
        results.hits = std::move(sorted);
        return results;
    }

    std::uint32_t catalog::match(const part &p, std::string_view text) const
    {
        std::string folded;
        entry e;
        std::vector<quantity> quantities;
        describe(p, folded, e, quantities);
        const std::string own_package = fold(p.package);
        const std::vector<term> terms = parse_terms(text, [&](std::string_view word) {
            if (is_package_word(word))
                return true;
            bool own = false;
            package_words(own_package, [&](std::string_view w) { own = own || w == word; });
            return own;
        });
        if (terms.empty())
            return 1;
        return entry_score(e, std::string_view(folded.data() + e.text, e.footprint_end), quantities.data(), terms);
    }

    // ---- loading ---------------------------------------------------------------------

    std::vector<std::filesystem::path> discover_libraries(std::span<const std::filesystem::path> directories)
    {
        std::vector<std::filesystem::path> found;
        std::set<std::filesystem::path> seen;
        for (const std::filesystem::path &directory : directories)
        {
            std::error_code error;
            if (!std::filesystem::is_directory(directory, error))
                continue;
            std::vector<std::filesystem::path> here;
            const auto options = std::filesystem::directory_options::skip_permission_denied;
            std::filesystem::recursive_directory_iterator it(directory, options, error);
            for (; !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
            {
                std::error_code ignored;
                if (it->is_regular_file(ignored) && has_library_extension(it->path()))
                    here.push_back(it->path());
            }
            std::sort(here.begin(), here.end());
            for (std::filesystem::path &path : here)
            {
                // A directory inside another one given finds its files twice.
                std::error_code absolute_error;
                const std::filesystem::path key = std::filesystem::absolute(path, absolute_error).lexically_normal();
                if (seen.insert(key).second)
                    found.push_back(std::move(path));
            }
        }
        return found;
    }

    bool scan_report::clean() const noexcept
    {
        if (cancelled)
            return false;
        for (const file &f : files)
            if ((!f.loaded && !f.shadowed) || !f.report.clean())
                return false;
        return true;
    }

    std::string scan_report::summary() const
    {
        std::string text = std::format("{} librar{} with {} part(s) loaded from {} file(s) in {:.1f} ms{}", libraries,
                                       libraries == 1 ? "y" : "ies", parts, files.size(), seconds * 1000.0,
                                       cancelled ? " (cancelled)" : "");
        for (const file &f : files)
        {
            if (f.shadowed)
                text += "\n  " + utf8(f.path) + ": " + f.error;
            else if (!f.loaded)
                text += "\n  " + utf8(f.path) + ": not loaded: " + f.error;
            if (!f.report.clean())
            {
                text += "\n  " + utf8(f.path) + ": ";
                std::string lines = f.report.summary();
                for (std::size_t at = lines.find('\n'); at != std::string::npos; at = lines.find('\n', at + 1))
                    lines.insert(at + 1, "  ");
                text += lines;
            }
        }
        return text;
    }

    catalog load_catalog(std::span<const std::filesystem::path> directories, scan_report &report,
                         const scan_options &options)
    {
        const auto start = std::chrono::steady_clock::now();
        report = {};
        catalog result;

        // The opened files first, each once; then what the directories
        // hold, less those.
        std::vector<std::filesystem::path> paths;
        std::set<std::filesystem::path> seen;
        const auto key = [](const std::filesystem::path &path) {
            std::error_code error;
            return std::filesystem::absolute(path, error).lexically_normal();
        };
        for (const std::filesystem::path &file : options.files)
        {
            if (seen.insert(key(file)).second)
                paths.push_back(file);
        }
        const std::size_t opened = paths.size();
        for (std::filesystem::path &path : discover_libraries(directories))
        {
            if (seen.insert(key(path)).second)
                paths.push_back(std::move(path));
        }

        // The report entry of each opened file's library, by name.
        std::map<std::string, std::size_t, std::less<>> opened_entry;
        for (std::size_t i = 0; i < paths.size(); ++i)
        {
            if (options.progress && !options.progress(i, paths.size(), paths[i]))
            {
                report.cancelled = true;
                break;
            }
            scan_report::file f;
            f.path = paths[i];
            f.opened = i < opened;
            auto loaded = load_library(paths[i]);
            if (!loaded)
                f.error = std::move(loaded.error());
            else if (const library *existing = result.find_library(loaded->library.name))
            {
                const auto by = opened_entry.find(loaded->library.name);
                if (by != opened_entry.end() && !f.opened)
                {
                    // Taken the place of by a file the user opened.
                    f.shadowed = true;
                    f.error = std::format("shadowed by {}, opened for this session", utf8(existing->path));
                    scan_report::file &winner = report.files[by->second];
                    if (winner.shadows.empty())
                        winner.shadows = paths[i];
                }
                else
                {
                    f.error = std::format("a library named \"{}\" is already loaded, from {}", loaded->library.name,
                                          utf8(existing->path));
                }
                f.report = std::move(loaded->report);
            }
            else
            {
                if (f.opened)
                    opened_entry.emplace(loaded->library.name, report.files.size());
                f.loaded = true;
                f.report = std::move(loaded->report);
                ++report.libraries;
                report.parts += loaded->library.parts.size();
                result.add(std::move(loaded->library));
            }
            report.files.push_back(std::move(f));
        }
        report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return result;
    }

} // namespace volt::library
