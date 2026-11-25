#pragma once

#include <app/board/scene.hpp>
#include <gpu/color.hpp>
#include <math/transform.hpp>

#include <numbers>
#include <vector>

// Something for the board renderer to draw until there is a board document:
// a board with a few parts on it, some boxes and some STEP models
// (assets/models/parts). (The schematic's demo, a resistor ladder, is a
// real sheet now: schematic::demo_ladder(), <schematic/demo.hpp>.) Nothing
// else should come to depend on this file.

namespace volt::demo
{

    // The board, and the parts on it that have no model, as boxes.
    inline board::scene board()
    {
        const auto color = [](float r, float g, float b) { return gpu::srgb_to_linear(math::vec3f{r, g, b}); };

        board::scene scene;
        scene.view.eye = {55.0f, -95.0f, 80.0f};
        scene.view.target = {0.0f, 0.0f, 0.0f};
        scene.background = color(0.07f, 0.08f, 0.10f);

        const float thickness = 1.6f;
        const math::vec3f body = color(0.10f, 0.10f, 0.11f);
        const math::vec3f metal = color(0.75f, 0.76f, 0.78f);

        // The board, its top side at z = 0.
        scene.boxes.push_back({{0.0f, 0.0f, -thickness / 2.0f}, {80.0f, 50.0f, thickness}, 0.0f, color(0.05f, 0.32f, 0.18f),
                               0, "Board"});

        // Sitting on it: a part of this footprint and height, here, turned
        // so, under its reference and what it is (the objects list's names).
        const auto part = [&](const char *name, float x, float y, float width, float depth, float height,
                              float rotation, const math::vec3f &part_color) {
            scene.boxes.push_back({{x, y, height / 2.0f}, {width, depth, height}, rotation, part_color, 0, name});
        };

        part("U1 Processor", -8.0f, 2.0f, 14.0f, 14.0f, 1.4f, 0.0f, body);
        part("U2 Regulator", 16.0f, -10.0f, 5.0f, 4.0f, 1.2f, std::numbers::pi_v<float> / 4.0f, body);
        part("J1 Connector", -33.0f, 12.0f, 9.0f, 12.0f, 7.0f, 0.0f, metal);
        return scene;
    }

    // A part of the demo board that has a model: its file in assets/models,
    // and where each copy goes. A model has its origin at the footprint's
    // and z = 0 on the board's top, so a placement is a position on the
    // board and a turn about z.
    struct board_part
    {
        const char *file;
        std::vector<math::mat4f> placements;
    };

    inline std::vector<board_part> board_parts()
    {
        const auto at = [](float x, float y, float rotation = 0.0f) {
            return math::affine(math::rotation_z(rotation), math::vec3f{x, y, 0.0f});
        };
        const float quarter = std::numbers::pi_v<float> / 2.0f;

        std::vector<board_part> parts;
        parts.push_back({"parts/SOIC-8_3.9x4.9mm_P1.27mm.step", {at(18.0f, 8.0f, quarter)}}); // memory
        parts.push_back({"parts/CP_Radial_D6.3mm_P2.50mm.step", // bulk capacitor, its stripe to the camera
                         {at(31.0f, -15.0f, 0.8f * std::numbers::pi_v<float>)}});
        parts.push_back({"parts/LED_D5.0mm.step", {at(8.0f, 17.0f)}});                        // power LED

        // A row of resistors, turned to run across the row.
        board_part resistors{"parts/R_1206_3216Metric.step", {}};
        for (int i = 0; i < 6; ++i)
            resistors.placements.push_back(at(-14.0f + 4.0f * static_cast<float>(i), -14.0f, quarter));
        parts.push_back(std::move(resistors));
        return parts;
    }

} // namespace volt::demo
