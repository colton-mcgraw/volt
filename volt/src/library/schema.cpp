#include <library/schema.hpp>

#include <math/electrical/engineering.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace volt::library
{

    namespace
    {
        namespace el = math::electrical;

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

        // A name as it is compared: ASCII letters in lower case, and spaces,
        // '_', '-' and '.' dropped. "Forward_Voltage" is "forwardvoltage".
        std::string fold(std::string_view name)
        {
            std::string folded;
            folded.reserve(name.size());
            for (const char c : name)
            {
                if (is_space(c) || c == '_' || c == '-' || c == '.')
                    continue;
                folded += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            }
            return folded;
        }

        // The ways a unit is written, the longest first. Most have one.
        std::vector<std::string_view> spellings(std::string_view unit)
        {
            if (unit == "\xCE\xA9") // Ω, the Greek letter
                return {"\xCE\xA9", "\xE2\x84\xA6" /* the ohm sign */, "Ohms", "ohms", "Ohm", "ohm", "R"};
            if (unit == "\xC2\xB0"
                        "C")
                return {"\xC2\xB0"
                        "C",
                        "degC", "C"};
            return {unit};
        }

        // Whether text is nothing, or an SI prefix alone.
        bool is_prefix(std::string_view text) noexcept
        {
            if (text.empty())
                return true;
            std::string one = "1";
            one += text;
            const auto parsed = el::parse_engineering(one);
            // One letter (the micro sign is two bytes), not part of a number.
            if (text.size() > 2 || (text.front() >= '0' && text.front() <= '9') || text.front() == '.' ||
                text.front() == 'e' || text.front() == 'E' || text.front() == 'R')
                return false;
            return parsed && parsed->unit.empty() && parsed->value != 1.0;
        }

        // What is in front of the unit when the text ends with one of its
        // spellings and the rest is an SI prefix or nothing: "k" for "kΩ"
        // or "kOhm", "" for "ohm". Empty optional when the text is in
        // another unit.
        std::optional<std::string_view> prefix_of(std::string_view text, std::string_view unit)
        {
            for (const std::string_view spelling : spellings(unit))
            {
                if (!text.ends_with(spelling))
                    continue;
                const std::string_view rest = trim(text.substr(0, text.size() - spelling.size()));
                if (is_prefix(rest))
                    return rest;
            }
            return std::nullopt;
        }

        // Whether the unit is one the definition is also given in.
        bool is_other_unit(std::string_view unit, const parameter_definition &definition)
        {
            for (const std::string &other : definition.other_units)
                if (prefix_of(unit, other))
                    return true;
            return false;
        }

        // A quantity's value without the unit, should it be written there
        // too: "100 nF" in F is "100 n". The value itself otherwise.
        std::string_view without_unit(std::string_view value, std::string_view unit)
        {
            value = trim(value);
            if (unit.empty())
                return value;
            for (const std::string_view spelling : spellings(unit))
            {
                if (value.size() > spelling.size() && value.ends_with(spelling))
                {
                    const std::string_view rest = trim(value.substr(0, value.size() - spelling.size()));
                    if (el::parse_engineering(rest))
                        return rest;
                }
            }
            return value;
        }

        // The number a value and a prefix make ("4.7" and "k"), when they
        // make one and nothing is left over.
        std::optional<double> number_of(std::string_view value, std::string_view prefix)
        {
            std::string text(value);
            text += prefix;
            const auto parsed = el::parse_engineering(text);
            if (!parsed || !parsed->unit.empty())
                return std::nullopt;
            return parsed->value;
        }

        // The optimal string alignment distance: insertions, deletions,
        // substitutions and swaps of neighbors, which is what typing
        // mistakes are. Stops counting past `limit`.
        std::size_t distance(std::string_view a, std::string_view b, std::size_t limit)
        {
            const std::size_t longer = std::max(a.size(), b.size());
            const std::size_t shorter = std::min(a.size(), b.size());
            if (longer - shorter > limit)
                return limit + 1;
            std::vector<std::size_t> before(b.size() + 1), previous(b.size() + 1), current(b.size() + 1);
            for (std::size_t j = 0; j <= b.size(); ++j)
                previous[j] = j;
            for (std::size_t i = 1; i <= a.size(); ++i)
            {
                current[0] = i;
                for (std::size_t j = 1; j <= b.size(); ++j)
                {
                    const std::size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
                    current[j] = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost});
                    if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                        current[j] = std::min(current[j], before[j - 2] + 1);
                }
                std::swap(before, previous);
                std::swap(previous, current);
            }
            return previous[b.size()];
        }

        // How many mistakes a name of this length may have and still be
        // taken for another; none for names too short to tell.
        std::size_t allowed_mistakes(std::size_t length) noexcept
        {
            return length < 3 ? 0 : length <= 5 ? 1 : length <= 10 ? 2 : 3;
        }

        void put(json::object &o, std::string_view key, const std::string &text)
        {
            if (!text.empty())
                o.set(key, json::value(text));
        }

        json::value strings(const std::vector<std::string> &list)
        {
            json::array array;
            array.reserve(list.size());
            for (const std::string &text : list)
                array.emplace_back(text);
            return json::value(std::move(array));
        }

        parameter_definition quantity(std::string_view name, std::string_view unit, std::string_view description,
                                      std::vector<std::string> aliases = {})
        {
            parameter_definition d;
            d.name = std::string(name);
            d.kind = value_kind::quantity;
            d.unit = std::string(unit);
            d.aliases = std::move(aliases);
            d.description = std::string(description);
            return d;
        }

        parameter_definition text(std::string_view name, std::string_view description,
                                  std::vector<std::string> aliases = {}, std::vector<std::string> choices = {})
        {
            parameter_definition d;
            d.name = std::string(name);
            d.kind = value_kind::text;
            d.aliases = std::move(aliases);
            d.choices = std::move(choices);
            d.description = std::string(description);
            return d;
        }

        device_definition device(std::string_view id, std::string_view name, std::string_view description,
                                 std::vector<device_parameter> parameters = {}, std::vector<std::string> aliases = {})
        {
            device_definition d;
            d.id = std::string(id);
            d.name = std::string(name);
            d.aliases = std::move(aliases);
            d.parameters = std::move(parameters);
            d.description = std::string(description);
            return d;
        }

        schema make_builtin()
        {
            constexpr std::string_view ohm = "\xCE\xA9";
            constexpr std::string_view celsius = "\xC2\xB0"
                                                 "C";
            schema s;

            // Passives.
            s.add(quantity(names::resistance, ohm, "The resistance between the two terminals", {"Ohms"}));
            s.add(quantity(names::capacitance, "F", "The capacitance between the two terminals"));
            s.add(quantity(names::inductance, "H", "The inductance between the two terminals"));
            parameter_definition tolerance =
                quantity(names::tolerance, "%", "How far the value may be from the nominal one, each way");
            tolerance.other_units = {"F", "H", std::string(ohm), "V"};
            s.add(std::move(tolerance));
            // "Voltage" and its like say which voltage only on a passive.
            const std::vector<std::string> passives = {std::string(devices::resistor),
                                                       std::string(devices::capacitor),
                                                       std::string(devices::inductor)};
            const auto with_short = [&](parameter_definition d, std::string_view name,
                                        std::vector<std::string> on) {
                d.short_names.push_back({std::string(name), std::move(on)});
                return d;
            };
            s.add(with_short(quantity(names::power_rating, "W", "The power it may dissipate continuously",
                                      {"Rated power"}),
                             "Power", {std::string(devices::resistor)}));
            s.add(with_short(quantity(names::voltage_rating, "V", "The voltage it may have across it continuously",
                                      {"Rated voltage"}),
                             "Voltage", passives));
            s.add(with_short(quantity(names::current_rating, "A", "The current it may carry continuously",
                                      {"Rated current"}),
                             "Current", passives));
            s.add(quantity(names::temperature_coefficient, "ppm/K", "How much the value changes with temperature",
                           {"Tempco", "TCR"}));
            s.add(with_short(quantity(names::temperature_rating, celsius, "The highest temperature it works at",
                                      {"Maximum temperature"}),
                             "Temperature", passives));
            s.add(text("Series", "The preferred number series its value belongs to: E24, E96"));
            s.add(text("Dielectric", "A ceramic capacitor's dielectric, by its EIA class code", {},
                       {"C0G", "NP0", "U2J", "X5R", "X6S", "X7R", "X7S", "X7T", "X8R", "Y5V", "Z5U"}));
            s.add(quantity(names::esr, ohm, "The equivalent series resistance", {"Equivalent series resistance"}));
            s.add(quantity(names::dc_resistance, ohm, "An inductor's winding resistance", {"DCR"}));
            s.add(quantity(names::saturation_current, "A", "The current at which an inductor's core saturates",
                           {"Isat"}));
            s.add(quantity("Self-resonant frequency", "Hz", "The frequency at which it resonates with itself",
                           {"SRF"}));
            s.add(quantity("Frequency", "Hz", "The frequency it works at: a crystal's, an oscillator's"));

            // Semiconductors.
            s.add(quantity(names::forward_voltage, "V", "The voltage across a diode that conducts", {"Vf"}));
            s.add(quantity(names::forward_current, "A", "The current a diode may conduct continuously"));
            s.add(quantity(names::reverse_voltage, "V", "The voltage a diode blocks", {"Vr", "Vrrm"}));
            s.add(quantity("Reverse current", "A", "The current that leaks through a diode that blocks"));
            s.add(quantity(names::zener_voltage, "V", "The voltage a Zener diode breaks down at", {"Vz"}));
            s.add(text("Type", "The kind of a transistor", {"Polarity"}, {"NPN", "PNP", "N-channel", "P-channel"}));
            s.add(quantity("Reverse recovery time", "s", "How long a diode takes to stop conducting backwards",
                           {"trr"}));
            s.add(quantity("Collector-emitter saturation voltage", "V",
                           "The voltage left across a bipolar transistor that is fully on", {"Vce(sat)"}));
            s.add(quantity("Transition frequency", "Hz", "The frequency at which a transistor's current gain is 1",
                           {"fT"}));
            s.add(quantity("Gate-source voltage", "V", "The gate voltage a MOSFET withstands", {"Vgs"}));
            s.add(quantity("Output voltage", "V", "The voltage a regulator or reference gives", {"Vout"}));
            s.add(quantity("Output current", "A", "The current a regulator can supply", {"Iout"}));
            s.add(quantity("Dropout voltage", "V", "How far above its output a regulator's input must stay"));
            s.add(quantity("Luminous intensity", "cd", "How bright an LED is in the direction it faces"));
            s.add(quantity("Peak wavelength", "nm", "The wavelength an LED emits most of"));
            s.add(quantity("Collector-emitter voltage", "V", "The voltage a bipolar transistor blocks",
                           {"Vce", "Vceo"}));
            s.add(quantity("Collector current", "A", "The collector current it may carry continuously"));
            s.add(quantity("Current gain", "", "A bipolar transistor's collector current over its base current",
                           {"hFE", "Beta"}));
            s.add(quantity("Drain-source voltage", "V", "The voltage a MOSFET blocks", {"Vds", "Vdss"}));
            s.add(quantity("Drain current", "A", "The drain current it may carry continuously"));
            s.add(quantity("Gate threshold voltage", "V", "The gate voltage at which a MOSFET starts to conduct",
                           {"Vgs(th)", "Threshold voltage"}));
            s.add(quantity("On resistance", ohm, "A MOSFET's drain to source resistance when on", {"Rds(on)"}));
            s.add(text("Color", "The color of an LED's light", {"Colour"}));
            s.add(quantity("Wavelength", "nm", "The dominant wavelength of an LED's light"));

            // Mechanical.
            s.add(quantity("Positions", "", "How many contacts a connector has", {"Pins", "Contacts"}));
            s.add(quantity("Rows", "", "How many rows a connector's contacts are in"));
            s.add(quantity("Pitch", "mm", "The distance between neighboring contacts or leads"));
            s.add(quantity("Diameter", "mm", "The diameter of a round body"));
            s.add(quantity("Height", "mm", "The height above the board"));

            // What makes a device that device is required whoever reads it.
            // What a simulation reads on top, the simulation adds: see
            // schema::require.
            s.add(device(devices::resistor, "Resistor", "Two terminals and a resistance",
                         {{std::string(names::resistance), true, "the DC operating point"},
                          {std::string(names::tolerance), false, {}},
                          {std::string(names::power_rating), false, {}}}));
            s.add(device(devices::capacitor, "Capacitor", "Two terminals and a capacitance",
                         {{std::string(names::capacitance), true, {}},
                          {std::string(names::voltage_rating), false, {}},
                          {std::string(names::esr), false, {}}}));
            s.add(device(devices::inductor, "Inductor", "Two terminals and an inductance",
                         {{std::string(names::inductance), true, {}},
                          {std::string(names::dc_resistance), false, {}},
                          {std::string(names::saturation_current), false, {}}}));
            s.add(device(devices::diode, "Diode", "A rectifier, switching or Schottky diode",
                         {{std::string(names::forward_voltage), false, {}},
                          {std::string(names::reverse_voltage), false, {}}}));
            s.add(device(devices::led, "LED", "A light emitting diode",
                         {{std::string(names::forward_voltage), true, {}},
                          {std::string(names::forward_current), false, {}}},
                         {"light emitting diode"}));
            s.add(device(devices::zener, "Zener diode", "A diode used for its breakdown voltage",
                         {{std::string(names::zener_voltage), true, {}}}, {"zener diode"}));
            s.add(device(devices::bjt, "Bipolar transistor", "An NPN or PNP transistor",
                         {{"Type", false, {}}}, {"bipolar transistor", "transistor"}));
            s.add(device(devices::mosfet, "MOSFET", "An N- or P-channel MOSFET", {{"Type", false, {}}}));
            s.add(device(devices::regulator, "Voltage regulator", "A linear or switching voltage regulator",
                         {{"Output voltage", false, {}}, {"Output current", false, {}}}, {"voltage regulator", "LDO"}));
            return s;
        }
    } // namespace

    // ---- issues ----------------------------------------------------------------------

    std::string_view describe(severity level) noexcept
    {
        switch (level)
        {
        case severity::note:
            return "note";
        case severity::warning:
            return "warning";
        case severity::error:
            return "error";
        }
        return "note";
    }

    std::string_view describe(issue_code code) noexcept
    {
        switch (code)
        {
        case issue_code::unknown_parameter:
            return "unknown-parameter";
        case issue_code::misspelled_parameter:
            return "misspelled-parameter";
        case issue_code::ambiguous_parameter:
            return "ambiguous-parameter";
        case issue_code::bad_range:
            return "bad-range";
        case issue_code::duplicate_parameter:
            return "duplicate-parameter";
        case issue_code::missing_value:
            return "missing-value";
        case issue_code::not_a_number:
            return "not-a-number";
        case issue_code::wrong_unit:
            return "wrong-unit";
        case issue_code::unknown_choice:
            return "unknown-choice";
        case issue_code::unknown_device:
            return "unknown-device";
        case issue_code::missing_parameter:
            return "missing-parameter";
        }
        return "unknown";
    }

    std::string_view describe(resolution::kind how) noexcept
    {
        switch (how)
        {
        case resolution::kind::name:
            return "name";
        case resolution::kind::alias:
            return "alias";
        case resolution::kind::short_name:
            return "short-name";
        case resolution::kind::ambiguous:
            return "ambiguous";
        case resolution::kind::unknown:
            return "unknown";
        }
        return "unknown";
    }

    severity worst(std::span<const issue> issues) noexcept
    {
        severity level = severity::note;
        for (const issue &i : issues)
            level = std::max(level, i.level);
        return level;
    }

    // ---- definitions -----------------------------------------------------------------

    const schema &schema::builtin()
    {
        static const schema instance = make_builtin();
        return instance;
    }

    void schema::index()
    {
        // Names first, so that an alias never takes a name's place.
        m_parameter_names.clear();
        for (std::uint32_t i = 0; i < m_parameters.size(); ++i)
            m_parameter_names.emplace(fold(m_parameters[i].name), i);
        for (std::uint32_t i = 0; i < m_parameters.size(); ++i)
            for (const std::string &alias : m_parameters[i].aliases)
                m_parameter_names.emplace(fold(alias), i);

        m_device_names.clear();
        for (std::uint32_t i = 0; i < m_devices.size(); ++i)
            m_device_names.emplace(fold(m_devices[i].id), i);
        for (std::uint32_t i = 0; i < m_devices.size(); ++i)
        {
            m_device_names.emplace(fold(m_devices[i].name), i);
            for (const std::string &alias : m_devices[i].aliases)
                m_device_names.emplace(fold(alias), i);
        }

        // A short name that is also a name or an alias would never be
        // reached, and is left out.
        m_short_names.clear();
        for (std::uint32_t i = 0; i < m_parameters.size(); ++i)
        {
            for (const parameter_definition::short_name &s : m_parameters[i].short_names)
            {
                std::string key = fold(s.name);
                if (m_parameter_names.contains(key))
                    continue;
                short_use use;
                use.parameter = i;
                for (const std::string &device : s.devices)
                    use.devices.push_back(fold(device));
                m_short_names[std::move(key)].push_back(std::move(use));
            }
        }
    }

    std::string schema::device_key(std::string_view device) const
    {
        const device_definition *definition = find_device(device);
        return definition != nullptr ? fold(definition->id) : std::string();
    }

    const parameter_definition *schema::find_short(std::string_view folded_name, std::string_view device) const
    {
        const auto it = m_short_names.find(folded_name);
        if (it == m_short_names.end())
            return nullptr;
        const std::string key = device_key(device);
        if (key.empty())
            return nullptr;
        for (const short_use &use : it->second)
            if (std::ranges::find(use.devices, key) != use.devices.end())
                return &m_parameters[use.parameter];
        return nullptr;
    }

    std::vector<std::string> schema::meanings(std::string_view folded_name) const
    {
        // What it is somewhere, and then every parameter in the same unit
        // whose name has it: for "voltage", the rating, and the forward,
        // reverse, Zener, ... voltages.
        std::vector<std::string> result;
        const auto it = m_short_names.find(folded_name);
        if (it == m_short_names.end())
            return result;
        for (const short_use &use : it->second)
            result.push_back(m_parameters[use.parameter].name);
        const std::string &unit = m_parameters[it->second.front().parameter].unit;
        for (const parameter_definition &d : m_parameters)
            if (d.unit == unit && fold(d.name).find(folded_name) != std::string::npos &&
                std::ranges::find(result, d.name) == result.end())
                result.push_back(d.name);
        return result;
    }

    void schema::add(parameter_definition definition)
    {
        const std::string key = fold(definition.name);
        const auto there = std::ranges::find_if(
            m_parameters, [&](const parameter_definition &d) { return fold(d.name) == key; });
        if (there != m_parameters.end())
            *there = std::move(definition);
        else
            m_parameters.push_back(std::move(definition));
        index();
    }

    void schema::add(device_definition definition)
    {
        const std::string key = fold(definition.id);
        const auto there =
            std::ranges::find_if(m_devices, [&](const device_definition &d) { return fold(d.id) == key; });
        if (there != m_devices.end())
            *there = std::move(definition);
        else
            m_devices.push_back(std::move(definition));
        index();
    }

    bool schema::require(std::string_view device_id, std::string_view parameter_name, std::string_view used_by,
                         bool required)
    {
        const auto device_at = m_device_names.find(fold(device_id));
        const parameter_definition *definition = find(parameter_name);
        if (device_at == m_device_names.end() || definition == nullptr)
            return false;
        device_definition &d = m_devices[device_at->second];
        const auto there = std::ranges::find_if(
            d.parameters, [&](const device_parameter &q) { return find(q.name) == definition; });
        if (there == d.parameters.end())
        {
            d.parameters.push_back({definition->name, required, std::string(used_by)});
            return true;
        }
        there->required = there->required || required;
        if (!used_by.empty() && there->used_by.find(used_by) == std::string::npos)
        {
            if (!there->used_by.empty())
                there->used_by += " and ";
            there->used_by += used_by;
        }
        return true;
    }

    const parameter_definition *schema::find(std::string_view name, std::string_view device) const
    {
        const std::string key = fold(name);
        const auto it = m_parameter_names.find(key);
        if (it != m_parameter_names.end())
            return &m_parameters[it->second];
        return find_short(key, device);
    }

    resolution schema::resolve(std::string_view name, std::string_view device) const
    {
        resolution r;
        const std::string key = fold(name);
        if (const auto it = m_parameter_names.find(key); it != m_parameter_names.end())
        {
            r.definition = &m_parameters[it->second];
            r.how = fold(r.definition->name) == key ? resolution::kind::name : resolution::kind::alias;
        }
        else if (const parameter_definition *found = find_short(key, device))
        {
            r.definition = found;
            r.how = resolution::kind::short_name;
        }
        else if (m_short_names.contains(key))
        {
            r.how = resolution::kind::ambiguous;
            r.candidates = meanings(key);
        }
        else
            r.candidates = suggest(name);
        return r;
    }

    const device_definition *schema::find_device(std::string_view id) const
    {
        const auto it = m_device_names.find(fold(id));
        return it == m_device_names.end() ? nullptr : &m_devices[it->second];
    }

    namespace
    {
        // The definitions whose name or alias is within the allowed
        // mistakes of the folded name, as positions, nearest first.
        template <typename Map>
        std::vector<std::uint32_t> nearest(const Map &folded_names, std::string_view name, std::size_t limit)
        {
            const std::string key = fold(name);
            const std::size_t allowed = allowed_mistakes(key.size());
            if (allowed == 0)
                return {};
            std::vector<std::pair<std::size_t, std::uint32_t>> found; // distance, position
            for (const auto &[candidate, position] : folded_names)
            {
                const std::size_t d = distance(key, candidate, allowed);
                if (d > allowed || d > allowed_mistakes(candidate.size()))
                    continue;
                const auto there = std::ranges::find_if(found, [&](const auto &f) { return f.second == position; });
                if (there == found.end())
                    found.emplace_back(d, position);
                else
                    there->first = std::min(there->first, d);
            }
            // By distance, then by position: the map's order is not to show.
            std::ranges::sort(found);
            std::vector<std::uint32_t> positions;
            for (const auto &[d, position] : found)
            {
                if (positions.size() == limit)
                    break;
                positions.push_back(position);
            }
            return positions;
        }
    } // namespace

    std::vector<std::string> schema::suggest(std::string_view name, std::size_t limit) const
    {
        std::vector<std::string> result;
        for (const std::uint32_t position : nearest(m_parameter_names, name, limit))
            result.push_back(m_parameters[position].name);
        return result;
    }

    std::vector<std::string> schema::suggest_device(std::string_view id, std::size_t limit) const
    {
        std::vector<std::string> result;
        for (const std::uint32_t position : nearest(m_device_names, id, limit))
            result.push_back(m_devices[position].id);
        return result;
    }

    // ---- reading a part ----------------------------------------------------------------

    const parameter *schema::find_parameter(const part &p, std::string_view name) const
    {
        const parameter_definition *definition = find(name, p.device);
        if (definition != nullptr)
        {
            for (const parameter &q : p.parameters)
                if (find(q.name, p.device) == definition)
                    return &q;
            return nullptr;
        }
        const std::string key = fold(name);
        for (const parameter &q : p.parameters)
            if (fold(q.name) == key)
                return &q;
        return nullptr;
    }

    namespace
    {
        // One of a quantity's figures (its value, or a limit) as a number
        // in the definition's unit, given the unit the parameter is
        // written in.
        std::optional<double> read_figure(std::string_view text, std::string_view written_unit,
                                          const parameter_definition &definition)
        {
            const std::string_view value = without_unit(text, definition.unit);
            std::string_view prefix;
            const std::string_view unit = trim(written_unit);
            if (!unit.empty() && !definition.unit.empty())
            {
                const auto front = prefix_of(unit, definition.unit);
                if (!front)
                    return std::nullopt; // another unit
                prefix = *front;
            }
            return number_of(value, prefix);
        }
    } // namespace

    quantity_range schema::range(const parameter &q, std::string_view device) const
    {
        quantity_range r;
        const parameter_definition *definition = find(q.name, device);
        if (definition != nullptr && definition->kind != value_kind::quantity)
            return r;
        const auto figure = [&](const std::string &text) -> std::optional<double> {
            if (trim(text).empty())
                return std::nullopt;
            // Unknown to the schema: as the parameter reads itself.
            return definition != nullptr ? read_figure(text, q.unit, *definition)
                                         : el::parse_engineering(text, q.unit);
        };
        r.min = figure(q.min);
        r.typical = figure(q.value);
        r.max = figure(q.max);
        return r;
    }

    quantity_range schema::range(const part &p, std::string_view name) const
    {
        const parameter *q = find_parameter(p, name);
        return q != nullptr ? range(*q, p.device) : quantity_range{};
    }

    std::optional<double> schema::number(const parameter &q, std::string_view device) const
    {
        const quantity_range r = range(q, device);
        if (!trim(q.value).empty())
            return r.typical;
        if (r.min && r.max)
            return (*r.min + *r.max) / 2.0;
        return std::nullopt;
    }

    std::optional<double> schema::number(const part &p, std::string_view name) const
    {
        const parameter *q = find_parameter(p, name);
        return q != nullptr ? number(*q, p.device) : std::nullopt;
    }

    // ---- checking a part ---------------------------------------------------------------

    std::vector<issue> schema::check(const part &p) const
    {
        std::vector<issue> issues;
        const auto add = [&](severity level, issue_code code, std::string_view parameter_name, std::string message,
                             std::string suggestion = {}) {
            issues.push_back({level, code, std::string(parameter_name), std::move(message), std::move(suggestion)});
        };

        std::vector<const parameter_definition *> seen;
        std::vector<std::string> seen_unknown;
        for (const parameter &q : p.parameters)
        {
            const parameter_definition *definition = find(q.name, p.device);
            if (definition == nullptr)
            {
                std::string key = fold(q.name);
                if (std::ranges::find(seen_unknown, key) != seen_unknown.end())
                {
                    add(severity::warning, issue_code::duplicate_parameter, q.name,
                        std::format("\"{}\" is given more than once; the first is used", q.name));
                    continue;
                }
                seen_unknown.push_back(key);

                // A short name on a part it does not say enough for.
                if (const std::vector<std::string> could_be = meanings(key); !could_be.empty())
                {
                    std::string list;
                    for (const std::string &name : could_be)
                        list += (list.empty() ? "\"" : ", \"") + name + "\"";
                    add(severity::warning, issue_code::ambiguous_parameter, q.name,
                        trim(p.device).empty()
                            ? std::format("\"{}\" does not say which: the part has no device to tell by, so nothing "
                                          "reads it. It could be {}",
                                          q.name, list)
                            : std::format("\"{}\" does not say which for a part whose device is \"{}\", so nothing "
                                          "reads it. It could be {}",
                                          q.name, p.device, list));
                    continue;
                }

                const std::vector<std::string> close = suggest(q.name, 1);
                if (!close.empty())
                    add(severity::warning, issue_code::misspelled_parameter, q.name,
                        std::format("\"{}\" is not a parameter Volt knows, so nothing that looks for \"{}\" will "
                                    "find it; did you mean \"{}\"?",
                                    q.name, close.front(), close.front()),
                        close.front());
                else
                    add(severity::note, issue_code::unknown_parameter, q.name,
                        std::format("\"{}\" is not a parameter Volt knows: it is kept and shown, but no simulation "
                                    "reads it",
                                    q.name));
                continue;
            }

            if (std::ranges::find(seen, definition) != seen.end())
            {
                add(severity::warning, issue_code::duplicate_parameter, q.name,
                    q.name == definition->name
                        ? std::format("\"{}\" is given more than once; the first is used", q.name)
                        : std::format("\"{}\" is \"{}\", which the part has already; the first is used", q.name,
                                      definition->name));
                continue;
            }
            seen.push_back(definition);

            const bool has_value = !trim(q.value).empty();
            const bool has_limits = !trim(q.min).empty() || !trim(q.max).empty();
            if (!has_value && !(has_limits && definition->kind == value_kind::quantity))
            {
                add(severity::warning, issue_code::missing_value, q.name,
                    std::format("\"{}\" has no value", q.name));
                continue;
            }

            if (definition->kind == value_kind::quantity)
            {
                // The figures there are, each with what to call it.
                struct figure
                {
                    const std::string *text;
                    std::string_view what; // "", "the minimum of ", ...
                };
                const figure figures[] = {{&q.value, ""}, {&q.min, "the minimum of "}, {&q.max, "the maximum of "}};
                const auto each_is_a_number = [&](auto &&reads, std::string_view in) {
                    bool all = true;
                    for (const figure &f : figures)
                    {
                        if (trim(*f.text).empty() || reads(*f.text))
                            continue;
                        all = false;
                        add(severity::error, issue_code::not_a_number, q.name,
                            std::format("{}\"{}\" should be a number{}, not \"{}\"", f.what, q.name, in, *f.text));
                    }
                    return all;
                };

                const std::string_view unit = trim(q.unit);
                if (!unit.empty() && is_other_unit(unit, *definition))
                {
                    each_is_a_number(
                        [&](const std::string &text) { return number_of(without_unit(text, unit), {}).has_value(); },
                        {});
                    continue;
                }
                if (!unit.empty() && !definition->unit.empty() && !prefix_of(unit, definition->unit))
                {
                    add(severity::error, issue_code::wrong_unit, q.name,
                        std::format("\"{}\" is a quantity in {}, not in \"{}\"", q.name, definition->unit, unit),
                        definition->unit);
                    continue;
                }
                const bool numbers = each_is_a_number(
                    [&](const std::string &text) { return read_figure(text, q.unit, *definition).has_value(); },
                    definition->unit.empty() ? std::string()
                                             : std::format(" in {} (such as 4.7k)", definition->unit));
                if (!numbers)
                    continue;
                const quantity_range r = range(q, p.device);
                if (r.min && r.max && *r.min > *r.max)
                    add(severity::warning, issue_code::bad_range, q.name,
                        std::format("the minimum of \"{}\" ({}) is more than its maximum ({})", q.name, q.min,
                                    q.max));
                else if (r.typical && ((r.min && *r.typical < *r.min) || (r.max && *r.typical > *r.max)))
                    add(severity::warning, issue_code::bad_range, q.name,
                        std::format("the value of \"{}\" ({}) is outside its limits", q.name, q.value));
            }
            else if (!definition->choices.empty())
            {
                const std::string value = fold(q.value);
                const bool known = std::ranges::any_of(
                    definition->choices, [&](const std::string &choice) { return fold(choice) == value; });
                if (!known)
                {
                    std::string list;
                    for (const std::string &choice : definition->choices)
                        list += (list.empty() ? "" : ", ") + choice;
                    add(severity::warning, issue_code::unknown_choice, q.name,
                        std::format("\"{}\" is \"{}\", which is not one of {}", q.name, q.value, list));
                }
            }
        }

        if (trim(p.device).empty())
            return issues;
        const device_definition *device = find_device(p.device);
        if (device == nullptr)
        {
            const std::vector<std::string> close = suggest_device(p.device, 1);
            add(severity::warning, issue_code::unknown_device, {},
                close.empty() ? std::format("\"{}\" is not a device Volt knows, so no simulation models this part",
                                            p.device)
                              : std::format("\"{}\" is not a device Volt knows, so no simulation models this part; "
                                            "did you mean \"{}\"?",
                                            p.device, close.front()),
                close.empty() ? std::string() : close.front());
            return issues;
        }
        for (const device_parameter &wanted : device->parameters)
        {
            if (!wanted.required || find_parameter(p, wanted.name) != nullptr)
                continue;
            add(severity::error, issue_code::missing_parameter, wanted.name,
                wanted.used_by.empty()
                    ? std::format("\"{}\" is missing: a part whose device is \"{}\" needs it", wanted.name,
                                  device->id)
                    : std::format("\"{}\" is missing: a part whose device is \"{}\" needs it, and {} reads it",
                                  wanted.name, device->id, wanted.used_by));
        }
        return issues;
    }

    std::size_t schema::normalize(part &p) const
    {
        std::size_t changes = 0;
        // The device first: it decides what a short name is.
        if (const device_definition *device = find_device(p.device); device != nullptr && p.device != device->id)
        {
            p.device = device->id;
            ++changes;
        }
        for (parameter &q : p.parameters)
        {
            const parameter_definition *definition = find(q.name, p.device);
            if (definition == nullptr)
                continue;
            if (q.name != definition->name)
            {
                q.name = definition->name;
                ++changes;
            }
            if (definition->kind != value_kind::quantity || definition->unit.empty() || q.unit == definition->unit)
                continue;
            const std::string_view unit = trim(q.unit);
            if (unit.empty())
                continue;
            // The unit's prefix moves to the figures: "700" "mV" is "700m"
            // "V". All of them, or none.
            const auto prefix = prefix_of(unit, definition->unit);
            if (!prefix)
                continue;
            std::string *const figures[] = {&q.value, &q.min, &q.max};
            const bool movable = std::ranges::all_of(figures, [&](const std::string *text) {
                return trim(*text).empty() || number_of(without_unit(*text, definition->unit), *prefix).has_value();
            });
            if (!movable)
                continue;
            for (std::string *text : figures)
            {
                if (trim(*text).empty())
                    continue;
                std::string moved(without_unit(*text, definition->unit));
                moved += *prefix;
                *text = std::move(moved);
            }
            q.unit = definition->unit;
            ++changes;
        }
        return changes;
    }

    // ---- JSON --------------------------------------------------------------------------

    json::value schema_to_json(const schema &s)
    {
        json::array parameters;
        for (const parameter_definition &d : s.parameters())
        {
            json::object o;
            o.set("name", json::value(d.name));
            o.set("kind", json::value(d.kind == value_kind::quantity ? "quantity" : "text"));
            put(o, "unit", d.unit);
            if (!d.other_units.empty())
                o.set("other_units", strings(d.other_units));
            if (!d.aliases.empty())
                o.set("aliases", strings(d.aliases));
            if (!d.short_names.empty())
            {
                json::array shorts;
                for (const parameter_definition::short_name &n : d.short_names)
                {
                    json::object entry;
                    entry.set("name", json::value(n.name));
                    entry.set("devices", strings(n.devices));
                    shorts.emplace_back(std::move(entry));
                }
                o.set("short_names", json::value(std::move(shorts)));
            }
            if (!d.choices.empty())
                o.set("choices", strings(d.choices));
            put(o, "description", d.description);
            parameters.emplace_back(std::move(o));
        }
        json::array devices;
        for (const device_definition &d : s.devices())
        {
            json::object o;
            o.set("id", json::value(d.id));
            put(o, "name", d.name);
            if (!d.aliases.empty())
                o.set("aliases", strings(d.aliases));
            put(o, "description", d.description);
            json::array wanted;
            for (const device_parameter &q : d.parameters)
            {
                json::object w;
                w.set("name", json::value(q.name));
                w.set("required", json::value(q.required));
                put(w, "used_by", q.used_by);
                wanted.emplace_back(std::move(w));
            }
            o.set("parameters", json::value(std::move(wanted)));
            devices.emplace_back(std::move(o));
        }
        json::object root;
        root.set("parameters", json::value(std::move(parameters)));
        root.set("devices", json::value(std::move(devices)));
        return json::value(std::move(root));
    }

    json::value issue_to_json(const issue &i)
    {
        json::object o;
        o.set("level", json::value(describe(i.level)));
        o.set("code", json::value(describe(i.code)));
        put(o, "parameter", i.parameter);
        o.set("message", json::value(i.message));
        put(o, "suggestion", i.suggestion);
        return json::value(std::move(o));
    }

} // namespace volt::library
