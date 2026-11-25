#include <schematic/io.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <unordered_set>

namespace volt::schematic
{

    namespace
    {
        constexpr std::string_view sheet_keys[] = {"format", "format_version", "parts", "wires", "labels"};
        constexpr std::string_view part_keys[] = {"id",     "symbol",       "reference", "value",  "device",
                                                  "library_part", "position", "rotation",  "mirrored"};
        constexpr std::string_view wire_keys[] = {"id", "a", "b"};
        constexpr std::string_view label_keys[] = {"id", "kind", "text", "position", "rotation"};

        template <std::size_t N>
        bool is_known(const std::string_view (&keys)[N], std::string_view key) noexcept
        {
            return std::ranges::find(keys, key) != std::end(keys);
        }

        std::string utf8_name(const std::filesystem::path &path)
        {
            const std::u8string name = path.filename().u8string();
            return std::string(name.begin(), name.end());
        }

        // ---- writing ---------------------------------------------------------------

        json::value point_to_json(point p)
        {
            return json::value(json::array{json::value(p.x()), json::value(p.y())});
        }

        int degrees(std::uint8_t rotation)
        {
            return 90 * (rotation % 4);
        }

        template <std::size_t N>
        void put_extra(json::object &o, const json::object &extra, const std::string_view (&keys)[N])
        {
            for (const json::member &m : extra)
                if (!is_known(keys, m.key))
                    o.set(m.key, m.value);
        }

        json::value part_to_json(item_id id, const part &p)
        {
            json::object o;
            o.set("id", json::value(id));
            o.set("symbol", json::value(p.symbol));
            o.set("reference", json::value(p.reference));
            o.set("value", json::value(p.value));
            if (!p.device.empty())
                o.set("device", json::value(p.device));
            if (!p.library_part.empty())
                o.set("library_part", json::value(p.library_part));
            o.set("position", point_to_json(p.position));
            o.set("rotation", json::value(degrees(p.rotation)));
            o.set("mirrored", json::value(p.mirrored));
            put_extra(o, p.extra, part_keys);
            return json::value(std::move(o));
        }

        json::value wire_to_json(item_id id, const wire &w)
        {
            json::object o;
            o.set("id", json::value(id));
            o.set("a", point_to_json(w.a));
            o.set("b", point_to_json(w.b));
            put_extra(o, w.extra, wire_keys);
            return json::value(std::move(o));
        }

        json::value label_to_json(item_id id, const label &l)
        {
            json::object o;
            o.set("id", json::value(id));
            o.set("kind", json::value(to_string(l.kind)));
            o.set("text", json::value(l.text));
            o.set("position", point_to_json(l.position));
            o.set("rotation", json::value(degrees(l.rotation)));
            put_extra(o, l.extra, label_keys);
            return json::value(std::move(o));
        }

        // ---- reading ---------------------------------------------------------------

        // Reads the records of one table: the fields, the problems with
        // them, and whether the record is to be left out.
        class record_reader
        {
        public:
            record_reader(load_report &report, std::string_view table, std::size_t index, const json::object &o)
                : m_report(report), m_table(table), m_index(index), m_object(o)
            {
            }

            void problem(std::string message, bool skipped = false)
            {
                m_report.problems.push_back({std::string(m_table), m_index, m_id, std::move(message), skipped});
            }

            const json::value *find(std::string_view key) const { return m_object.find(key); }

            // The id: a whole number from 1 to 2^32 - 1.
            std::optional<item_id> id()
            {
                const json::value *v = find("id");
                if (v == nullptr)
                {
                    problem("has no \"id\"", true);
                    return std::nullopt;
                }
                const auto number = v->as_integer();
                if (!number || *number < 1 || *number > std::numeric_limits<item_id>::max())
                {
                    problem("\"id\" should be a whole number from 1 to 4294967295", true);
                    return std::nullopt;
                }
                m_id = static_cast<item_id>(*number);
                return m_id;
            }

            void text(std::string_view key, std::string &out)
            {
                const json::value *v = find(key);
                if (v == nullptr || v->is_null())
                    return;
                if (const auto s = v->as_string())
                    out = std::string(*s);
                else
                    problem(std::format("\"{}\" should be a string, not {}; ignored", key, json::describe(v->type())));
            }

