#include <library/part.hpp>

#include <math/electrical/engineering.hpp>

#include <format>

namespace volt::library
{

    namespace
    {
        bool is_space(char c) noexcept
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        std::string_view trim(std::string_view text) noexcept
        {
            while (!text.empty() && is_space(text.front()))
                text.remove_prefix(1);
            while (!text.empty() && is_space(text.back()))
                text.remove_suffix(1);
            return text;
        }

        // A string field: problems for anything but a string.
        void read_string(const json::value &v, std::string_view key, std::string &out, std::vector<std::string> &problems)
        {
            if (const auto text = v.as_string())
                out = std::string(*text);
            else if (!v.is_null())
                problems.push_back(std::format("\"{}\" should be a string, not {}; ignored", key, json::describe(v.type())));
        }

        void put(json::object &o, std::string_view key, const std::string &text)
        {
            if (!text.empty())
                o.set(key, json::value(text));
        }

        parameter parameter_from_json(const json::object &o, std::vector<std::string> &problems)
        {
            parameter p;
            for (const json::member &m : o)
            {
                if (m.key == "name")
                    read_string(m.value, "parameters[].name", p.name, problems);
                else if (m.key == "value")
                {
                    // A number is taken as the text it would be written as:
                    // hand-written files say "value": 25 for 25 V.
                    if (m.value.is_number())
                        p.value = json::write_compact(m.value);
                    else
                        read_string(m.value, "parameters[].value", p.value, problems);
                }
                else if (m.key == "unit")
                    read_string(m.value, "parameters[].unit", p.unit, problems);
                else if (m.key == "min" || m.key == "max")
                {
                    std::string &limit = m.key == "min" ? p.min : p.max;
                    if (m.value.is_number())
                        limit = json::write_compact(m.value);
                    else
                        read_string(m.value, m.key == "min" ? "parameters[].min" : "parameters[].max", limit,
                                    problems);
                }
                else if (m.key == "condition")
                    read_string(m.value, "parameters[].condition", p.condition, problems);
                else
                    p.extra.set(m.key, m.value);
            }
            return p;
        }

        json::value parameter_to_json(const parameter &p)
        {
            json::object o;
            o.set("name", json::value(p.name));
            // A parameter of limits alone has no value to write.
            if (!p.value.empty() || (p.min.empty() && p.max.empty()))
                o.set("value", json::value(p.value));
            put(o, "min", p.min);
            put(o, "max", p.max);
            put(o, "unit", p.unit);
            put(o, "condition", p.condition);
            for (const json::member &m : p.extra)
                if (!o.contains(m.key))
                    o.set(m.key, m.value);
            return json::value(std::move(o));
        }

        // The keys part_to_json writes, which extra must not repeat.
        constexpr std::string_view known_keys[] = {
            "id",      "name",   "category", "description", "manufacturer", "mpn",        "value",      "package",
            "prefix",  "symbol", "footprint", "model",      "datasheet",    "keywords",   "parameters", "origin",
            "device",  "suppliers",
        };

        bool is_known(std::string_view key) noexcept
        {
            for (const std::string_view k : known_keys)
                if (k == key)
                    return true;
            return false;
        }
    } // namespace

    // ---- parameter ---------------------------------------------------------------

    std::optional<double> parameter::number() const noexcept
    {
        return math::electrical::parse_engineering(value, unit);
    }

    std::string parameter::text() const
    {
        std::string shown;
        const bool limits = !min.empty() || !max.empty();
        const std::string range = !min.empty() && !max.empty() ? min + " to " + max
                                  : !max.empty()               ? "up to " + max
                                  : !min.empty()               ? "from " + min
                                                               : std::string();
        if (!value.empty() || !limits)
        {
            shown = value + unit;
            if (limits)
                shown += " (" + range + ")";
        }
        else
            shown = range + unit;
        if (!condition.empty())
            shown += " at " + condition;
        return shown;
    }

    parameter parameter::quantity(std::string name, double value, std::string unit, int significant_digits)
    {
        parameter p;
        p.name = std::move(name);
        math::electrical::engineering_format format;
        format.significant_digits = significant_digits;
        p.value = math::electrical::format_engineering(value, format);
        p.unit = std::move(unit);
        return p;
    }

    // ---- part --------------------------------------------------------------------

    const parameter *part::find_parameter(std::string_view parameter_name) const noexcept
    {
        for (const parameter &p : parameters)
            if (p.name == parameter_name)
                return &p;
        return nullptr;
    }

