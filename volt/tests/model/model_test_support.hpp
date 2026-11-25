#pragma once

#include "../math/check.hpp"

#include <model/measure.hpp>
#include <model/model.hpp>
#include <model/options.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

// Shared by the model tests: reading the sample files, and checking that a
// mesh is a sound solid.

namespace volt::tests
{

    inline std::string read_asset(const std::string &name)
    {
        std::ifstream in(std::string(VOLT_MODEL_TEST_ASSETS) + "/" + name, std::ios::binary);
        if (!in)
        {
            std::fprintf(stderr, "cannot open test asset %s\n", name.c_str());
            return {};
        }
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    inline bool near(double a, double b, double tolerance)
    {
        return std::abs(a - b) <= tolerance;
    }

    // The flattened model's meshes measured together, as one solid would be.
    struct solid_check
    {
        model::mesh_measure measure;
        double worst_normal = 0.0;
        model::bounds bounds;
    };

    inline solid_check check_solid(const model::model &source)
    {
        const model::model flat = model::flatten(source);
        model::mesh all;
        for (const model::mesh &mesh : flat.meshes)
        {
            const auto base = static_cast<std::uint32_t>(all.vertices.size());
            all.vertices.insert(all.vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
            for (std::uint32_t index : mesh.indices)
                all.indices.push_back(base + index);
        }
        solid_check result;
        result.measure = model::measure(all);
        result.worst_normal = model::worst_normal_deviation(all);
        result.bounds = model::bounds_of(flat);
        return result;
    }

    inline void print(const char *name, const solid_check &c, const model::load_report &report)
    {
        std::printf("%-22s %6zu triangles, volume %.6f, area %.6f, boundary %zu, non-manifold %zu, "
                    "inconsistent %zu, worst normal %.1f deg, faces %zu (failed %zu)\n",
                    name, c.measure.triangles, c.measure.volume, c.measure.area, c.measure.boundary_edges,
                    c.measure.non_manifold_edges, c.measure.inconsistent_edges, c.worst_normal * 57.29578,
                    report.faces, report.faces_failed);
        for (const std::string &warning : report.warnings)
            std::printf("    %s\n", warning.c_str());
        for (const auto &[type, count] : report.unsupported)
            std::printf("    unsupported %s: %zu\n", type.c_str(), count);
    }

} // namespace volt::tests
