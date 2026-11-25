#pragma once

#include <math/vector.hpp>

#include <cstdint>
#include <vector>

// -------------------------------------------------------------------------
// schematic scene
// -------------------------------------------------------------------------
//
// What the schematic view draws, as plain data: a camera, the sheet's
// background and grid, and a list of round-ended line segments. Building a
// scene needs no GPU (<schematic/draw.hpp> turns a sheet into segments);
// drawing one is the renderer's (<app/schematic/renderer.hpp>), which is the
// only part that knows about Vulkan.
//
// World units are grid squares, y up: a sheet unit is 1 / units_per_grid of
// one (<schematic/sheet.hpp>).

namespace volt::schematic
{

    // Which part of the sheet the view shows. World units are whatever the
    // schematic is drawn in (grid squares, here); y points up.
    struct camera
    {
        // The world position in the middle of the view.
        math::vec2f center{};

        // Pixels per world unit.
        float zoom = 32.0f;
    };

    // The one shape a schematic is drawn with: a line with round ends. Wires
    // and the outlines of symbols are lines; a junction is a line whose two
    // ends are the same point, which makes it a dot.
    struct segment
    {
        math::vec2f a;
        math::vec2f b;

        // In world units, so lines get thicker as the view zooms in. Never
        // drawn thinner than a pixel.
        float width = 0.1f;

        // Working space (<gpu/color.hpp>), alpha not multiplied in.
        math::vec4f color{1.0f, 1.0f, 1.0f, 1.0f};

        // The simulation's node the line is part of, or -1 for none. While
        // there are values to show (see `values` in <app/schematic/
        // renderer.hpp>) such a line takes its color from its node's value
        // instead.
        std::int32_t node = -1;
    };

    // What to draw this frame. Whoever owns the schematic fills one; the
    // renderer only turns it into commands.
    struct scene
    {
        camera view;

        math::vec3f background{0.0f, 0.0f, 0.0f};

        // Spacing of the grid in world units, with every tenth line drawn
        // stronger. Zero or less for no grid.
        float grid = 1.0f;
        math::vec4f grid_color{1.0f, 1.0f, 1.0f, 0.1f};

        // Drawn in order, later ones on top.
        std::vector<segment> segments;
    };

} // namespace volt::schematic
