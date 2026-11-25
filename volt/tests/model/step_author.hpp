#pragma once

#include <math/geometry.hpp>
#include <math/vector.hpp>

#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Writes STEP test files the way exporters lay them out: an AP214 header,
// products with their shape representations, one length/angle context of
// complex unit instances, styled items for colors. The B-rep entities are
// added one call at a time, so a test spells out a shape's topology (which
// edge bounds which face, in which direction) and nothing else.

namespace volt::tests
{

    class step_author
    {
    public:
        using vec3 = math::vec3d;

        int add(const std::string &entity)
        {
            const int id = m_next++;
            m_lines.push_back(std::format("#{} = {};", id, entity));
            return id;
        }

        static std::string ref(int id) { return std::format("#{}", id); }

        static std::string real(double v)
        {
            std::string text = std::format("{}", v);
            if (text.find('.') == std::string::npos && text.find('e') == std::string::npos)
                text += ".";
            const std::size_t e = text.find('e');
            if (e != std::string::npos)
            {
                std::string mantissa = text.substr(0, e);
                if (mantissa.find('.') == std::string::npos)
                    mantissa += ".";
                text = mantissa + "E" + text.substr(e + 1);
            }
            return text;
        }

        static std::string triple(const vec3 &v)
        {
            return std::format("({},{},{})", real(v[0]), real(v[1]), real(v[2]));
        }

        int point(const vec3 &p) { return add("CARTESIAN_POINT(''," + triple(p) + ")"); }
        int direction(const vec3 &d) { return add("DIRECTION(''," + triple(math::normalized(d)) + ")"); }

        int placement(const vec3 &origin, const vec3 &axis = {0, 0, 1}, const vec3 &x = {1, 0, 0})
        {
            const int o = point(origin);
            const int z = direction(axis);
            const int r = direction(x);
            return add(std::format("AXIS2_PLACEMENT_3D('',{},{},{})", ref(o), ref(z), ref(r)));
        }

        int vertex(const vec3 &p) { return add(std::format("VERTEX_POINT('',{})", ref(point(p)))); }

        int line(const vec3 &from, const vec3 &to)
        {
            const vec3 d = to - from;
            const int dir = direction(d);
            const int vector = add(std::format("VECTOR('',{},{})", ref(dir), real(math::magnitude(d))));
            return add(std::format("LINE('',{},{})", ref(point(from)), ref(vector)));
        }

        int circle(const vec3 &centre, const vec3 &axis, const vec3 &x, double radius)
        {
            return add(std::format("CIRCLE('',{},{})", ref(placement(centre, axis, x)), real(radius)));
        }

        int edge(int from, int to, int curve, bool same_sense = true)
        {
            return add(std::format("EDGE_CURVE('',{},{},{},{})", ref(from), ref(to), ref(curve),
                                   same_sense ? ".T." : ".F."));
        }

        // An edge whose geometry is a SURFACE_CURVE (or SEAM_CURVE) around
        // a 3D curve, as OpenCASCADE writes them.
        int surface_edge(int from, int to, int curve, int surface, bool seam = false, bool same_sense = true)
        {
            const int wrapped = add(std::format("{}('',{},({}),.PCURVE_S1.)", seam ? "SEAM_CURVE" : "SURFACE_CURVE",
                                                ref(curve), ref(surface)));
            return edge(from, to, wrapped, same_sense);
        }

        int loop(const std::vector<std::pair<int, bool>> &edges)
        {
            std::string list;
            for (const auto &[e, forward] : edges)
            {
                const int oriented = add(std::format("ORIENTED_EDGE('',*,*,{},{})", ref(e), forward ? ".T." : ".F."));
                list += (list.empty() ? "" : ",") + ref(oriented);
            }
            return add(std::format("EDGE_LOOP('',({}))", list));
        }