    bool valid_id(std::string_view text) noexcept
    {
        if (text.empty() || is_space(text.front()) || is_space(text.back()))
            return false;
        for (const char c : text)
            if (c == ':' || static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
                return false;
        return true;
    }

    std::string normalize_category(std::string_view category)
    {
        std::string result;
        while (!category.empty())
        {
            const std::size_t slash = category.find('/');
            const std::string_view segment = trim(category.substr(0, slash));
            if (!segment.empty())
            {
                if (!result.empty())
                    result += '/';
                result += segment;
            }
            if (slash == std::string_view::npos)
                break;
            category.remove_prefix(slash + 1);
        }
        return result;
    }

    std::string part_hash(const part &p)
    {
        part content = p;
        content.id.clear();
        content.origin.reset();
        const std::string text = json::write_compact(part_to_json(content));
        std::uint64_t hash = 14695981039346656037ull;
        for (const char c : text)
        {
            hash ^= static_cast<unsigned char>(c);
            hash *= 1099511628211ull;
        }
        return std::format("{:016x}", hash);
    }

    json::value part_to_json(const part &p)
    {
        json::object o;
        put(o, "id", p.id);
        put(o, "name", p.name);
        put(o, "category", p.category);
        put(o, "description", p.description);
        put(o, "manufacturer", p.manufacturer);
        put(o, "mpn", p.mpn);
        put(o, "value", p.value);
        put(o, "package", p.package);
        put(o, "prefix", p.prefix);
        put(o, "device", p.device);
        put(o, "symbol", p.symbol);
        put(o, "footprint", p.footprint);
        put(o, "model", p.model);
        put(o, "datasheet", p.datasheet);
        if (!p.keywords.empty())
        {
            json::array keywords;
            keywords.reserve(p.keywords.size());
            for (const std::string &k : p.keywords)
                keywords.emplace_back(k);
            o.set("keywords", json::value(std::move(keywords)));
        }
        if (!p.parameters.empty())
        {
            json::array parameters;
            parameters.reserve(p.parameters.size());
            for (const parameter &q : p.parameters)
                parameters.push_back(parameter_to_json(q));
            o.set("parameters", json::value(std::move(parameters)));
        }
        if (!p.suppliers.empty())
        {
            json::array suppliers;
            suppliers.reserve(p.suppliers.size());
            for (const supplier &s : p.suppliers)
            {
                json::object entry;
                put(entry, "name", s.name);
                put(entry, "sku", s.sku);
                put(entry, "url", s.url);
                for (const json::member &m : s.extra)
                    if (!entry.contains(m.key))
                        entry.set(m.key, m.value);
                suppliers.emplace_back(std::move(entry));
            }
            o.set("suppliers", json::value(std::move(suppliers)));
        }
        if (p.origin)
        {
            json::object origin;
            put(origin, "library", p.origin->library);
            put(origin, "part", p.origin->part);
            put(origin, "version", p.origin->version);
            put(origin, "hash", p.origin->hash);
            o.set("origin", json::value(std::move(origin)));
        }
        for (const json::member &m : p.extra)
            if (!is_known(m.key))
                o.set(m.key, m.value);
        return json::value(std::move(o));
    }

    part part_from_json(const json::object &object, std::vector<std::string> &problems)
    {
        part p;
        for (const json::member &m : object)
        {
            const std::string_view key = m.key;
            if (key == "id")
                read_string(m.value, key, p.id, problems);
            else if (key == "name")
                read_string(m.value, key, p.name, problems);
            else if (key == "category")
            {
                read_string(m.value, key, p.category, problems);
                p.category = normalize_category(p.category);
            }
            else if (key == "description")
                read_string(m.value, key, p.description, problems);
            else if (key == "manufacturer")
                read_string(m.value, key, p.manufacturer, problems);
            else if (key == "mpn")
                read_string(m.value, key, p.mpn, problems);
            else if (key == "value")
            {
                if (m.value.is_number())
                    p.value = json::write_compact(m.value);
                else
                    read_string(m.value, key, p.value, problems);
            }
            else if (key == "package")
                read_string(m.value, key, p.package, problems);
            else if (key == "prefix")
                read_string(m.value, key, p.prefix, problems);
            else if (key == "device")
                read_string(m.value, key, p.device, problems);
            else if (key == "symbol")
                read_string(m.value, key, p.symbol, problems);
            else if (key == "footprint")
                read_string(m.value, key, p.footprint, problems);
            else if (key == "model")
                read_string(m.value, key, p.model, problems);
            else if (key == "datasheet")
                read_string(m.value, key, p.datasheet, problems);
            else if (key == "keywords")
            {
                if (const json::array *keywords = m.value.as_array())
                {
                    for (const json::value &k : *keywords)
                    {
                        if (const auto text = k.as_string())
                        {
                            if (!trim(*text).empty())
                                p.keywords.emplace_back(trim(*text));
                        }
                        else
                            problems.push_back(std::format("a keyword should be a string, not {}; ignored",
                                                           json::describe(k.type())));
                    }
                }
                else if (const auto text = m.value.as_string())
                {
                    // "resistor, smd" as hand-written: split at commas.
                    std::string_view rest = *text;
                    while (!rest.empty())
                    {
                        const std::size_t comma = rest.find(',');
                        const std::string_view k = trim(rest.substr(0, comma));
                        if (!k.empty())
                            p.keywords.emplace_back(k);
                        if (comma == std::string_view::npos)
                            break;
                        rest.remove_prefix(comma + 1);
                    }
                }
                else if (!m.value.is_null())
                    problems.push_back(std::format("\"keywords\" should be an array of strings, not {}; ignored",
                                                   json::describe(m.value.type())));
            }
            else if (key == "parameters")
            {
                if (const json::array *parameters = m.value.as_array())
                {
                    for (std::size_t i = 0; i < parameters->size(); ++i)
                    {
                        const json::object *o = (*parameters)[i].as_object();
                        if (o == nullptr)
                        {
                            problems.push_back(std::format("parameter {} should be an object, not {}; ignored", i,
                                                           json::describe((*parameters)[i].type())));
                            continue;
                        }
                        parameter q = parameter_from_json(*o, problems);
                        if (trim(q.name).empty())
                        {
                            problems.push_back(std::format("parameter {} has no name; ignored", i));
                            continue;
                        }
                        p.parameters.push_back(std::move(q));
                    }
                }
                else if (!m.value.is_null())
                    problems.push_back(std::format("\"parameters\" should be an array, not {}; ignored",
                                                   json::describe(m.value.type())));
            }
            else if (key == "suppliers")
            {
                if (const json::array *suppliers = m.value.as_array())
                {
                    for (const json::value &entry : *suppliers)
                    {
                        const json::object *o = entry.as_object();
                        if (o == nullptr)
                        {
                            problems.push_back(std::format("a supplier should be an object, not {}; ignored",
                                                           json::describe(entry.type())));
                            continue;
                        }
                        supplier s;
                        for (const json::member &field : *o)
                        {
                            if (field.key == "name")
                                read_string(field.value, "suppliers[].name", s.name, problems);
                            else if (field.key == "sku")
                                read_string(field.value, "suppliers[].sku", s.sku, problems);
                            else if (field.key == "url")
                                read_string(field.value, "suppliers[].url", s.url, problems);
                            else
                                s.extra.set(field.key, field.value);
                        }
                        if (s.name.empty() && s.sku.empty() && s.url.empty())
                        {
                            problems.push_back("a supplier needs a name, an sku or a url; ignored");
                            continue;
                        }
                        p.suppliers.push_back(std::move(s));
                    }
                }
                else if (!m.value.is_null())
                    problems.push_back(std::format("\"suppliers\" should be an array, not {}; ignored",
                                                   json::describe(m.value.type())));
            }
            else if (key == "origin")
            {
                if (m.value.is_object())
                {
                    part_origin origin;
                    read_string(m.value.at("library"), "origin.library", origin.library, problems);
                    read_string(m.value.at("part"), "origin.part", origin.part, problems);
                    read_string(m.value.at("version"), "origin.version", origin.version, problems);
                    read_string(m.value.at("hash"), "origin.hash", origin.hash, problems);
                    if (origin.library.empty() || origin.part.empty())
                        problems.push_back("\"origin\" needs a library and a part; ignored");
                    else
                        p.origin = std::move(origin);
                }
                else if (!m.value.is_null())
                    problems.push_back(std::format("\"origin\" should be an object, not {}; ignored",
                                                   json::describe(m.value.type())));
            }
            else
                p.extra.set(m.key, m.value);
        }
        return p;
    }

} // namespace volt::library
