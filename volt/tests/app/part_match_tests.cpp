#include "../math/check.hpp"

#include <app/ui/windows/part_match.hpp>
#include <schematic/symbol.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

// Placeholders and the library parts that can stand in for them, without a
// window: which placed parts are placeholders, the Generic scope's entries
// and search, the candidates for a placeholder (device, value, symbol and
// pins, the project's parts first, the order), and a placed part after a
// part is assigned to it.

using namespace volt;

namespace
{
    library::parameter param(std::string name, std::string value, std::string unit = {})
    {
        library::parameter q;
        q.name = std::move(name);
        q.value = std::move(value);
        q.unit = std::move(unit);
        return q;
    }

    library::part make(std::string id, std::string device, std::string symbol, std::string value, std::string package,
                       std::vector<library::parameter> parameters = {}, std::string mpn = {})
    {
        library::part p;
        p.id = std::move(id);
        p.name = p.id;
        p.device = std::move(device);
        p.symbol = std::move(symbol);
        p.value = std::move(value);
        p.package = std::move(package);
        p.parameters = std::move(parameters);
        p.mpn = std::move(mpn);
        return p;
    }

    library::part resistor(std::string id, std::string value, std::string package, std::string mpn = "MPN")
    {
        return make(std::move(id), "resistor", "R", value, std::move(package), {param("Resistance", value, "\xCE\xA9")},
                    std::move(mpn));
    }

    library::catalog sample()
    {
        library::library lib;
        lib.name = "lib";
        lib.version = "1";
        lib.parts.push_back(resistor("R1k_1206", "1k", "1206"));             // 0
        lib.parts.push_back(resistor("R1k_0805", "1k", "0805"));             // 1
        lib.parts.push_back(resistor("R1k_0603_nompn", "1k", "0603", ""));   // 2
        lib.parts.push_back(resistor("R1k_0603", "1k", "0603"));             // 3
        lib.parts.push_back(resistor("R11k_0603", "11k", "0603"));           // 4
        lib.parts.push_back(resistor("R10k_0603", "10k", "0603"));           // 5
        lib.parts.push_back(make("C10u_cer", "capacitor", "C", "10uF", "0805", {param("Capacitance", "10u", "F")}));
        lib.parts.push_back(
            make("C10u_el", "capacitor", "C_Polarized", "10uF", "Radial", {param("Capacitance", "10u", "F")}));
        lib.parts.push_back(make("D_1N4148", "diode", "D", "1N4148", "DO-35"));
        lib.parts.push_back(make("D_1N5817", "diode", "D_Schottky", "1N5817", "DO-41"));
        lib.parts.push_back(make("D_BAV99", "diode", "D_Dual_Series", "BAV99", "SOT-23")); // pins 1 to 8: no
        lib.parts.push_back(make("J_1x4", "", "Conn_01x04", "1x4", "1x04 P2.54mm"));
        lib.parts.push_back(make("J_1x2", "", "Conn_01x02", "1x2", "1x02 P2.54mm"));
        lib.parts.push_back(make("U_MCP6001", "", "OpAmp", "MCP6001", "SOT-23-5"));
        lib.parts.push_back(make("U_LM358", "", "OpAmp_Dual", "LM358", "SOIC-8"));
        library::catalog c;
        c.add(std::move(lib));
        return c;
    }

    schematic::part placed(std::string symbol, std::string device, std::string value)
    {
        schematic::part p;
        p.symbol = std::move(symbol);
        p.device = std::move(device);
        p.value = std::move(value);
        p.reference = "X1";
        p.position = {100, 200};
        p.rotation = 1;
        return p;
    }

    std::vector<std::string> ids(const std::vector<ui::part_candidate> &found)
    {
        std::vector<std::string> out;
        for (const ui::part_candidate &c : found)
            out.push_back((c.in_project() ? "project:" : "") + c.part->id);
        return out;
    }

