#pragma once

#include <schematic/sheet.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// symbols
// -------------------------------------------------------------------------
//
// What a part looks like on the sheet, and where its pins are. A library
// part only names its symbol ("R", "Conn_01x04"); the geometry is here,
// built into Volt, until symbols become files of their own.
//
// A symbol is drawn in its own coordinates, in sheet units, around its
// origin: lines (open polylines; circles and arcs are polylines of short
// segments) and pins. A pin is the point where a wire attaches, plus the
// direction from that point into the body, for drawing its number and name.
// Every pin is on the grid (a multiple of units_per_grid in x and y), so a
// part placed on the grid has its pins on it in all eight orientations.
// Two-terminal parts are 200 units between pins, drawn the conventional
// way: horizontal at rotation 0 with pin 1 on the left (a resistor,
// capacitor, inductor, diode, LED); the sources (V_DC, I_DC) are 400 long
// and vertical, pin 1 ("+") at the top.
//
// find_symbol() never fails: a name it knows gives the built-in symbol;
// "Conn_01xNN" and "Conn_02xNN" give a connector of NN rows, generated; any
// other name gives a box with four pins on each side (numbered 1 to 8, as an
// eight-pin package is) and the name to be drawn inside it, so a part whose
// symbol is not known yet still places, connects and draws as something.
// Symbols are made once and kept for the life of the program: references
// stay valid, and asking again is a map lookup. Thread-safe.
//
// Placing: a part's symbol is mirrored in x when `mirrored` is set, then
// turned `rotation` quarter turns counter-clockwise, then moved to the
// part's position, all in exact integer arithmetic (transform()).

namespace volt::schematic
{

    struct pin
    {
        std::string name;   // "+", "OUT", "K", "Pin_3"; empty for a plain passive's
        std::string number; // "1", "2", ...: unique within the symbol
        point position{};   // where a wire attaches, in symbol coordinates
        // A unit step along an axis, from the pin's end into the body:
        // {1, 0} for a pin on the left side.
        point direction{};

        bool operator==(const pin &) const = default;
    };

    struct symbol
    {
        std::string name;
        // Open polylines, each two points or more, in symbol coordinates.
        std::vector<std::vector<point>> strokes;
        std::vector<pin> pins;
        // The bounds of the drawing (strokes and pins), for hit testing and
        // the view.
        point min{};
        point max{};
        // Where the reference ("R1") and the value ("10k") are drawn,
        // centred on these points, in symbol coordinates.
        point reference_anchor{};
        point value_anchor{};
        // The letters a new part's reference starts with: "R", "C", "U",
        // "J"; "#PWR" for a power glyph.
        std::string prefix;
        // The device a new part of this symbol is, for a simulation
        // ("resistor", "voltage_source"); empty for none.
        std::string device;
        // A box with the symbol's name to be drawn inside it, at the centre
        // of its bounds: the box made up for a name not known, and the
        // built-in ICs (Timer_555).
        bool name_inside = false;

        // The pin of that number, or nullptr.
        const pin *find_pin(std::string_view number) const noexcept;

        bool operator==(const symbol &) const = default;
    };

    // The symbol of that name, built in or generated (see the top of this
    // file). The reference stays valid until the program ends.
    const symbol &find_symbol(std::string_view name);

    // Whether the name is one of the built-in symbols (not generated).
    bool is_builtin_symbol(std::string_view name) noexcept;

    // The names of the built-in symbols, in a fixed order.
    std::span<const std::string_view> builtin_symbol_names() noexcept;

    // Whether a power port of that name is a ground ("GND", "AGND", "GNDA",
    // "DGND", "VSS", "0", ...), drawn as the ground glyph.
    bool is_ground_name(std::string_view text) noexcept;

    // The glyph of a power label: GND for a ground name, PWR otherwise.
    const symbol &power_symbol(std::string_view text);

    // ---- placing -------------------------------------------------------------

    // A point of the symbol, in symbol coordinates, mirrored and turned
    // (no move): what the orientation does to it.
    point orient(point local, std::uint8_t rotation, bool mirrored) noexcept;

    // A point of the part's symbol on the sheet.
    point transform(const part &p, point local) noexcept;

    // A direction of the symbol (a pin's) as it is on the sheet.
    point transform_direction(const part &p, point local) noexcept;

    // A pin of a placed part, on the sheet.
    struct placed_pin
    {
        const schematic::pin *pin = nullptr; // the symbol's
        point position{};
        point direction{};
    };

    // The part's pins on the sheet, in the symbol's order.
    std::vector<placed_pin> placed_pins(const part &p);

    // Just the positions.
    std::vector<point> pin_positions(const part &p);

    // The part's drawing bounds on the sheet: the symbol's, oriented and
    // moved.
    void part_bounds(const part &p, point &min, point &max);

} // namespace volt::schematic
