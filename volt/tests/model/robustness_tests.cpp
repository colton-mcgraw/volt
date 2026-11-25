// What the STEP reader does with files that are wrong: references to
// nothing, references in circles, geometry it cannot use, and files damaged
// at random. It must answer every one with a model or an error, without
// crashing and without hanging.

#include "model_test_support.hpp"
#include "step_shapes.hpp"

#include <model/step.hpp>

#include <bit>
#include <chrono>
#include <functional>

using namespace volt;

namespace
{
    struct sample
    {
        const char *name;
        std::string text;
    };

    std::vector<sample> samples()
    {
        return {
            {"cube", tests::read_asset("cube.step")},
            {"cylinder", tests::shapes::cylinder()},
            {"plate", tests::shapes::plate_with_hole()},
            {"cone", tests::shapes::cone_point()},
            {"sphere", tests::shapes::sphere()},
            {"torus", tests::shapes::torus()},
            {"revolved", tests::shapes::revolved_frustum()},
            {"extruded", tests::shapes::extruded_ellipse()},
            {"bspline", tests::shapes::bspline_block()},
            {"rational", tests::shapes::rational_cylinder()},
            {"assembly", tests::shapes::assembly()},
            {"tessellated", tests::shapes::tessellated_cube()},
            {"faceted", tests::shapes::faceted_box()},
        };
    }

    bool same_value(const model::part21::parameter &a, const model::part21::parameter &b)
    {
        using model::part21::kind;
        if (a.type() != b.type())
            return false;
        switch (a.type())
        {
        case kind::integer:
            return a.integer() == b.integer();
        case kind::real:
            return std::bit_cast<std::uint64_t>(a.number()) == std::bit_cast<std::uint64_t>(b.number());
        case kind::string:
        case kind::enumeration:
        case kind::binary:
            return a.text() == b.text();
        case kind::reference:
            return a.reference() == b.reference();
        case kind::list:
            if (a.size() != b.size())
                return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (!same_value(a[i], b[i]))
                    return false;
            return true;
        case kind::typed:
            return a.type_name() == b.type_name() && same_value(a.inner(), b.inner());
        default:
            return true;
        }
    }

    // Parse, write, parse: the same entities, and the same model from them.
    void part21_round_trips()
    {
        for (const sample &s : samples())
        {
            const auto first = model::part21::parse(s.text);
            CHECK(first.has_value());
            if (!first)
                continue;
            const auto second = model::part21::parse(model::part21::write(*first));
            CHECK(second.has_value());
            if (!second)
                continue;
            bool same = first->size() == second->size();
            for (std::size_t i = 0; same && i < first->size(); ++i)
            {
                const auto a = (*first)[i];
                const auto b = (*second)[i];
                same = a.id() == b.id() && a.part_count() == b.part_count();
                for (std::size_t p = 0; same && p < a.part_count(); ++p)
                    same = a.name(p) == b.name(p) && same_value(a.parameters(p), b.parameters(p));
            }
            CHECK(same);

            model::load_report r1;
            model::load_report r2;
            const auto m1 = model::step::read(*first, {}, r1);
            const auto m2 = model::step::read(*second, {}, r2);
            CHECK(m1.has_value() && m2.has_value() && m1->triangle_count() == m2->triangle_count());
        }
    }

    std::string wrap(const std::string &data)
    {
        return "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION((''),'2;1');\nENDSEC;\nDATA;\n" + data +
               "ENDSEC;\nEND-ISO-10303-21;\n";
    }

