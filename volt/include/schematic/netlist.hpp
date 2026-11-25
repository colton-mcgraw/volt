#pragma once

#include <app/simulation/network.hpp>
#include <schematic/sheet.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// -------------------------------------------------------------------------
// netlist
// -------------------------------------------------------------------------
//
// What on a sheet is connected to what: its nets, and from them the network
// of conductances the simulation solves (<app/simulation/network.hpp>). No
// GPU, no widgets: the editor colours wires and lists problems from these
// records, and anything that exports a netlist builds on them.
//
// Connectivity is exact integer equality (<schematic/sheet.hpp>):
//
//   - Wire ends at the same point connect. A wire end strictly inside
//     another wire connects to it (a T). Wires that only cross, interior to
//     interior, do not. Collinear wires that overlap connect, since an end
//     of one is then on the other.
//   - A pin connects to a wire its point is on (an end or inside), and to
//     any pin at the same point. With the project's connectivity saying
//     otherwise (connectivity::pins_inside_wires false, <schematic/sheet.hpp>)
//     a pin connects to a wire only at one of its ends: a wire drawn past a
//     pin goes by it. Labels and wire ends inside wires connect either way.
//   - A net label names the net of whatever wire or pin is at its point.
//     A power port connects as a pin does and names its net too. Nets of
//     the same name are one net, whatever kind of label named them. A
//     label that touches no wire and no pin is a problem.
//   - A net any of whose names is a ground name (is_ground_name: "GND",
//     "AGND", "0", ...) is a ground net. Different ground names stay
//     different nets in the netlist, but are the one ground node of the
//     circuit.
//   - Notes mean nothing. Junction dots are not stored: they are drawn
//     where three or more things meet, from these rules.
//
// build_netlist() is deterministic: the same sheet gives the same nets, in
// the same order, with the same names, whatever order the tables hold their
// records in (only the ids matter). Nets are ordered by their smallest
// member: the smallest id among their wires, labels and parts, a part's
// pins in symbol order. Within a net, wires and labels are in id order,
// pins by part id and then symbol order.
//
// A net is named by its labels: a power port's text before a net label's,
// and the alphabetically first among several of one kind (several names on
// one net are a problem). An unnamed net is named after its first pin,
// "Net-(R1-2)" (the part's reference, or its symbol and "?" when it has
// none), or after its smallest member id when it has no pins, "N17". A
// generated name that is taken already gets "_2", "_3", ... appended.
//
// Cost: every wire end, pin and label point goes into a hash of points
// (constant time each). Wires are grouped by the line they lie on
// (direction reduced by the gcd, and offset), each line's wires sorted
// along it and merged into runs of overlapping wires, so whether a point is
// inside some wire is a binary search per wire direction present on the
// sheet. Joining is a union-find. In all O(n log n + q * d log n) for n
// items, q points and d distinct wire directions (two for a sheet of
// horizontal and vertical wires).
//
// The circuit (build_circuit) is the DC operating point's network:
//
//   resistor        a branch of 1/R siemens, R from the part's value in
//                   engineering notation ("4.7k", "4k7", "100R", "1M Ohm").
//                   A missing, unreadable or non-positive value is an
//                   error, and the resistor is left out.
//   voltage_source  V_DC: pin 1 "+", pin 2 "-". The solver holds nodes at
//                   potentials, so a source holds one terminal's node at the
//                   other's potential plus or minus its value, once the
//                   other is held: from ground outwards, through any chain
//                   of sources. A source neither of whose terminals gets
//                   there is not supported (an error) - unless the sheet has
//                   no ground at all, when the "-" net of the first such
//                   source in each separate circuit is taken as 0 V (a
//                   warning). A source that would hold a node at another
//                   potential than it has is in conflict (an error), and
//                   left out.
//   current_source  I_DC: the current flows through the source from pin 2
//                   to pin 1 (the arrow points to pin 1), so +I is fed into
//                   pin 1's node and -I into pin 2's.
//   inductor        a short at DC: its two nets are made one node (rather
//                   than a large conductance, which the relaxation would
//                   take hundreds of thousands of sweeps to settle).
//   capacitor       open at DC: left out, silently.
//   anything else   left out, listed once per device kind as not simulated
//                   yet (diode, led, zener, bjt, mosfet, ...), or as having
//                   no model (no device at all: connectors, op amps).
//
// A part's device is its own `device`, or its symbol's when that is empty.
// Values may carry their unit: "5V", "2mA", "10kOhm".
//
// Nodes are numbered from 0: the ground node first when the sheet has a
// ground net with a wire or pin, then one node per net (or per group of
// nets shorted by inductors) in net order. Nets with no pin and no wire
// (labels alone) get no node. Every ground net is held at 0 V.

namespace volt::schematic
{

    // ---- problems --------------------------------------------------------------

    enum class severity : std::uint8_t
    {
        info,    // worth knowing; nothing is wrong
        warning, // probably a mistake; the result stands
        error,   // something was left out of the result
    };

    enum class problem_kind : std::uint8_t
    {
        // The netlist's.
        unconnected_pin,   // warning: a pin touching nothing
        single_pin_net,    // warning: a net with wires or labels, but one pin
        label_unconnected, // warning: a net label or power port touching no wire or pin
        conflicting_names, // warning: one net, several names