    void placeholders()
    {
        CHECK(ui::is_placeholder(placed("R", "resistor", "10k")));
        CHECK(ui::is_placeholder(placed("OpAmp", "", "")));
        schematic::part real = placed("R", "resistor", "10k");
        real.library_part = "R_10k";
        CHECK(!ui::is_placeholder(real));
        // The simulation's sources and power glyphs never have a part.
        CHECK(!ui::is_placeholder(placed("V_DC", "voltage_source", "5V")));
        CHECK(!ui::is_placeholder(placed("I_DC", "current_source", "1mA")));
        CHECK(!ui::is_placeholder(placed("GND", "", "GND")));

        CHECK(ui::describe(ui::placeholder_of(placed("R", "resistor", "1k"))) == "1k\xCE\xA9 resistor");
        CHECK(ui::describe(ui::placeholder_of(placed("C", "capacitor", "100n"))) == "100nF capacitor");
        CHECK(ui::describe(ui::placeholder_of(placed("LED", "led", ""))) == "LED");
        CHECK(ui::describe(ui::placeholder_of(placed("Conn_01x04", "", ""))) == "Conn_01x04");

        CHECK(ui::pins_compatible("C", "C_Polarized"));
        CHECK(ui::pins_compatible("D", "D_Schottky"));
        CHECK(!ui::pins_compatible("D", "D_Dual_Series"));
        CHECK(!ui::pins_compatible("Q_NPN", "Q_PNP")); // collector and emitter swap places
        CHECK(!ui::pins_compatible("R", ""));
    }

    void generic()
    {
        const auto entries = ui::generic_entries();
        const auto find = [&](std::string_view name) {
            return std::ranges::find_if(entries, [&](const ui::generic_entry &e) { return e.name == name; });
        };
        CHECK(find("Resistor") != entries.end() && find("Resistor")->symbol == "R" && find("Resistor")->value == "10k");
        CHECK(find("GND") != entries.end() && find("GND")->what == ui::generic_entry::kind::power);
        CHECK(find("+3V3") != entries.end() && find("+3V3")->symbol == "PWR");
        CHECK(find("Connector 1x8") != entries.end());
        // Every part entry is a built-in or generated symbol.
        for (const ui::generic_entry &e : entries)
            CHECK(e.what == ui::generic_entry::kind::power || schematic::is_builtin_symbol(e.symbol) ||
                  e.symbol.starts_with("Conn_01x"));

        const auto count = [&](std::string_view text) {
            return std::ranges::count_if(entries, [&](const ui::generic_entry &e) { return ui::matches(e, text); });
        };
        CHECK(count("") == static_cast<std::ptrdiff_t>(entries.size()));
        CHECK(count("res") == 1);
        CHECK(count("GND") == 1);
        CHECK(count("power") == 6);
        CHECK(count("conn 1x4") == 1);
        CHECK(count("mosfet") == 2);
        CHECK(count("zzz") == 0);
    }

