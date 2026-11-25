#include <schematic/demo.hpp>

#include <string>

namespace volt::schematic
{

    sheet demo_ladder()
    {
        constexpr int stages = 4;
        constexpr int pitch = 4 * units_per_grid;
        constexpr int top = 2 * units_per_grid;
        constexpr int bottom = -2 * units_per_grid;
        constexpr int left = -pitch * stages / 2;

        sheet s;
        item_id next = 1;

        // The source, its + pin on the top rail and its - pin on the
        // ground rail.
        s.parts.insert(next++, part{"V_DC", {left, 0}, 0, false, "V1", "5", "voltage_source", {}, {}});

        // Each stage: a resistor along the top (pins 100 in from either
        // node), and one turned upright from the node down to ground (pin 2
        // at the top).
        int reference = 1;
        for (int stage = 0; stage < stages; ++stage)
        {
            const int x0 = left + pitch * stage;
            const int x1 = x0 + pitch;
            s.parts.insert(next++, part{"R", {x0 + pitch / 2, top}, 0, false, "R" + std::to_string(reference++), "100",
                                        "resistor", {}, {}});
            s.parts.insert(next++, part{"R", {x1, 0}, 1, false, "R" + std::to_string(reference++), "220", "resistor",
                                        {}, {}});
        }

        const auto wire_from = [&](point a, point b) { s.wires.insert(next++, wire{a, b, {}}); };
        for (int stage = 0; stage < stages; ++stage)
        {
            const int x0 = left + pitch * stage;
            const int x1 = x0 + pitch;
            wire_from({x0, top}, {x0 + 100, top});         // node to the series resistor
            wire_from({x0 + 300, top}, {x1, top});         // the series resistor to the next node
            wire_from({x1, top}, {x1, 100});               // the node down to the shunt
            wire_from({x1, -100}, {x1, bottom});           // the shunt down to the ground rail
            wire_from({x0, bottom}, {x1, bottom});         // the ground rail, node to node
        }

        s.labels.insert(next++, label{label_kind::power, {0, bottom}, 0, "GND", {}});
        return s;
    }

} // namespace volt::schematic