        // The circuit's.
        bad_value,           // error: a resistor's or source's value is missing or unreadable
        missing_pin,         // error: a device's symbol lacks the pin it needs
        shorted_source,      // error: a voltage source with both terminals on one node
        conflicting_sources, // error: a source would hold a node at a second potential
        floating_source,     // error: a voltage source with no path of sources to ground
        reference_chosen,    // warning: no ground, so a source's "-" was taken as 0 V
        floating_net,        // warning: a net with no path to a held node
        nothing_held,        // warning: devices, but no ground and no voltage source
        inductor_short,      // info: inductors are shorts at DC
        not_simulated,       // info: parts of a device kind the simulation has no model of
    };

    struct problem
    {
        problem_kind kind = problem_kind::unconnected_pin;
        severity level = severity::warning;
        // The items concerned, for selecting them: parts, wires, labels.
        std::vector<item_ref> items;
        // One sentence for a status bar or a list.
        std::string message;
    };

    std::string_view to_string(severity level) noexcept;

    // ---- nets --------------------------------------------------------------------

    // A pin of a placed part: the part's id and the pin's number.
    struct pin_ref
    {
        item_id part = 0;
        std::string number;

        bool operator==(const pin_ref &) const = default;
        auto operator<=>(const pin_ref &) const = default;
    };

    struct net
    {
        // From a label or power port when `named`; generated otherwise
        // (see the top of this file). Unique within the netlist.
        std::string name;
        bool named = false;
        // One of its names is a ground name.
        bool ground = false;

        std::vector<item_id> wires; // in id order
        std::vector<pin_ref> pins;  // by part id, then symbol order
        std::vector<item_id> labels; // net labels and power ports, in id order
    };

    class netlist
    {
    public:
        std::vector<net> nets;
        // Unconnected pins, single-pin nets, labels touching nothing, nets
        // of several names: in net order, then label order.
        std::vector<problem> problems;

        // The index of the net of a wire, of a part's pin (by number), of
        // a net label or power port; nullopt for an item the sheet built
        // from did not have.
        std::optional<std::size_t> net_of_wire(item_id wire) const;
        std::optional<std::size_t> net_of_pin(item_id part, std::string_view number) const;
        std::optional<std::size_t> net_of_pin(const pin_ref &pin) const { return net_of_pin(pin.part, pin.number); }
        std::optional<std::size_t> net_of_label(item_id label) const;

        // The net at a point of the sheet: of a wire end, pin or label
        // there, or of a wire the point is inside; nullopt for none.
        std::optional<std::size_t> net_at(point p) const;

        // The net of that name; nullopt for none.
        std::optional<std::size_t> find(std::string_view name) const;

    private:
        friend netlist build_netlist(const sheet &s, const connectivity &rules);

        // Wires on one line, merged where they overlap: a stretch along the
        // line every point of which is on one of them.
        struct run
        {
            std::int64_t from = 0; // along the line (dot product with its direction)
            std::int64_t to = 0;
            std::uint32_t net = 0;
        };

        struct direction
        {
            std::int64_t dx = 0; // reduced by their gcd; dx > 0, or dx == 0 and dy > 0
            std::int64_t dy = 0;
            // By the line's offset (cross product of the direction and a
            // point on it): its runs, in order along it.
            std::unordered_map<std::int64_t, std::vector<run>> lines;
        };

        // The `net` of every run the point is on, one per direction at
        // most, appended to `out`.
        void runs_at(point p, std::vector<std::uint32_t> &out) const;

        std::unordered_map<item_id, std::uint32_t> m_wire_net;
        std::unordered_map<item_id, std::uint32_t> m_label_net;
        std::unordered_map<item_id, std::vector<std::pair<std::string, std::uint32_t>>> m_pin_net; // per part
        std::unordered_map<std::uint64_t, std::uint32_t> m_point_net; // wire ends, pins, labels
        std::vector<direction> m_directions;
        std::unordered_map<std::string, std::uint32_t> m_name_net;
    };

    // The nets of the sheet under the rules, and what is wrong with them.
    netlist build_netlist(const sheet &s, const connectivity &rules = {});

    // ---- the circuit ---------------------------------------------------------------

    struct circuit
    {
        simulation::network network;
        // Per net of the netlist: its node, or -1 for none.
        std::vector<std::int32_t> node_of_net;
        // Every wire's node, for colouring wires by potential.
        std::unordered_map<item_id, std::int32_t> wire_nodes;
        // The range of the held potentials, for the colour ramp; both 0
        // when nothing is held.
        float low = 0.0f;
        float high = 0.0f;
        // Values, sources and nets the network could not take as drawn,
        // and what it leaves out: in part order, then net order.
        std::vector<problem> problems;
        // For the status bar: "6 nodes, 8 resistors, 1 source".
        std::string summary;
    };

    // The DC network of the sheet, whose nets are `nets` (build_netlist of
    // the same sheet).
    circuit build_circuit(const sheet &s, const netlist &nets);

} // namespace volt::schematic
