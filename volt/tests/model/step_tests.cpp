#include "model_test_support.hpp"
#include "step_shapes.hpp"

#include <model/step.hpp>

#include <chrono>
#include <numbers>

using namespace volt;
using volt::tests::near;

namespace
{
    constexpr double pi = std::numbers::pi;

    double as_double(float v)
    {
        return static_cast<double>(v);
    }

    std::optional<model::model> load(const std::string &text, model::load_report &report,
                                     const model::load_options &options = {})
    {
        auto result = model::step::read(text, options, report);
        CHECK(result.has_value());
        if (!result)
        {
            std::fprintf(stderr, "  load failed: %s\n", result.error().c_str());
            return std::nullopt;
        }
        return std::move(*result);
    }

    struct expected_solid
    {
        double volume = 0.0;
        double area = 0.0;
        math::vec3d min{};
        math::vec3d max{};
    };

    // Loads a single solid and holds it to its analytic volume, area and
    // bounds. A tessellation lies inside a convex surface, so its volume
    // falls short by at most about area times the chord tolerance.
    tests::solid_check solid_case(const char *name, const std::string &text, const expected_solid &expected,
                                  double quality = 1.0)
    {
        model::load_options options;
        options.tessellation = model::tessellation_options::with_quality(quality);
        model::load_report report;
        const auto model = load(text, report, options);
        if (!model)
            return {};
        const tests::solid_check c = tests::check_solid(*model);
        tests::print(name, c, report);

        const double chord = options.tessellation.chord_tolerance;
        const double volume_tolerance = expected.area * chord * 1.5 + 1e-9;
        CHECK(c.measure.closed());
        CHECK(report.faces_failed == 0);
        CHECK(report.unsupported.empty());
        CHECK(c.measure.volume > 0.0);
        CHECK(near(c.measure.volume, expected.volume, volume_tolerance));
        CHECK(near(c.measure.area, expected.area, expected.area * 0.03));
        CHECK(c.worst_normal < 45.0 * pi / 180.0);
        for (std::size_t i = 0; i < 3; ++i)
        {
            CHECK(near(as_double(c.bounds.min[i]), expected.min[i], 2.0 * chord + 1e-5));
            CHECK(near(as_double(c.bounds.max[i]), expected.max[i], 2.0 * chord + 1e-5));
        }
        if (!near(c.measure.volume, expected.volume, volume_tolerance))
            std::fprintf(stderr, "  %s: volume %.6f, expected %.6f +- %.6f\n", name, c.measure.volume, expected.volume,
                         volume_tolerance);
        return c;
    }

    void cube()
    {
        model::load_report report;
        const auto model = load(tests::read_asset("cube.step"), report);
        if (!model)
            return;
        const auto c = tests::check_solid(*model);
        tests::print("cube", c, report);

        CHECK(model->name == "Cube");
        CHECK(model->meshes.size() == 1);
        CHECK(c.measure.triangles == 12);
        CHECK(c.measure.closed());
        CHECK(near(c.measure.volume, 1.0, 1e-9));
        CHECK(near(c.measure.area, 6.0, 1e-9));
        CHECK(c.worst_normal < 1e-3);
        CHECK(near(as_double(c.bounds.min[0]), 0.0, 1e-6) && near(as_double(c.bounds.max[2]), 1.0, 1e-6));
        CHECK(report.clean());

        // The color, sRGB 0.8 0.2 0.2, in linear light.
        const math::vec3f color = model->meshes[0].material.color;
        CHECK(near(as_double(color[0]), 0.6038273, 1e-5) && near(as_double(color[1]), 0.0331048, 1e-5) &&
              near(as_double(color[2]), 0.0331048, 1e-5));
        CHECK(model->meshes[0].colors.empty());
    }

    void cube_in_inches()
    {
        std::string text = tests::read_asset("cube.step");
        const std::string_view millimeters = "#201 = ( LENGTH_UNIT() NAMED_UNIT(*) SI_UNIT(.MILLI.,.METRE.) );";
        const std::string inches = "#201 = ( CONVERSION_BASED_UNIT('INCH',#209) LENGTH_UNIT() NAMED_UNIT(#210) );\n"
                                   "#209 = LENGTH_MEASURE_WITH_UNIT(LENGTH_MEASURE(25.4),#211);\n"
                                   "#210 = DIMENSIONAL_EXPONENTS(1.,0.,0.,0.,0.,0.,0.);\n"
                                   "#211 = ( LENGTH_UNIT() NAMED_UNIT(*) SI_UNIT(.MILLI.,.METRE.) );";
        const std::size_t at = text.find(millimeters);
        CHECK(at != std::string::npos);
        if (at == std::string::npos)
            return;
        text.replace(at, millimeters.size(), inches);

        model::load_report report;
        const auto model = load(text, report);
        if (!model)
            return;
        const auto c = tests::check_solid(*model);
        tests::print("cube (inches)", c, report);
        CHECK(c.measure.closed());
        CHECK(near(c.measure.volume, 25.4 * 25.4 * 25.4, 1e-2));
        CHECK(near(as_double(c.bounds.max[0]), 25.4, 1e-4));
    }

