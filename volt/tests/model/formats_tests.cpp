#include "model_test_support.hpp"
#include "step_shapes.hpp"

#include <model/io.hpp>
#include <model/obj.hpp>
#include <model/stl.hpp>
#include <model/step.hpp>

#include <filesystem>
#include <numbers>

using namespace volt;
using volt::tests::near;

namespace
{
    constexpr double pi = std::numbers::pi;

    model::model step_model(const std::string &text)
    {
        model::load_report report;
        auto result = model::step::read(text, {}, report);
        CHECK(result.has_value());
        return result ? std::move(*result) : model::model{};
    }

    bool near_color(const math::vec3f &a, const math::vec3f &b, double tolerance = 2e-3)
    {
        return near(static_cast<double>(a[0]), static_cast<double>(b[0]), tolerance) &&
               near(static_cast<double>(a[1]), static_cast<double>(b[1]), tolerance) &&
               near(static_cast<double>(a[2]), static_cast<double>(b[2]), tolerance);
    }

    void stl_round_trip()
    {
        const model::model cube = step_model(tests::read_asset("cube.step"));
        for (bool ascii : {false, true})
        {
            const std::string bytes = ascii ? model::stl::write_ascii(cube) : model::stl::write_binary(cube);
            CHECK(model::stl::is_binary(bytes) == !ascii);
            model::load_report report;
            const auto back = model::stl::read(bytes, report);
            CHECK(back.has_value());
            if (!back)
                continue;
            const auto c = tests::check_solid(*back);
            CHECK(c.measure.triangles == 12);
            CHECK(c.measure.closed());
            CHECK(near(c.measure.volume, 1.0, 1e-6));
            // Hard edges: every corner of the cube is three vertices.
            CHECK(back->meshes[0].vertices.size() == 24);
            CHECK(c.worst_normal < 1e-3);
        }

        // A curved part keeps its smooth normals through STL: welded, and
        // averaged across the shallow creases between triangles.
        const model::model cylinder = step_model(tests::shapes::cylinder());
        model::load_report report;
        const auto back = model::stl::read(model::stl::write_binary(cylinder), report);
        CHECK(back.has_value());
        if (back)
        {
            const auto c = tests::check_solid(*back);
            CHECK(c.measure.closed());
            CHECK(near(c.measure.volume, tests::check_solid(cylinder).measure.volume, 1e-5));
            CHECK(c.worst_normal < 30.0 * pi / 180.0);
        }

        // Not STL.
        model::load_report bad;
        CHECK(!model::stl::read("solid x\n  facet normal 0 0 1\n    outer loop\n      vertex 1 2\n", bad));
        CHECK(!model::stl::read(std::string(200, 'x'), bad));
    }

    void obj_round_trip()
    {
        const model::model assembly = step_model(tests::shapes::assembly());
        const model::obj::written written = model::obj::write(assembly, "assembly.mtl");
        model::load_report report;
        const auto back = model::obj::read(
            written.obj,
            [&](std::string_view name) -> std::optional<std::string> {
                if (name == "assembly.mtl")
                    return written.mtl;
                return std::nullopt;
            },
            report);
        CHECK(back.has_value());
        if (!back)
            return;

        // Flattened: three instances, three meshes.
        CHECK(back->meshes.size() == 3);
        const auto a = tests::check_solid(assembly);
        const auto b = tests::check_solid(*back);
        CHECK(b.measure.closed());
        CHECK(near(a.measure.volume, b.measure.volume, 1e-5));
        CHECK(a.measure.triangles == b.measure.triangles);
        if (back->meshes.size() == 3)
        {
            const model::model flat = model::flatten(assembly);
            for (std::size_t i = 0; i < 3; ++i)
                CHECK(near_color(back->meshes[i].material.color, flat.meshes[i].material.color));
        }
    }

    void obj_features()
    {
        // A square as a quad with negative indices, a triangle with vertex
        // colors and no normals, a group, a comment, a continued line, a
        // material from a library that is missing.
        const std::string text = "# test\n"
                                 "mtllib missing.mtl\n"
                                 "o first\n"
                                 "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                                 "vn 0 0 1\n"
                                 "f -4//1 -3//1 \\\n -2//1 -1//1\n"
                                 "g second\n"
                                 "v 0 0 1 1 0 0\nv 1 0 1 0 1 0\nv 0 1 1 0 0 1\n"
                                 "f 5 6 7\n";
        model::load_report report;
        const auto m = model::obj::read(text, {}, report);
        CHECK(m.has_value());
        if (!m)
            return;
        CHECK(m->meshes.size() == 2);
        CHECK(report.warnings.size() == 1); // the missing library
        if (m->meshes.size() == 2)
        {
            CHECK(m->meshes[0].name == "first");
            CHECK(m->meshes[0].triangle_count() == 2);
            CHECK(m->meshes[1].name == "second");
            CHECK(m->meshes[1].triangle_count() == 1);
            // Colors from the vertices, normals computed facing +z.
            CHECK(m->meshes[1].colors.size() == m->meshes[1].vertices.size());
            for (const model::vertex &v : m->meshes[1].vertices)
                CHECK(near(static_cast<double>(v.normal[2]), 1.0, 1e-6));
        }

        // Errors name the line.
        model::load_report bad;
        const auto missing = model::obj::read("v 0 0 0\nf 1 2 3\n", {}, bad);
        CHECK(!missing.has_value() && missing.error().starts_with("line 2"));
        const auto garbage = model::obj::read("v 0 zero 0\n", {}, bad);
        CHECK(!garbage.has_value() && garbage.error().starts_with("line 1"));
    }

