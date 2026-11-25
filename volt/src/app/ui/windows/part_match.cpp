#include <app/ui/windows/part_match.hpp>

#include <library/schema.hpp>
#include <math/electrical/engineering.hpp>
#include <schematic/symbol.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace volt::ui
{
    namespace
    {
        constexpr generic_entry part_entry(std::string_view name, std::string_view symbol, std::string_view value,
                                           std::string_view keywords)
        {
            return {generic_entry::kind::part, name, symbol, value, keywords};
        }

        constexpr generic_entry power_entry(std::string_view name, std::string_view glyph, std::string_view keywords)
        {
            return {generic_entry::kind::power, name, glyph, {}, keywords};
        }

        constexpr generic_entry entries[] = {
            part_entry("Resistor", "R", "10k", "generic placeholder passive res"),
            part_entry("Capacitor", "C", "100n", "generic placeholder passive cap ceramic"),
            part_entry("Polarized Capacitor", "C_Polarized", "10u", "generic placeholder passive cap electrolytic"),
            part_entry("Inductor", "L", "10u", "generic placeholder passive coil choke"),
            part_entry("Diode", "D", "", "generic placeholder rectifier"),
            part_entry("Zener Diode", "D_Zener", "", "generic placeholder reference"),
            part_entry("Schottky Diode", "D_Schottky", "", "generic placeholder"),
            part_entry("LED", "LED", "", "generic placeholder light diode"),
            part_entry("NPN Transistor", "Q_NPN", "", "generic placeholder bjt bipolar"),
            part_entry("PNP Transistor", "Q_PNP", "", "generic placeholder bjt bipolar"),
            part_entry("N-MOSFET", "Q_NMOS", "", "generic placeholder fet transistor n-channel"),
            part_entry("P-MOSFET", "Q_PMOS", "", "generic placeholder fet transistor p-channel"),
            part_entry("Op-amp", "OpAmp", "", "generic placeholder opamp amplifier"),
            part_entry("DC Voltage Source", "V_DC", "5V", "simulation supply battery"),
            part_entry("DC Current Source", "I_DC", "1mA", "simulation"),
            part_entry("Connector 1x2", "Conn_01x02", "", "generic placeholder header pins"),
            part_entry("Connector 1x3", "Conn_01x03", "", "generic placeholder header pins"),
            part_entry("Connector 1x4", "Conn_01x04", "", "generic placeholder header pins"),
            part_entry("Connector 1x5", "Conn_01x05", "", "generic placeholder header pins"),
            part_entry("Connector 1x6", "Conn_01x06", "", "generic placeholder header pins"),
            part_entry("Connector 1x7", "Conn_01x07", "", "generic placeholder header pins"),
            part_entry("Connector 1x8", "Conn_01x08", "", "generic placeholder header pins"),
            power_entry("GND", "GND", "power port ground 0V"),
            power_entry("+3V3", "PWR", "power port supply 3.3V"),
            power_entry("+5V", "PWR", "power port supply"),
            power_entry("+12V", "PWR", "power port supply"),
            power_entry("-12V", "PWR", "power port supply negative"),
            power_entry("VCC", "PWR", "power port supply"),
        };

        constexpr char fold(char c) noexcept
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        std::string folded(std::string_view text)
        {
            std::string out(text);
            for (char &c : out)
                c = fold(c);
            return out;
        }

        // A device's id however it is written ("Resistor", "resistor").
        std::string device_id(std::string_view device)
        {
            if (const library::device_definition *d = library::schema::builtin().find_device(device))
                return d->id;
            return folded(device);
        }

        // The placeholder's value as its device's principal quantity, when
        // it reads as one.
        std::optional<double> wanted_value(const placeholder &p)
        {
            if (p.device.empty() || p.value.empty())
                return std::nullopt;
            const library::principal_parameter principal = library::principal_of(p.device);
            if (principal.name.empty())
                return std::nullopt;
            const auto q = library::query_quantity(p.value);
            if (!q || (q->unit != library::quantity_unit::none && q->unit != principal.unit))
                return std::nullopt;
            return q->value;
        }

        // 0603 first, as the chip size most designs use, then 0805, 0402,
        // 1206; anything else after.
        int chip_rank(std::string_view package) noexcept
        {
            constexpr std::string_view order[] = {"0603", "0805", "0402", "1206"};
            for (std::size_t i = 0; i < std::size(order); ++i)
                if (package == order[i])
                    return static_cast<int>(i);
            return static_cast<int>(std::size(order));
        }
    } // namespace

    // ---- the Generic scope ------------------------------------------------------------

    std::span<const generic_entry> generic_entries() noexcept
    {
        return entries;
    }

    bool matches(const generic_entry &entry, std::string_view text)
    {
        std::string haystack = folded(entry.name);
        haystack += ' ';
        haystack += folded(entry.symbol);
        haystack += ' ';
        haystack += folded(entry.value);
        haystack += ' ';
        haystack += folded(entry.keywords);
        std::size_t at = 0;
        while (at < text.size())
        {
            while (at < text.size() && (text[at] == ' ' || text[at] == '\t'))
                ++at;
            std::size_t end = at;
            while (end < text.size() && text[end] != ' ' && text[end] != '\t')
                ++end;
            if (end > at && haystack.find(folded(text.substr(at, end - at))) == std::string::npos)
                return false;
            at = end;
        }
        return true;
    }

    // ---- placeholders -------------------------------------------------------------------

    bool is_placeholder(const schematic::part &p)
    {
        if (!p.library_part.empty())
            return false;
        if (p.device == "voltage_source" || p.device == "current_source")
            return false;
        return schematic::find_symbol(p.symbol).prefix != "#PWR";
    }

    placeholder placeholder_of(const schematic::part &p)
    {
        return {p.device, p.value, p.symbol};
    }

    std::string describe(const placeholder &p)
    {
        std::string what;
        if (const library::device_definition *d = library::schema::builtin().find_device(p.device))
        {
            what = d->name;
            // "Resistor" reads as "resistor" after a value; "LED" stays.
            if (what.size() > 1 && what[0] >= 'A' && what[0] <= 'Z' && what[1] >= 'a' && what[1] <= 'z')
                what[0] = static_cast<char>(what[0] - 'A' + 'a');
        }
        else
            what = !p.device.empty() ? p.device : p.symbol;

        std::string value = p.value;
        if (const auto number = wanted_value(p))
            value = math::electrical::format_engineering(*number) +
                    std::string(library::unit_symbol(library::principal_of(p.device).unit));
        if (value.empty())
            return what;
        return value + " " + what;
    }

    bool pins_compatible(std::string_view symbol_a, std::string_view symbol_b)
    {
        if (symbol_a == symbol_b)
            return true;
        if (symbol_a.empty() || symbol_b.empty())
            return false;
        const schematic::symbol &a = schematic::find_symbol(symbol_a);
        const schematic::symbol &b = schematic::find_symbol(symbol_b);
        if (a.pins.size() != b.pins.size())
            return false;
        for (const schematic::pin &pin : a.pins)
        {
            const schematic::pin *other = b.find_pin(pin.number);
            if (other == nullptr || other->position != pin.position)
                return false;
        }
        return true;
    }

    // ---- candidates ------------------------------------------------------------------------

    std::vector<part_candidate> match_parts(const placeholder &p, const library::catalog *catalog,
                                            const library::project *project)
    {
        const std::string device = p.device.empty() ? std::string() : device_id(p.device);
        const std::optional<double> value = wanted_value(p);
        // A value that is not a quantity of the device: a part number,
        // perhaps ("1N4148"), compared as text.
        const std::string text_value = value || p.value.empty() ? std::string() : folded(p.value);

        std::vector<part_candidate> found;
        const auto consider = [&](const library::part &part, std::string project_id,
                                  std::optional<library::part_ref> ref) {
            if (!device.empty())
            {
                if (part.device.empty() || device_id(part.device) != device)
                    return;
            }
            else if (part.symbol != p.symbol)
                return;
            const bool same_symbol = part.symbol == p.symbol;
            if (!same_symbol && !pins_compatible(part.symbol, p.symbol))
                return;
            part_candidate c;
            c.part = &part;
            c.project_id = std::move(project_id);
            c.ref = ref;
            c.other_symbol = !same_symbol;
            if (value)
            {
                const auto principal = library::principal_value(part);
                if (!principal || !library::same_value(principal->value, *value))
                    return;
                c.value_matched = true;
            }
            else if (!text_value.empty())
                c.value_matched = folded(part.value) == text_value;
            found.push_back(std::move(c));
        };

        // The project's parts, and which catalog parts they are copies of.
        std::set<std::pair<std::string, std::string>, std::less<>> copied;
        std::map<std::string, int, std::less<>> package_uses;
        if (project != nullptr)
        {
            for (const library::part &part : project->parts.parts)
            {
                if (part.origin)
                    copied.emplace(part.origin->library, part.origin->part);
                if (!part.package.empty())
                    ++package_uses[part.package];
                consider(part, part.id, std::nullopt);
            }
        }
        if (catalog != nullptr)
        {
            for (std::uint32_t l = 0; l < catalog->libraries().size(); ++l)
            {
                const library::library &lib = catalog->libraries()[l];
                for (std::uint32_t i = 0; i < lib.parts.size(); ++i)
                {
                    const library::part &part = lib.parts[i];
                    if (copied.contains(std::pair{lib.name, part.id}))
                        continue;
                    consider(part, {}, library::part_ref{l, i});
                }
            }
        }

        // A part number typed as the value: only the parts that have it,
        // when some do.
        if (!text_value.empty() && std::ranges::any_of(found, [](const part_candidate &c) { return c.value_matched; }))
            std::erase_if(found, [](const part_candidate &c) { return !c.value_matched; });

        const auto uses = [&](const part_candidate &c) {
            const auto it = package_uses.find(c.part->package);
            return it == package_uses.end() ? 0 : it->second;
        };
        std::stable_sort(found.begin(), found.end(), [&](const part_candidate &a, const part_candidate &b) {
            if (a.in_project() != b.in_project())
                return a.in_project();
            if (a.other_symbol != b.other_symbol)
                return !a.other_symbol;
            if (const int ua = uses(a), ub = uses(b); ua != ub)
                return ua > ub;
            if (const int ra = chip_rank(a.part->package), rb = chip_rank(b.part->package); ra != rb)
                return ra < rb;
            if (a.part->mpn.empty() != b.part->mpn.empty())
                return !a.part->mpn.empty();
            return false;
        });
        return found;
    }

    std::vector<part_candidate> filter_candidates(std::span<const part_candidate> candidates,
                                                  const library::catalog *catalog, std::string_view text)
    {
        std::vector<part_candidate> kept;
        for (const part_candidate &c : candidates)
        {
            if (catalog == nullptr || c.part == nullptr || catalog->match(*c.part, text) != 0)
                kept.push_back(c);
        }
        return kept;
    }

    // ---- assigning ----------------------------------------------------------------------------

    schematic::part assigned(const schematic::part &placed, const library::part &part, std::string_view project_id)
    {
        schematic::part result = placed;
        result.library_part = std::string(project_id);
        if (!part.value.empty())
            result.value = part.value;
        if (!part.device.empty())
            result.device = part.device;
        if (!part.symbol.empty() && pins_compatible(part.symbol, placed.symbol))
            result.symbol = part.symbol;
        return result;
    }

} // namespace volt::ui
