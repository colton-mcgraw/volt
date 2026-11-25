#pragma once

#include <schematic/sheet.hpp>

// -------------------------------------------------------------------------
// demo sheet
// -------------------------------------------------------------------------
//
// A sheet to show when there is nothing else to show (no project open): a
// resistor ladder. A 5 V source (V1) on the left, between a top rail at
// y = +2 grid squares and a ground rail at y = -2; four 100 Ω resistors in
// series along the top, and a 220 Ω resistor down to the ground rail after
// each; a GND power port on the ground rail. The stages are four grid
// squares apart, centred on the origin, everything on the grid, and every
// wire ends exactly on a pin or another wire's end, so the sheet is
// connected as drawn. The application opens it as its scratch sheet
// (<app/ui/schematic_document.hpp>).
//
// Ids run from 1 in table order: the parts (V1, then R1 to R8: each
// stage's series resistor, then its shunt), then the wires, then the label.

namespace volt::schematic
{

    sheet demo_ladder();

} // namespace volt::schematic