    // A model written as STEP reads back as the same model: the same tree,
    // the same triangles, the same colors.
    void step_round_trip()
    {
        const model::model assembly = step_model(tests::shapes::assembly());
        const std::string text = model::step::write(assembly);
        CHECK(model::part21::parse(text).has_value());

        model::load_report report;
        const auto back = model::step::read(text, {}, report);
        CHECK(back.has_value());
        if (!back)
            return;
        tests::print("step export", tests::check_solid(*back), report);
        CHECK(report.clean());
        CHECK(back->name == assembly.name);
        CHECK(back->meshes.size() == assembly.meshes.size());
        CHECK(back->nodes[0].children.size() == assembly.nodes[0].children.size());
        if (back->nodes[0].children.size() == 3)
        {
            for (std::size_t c = 0; c < 3; ++c)
            {
                const model::node &x = assembly.nodes[assembly.nodes[0].children[c]];
                const model::node &y = back->nodes[back->nodes[0].children[c]];
                CHECK(x.name == y.name);
                for (std::size_t i = 0; i < 4; ++i)
                    for (std::size_t j = 0; j < 4; ++j)
                        CHECK(near(static_cast<double>(x.transform(i, j)), static_cast<double>(y.transform(i, j)), 1e-6));
                CHECK(y.meshes.size() == 1);
                if (y.meshes.size() == 1 && x.meshes.size() == 1)
                {
                    CHECK(back->meshes[y.meshes[0]].triangle_count() == assembly.meshes[x.meshes[0]].triangle_count());
                    CHECK(near_color(back->meshes[y.meshes[0]].material.color, assembly.meshes[x.meshes[0]].material.color));
                }
            }
            // Still one mesh placed twice.
            CHECK(back->nodes[back->nodes[0].children[0]].meshes == back->nodes[back->nodes[0].children[1]].meshes);
        }
        const auto a = tests::check_solid(assembly);
        const auto b = tests::check_solid(*back);
        CHECK(b.measure.closed());
        CHECK(near(a.measure.volume, b.measure.volume, 1e-5));

        // And a mesh that is not closed goes out as a shell model.
        model::model open = step_model(tests::read_asset("cube.step"));
        open.meshes[0].indices.resize(open.meshes[0].indices.size() - 3);
        model::load_report open_report;
        const auto open_back = model::step::read(model::step::write(open), {}, open_report);
        CHECK(open_back.has_value() && open_back->meshes.size() == 1 && open_back->meshes[0].triangle_count() == 11);
    }

    void front_door()
    {
        CHECK(model::detect("ISO-10303-21;\nHEADER;") == model::format::step);
        CHECK(model::detect("\xEF\xBB\xBF  ISO-10303-21;") == model::format::step);
        CHECK(model::detect("solid x\n facet normal 0 0 1\n") == model::format::stl);
        CHECK(model::detect("# comment\nv 1 2 3\n") == model::format::obj);
        CHECK(model::detect("whatever", ".STP") == model::format::step);
        CHECK(model::detect("whatever") == model::format::unknown);
        CHECK(model::format_of("part.Step") == model::format::step);
        CHECK(model::format_of("part.txt") == model::format::unknown);

        const std::filesystem::path directory = std::filesystem::temp_directory_path() / "volt_model_tests";
        std::filesystem::create_directories(directory);
        const model::model capsule = step_model(tests::shapes::capsule());
        const double volume = tests::check_solid(capsule).measure.volume;

        for (const char *name : {"capsule.step", "capsule.obj", "capsule.stl"})
        {
            const std::filesystem::path path = directory / name;
            const auto saved = model::save(path, capsule);
            CHECK(saved.has_value());
            const auto loaded = model::load(path);
            CHECK(loaded.has_value());
            if (!loaded)
            {
                std::fprintf(stderr, "  %s: %s\n", name, loaded.error().c_str());
                continue;
            }
            const auto c = tests::check_solid(loaded->model);
            CHECK(c.measure.closed());
            CHECK(near(c.measure.volume, volume, 1e-4));
        }
        CHECK(std::filesystem::exists(directory / "capsule.mtl"));
        CHECK(!model::save(directory / "capsule.xyz", capsule).has_value());
        CHECK(!model::load(directory / "does_not_exist.step").has_value());
        std::filesystem::remove_all(directory);
    }
} // namespace

int main()
{
    stl_round_trip();
    obj_round_trip();
    obj_features();
    step_round_trip();
    front_door();
    return volt::tests::result("model formats");
}