        int bound(int loop, bool outer = true, bool orientation = true)
        {
            return add(std::format("{}('',{},{})", outer ? "FACE_OUTER_BOUND" : "FACE_BOUND", ref(loop),
                                   orientation ? ".T." : ".F."));
        }

        int face(const std::vector<int> &bounds, int surface, bool same_sense = true)
        {
            std::string list;
            for (int b : bounds)
                list += (list.empty() ? "" : ",") + ref(b);
            return add(std::format("ADVANCED_FACE('',({}),{},{})", list, ref(surface), same_sense ? ".T." : ".F."));
        }

        int plane(const vec3 &origin, const vec3 &normal, const vec3 &x = {1, 0, 0})
        {
            return add(std::format("PLANE('',{})", ref(placement(origin, normal, x))));
        }

        int solid(const std::vector<int> &faces)
        {
            std::string list;
            for (int f : faces)
                list += (list.empty() ? "" : ",") + ref(f);
            const int shell = add(std::format("CLOSED_SHELL('',({}))", list));
            return add(std::format("MANIFOLD_SOLID_BREP('',{})", ref(shell)));
        }

        // The one representation context every shape here uses:
        // millimeters, radians.
        int context()
        {
            if (m_context != 0)
                return m_context;
            const int length = add("( LENGTH_UNIT() NAMED_UNIT(*) SI_UNIT(.MILLI.,.METRE.) )");
            const int angle = add("( NAMED_UNIT(*) PLANE_ANGLE_UNIT() SI_UNIT($,.RADIAN.) )");
            const int solid_angle = add("( NAMED_UNIT(*) SI_UNIT($,.STERADIAN.) SOLID_ANGLE_UNIT() )");
            const int uncertainty =
                add(std::format("UNCERTAINTY_MEASURE_WITH_UNIT(LENGTH_MEASURE(1.E-07),{},'distance_accuracy_value',"
                                "'confusion accuracy')",
                                ref(length)));
            m_context = add(std::format("( GEOMETRIC_REPRESENTATION_CONTEXT(3) GLOBAL_UNCERTAINTY_ASSIGNED_CONTEXT(({})) "
                                        "GLOBAL_UNIT_ASSIGNED_CONTEXT(({},{},{})) REPRESENTATION_CONTEXT('Context #1',"
                                        "'3D Context with UNIT and UNCERTAINTY') )",
                                        ref(uncertainty), ref(length), ref(angle), ref(solid_angle)));
            return m_context;
        }

        void color(int item, const vec3 &srgb)
        {
            const int rgb = add(std::format("COLOUR_RGB('',{},{},{})", real(srgb[0]), real(srgb[1]), real(srgb[2])));
            const int fill_colour = add(std::format("FILL_AREA_STYLE_COLOUR('',{})", ref(rgb)));
            const int fill = add(std::format("FILL_AREA_STYLE('',({}))", ref(fill_colour)));
            const int area = add(std::format("SURFACE_STYLE_FILL_AREA({})", ref(fill)));
            const int side = add(std::format("SURFACE_SIDE_STYLE('',({}))", ref(area)));
            const int usage = add(std::format("SURFACE_STYLE_USAGE(.BOTH.,{})", ref(side)));
            const int assignment = add(std::format("PRESENTATION_STYLE_ASSIGNMENT(({}))", ref(usage)));
            const int styled = add(std::format("STYLED_ITEM('color',({}),{})", ref(assignment), ref(item)));
            m_styled.push_back(styled);
        }

        struct part_ids
        {
            int definition = 0;
            int representation = 0;
            int origin = 0; // the representation's own placement
        };