    void malformed()
    {
        struct bad
        {
            const char *what;
            std::string text;
            bool loads; // whether something is still expected to come out
        };
        const std::string plane = "#10=CARTESIAN_POINT('',(0.,0.,0.));\n#11=DIRECTION('',(0.,0.,1.));\n"
                                  "#12=AXIS2_PLACEMENT_3D('',#10,#11,$);\n#13=PLANE('',#12);\n";
        const bad cases[] = {
            {"a solid of nothing", wrap("#1=MANIFOLD_SOLID_BREP('',#999);\n"), false},
            {"a shell in a circle", wrap("#1=MANIFOLD_SOLID_BREP('',#2);\n#2=ORIENTED_CLOSED_SHELL('',*,#2,.T.);\n"),
             false},
            {"a face in a circle", wrap("#1=MANIFOLD_SOLID_BREP('',#2);\n#2=CLOSED_SHELL('',(#3));\n"
                                        "#3=ORIENTED_FACE('',*,#3,.F.);\n"),
             false},
            {"curves in a circle", wrap(plane + "#1=MANIFOLD_SOLID_BREP('',#2);\n#2=CLOSED_SHELL('',(#3));\n"
                                                "#3=ADVANCED_FACE('',(#4),#13,.T.);\n#4=FACE_OUTER_BOUND('',#5,.T.);\n"
                                                "#5=EDGE_LOOP('',(#6));\n#6=ORIENTED_EDGE('',*,*,#7,.T.);\n"
                                                "#7=EDGE_CURVE('',#8,#8,#9,.T.);\n#8=VERTEX_POINT('',#10);\n"
                                                "#9=TRIMMED_CURVE('',#20,(),(),.T.,.UNSPECIFIED.);\n"
                                                "#20=SURFACE_CURVE('',#9,(),.CURVE_3D.);\n"),
             false},
            {"strings where references belong",
             wrap("#1=MANIFOLD_SOLID_BREP('',#2);\n#2=CLOSED_SHELL('',('x',3,.T.,$));\n"), false},
            {"a surface it does not know",
             wrap(plane + "#1=MANIFOLD_SOLID_BREP('',#2);\n#2=CLOSED_SHELL('',(#3));\n"
                          "#3=ADVANCED_FACE('',(#4),#30,.T.);\n#4=FACE_OUTER_BOUND('',#5,.T.);\n"
                          "#5=POLY_LOOP('',(#10,#14,#15));\n#14=CARTESIAN_POINT('',(1.,0.,0.));\n"
                          "#15=CARTESIAN_POINT('',(0.,1.,0.));\n#30=OFFSET_SURFACE('',#13,1.,.F.);\n"),
             true},
            {"a B-spline with knots that do not fit",
             wrap("#1=MANIFOLD_SOLID_BREP('',#2);\n#2=CLOSED_SHELL('',(#3));\n#3=ADVANCED_FACE('',(),#4,.T.);\n"
                  "#4=B_SPLINE_SURFACE_WITH_KNOTS('',3,3,((#10,#10),(#10,#10)),.UNSPECIFIED.,.F.,.F.,.F.,(4),(4),"
                  "(0.),(0.),.UNSPECIFIED.);\n#10=CARTESIAN_POINT('',(0.,0.,0.));\n"),
             false},
            {"absurd coordinates",
             wrap("#1=TESSELLATED_SOLID('',(#2),$);\n#2=TRIANGULATED_FACE('',#3,3,(),$,(),((1,2,3),(1,2,99),(-1,0,2)));\n"
                  "#3=COORDINATES_LIST('',3,((1.E308,0.,0.),(0.,-1.E308,0.),(0.,0.,1.E308)));\n"),
             true},
            {"an assembly that contains itself",
             tests::shapes::assembly() + "", true},
        };
        for (const bad &c : cases)
        {
            std::string text = c.text;
            if (std::string_view(c.what) == "an assembly that contains itself")
            {
                // The block becomes the board's parent as well as its child.
                const std::size_t end = text.rfind("ENDSEC;");
                const std::size_t board = text.find("PRODUCT('Board'");
                const std::size_t block = text.find("PRODUCT('Block'");
                CHECK(board != std::string::npos && block != std::string::npos);
                // Find the definitions: the PRODUCT_DEFINITION lines after each product.
                const auto definition_after = [&](std::size_t at) {
                    const std::size_t line = text.find("PRODUCT_DEFINITION('design'", at);
                    const std::size_t hash = text.rfind('#', line);
                    return text.substr(hash, text.find(' ', hash) - hash);
                };
                const std::string board_definition = definition_after(board);
                const std::string block_definition = definition_after(block);
                text.insert(end, "#90000=NEXT_ASSEMBLY_USAGE_OCCURRENCE('X','X',''," + block_definition + "," +
                                     board_definition + ",$);\n");
            }
            model::load_report report;
            const auto start = std::chrono::steady_clock::now();
            const auto result = model::step::read(text, {}, report);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::printf("%-40s %s (%.3f s)%s\n", c.what, result ? "loaded" : result.error().c_str(), seconds,
                        report.faces_failed ? " with failed faces" : "");
            CHECK(result.has_value() == c.loads);
            CHECK(seconds < 5.0);
        }
    }

    // Damaged copies of every sample through the whole reader.
    void corruption()
    {
        std::uint32_t state = 2463534242u;
        const auto random = [&] {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        };
        model::load_options options;
        // Coarser than default: the point is to survive, not to be pretty.
        options.tessellation = model::tessellation_options::with_quality(0.5);

        std::size_t runs = 0;
        std::size_t loaded = 0;
        double slowest = 0.0;
        for (const sample &s : samples())
        {
            for (int round = 0; round < 120; ++round)
            {
                std::string copy = s.text;
                const std::uint32_t how = random() % 4;
                if (how == 0)
                    copy.resize(random() % copy.size());
                else if (how == 1)
                {
                    for (int f = 0; f < 1 + static_cast<int>(random() % 6); ++f)
                    {
                        const std::size_t at = random() % copy.size();
                        copy[at] = static_cast<char>(copy[at] ^ static_cast<char>(1u << (random() % 8)));
                    }
                }
                else if (how == 2)
                {
                    // Digits changed: references to other instances, other
                    // numbers, which keeps the file parseable more often.
                    for (int f = 0; f < 1 + static_cast<int>(random() % 4); ++f)
                    {
                        const std::size_t at = random() % copy.size();
                        if (copy[at] >= '0' && copy[at] <= '9')
                            copy[at] = static_cast<char>('0' + random() % 10);
                    }
                }
                else
                {
                    // A line dropped.
                    const std::size_t at = copy.find('\n', random() % copy.size());
                    const std::size_t next = at == std::string::npos ? std::string::npos : copy.find('\n', at + 1);
                    if (next != std::string::npos)
                        copy.erase(at, next - at);
                }

                model::load_report report;
                const auto start = std::chrono::steady_clock::now();
                const auto result = model::step::read(copy, options, report);
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                if (seconds > 1.0)
                    std::printf("  slow: %s, damaged copy %d, %.2f s\n", s.name, round, seconds);
                slowest = std::max(slowest, seconds);
                ++runs;
                if (result)
                {
                    ++loaded;
                    CHECK(model::is_valid(*result));
                }
            }
        }
        std::printf("corruption: %zu damaged files, %zu still gave a model, slowest %.3f s\n", runs, loaded, slowest);
        CHECK(slowest < 10.0);
    }
} // namespace

int main()
{
    part21_round_trips();
    malformed();
    corruption();
    return volt::tests::result("model robustness");
}