            // [x, y], whole numbers in the range of an int; nullopt when it
            // is missing or not that, reported when `report_it` (the caller
            // reports it otherwise).
            std::optional<point> position(std::string_view key, bool report_it = true)
            {
                const json::value *v = find(key);
                if (v == nullptr || v->is_null())
                {
                    if (report_it)
                        problem(std::format("has no \"{}\"; read as [0, 0]", key));
                    return std::nullopt;
                }
                const json::array *a = v->as_array();
                if (a != nullptr && a->size() == 2)
                {
                    const auto x = (*a)[0].as_integer();
                    const auto y = (*a)[1].as_integer();
                    constexpr std::int64_t lowest = std::numeric_limits<int>::min();
                    constexpr std::int64_t highest = std::numeric_limits<int>::max();
                    if (x && y && *x >= lowest && *x <= highest && *y >= lowest && *y <= highest)
                        return point{static_cast<int>(*x), static_cast<int>(*y)};
                }
                if (report_it)
                    problem(std::format("\"{}\" should be [x, y] in whole units; read as [0, 0]", key));
                return std::nullopt;
            }

            // Degrees counter-clockwise, a multiple of 90, as quarter turns.
            std::uint8_t rotation()
            {
                const json::value *v = find("rotation");
                if (v == nullptr || v->is_null())
                    return 0;
                const auto number = v->as_integer();
                if (!number || *number % 90 != 0)
                {
                    problem("\"rotation\" should be 0, 90, 180 or 270; read as 0");
                    return 0;
                }
                return static_cast<std::uint8_t>(((*number / 90) % 4 + 4) % 4);
            }

            void flag(std::string_view key, bool &out)
            {
                const json::value *v = find(key);
                if (v == nullptr || v->is_null())
                    return;
                if (const auto b = v->as_bool())
                    out = *b;
                else
                    problem(std::format("\"{}\" should be true or false, not {}; ignored", key,
                                        json::describe(v->type())));
            }

            template <std::size_t N>
            void extra(json::object &out, const std::string_view (&keys)[N]) const
            {
                for (const json::member &m : m_object)
                    if (!is_known(keys, m.key))
                        out.set(m.key, m.value);
            }

        private:
            load_report &m_report;
            std::string_view m_table;
            std::size_t m_index;
            const json::object &m_object;
            item_id m_id = 0;
        };

        // Reads one table's array into `table`; `read` makes the record or
        // nullopt to leave it out. False (with `error`) when the table is
        // there but not an array.
        template <typename Record, typename Read>
        bool read_table(const json::object &root, std::string_view key, history::table<item_id, Record> &table,
                        std::unordered_set<item_id> &ids, load_report &report, std::string &error, Read read)
        {
            const json::value *v = root.find(key);
            if (v == nullptr || v->is_null())
                return true;
            const json::array *array = v->as_array();
            if (array == nullptr)
            {
                error = std::format("\"{}\" should be an array, not {}", key, json::describe(v->type()));
                return false;
            }
            for (std::size_t i = 0; i < array->size(); ++i)
            {
                const json::object *o = (*array)[i].as_object();
                if (o == nullptr)
                {
                    report.problems.push_back({std::string(key), i, 0,
                                               std::format("should be an object, not {}",
                                                           json::describe((*array)[i].type())),
                                               true});
                    ++report.records_skipped;
                    continue;
                }
                record_reader reader(report, key, i, *o);
                const std::optional<item_id> id = reader.id();
                if (!id)
                {
                    ++report.records_skipped;
                    continue;
                }
                if (ids.contains(*id))
                {
                    reader.problem("the id is used by another record before it", true);
                    ++report.records_skipped;
                    continue;
                }
                std::optional<Record> record = read(reader);
                if (!record)
                {
                    ++report.records_skipped;
                    continue;
                }
                ids.insert(*id);
                table.insert(*id, std::move(*record));
                ++report.records_loaded;
            }
            return true;
        }
    } // namespace

    // ---- the report ------------------------------------------------------------------------

    std::string load_report::summary() const
    {
        std::string text = std::format("{} record(s) loaded, {} skipped, {} problem(s)", records_loaded,
                                       records_skipped, problems.size());
        for (const problem &p : problems)
        {
            text += "\n  ";
            if (p.record != problem::no_record)
            {
                text += std::format("{}[{}]", p.table, p.record);
                if (p.id != 0)
                    text += std::format(" (id {})", p.id);
                text += ": ";
            }
            text += p.message;
            if (p.skipped)
                text += " (left out)";
        }
        return text;
    }

    // ---- writing ---------------------------------------------------------------------------

    json::value sheet_to_json(const sheet &s)
    {
        json::object o;
        o.set("format", json::value(sheet_format));
        o.set("format_version", json::value(sheet_format_version));
        json::array parts;
        parts.reserve(s.parts.size());
        for (const auto &e : s.parts)
            parts.push_back(part_to_json(e.id, e.record));
        o.set("parts", json::value(std::move(parts)));
        json::array wires;
        wires.reserve(s.wires.size());
        for (const auto &e : s.wires)
            wires.push_back(wire_to_json(e.id, e.record));
        o.set("wires", json::value(std::move(wires)));
        json::array labels;
        labels.reserve(s.labels.size());
        for (const auto &e : s.labels)
            labels.push_back(label_to_json(e.id, e.record));
        o.set("labels", json::value(std::move(labels)));
        put_extra(o, s.extra, sheet_keys);
        return json::value(std::move(o));
    }