    void curved_solids()
    {
        solid_case("cylinder", tests::shapes::cylinder(1.0, 2.0),
                   {pi * 2.0, 2.0 * pi + 4.0 * pi, {-1, -1, 0}, {1, 1, 2}});

        const double plate_volume = 10.0 * 8.0 * 2.0 - pi * 1.5 * 1.5 * 2.0;
        const double plate_area = 2.0 * (10.0 * 8.0 - pi * 1.5 * 1.5) + 2.0 * (10.0 + 8.0) * 2.0 + 2.0 * pi * 1.5 * 2.0;
        solid_case("plate with hole", tests::shapes::plate_with_hole(), {plate_volume, plate_area, {0, 0, 0}, {10, 8, 2}});

        const double slant = std::sqrt(1.0 + 4.0);
        solid_case("cone frustum", tests::shapes::cone_frustum(),
                   {pi * 2.0 / 3.0 * (4.0 + 2.0 + 1.0), pi * (4.0 + 1.0) + pi * (2.0 + 1.0) * slant, {-2, -2, 0}, {2, 2, 2}});
        solid_case("cone point", tests::shapes::cone_point(),
                   {pi * 2.0 / 3.0, pi + pi * std::sqrt(5.0), {-1, -1, 0}, {1, 1, 2}});
        solid_case("sphere", tests::shapes::sphere(1.5),
                   {4.0 / 3.0 * pi * 3.375, 4.0 * pi * 2.25, {-1.5, -1.5, -1.5}, {1.5, 1.5, 1.5}});
        solid_case("capsule", tests::shapes::capsule(),
                   {pi * 2.0 + 2.0 / 3.0 * pi, 2.0 * pi * 2.0 + 2.0 * pi + pi, {-1, -1, 0}, {1, 1, 3}});
        solid_case("torus", tests::shapes::torus(3.0, 1.0),
                   {2.0 * pi * pi * 3.0, 4.0 * pi * pi * 3.0, {-4, -4, -1}, {4, 4, 1}});
        solid_case("revolved frustum", tests::shapes::revolved_frustum(),
                   {pi * 2.0 / 3.0 * 7.0, pi * 5.0 + pi * 3.0 * slant, {-2, -2, 0}, {2, 2, 2}});

        // The perimeter of an ellipse, Ramanujan's approximation.
        const double h = (2.0 - 1.0) * (2.0 - 1.0) / ((2.0 + 1.0) * (2.0 + 1.0));
        const double perimeter = pi * 3.0 * (1.0 + 3.0 * h / (10.0 + std::sqrt(4.0 - 3.0 * h)));
        solid_case("extruded ellipse", tests::shapes::extruded_ellipse(),
                   {pi * 2.0 * 3.0, 2.0 * pi * 2.0 + perimeter * 3.0, {-2, -1, 0}, {2, 1, 3}});

        // The patch's area has no closed form; its volume does.
        model::load_report report;
        const auto block = load(tests::shapes::bspline_block(), report);
        if (block)
        {
            const auto c = tests::check_solid(*block);
            tests::print("b-spline block", c, report);
            CHECK(c.measure.closed());
            CHECK(report.faces_failed == 0);
            CHECK(near(c.measure.volume, tests::shapes::bspline_block_volume(), c.measure.area * 0.01 * 1.5));
            // Highest in the middle: 1 + (3/8 + 3/8)^2 (2 - 1).
            CHECK(near(as_double(c.bounds.max[2]), 1.5625, 0.01));
            CHECK(c.worst_normal < 45.0 * pi / 180.0);
        }

        solid_case("rational cylinder", tests::shapes::rational_cylinder(),
                   {pi * 2.0, 2.0 * pi + 4.0 * pi, {-1, -1, 0}, {1, 1, 2}});
    }

    // Finer tolerances give closer volumes.
    void converges()
    {
        double previous = 1.0;
        for (double quality : {1.0, 2.0, 4.0})
        {
            model::load_options options;
            options.tessellation = model::tessellation_options::with_quality(quality);
            model::load_report report;
            const auto model = load(tests::shapes::sphere(1.5), report, options);
            if (!model)
                return;
            const auto c = tests::check_solid(*model);
            const double error = std::abs(c.measure.volume - 4.0 / 3.0 * pi * 3.375) / (4.0 / 3.0 * pi * 3.375);
            std::printf("sphere at quality %4.0f: %6zu triangles, volume error %.2e\n", quality, c.measure.triangles, error);
            CHECK(c.measure.closed());
            CHECK(error < previous);
            previous = error;
        }
        CHECK(previous < 2.5e-3);
    }

