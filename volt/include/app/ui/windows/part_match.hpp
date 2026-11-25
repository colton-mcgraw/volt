#pragma once

#include <library/catalog.hpp>
#include <library/project.hpp>
#include <schematic/sheet.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// placeholders and the parts that stand in for them
// -------------------------------------------------------------------------
//
// A part placed from a built-in symbol alone (a "generic" resistor, 10k) is
// a placeholder: it has a symbol, a value and a device, and no library part,
// so nothing says what to buy. Assigning a library part to it later makes it
// a real part without placing it again. This is the headless half of that:
//
//   generic_entries()   what the parts panel's Generic scope lists: the
//                       placeholders it places (a resistor, a capacitor,
//                       ..., connectors) and the power ports
//   is_placeholder()    which placed parts are placeholders
//   match_parts()       the library parts that can stand in for one, ranked
//   assigned()          the placed part after a library part is assigned
//
// Interface thread or any other: nothing here keeps state.
//
// Pins. A library part's symbol may differ from the placed one: then it can
// stand in only when its pins are the same, numbered alike and in the same
// places, so that the wires on the sheet stay on them (C and C_Polarized; D
// and D_Schottky); any other symbol cannot.

namespace volt::ui
{

    // ---- the Generic scope ----------------------------------------------------

    struct generic_entry
    {
        enum class kind : std::uint8_t
        {
            part,  // a placeholder part: `symbol` placed with `value`
            power, // a power port: `name` is its net
        };

        kind what = kind::part;
        std::string_view name;     // "Resistor", "GND"
        std::string_view symbol;   // "R"; for a power port its glyph, "GND" or "PWR"
        std::string_view value;    // the value it starts with: "10k"; empty for none
        std::string_view keywords; // more words the search finds it by: "res"
    };

    // In the order the panel lists them: the placeholders (Resistor,
    // Capacitor, Polarized Capacitor, Inductor, Diode, Zener Diode,
    // Schottky Diode, LED, NPN, PNP, N-MOSFET, P-MOSFET, Op-amp, DC Voltage
    // Source, DC Current Source, connectors 1×2 to 1×8), then the power
    // ports (GND, +3V3, +5V, +12V, -12V, VCC).
    std::span<const generic_entry> generic_entries() noexcept;

    // Whether every word of `text` is in the entry's name, symbol, value or
    // keywords (ASCII case ignored): the Generic scope's search, "res",
    // "gnd", "conn 1x4", "power".
    bool matches(const generic_entry &entry, std::string_view text);

    // ---- placeholders -----------------------------------------------------------

    // A placed part with no library part, other than what never has one:
    // the simulation's sources (V_DC, I_DC: "voltage_source" and
    // "current_source", which nothing is bought for) and power glyphs.
    bool is_placeholder(const schematic::part &p);

    // What a placeholder is, for matching and for saying so: its device,
    // value and symbol.
    struct placeholder
    {
        std::string device; // "resistor"; empty for a part no simulation models
        std::string value;  // "10k"
        std::string symbol; // "R"

        bool operator==(const placeholder &) const = default;
    };

    placeholder placeholder_of(const schematic::part &p);

    // "1 kΩ resistor", "100nF capacitor", "LED", "Conn_01x04": a placeholder
    // in a few words, for titles.
    std::string describe(const placeholder &p);

    // Whether two symbols' pins are the same: the same numbers, each at the
    // same place (see the top of this file). True for one symbol.
    bool pins_compatible(std::string_view symbol_a, std::string_view symbol_b);

    // ---- candidates -------------------------------------------------------------------

    struct part_candidate
    {
        // The part: in the project library (project_id set), or in the
        // catalog (ref set). Valid while that library and catalog are.
        const library::part *part = nullptr;
        std::string project_id;
        std::optional<library::part_ref> ref;
        // Its symbol is not the placeholder's, though its pins are.
        bool other_symbol = false;
        // Its value is the placeholder's, as a number or as text; false
        // when the placeholder's value says nothing (empty, or a quantity
        // its device does not have).
        bool value_matched = false;

        bool in_project() const noexcept { return !project_id.empty(); }
    };

    // The library parts that can stand in for `p`, best first:
    //
    //   which    the same device (or, for a placeholder of no device, the
    //            same symbol); a principal value (library::principal_value)
    //            equal to the placeholder's value read as a quantity, when
    //            it reads as one in the device's unit, else (a part number
    //            typed as the value) the value's text, without case, when
    //            some candidate has it; with an empty or unreadable value,
    //            any of the device; and pins compatible (see the top)
    //   order    the project library's parts first (reuse what the design
    //            uses), then the catalog's, leaving out a catalog part the
    //            project has a copy of; within each, the placeholder's own
    //            symbol first, then a package the project's other parts
    //            use most, then a common chip size (0603, 0805, 0402,
    //            1206, in that order) before anything else, then a part
    //            with an MPN, then the order they were found in
    //
    // `catalog` or `project` may be null.
    std::vector<part_candidate> match_parts(const placeholder &p, const library::catalog *catalog,
                                            const library::project *project);

    // The candidates whose part matches `text` as the catalog's search
    // would (catalog::match; every one without a catalog), in their order.
    std::vector<part_candidate> filter_candidates(std::span<const part_candidate> candidates,
                                                  const library::catalog *catalog, std::string_view text);

    // ---- assigning ----------------------------------------------------------------------

    // `placed` with `part` assigned: library_part is `project_id`, the value
    // the part's value (kept when the part has none), the device the
    // part's (kept when it has none), the symbol the part's when the pins
    // are compatible (kept otherwise). Its reference, position, rotation,
    // mirroring and unknown fields are untouched.
    schematic::part assigned(const schematic::part &placed, const library::part &part, std::string_view project_id);

} // namespace volt::ui