    std::string write_sheet(const sheet &s)
    {
        return json::write(sheet_to_json(s));
    }

    // ---- reading ---------------------------------------------------------------------------

    std::expected<loaded_sheet, std::string> parse_sheet(std::string_view text)
    {
        auto document = json::parse(text);
        if (!document)
            return std::unexpected(document.error().message());
        const json::object *root = document->as_object();
        if (root == nullptr)
            return std::unexpected(std::format("not a Volt schematic: the file holds {}, not an object",
                                               json::describe(document->type())));
        const auto format = document->at("format").as_string();
        if (!format || *format != sheet_format)
            return std::unexpected(std::format("not a Volt schematic: \"format\" should be \"{}\"", sheet_format));

        loaded_sheet result;
        load_report &report = result.report;
        const auto file_problem = [&](std::string message) {
            load_report::problem p;
            p.message = std::move(message);
            report.problems.push_back(std::move(p));
        };

        if (const json::value *version = root->find("format_version"))
        {
            const auto number = version->as_integer();
            if (!number || *number < 1)
                return std::unexpected("\"format_version\" should be a whole number from 1");
            result.format_version = static_cast<int>(std::min<std::int64_t>(*number, 1'000'000));
            if (result.format_version > sheet_format_version)
                file_problem(std::format("written in format version {}, newer than this Volt's {}: fields it does "
                                         "not know are kept but not used",
                                         result.format_version, sheet_format_version));
        }
        else
            file_problem(std::format("no \"format_version\"; read as version {}", sheet_format_version));

        sheet &s = result.sheet;
        std::unordered_set<item_id> ids;
        std::string error;

        const bool parts_read = read_table(*root, "parts", s.parts, ids, report, error, [](record_reader &r) {
            part p;
            r.text("symbol", p.symbol);
            if (p.symbol.empty())
                r.problem("has no \"symbol\"");
            r.text("reference", p.reference);
            r.text("value", p.value);
            r.text("device", p.device);
            r.text("library_part", p.library_part);
            p.position = r.position("position").value_or(point{});
            p.rotation = r.rotation();
            r.flag("mirrored", p.mirrored);
            r.extra(p.extra, part_keys);
            return std::optional<part>(std::move(p));
        });
        if (!parts_read)
            return std::unexpected(error);

        const bool wires_read = read_table(*root, "wires", s.wires, ids, report, error, [](record_reader &r) {
            const std::optional<point> a = r.position("a", false);
            const std::optional<point> b = r.position("b", false);
            if (!a || !b)
            {
                r.problem("\"a\" and \"b\" should be [x, y] in whole units", true);
                return std::optional<wire>();
            }
            wire w{*a, *b, {}};
            r.extra(w.extra, wire_keys);
            return std::optional<wire>(std::move(w));
        });
        if (!wires_read)
            return std::unexpected(error);

        const bool labels_read = read_table(*root, "labels", s.labels, ids, report, error, [](record_reader &r) {
            label l;
            if (const json::value *kind = r.find("kind"); kind != nullptr && !kind->is_null())
            {
                const auto name = kind->as_string();
                if (!name || !parse_label_kind(*name, l.kind))
                {
                    r.problem(name ? std::format("a label of the kind \"{}\", which this version does not know", *name)
                                   : std::string("\"kind\" should be \"net\", \"power\" or \"note\""),
                              true);
                    return std::optional<label>();
                }
            }
            else
                r.problem("has no \"kind\"; read as a net label");
            r.text("text", l.text);
            l.position = r.position("position").value_or(point{});
            l.rotation = r.rotation();
            r.extra(l.extra, label_keys);
            return std::optional<label>(std::move(l));
        });
        if (!labels_read)
            return std::unexpected(error);

        for (const json::member &m : *root)
            if (!is_known(sheet_keys, m.key))
                s.extra.set(m.key, m.value);
        return result;
    }

    std::expected<loaded_sheet, std::string> load_sheet(const std::filesystem::path &path)
    {
        auto bytes = json::read_text_file(path);
        if (!bytes)
            return std::unexpected(std::move(bytes.error()));
        auto result = parse_sheet(*bytes);
        if (!result)
            return std::unexpected(utf8_name(path) + ": " + result.error());
        return result;
    }

    std::expected<void, std::string> save_sheet(const std::filesystem::path &path, const sheet &s)
    {
        return json::write_text_file(path, write_sheet(s));
    }

} // namespace volt::schematic