    void candidates()
    {
        const library::catalog c = sample();

        // The same device and value, numerically: not 11k, not 10k; a
        // common chip size first, then an MPN.
        const auto one_k = ui::match_parts(ui::placeholder_of(placed("R", "resistor", "1k")), &c, nullptr);
        CHECK(ids(one_k) == (std::vector<std::string>{"R1k_0603", "R1k_0603_nompn", "R1k_0805", "R1k_1206"}));
        CHECK(std::ranges::all_of(one_k, [](const ui::part_candidate &x) { return x.value_matched && !x.other_symbol; }));
        CHECK(ids(ui::match_parts(ui::placeholder_of(placed("R", "resistor", "1000")), &c, nullptr)) == ids(one_k));
        // An empty or unreadable value: the device only.
        CHECK(ui::match_parts(ui::placeholder_of(placed("R", "resistor", "")), &c, nullptr).size() == 6);
        CHECK(ui::match_parts(ui::placeholder_of(placed("R", "resistor", "Rload")), &c, nullptr).size() == 6);

        // Another symbol with the same pins comes after the own one, flagged.
        const auto polarized = ui::match_parts(ui::placeholder_of(placed("C_Polarized", "capacitor", "10u")), &c, nullptr);
        CHECK(ids(polarized) == (std::vector<std::string>{"C10u_el", "C10u_cer"}));
        CHECK(polarized.size() == 2 && !polarized[0].other_symbol && polarized[1].other_symbol);

        // A diode: any diode whose pins fit (not the dual one); a part
        // number typed as the value picks it out.
        CHECK(ids(ui::match_parts(ui::placeholder_of(placed("D", "diode", "")), &c, nullptr)) ==
              (std::vector<std::string>{"D_1N4148", "D_1N5817"}));
        CHECK(ids(ui::match_parts(ui::placeholder_of(placed("D", "diode", "1n5817")), &c, nullptr)) ==
              std::vector<std::string>{"D_1N5817"});

        // No device: the same symbol.
        CHECK(ids(ui::match_parts(ui::placeholder_of(placed("Conn_01x04", "", "")), &c, nullptr)) ==
              std::vector<std::string>{"J_1x4"});
        CHECK(ids(ui::match_parts(ui::placeholder_of(placed("OpAmp", "", "")), &c, nullptr)) ==
              std::vector<std::string>{"U_MCP6001"});

        // The project's parts first; a catalog part it has a copy of once.
        library::project p;
        p.parts.name = "project";
        library::part copy = c.get(*c.find("lib:R1k_1206"));
        copy.id = "R_1k";
        copy.origin = library::part_origin{"lib", "R1k_1206", "1", ""};
        p.parts.parts.push_back(copy);
        library::part other = resistor("R_10k", "10k", "0805");
        p.parts.parts.push_back(other);
        const auto reused = ui::match_parts(ui::placeholder_of(placed("R", "resistor", "1k")), &c, &p);
        // 0805 is the package the project uses most now (with 1206).
        CHECK(ids(reused) == (std::vector<std::string>{"project:R_1k", "R1k_0805", "R1k_0603", "R1k_0603_nompn"}));
        CHECK(!reused.empty() && reused[0].project_id == "R_1k" && !reused[0].ref);
        CHECK(reused.size() == 4 && reused[1].ref && c.get(*reused[1].ref).id == "R1k_0805");

        // Searching within them.
        CHECK(ids(ui::filter_candidates(one_k, &c, "0805")) == std::vector<std::string>{"R1k_0805"});
        CHECK(ui::filter_candidates(one_k, &c, "").size() == one_k.size());
        CHECK(ui::filter_candidates(one_k, &c, "nothing").empty());
    }

    void assigning()
    {
        const library::catalog c = sample();
        const schematic::part placeholder = placed("C_Polarized", "capacitor", "10u");

        // A ceramic part assigned to a polarized placeholder: same pins,
        // so its symbol; its value; the project's id.
        const schematic::part done = ui::assigned(placeholder, c.get(*c.find("lib:C10u_cer")), "C_10u");
        CHECK(done.library_part == "C_10u");
        CHECK(done.value == "10uF");
        CHECK(done.symbol == "C");
        CHECK(done.device == "capacitor");
        CHECK(done.reference == "X1" && done.position == placeholder.position && done.rotation == 1 && !done.mirrored);
        CHECK(!ui::is_placeholder(done));

        // A part whose symbol does not fit keeps the placed one; a part
        // without a value or device keeps those.
        library::part odd = make("Odd", "", "D_Dual_Series", "", "");
        const schematic::part kept = ui::assigned(placed("D", "diode", "1N4148"), odd, "Odd");
        CHECK(kept.symbol == "D" && kept.value == "1N4148" && kept.device == "diode" && kept.library_part == "Odd");
    }
} // namespace

int main()
{
    placeholders();
    generic();
    candidates();
    assigning();
    return volt::tests::result("app part match");
}