    void assembly()
    {
        model::load_report report;
        const auto model = load(tests::shapes::assembly(), report);
        if (!model)
            return;
        const auto c = tests::check_solid(*model);
        tests::print("assembly", c, report);

        CHECK(model->name == "Board");
        CHECK(model->meshes.size() == 2);
        CHECK(model->nodes[0].children.size() == 3);
        if (model->nodes[0].children.size() == 3)
        {
            const model::node &b1 = model->nodes[model->nodes[0].children[0]];
            const model::node &b2 = model->nodes[model->nodes[0].children[1]];
            const model::node &p1 = model->nodes[model->nodes[0].children[2]];
            CHECK(b1.name == "Block" && b2.name == "Block" && p1.name == "Pin");
            // One mesh, placed twice.
            CHECK(b1.meshes.size() == 1 && b2.meshes.size() == 1 && b1.meshes == b2.meshes);
            CHECK(p1.meshes.size() == 1 && p1.meshes != b1.meshes);
            CHECK(near(as_double(b2.transform(0, 3)), 5.0, 1e-6));
            if (b1.meshes.size() == 1 && p1.meshes.size() == 1)
            {
                const math::vec3f red = model->meshes[b1.meshes[0]].material.color;
                const math::vec3f blue = model->meshes[p1.meshes[0]].material.color;
                CHECK(red[0] > 0.5f && red[2] < 0.05f);
                CHECK(blue[2] > 0.5f && blue[0] < 0.05f);
            }
        }

        // Block at the origin (2 x 1 x 1), block turned a quarter at x = 5
        // (x from 4 to 5, y from 0 to 2), pin at (2, 2) of radius 0.5, 3 high.
        CHECK(near(as_double(c.bounds.min[0]), 0.0, 1e-5) && near(as_double(c.bounds.max[0]), 5.0, 1e-5));
        CHECK(near(as_double(c.bounds.min[1]), 0.0, 1e-5) && near(as_double(c.bounds.max[1]), 2.5, 0.02));
        CHECK(near(as_double(c.bounds.max[2]), 3.0, 1e-5));
        CHECK(near(c.measure.volume, 2.0 * 2.0 + pi * 0.25 * 3.0, 0.1));
        CHECK(model::is_valid(*model));
    }

    void tessellated_and_faceted()
    {
        {
            model::load_report report;
            const auto model = load(tests::shapes::tessellated_cube(), report);
            if (model)
            {
                const auto c = tests::check_solid(*model);
                tests::print("tessellated cube", c, report);
                CHECK(c.measure.triangles == 12);
                CHECK(c.measure.closed());
                CHECK(near(c.measure.volume, 1.0, 1e-6));
                CHECK(c.worst_normal < 1e-3);
                CHECK(report.clean());
            }
        }
        {
            model::load_report report;
            const auto model = load(tests::shapes::faceted_box(), report);
            if (model)
            {
                const auto c = tests::check_solid(*model);
                tests::print("faceted box", c, report);
                CHECK(c.measure.triangles == 12);
                CHECK(c.measure.closed());
                CHECK(near(c.measure.volume, 2.0, 1e-6));
                CHECK(c.worst_normal < 1e-3);
            }
        }
    }
    // AP242 can carry a solid twice: exactly, and as the exporter's own
    // tessellation of it, linked. One of them is used, not both.
    void tessellation_and_brep()
    {
        tests::step_author a;
        const int brep = tests::shapes::box_solid(a, {0, 0, 0}, {1, 1, 1});
        // The tessellation here sits 10 mm along x, so the bounds tell which
        // of the two was taken.
        const int coordinates = a.add("COORDINATES_LIST('',8,((10.,0.,0.),(11.,0.,0.),(11.,1.,0.),(10.,1.,0.),"
                                      "(10.,0.,1.),(11.,0.,1.),(11.,1.,1.),(10.,1.,1.)))");
        const std::string c = tests::step_author::ref(coordinates);
        const int face = a.add("COMPLEX_TRIANGULATED_FACE(''," + c +
                               ",8,(),$,(),(),((1,4,3,2),(5,6,7,8),(1,2,6,5),(2,3,7,6),(3,4,8,7),(4,1,5,8)))");
        const int solid = a.add(std::format("TESSELLATED_SOLID('',({}),{})", tests::step_author::ref(face),
                                            tests::step_author::ref(brep)));
        a.part("Both", {brep, solid});
        const std::string text = a.text();

        for (bool prefer : {true, false})
        {
            model::load_options options;
            options.prefer_tessellation = prefer;
            model::load_report report;
            const auto m = load(text, report, options);
            if (!m)
                continue;
            const auto c2 = tests::check_solid(*m);
            CHECK(m->meshes.size() == 1);
            CHECK(c2.measure.closed() && near(c2.measure.volume, 1.0, 1e-6));
            CHECK(near(as_double(c2.bounds.min[0]), prefer ? 10.0 : 0.0, 1e-6));
        }
    }
} // namespace

int main()
{
    cube();
    cube_in_inches();
    curved_solids();
    converges();
    assembly();
    tessellated_and_faceted();
    tessellation_and_brep();
    return volt::tests::result("model step");
}
