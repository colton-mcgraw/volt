// Timings of the model library: Part 21 parsing and writing of a large
// file, and the whole STEP read (product structure and tessellation) of
// whatever files are named on the command line.
//
//   volt_model_bench [file.step ...]

#include "step_shapes.hpp"

#include <model/io.hpp>
#include <model/part21.hpp>
#include <model/step.hpp>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>

using namespace volt;

namespace
{
    double seconds_since(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

    // A wavy sheet of n x n quads, two triangles each, as one mesh: written
    // as STEP it is a faceted B-rep of millions of entities.
    model::model sheet(int n)
    {
        model::model m;
        model::mesh mesh;
        mesh.name = "sheet";
        for (int j = 0; j <= n; ++j)
        {
            for (int i = 0; i <= n; ++i)
            {
                const float x = static_cast<float>(i) * 0.1f;
                const float y = static_cast<float>(j) * 0.1f;
                const float z = 0.5f * std::sin(x) * std::cos(y);
                mesh.vertices.push_back({{x, y, z}, {0.0f, 0.0f, 1.0f}});
            }
        }
        const auto at = [&](int i, int j) { return static_cast<std::uint32_t>(j * (n + 1) + i); };
        for (int j = 0; j < n; ++j)
        {
            for (int i = 0; i < n; ++i)
            {
                mesh.indices.insert(mesh.indices.end(), {at(i, j), at(i + 1, j), at(i + 1, j + 1)});
                mesh.indices.insert(mesh.indices.end(), {at(i, j), at(i + 1, j + 1), at(i, j + 1)});
            }
        }
        m.meshes.push_back(std::move(mesh));
        m.nodes.push_back({});
        m.nodes[0].meshes.push_back(0);
        m.name = "sheet";
        return m;
    }
} // namespace

int main(int argc, char **argv)
{
    // ---- Part 21 throughput ----------------------------------------------
    {
        const model::model big = sheet(400);
        auto start = std::chrono::steady_clock::now();
        const std::string text = model::step::write(big);
        const double write_seconds = seconds_since(start);

        start = std::chrono::steady_clock::now();
        const auto file = model::part21::parse(text);
        const double parse_seconds = seconds_since(start);
        if (!file)
        {
            std::printf("parse failed: %s\n", file.error().c_str());
            return 1;
        }
        start = std::chrono::steady_clock::now();
        const std::string again = model::part21::write(*file);
        const double rewrite_seconds = seconds_since(start);

        start = std::chrono::steady_clock::now();
        model::load_report report;
        const auto read = model::step::read(*file, {}, report);
        const double read_seconds = seconds_since(start);

        const double megabytes = static_cast<double>(text.size()) / (1024.0 * 1024.0);
        std::printf("faceted sheet: %.1f MB, %zu instances, %zu triangles\n", megabytes, file->size(),
                    big.meshes[0].triangle_count());
        std::printf("  model -> STEP text      %7.3f s\n", write_seconds);
        std::printf("  Part 21 parse           %7.3f s  (%.0f MB/s, %.2f M instances/s)\n", parse_seconds,
                    megabytes / parse_seconds, static_cast<double>(file->size()) / parse_seconds / 1e6);
        std::printf("  Part 21 write           %7.3f s  (%.0f MB/s)\n", rewrite_seconds,
                    static_cast<double>(again.size()) / (1024.0 * 1024.0) / rewrite_seconds);
        std::printf("  in memory               %7.1f MB  (%.1f x the text)\n",
                    static_cast<double>(file->memory_used()) / (1024.0 * 1024.0),
                    static_cast<double>(file->memory_used()) / static_cast<double>(text.size()));
        std::printf("  STEP read (to meshes)   %7.3f s  %s\n", read_seconds, read ? "" : read.error().c_str());
    }

    // ---- tessellation of the test shapes -----------------------------------
    {
        const std::pair<const char *, std::string> shapes[] = {
            {"cylinder", tests::shapes::cylinder()},       {"plate", tests::shapes::plate_with_hole()},
            {"sphere", tests::shapes::sphere()},           {"torus", tests::shapes::torus()},
            {"bspline", tests::shapes::bspline_block()},   {"rational", tests::shapes::rational_cylinder()},
        };
        for (const auto &[name, text] : shapes)
        {
            const auto start = std::chrono::steady_clock::now();
            std::size_t triangles = 0;
            constexpr int rounds = 20;
            for (int r = 0; r < rounds; ++r)
            {
                model::load_report report;
                const auto m = model::step::read(text, {}, report);
                triangles = m ? m->triangle_count() : 0;
            }
            std::printf("%-10s %6zu triangles  %8.3f ms per read\n", name, triangles, 1000.0 * seconds_since(start) / rounds);
        }
    }

    // ---- real files ----------------------------------------------------------
    for (int i = 1; i < argc; ++i)
    {
        const auto start = std::chrono::steady_clock::now();
        const auto loaded = model::load(argv[i]);
        const double total = seconds_since(start);
        if (!loaded)
        {
            std::printf("%s: %s\n", argv[i], loaded.error().c_str());
            continue;
        }
        // Entity fidelity: parse, write, parse again; the second text must
        // be the first, instance for instance.
        std::ifstream in(argv[i], std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto first = model::part21::parse(text);
        bool stable = false;
        if (first)
        {
            const std::string written = model::part21::write(*first);
            const auto second = model::part21::parse(written);
            stable = second && second->size() == first->size() && model::part21::write(*second) == written;
        }
        std::printf("%-40s %6zu entities %6zu triangles  parse %5.1f ms  build %6.1f ms  total %6.1f ms  failed faces %zu  "
                    "round trip %s\n",
                    loaded->model.name.c_str(), loaded->report.entities, loaded->model.triangle_count(),
                    1000.0 * loaded->report.parse_seconds, 1000.0 * loaded->report.build_seconds, 1000.0 * total,
                    loaded->report.faces_failed, stable ? "identical" : "DIFFERS");
    }
    return 0;
}
