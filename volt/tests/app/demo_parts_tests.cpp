#include "demo_parts.hpp"

#include "math/check.hpp"

#include <model/io.hpp>
#include <model/measure.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

// The demo board's part models (assets/models/parts): the committed files
// are what demo_parts.hpp makes, and each loads clean, every solid closed
// and facing out, at the size the part is.
//
//   volt_app_demo_parts_tests                  checks the files
//   volt_app_demo_parts_tests --write <dir>    writes them into <dir>

#ifndef VOLT_MODEL_ASSETS
#error "VOLT_MODEL_ASSETS must point at the repository's assets/models directory"
#endif

using namespace volt;
namespace parts = volt::tests::demo_parts;

namespace
{
    // Line ends as a checkout may have made them.
    std::string normalized(std::string text)
    {
        std::erase(text, '\r');
        return text;
    }

    std::string read(const std::filesystem::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    struct expected_part
    {
        const char *file;
        math::vec3f min;
        math::vec3f max;
        std::size_t colours; // distinct colours among its parts
    };

    // Footprint origin at the centre, z = 0 on the board.
    constexpr expected_part expected[] = {
        {"SOIC-8_3.9x4.9mm_P1.27mm.step", {-2.45f, -3.00f, 0.00f}, {2.45f, 3.00f, 1.61f}, 3},
        {"CP_Radial_D6.3mm_P2.50mm.step", {-3.15f, -3.15f, -3.50f}, {3.15f, 3.15f, 7.76f}, 5},
        {"LED_D5.0mm.step", {-2.90f, -2.90f, -3.50f}, {2.90f, 2.90f, 8.60f}, 3},
        {"R_1206_3216Metric.step", {-1.60f, -0.80f, 0.00f}, {1.60f, 0.80f, 0.58f}, 3},
    };

    void check_file(const expected_part &part, const std::string &made)
    {
        const std::filesystem::path path = std::filesystem::path(VOLT_MODEL_ASSETS) / "parts" / part.file;
        const std::string committed = read(path);
        CHECK(!committed.empty());
        CHECK(normalized(committed) == normalized(made));

        const auto loaded = model::load(path);
        CHECK(loaded.has_value());
        if (!loaded)
        {
            std::fprintf(stderr, "%s\n", loaded.error().c_str());
            return;
        }
        const model::load_report &report = loaded->report;
        if (!report.clean())
            std::fprintf(stderr, "%s: %s\n", part.file, report.summary().c_str());
        CHECK(report.clean());
        CHECK(report.faces_failed == 0);

        // Every solid closed, its triangles facing out.
        const model::model flat = model::flatten(loaded->model);
        for (const model::mesh &mesh : flat.meshes)
        {
            const model::mesh_measure m = model::measure(mesh);
            if (!m.closed() || m.volume <= 0.0)
                std::fprintf(stderr, "%s, %s: %zu triangles, boundary %zu, inconsistent %zu, volume %.4f\n", part.file,
                             mesh.name.c_str(), m.triangles, m.boundary_edges, m.inconsistent_edges, m.volume);
            CHECK(m.closed());
            CHECK(m.volume > 0.0);
        }

        const model::bounds bounds = model::bounds_of(loaded->model);
        for (std::size_t i = 0; i < 3; ++i)
        {
            CHECK(std::abs(bounds.min[i] - part.min[i]) < 0.02f);
            CHECK(std::abs(bounds.max[i] - part.max[i]) < 0.02f);
        }

        // The colours, a sector of a solid's included.
        std::vector<math::vec3f> colours;
        for (const model::mesh &mesh : loaded->model.meshes)
        {
            const auto note = [&](const math::vec3f &c) {
                if (std::ranges::find(colours, c) == colours.end())
                    colours.push_back(c);
            };
            if (mesh.colors.empty())
                note(mesh.material.color);
            for (const math::vec3f &c : mesh.colors)
                note(c);
        }
        if (colours.size() != part.colours)
            std::fprintf(stderr, "%s: %zu colours\n", part.file, colours.size());
        CHECK(colours.size() == part.colours);
    }
} // namespace

int main(int argc, char **argv)
{
    if (argc == 3 && std::string_view(argv[1]) == "--write")
    {
        for (const parts::part_file &part : parts::parts)
        {
            const std::filesystem::path path = std::filesystem::path(argv[2]) / part.file;
            std::ofstream out(path, std::ios::binary);
            out << part.make();
            std::printf("wrote %s\n", path.string().c_str());
        }
        return 0;
    }

    static_assert(std::size(parts::parts) == std::size(expected));
    for (std::size_t i = 0; i < std::size(expected); ++i)
    {
        CHECK(std::string_view(parts::parts[i].file) == expected[i].file);
        check_file(expected[i], parts::parts[i].make());
    }
    return volt::tests::result("demo parts");
}