        // A product whose shape is `items`, in a representation of type
        // `representation_type`.
        part_ids part(const std::string &name, const std::vector<int> &items,
                      const std::string &representation_type = "ADVANCED_BREP_SHAPE_REPRESENTATION")
        {
            if (m_application == 0)
                m_application = add("APPLICATION_CONTEXT('core data for automotive mechanical design processes')");
            const int product_context = add(std::format("PRODUCT_CONTEXT('',{},'mechanical')", ref(m_application)));
            const int product = add(std::format("PRODUCT('{}','{}','',({}))", name, name, ref(product_context)));
            const int formation = add(std::format("PRODUCT_DEFINITION_FORMATION('','',{})", ref(product)));
            const int definition_context =
                add(std::format("PRODUCT_DEFINITION_CONTEXT('part definition',{},'design')", ref(m_application)));
            part_ids ids;
            ids.definition =
                add(std::format("PRODUCT_DEFINITION('design','',{},{})", ref(formation), ref(definition_context)));
            const int shape = add(std::format("PRODUCT_DEFINITION_SHAPE('','',{})", ref(ids.definition)));
            ids.origin = placement({0, 0, 0});
            std::string list = ref(ids.origin);
            for (int item : items)
                list += "," + ref(item);
            ids.representation = add(std::format("{}('',({}),{})", representation_type, list, ref(context())));
            add(std::format("SHAPE_DEFINITION_REPRESENTATION({},{})", ref(shape), ref(ids.representation)));
            return ids;
        }

        // `child` placed in `parent` at `where` (an AXIS2_PLACEMENT_3D added
        // to the parent's representation by the caller).
        void occurrence(const part_ids &parent, const part_ids &child, int where, const std::string &name)
        {
            const int usage = add(std::format("NEXT_ASSEMBLY_USAGE_OCCURRENCE('{}','{}','',{},{},$)", name, name,
                                              ref(parent.definition), ref(child.definition)));
            const int shape = add(std::format("PRODUCT_DEFINITION_SHAPE('Placement','Placement of an item',{})", ref(usage)));
            const int transform =
                add(std::format("ITEM_DEFINED_TRANSFORMATION('','',{},{})", ref(child.origin), ref(where)));
            const int relation = add(std::format("( REPRESENTATION_RELATIONSHIP('','',{},{}) "
                                                 "REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION({}) "
                                                 "SHAPE_REPRESENTATION_RELATIONSHIP() )",
                                                 ref(child.representation), ref(parent.representation), ref(transform)));
            add(std::format("CONTEXT_DEPENDENT_SHAPE_REPRESENTATION({},{})", ref(relation), ref(shape)));
        }

        // Reserves an id now for an entity whose text is given later; for
        // representations that must list placements created after them.
        int reserve()
        {
            const int id = m_next++;
            m_lines.push_back({});
            m_reserved.push_back({id, m_lines.size() - 1});
            return id;
        }

        void fill(int id, const std::string &entity)
        {
            for (const auto &[reserved, line] : m_reserved)
            {
                if (reserved == id)
                    m_lines[line] = std::format("#{} = {};", id, entity);
            }
        }

        std::string text(const std::string &file_name = "test.step")
        {
            if (!m_styled.empty())
            {
                std::string list;
                for (int s : m_styled)
                    list += (list.empty() ? "" : ",") + ref(s);
                add(std::format("MECHANICAL_DESIGN_GEOMETRIC_PRESENTATION_REPRESENTATION('',({}),{})", list,
                                ref(context())));
                m_styled.clear();
            }
            std::string out = "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('Volt test model'),'2;1');\n";
            out += std::format("FILE_NAME('{}','2026-11-21T09:00:00',('Volt tests'),(''),\n  'Volt test author',"
                               "'Volt','Unknown');\n",
                               file_name);
            out += "FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));\nENDSEC;\nDATA;\n";
            for (const std::string &line : m_lines)
                out += line + "\n";
            out += "ENDSEC;\nEND-ISO-10303-21;\n";
            return out;
        }

    private:
        int m_next = 1;
        int m_context = 0;
        int m_application = 0;
        std::vector<std::string> m_lines;
        std::vector<int> m_styled;
        std::vector<std::pair<int, std::size_t>> m_reserved;
    };

} // namespace volt::tests
