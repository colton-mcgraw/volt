// A tool, not a test: loads a model (a file, or one of the test shapes by
// name), prints what the loader made of it and renders it to PNG images,
// to be looked at.
//
//   volt_model_view <file | shape name | all> <output directory> [--wire] [--quality q]

#include "model_test_support.hpp"
#include "render.hpp"
#include "step_shapes.hpp"

#include <model/step.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>

using namespace volt;

namespace
{
    const std::map<std::string, std::function<std::string()>> &shapes()
    {
        static const std::map<std::string, std::function<std::string()>> table = {
            {"cube", [] { return tests::read_asset("cube.step"); }},
            {"cylinder", [] { return tests::shapes::cylinder(); }},
            {"plate", [] { return tests::shapes::plate_with_hole(); }},
            {"frustum", [] { return tests::shapes::cone_frustum(); }},
            {"cone", [] { return tests::shapes::cone_point(); }},
            {"sphere", [] { return tests::shapes::sphere(); }},
            {"capsule", [] { return tests::shapes::capsule(); }},
            {"torus", [] { return tests::shapes::torus(); }},
            {"revolved", [] { return tests::shapes::revolved_frustum(); }},
            {"extruded", [] { return tests::shapes::extruded_ellipse(); }},
            {"bspline", [] { return tests::shapes::bspline_block(); }},
            {"rational", [] { return tests::shapes::rational_cylinder(); }},
            {"assembly", [] { return tests::shapes::assembly(); }},
            {"tessellated", [] { return tests::shapes::tessellated_cube(); }},
            {"faceted", [] { return tests::shapes::faceted_box(); }},
        };
        return table;
    }

    void report_worst_normal(const model::model &m)
    {
        const model::model flat = model::flatten(m);
        double worst = 0.0;
        std::string where;
        for (const model::mesh &mesh : flat.meshes)
        {
            for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
            {
                const model::vertex &a = mesh.vertices[mesh.indices[t]];
                const model::vertex &b = mesh.vertices[mesh.indices[t + 1]];
                const model::vertex &c = mesh.vertices[mesh.indices[t + 2]];
                const math::vec3d n = math::cross((b.position - a.position).cast<double>(),
                                                  (c.position - a.position).cast<double>());
                const double length = math::magnitude(n);
                if (length < 1e-12)
                    continue;
                for (const model::vertex *v : {&a, &b, &c})
                {
                    const double angle =
                        std::acos(std::clamp(math::dot(n / length, v->normal.cast<double>()), -1.0, 1.0));
                    if (angle > worst)
                    {
                        worst = angle;
                        where = std::format("triangle {} area {:.3g}: {} {} {} normal {} at {}", t / 3, length / 2,
                                            a.position, b.position, c.position, v->normal, v->position);
                    }
                }
            }
        }
        std::printf("  worst normal %.1f deg: %s\n", worst * 57.29578, where.c_str());
    }

    int view_one(const std::string &name, const std::string &text, const std::filesystem::path &out, bool wire,
                 double quality)
    {
        model::load_options options;
        options.tessellation = model::tessellation_options::with_quality(quality);
        model::load_report report;
        const auto start = std::chrono::steady_clock::now();
        auto loaded = model::step::read(text, options, report);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (!loaded)
        {
            std::printf("%s: %s\n%s\n", name.c_str(), loaded.error().c_str(), report.summary().c_str());
            return 1;
        }
        const tests::solid_check c = tests::check_solid(*loaded);
        tests::print(name.c_str(), c, report);
        std::printf("  %.3f s; bounds %s .. %s\n", seconds, std::format("{}", c.bounds.min).c_str(),
                    std::format("{}", c.bounds.max).c_str());
        report_worst_normal(*loaded);

        tests::image img;
        tests::view v;
        v.wireframe = wire;
        tests::render(*loaded, img, 640, 480, v);
        const std::filesystem::path above = out / (name + ".png");
        tests::write_png(above.string(), img);
        v.direction = {0.8, 1.0, -0.7};
        tests::render(*loaded, img, 640, 480, v);
        const std::filesystem::path below = out / (name + "_below.png");
        tests::write_png(below.string(), img);
        std::printf("  wrote %s and %s\n", above.string().c_str(), below.string().c_str());
        return 0;
    }
} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: volt_model_view <file | shape | all> <output directory> [--wire] [--quality q]\n");
        return 2;
    }
    bool wire = false;
    double quality = 1.0;
    for (int i = 3; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--wire") == 0)
            wire = true;
        else if (std::strcmp(argv[i], "--quality") == 0 && i + 1 < argc)
            quality = std::atof(argv[++i]);
    }
    const std::filesystem::path out = argv[2];
    std::filesystem::create_directories(out);

    const std::string what = argv[1];
    if (what == "all")
    {
        int failures = 0;
        for (const auto &[name, make] : shapes())
            failures += view_one(name, make(), out, wire, quality);
        return failures;
    }
    if (const auto it = shapes().find(what); it != shapes().end())
        return view_one(what, it->second(), out, wire, quality);

    std::ifstream in(what, std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "cannot open %s\n", what.c_str());
        return 1;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return view_one(std::filesystem::path(what).stem().string(), text, out, wire, quality);
}
