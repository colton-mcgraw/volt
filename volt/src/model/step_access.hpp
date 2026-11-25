#pragma once

#include <model/options.hpp>
#include <model/part21.hpp>

#include <math/matrix.hpp>
#include <math/vector.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

// Private to volt_model: how the STEP reader looks at a Part 21 file. Entity
// types are numbered once per file, so asking an instance what it is costs an
// array lookup; attributes are reached the same way for simple and complex
// instances; points, directions, placements and units come out in double.

namespace volt::model::step
{

#define VOLT_STEP_TYPES(X)                                                                                             \
    X(cartesian_point, "CARTESIAN_POINT")                                                                              \
    X(direction, "DIRECTION")                                                                                          \
    X(vector, "VECTOR")                                                                                                \
    X(axis1_placement, "AXIS1_PLACEMENT")                                                                              \
    X(axis2_placement_2d, "AXIS2_PLACEMENT_2D")                                                                        \
    X(axis2_placement_3d, "AXIS2_PLACEMENT_3D")                                                                        \
    X(cartesian_transformation_operator_3d, "CARTESIAN_TRANSFORMATION_OPERATOR_3D")                                    \
    X(line, "LINE")                                                                                                    \
    X(circle, "CIRCLE")                                                                                                \
    X(ellipse, "ELLIPSE")                                                                                              \
    X(polyline, "POLYLINE")                                                                                            \
    X(b_spline_curve, "B_SPLINE_CURVE")                                                                                \
    X(b_spline_curve_with_knots, "B_SPLINE_CURVE_WITH_KNOTS")                                                          \
    X(rational_b_spline_curve, "RATIONAL_B_SPLINE_CURVE")                                                              \
    X(bezier_curve, "BEZIER_CURVE")                                                                                    \
    X(uniform_curve, "UNIFORM_CURVE")                                                                                  \
    X(quasi_uniform_curve, "QUASI_UNIFORM_CURVE")                                                                      \
    X(trimmed_curve, "TRIMMED_CURVE")                                                                                  \
    X(surface_curve, "SURFACE_CURVE")                                                                                  \
    X(seam_curve, "SEAM_CURVE")                                                                                        \
    X(intersection_curve, "INTERSECTION_CURVE")                                                                        \
    X(bounded_surface_curve, "BOUNDED_SURFACE_CURVE")                                                                  \
    X(pcurve, "PCURVE")                                                                                                \
    X(composite_curve, "COMPOSITE_CURVE")                                                                              \
    X(plane, "PLANE")                                                                                                  \
    X(cylindrical_surface, "CYLINDRICAL_SURFACE")                                                                      \
    X(conical_surface, "CONICAL_SURFACE")                                                                              \
    X(spherical_surface, "SPHERICAL_SURFACE")                                                                          \
    X(toroidal_surface, "TOROIDAL_SURFACE")                                                                            \
    X(degenerate_toroidal_surface, "DEGENERATE_TOROIDAL_SURFACE")                                                      \
    X(surface_of_linear_extrusion, "SURFACE_OF_LINEAR_EXTRUSION")                                                      \
    X(surface_of_revolution, "SURFACE_OF_REVOLUTION")                                                                  \
    X(b_spline_surface, "B_SPLINE_SURFACE")                                                                            \
    X(b_spline_surface_with_knots, "B_SPLINE_SURFACE_WITH_KNOTS")                                                      \
    X(rational_b_spline_surface, "RATIONAL_B_SPLINE_SURFACE")                                                          \
    X(bezier_surface, "BEZIER_SURFACE")                                                                                \
    X(uniform_surface, "UNIFORM_SURFACE")                                                                              \
    X(quasi_uniform_surface, "QUASI_UNIFORM_SURFACE")                                                                  \
    X(vertex_point, "VERTEX_POINT")                                                                                    \
    X(edge_curve, "EDGE_CURVE")                                                                                        \
    X(oriented_edge, "ORIENTED_EDGE")                                                                                  \
    X(edge_loop, "EDGE_LOOP")                                                                                          \
    X(vertex_loop, "VERTEX_LOOP")                                                                                      \
    X(poly_loop, "POLY_LOOP")                                                                                          \
    X(face_bound, "FACE_BOUND")                                                                                        \
    X(face_outer_bound, "FACE_OUTER_BOUND")                                                                            \
    X(advanced_face, "ADVANCED_FACE")                                                                                  \
    X(face_surface, "FACE_SURFACE")                                                                                    \
    X(face, "FACE")                                                                                                    \
    X(oriented_face, "ORIENTED_FACE")                                                                                  \
    X(closed_shell, "CLOSED_SHELL")                                                                                    \
    X(open_shell, "OPEN_SHELL")                                                                                        \
    X(oriented_closed_shell, "ORIENTED_CLOSED_SHELL")                                                                  \
    X(oriented_open_shell, "ORIENTED_OPEN_SHELL")                                                                      \
    X(connected_face_set, "CONNECTED_FACE_SET")                                                                        \
    X(manifold_solid_brep, "MANIFOLD_SOLID_BREP")                                                                      \
    X(brep_with_voids, "BREP_WITH_VOIDS")                                                                              \
    X(faceted_brep, "FACETED_BREP")                                                                                    \
    X(shell_based_surface_model, "SHELL_BASED_SURFACE_MODEL")                                                          \
    X(face_based_surface_model, "FACE_BASED_SURFACE_MODEL")                                                            \
    X(coordinates_list, "COORDINATES_LIST")                                                                            \
    X(tessellated_solid, "TESSELLATED_SOLID")                                                                          \
    X(tessellated_shell, "TESSELLATED_SHELL")                                                                          \
    X(triangulated_face, "TRIANGULATED_FACE")                                                                          \
    X(complex_triangulated_face, "COMPLEX_TRIANGULATED_FACE")                                                          \
    X(triangulated_surface_set, "TRIANGULATED_SURFACE_SET")                                                            \
    X(complex_triangulated_surface_set, "COMPLEX_TRIANGULATED_SURFACE_SET")                                            \
    X(mapped_item, "MAPPED_ITEM")                                                                                      \
    X(representation_map, "REPRESENTATION_MAP")                                                                        \
    X(representation, "REPRESENTATION")                                                                               \
    X(shape_representation, "SHAPE_REPRESENTATION")                                                                   \
    X(representation_relationship, "REPRESENTATION_RELATIONSHIP")                                                     \
    X(shape_representation_relationship, "SHAPE_REPRESENTATION_RELATIONSHIP")                                         \
    X(representation_relationship_with_transformation, "REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION")             \
    X(item_defined_transformation, "ITEM_DEFINED_TRANSFORMATION")                                                     \
    X(context_dependent_shape_representation, "CONTEXT_DEPENDENT_SHAPE_REPRESENTATION")                               \
    X(shape_definition_representation, "SHAPE_DEFINITION_REPRESENTATION")                                            \
    X(product_definition_shape, "PRODUCT_DEFINITION_SHAPE")                                                            \
    X(product_definition, "PRODUCT_DEFINITION")                                                                        \
    X(product_definition_formation, "PRODUCT_DEFINITION_FORMATION")                                                    \
    X(product_definition_formation_with_specified_source, "PRODUCT_DEFINITION_FORMATION_WITH_SPECIFIED_SOURCE")       \
    X(product, "PRODUCT")                                                                                              \
    X(next_assembly_usage_occurrence, "NEXT_ASSEMBLY_USAGE_OCCURRENCE")                                               \
    X(geometric_representation_context, "GEOMETRIC_REPRESENTATION_CONTEXT")                                           \
    X(global_unit_assigned_context, "GLOBAL_UNIT_ASSIGNED_CONTEXT")                                                   \
    X(global_uncertainty_assigned_context, "GLOBAL_UNCERTAINTY_ASSIGNED_CONTEXT")                                     \
    X(length_unit, "LENGTH_UNIT")                                                                                      \
    X(plane_angle_unit, "PLANE_ANGLE_UNIT")                                                                            \
    X(si_unit, "SI_UNIT")                                                                                              \
    X(conversion_based_unit, "CONVERSION_BASED_UNIT")                                                                  \
    X(named_unit, "NAMED_UNIT")                                                                                        \
    X(length_measure_with_unit, "LENGTH_MEASURE_WITH_UNIT")                                                            \
    X(plane_angle_measure_with_unit, "PLANE_ANGLE_MEASURE_WITH_UNIT")                                                  \
    X(measure_with_unit, "MEASURE_WITH_UNIT")                                                                          \
    X(uncertainty_measure_with_unit, "UNCERTAINTY_MEASURE_WITH_UNIT")                                                  \
    X(styled_item, "STYLED_ITEM")                                                                                      \
    X(over_riding_styled_item, "OVER_RIDING_STYLED_ITEM")                                                              \
    X(context_dependent_over_riding_styled_item, "CONTEXT_DEPENDENT_OVER_RIDING_STYLED_ITEM")                         \
    X(colour_rgb, "COLOUR_RGB")                                                                                        \
    X(draughting_pre_defined_colour, "DRAUGHTING_PRE_DEFINED_COLOUR")                                                  \
    X(surface_style_transparent, "SURFACE_STYLE_TRANSPARENT")                                                          \
    X(curve_style, "CURVE_STYLE")                                                                                      \
    X(point_style, "POINT_STYLE")                                                                                      \
    X(surface_style_boundary, "SURFACE_STYLE_BOUNDARY")                                                                \
    X(surface_style_silhouette, "SURFACE_STYLE_SILHOUETTE")                                                            \
    X(surface_style_segmentation_curve, "SURFACE_STYLE_SEGMENTATION_CURVE")                                            \
    X(surface_style_control_grid, "SURFACE_STYLE_CONTROL_GRID")                                                        \
    X(surface_style_parameter_line, "SURFACE_STYLE_PARAMETER_LINE")                                                    \
    X(text_style, "TEXT_STYLE")                                                                                        \
    X(surface_style_usage, "SURFACE_STYLE_USAGE")                                                                      \
    X(surface_side_style, "SURFACE_SIDE_STYLE")

    enum class type : std::uint16_t
    {
#define VOLT_STEP_ENUM(identifier, name) identifier,
        VOLT_STEP_TYPES(VOLT_STEP_ENUM)
#undef VOLT_STEP_ENUM
            count,
        unknown = count,
    };

    std::string_view type_name(type type) noexcept;

    // Up to 32 attributes of one instance in schema order, gathered from a
    // simple instance's parameters or from the parts of a complex one.
    class attributes
    {
    public:
        std::size_t size() const noexcept { return m_count; }
        part21::parameter operator[](std::size_t index) const noexcept
        {
            return index < m_count ? m_values[index] : part21::parameter{};
        }
        void push(part21::parameter value) noexcept
        {
            if (m_count < m_values.size())
                m_values[m_count++] = value;
        }

    private:
        std::array<part21::parameter, 32> m_values{};
        std::size_t m_count = 0;
    };

    // A right-handed frame: where its origin is and where its axes point.
    struct frame
    {
        math::vec3d origin{};
        math::vec3d x{1.0, 0.0, 0.0};
        math::vec3d y{0.0, 1.0, 0.0};
        math::vec3d z{0.0, 0.0, 1.0};

        math::vec3d to_world(const math::vec3d &local) const noexcept
        {
            return origin + x * local[0] + y * local[1] + z * local[2];
        }
        math::mat4d matrix() const noexcept;
    };

    // The units of one representation context, converted to the ones the
    // model uses: how many millimeters in one of its length units, how many
    // radians in one of its angle units.
    struct units
    {
        double length = 1.0;
        double angle = 1.0;
        double uncertainty = 1e-7; // in the context's own length unit
    };

    class access
    {
    public:
        access(const part21::file &file, load_report &report);

        const part21::file &file() const noexcept { return m_file; }
        load_report &report() const noexcept { return m_report; }

        // The type of a simple instance, or the first known type among the
        // parts of a complex one.
        type type_of(const part21::entity &entity) const noexcept;

        // Whether the instance is of the type, as a simple instance or a
        // part of a complex one.
        bool is(const part21::entity &entity, type type) const noexcept;

        // The instance a parameter refers to; invalid when it refers to
        // nothing or to an instance the file does not have.
        part21::entity resolve(const part21::parameter &parameter) const noexcept;

        // The attributes of an instance in schema order. For a simple
        // instance its parameters as they are. For a complex one, the
        // parameters of each listed part that it has, in the order listed:
        // the supertypes first, as the schema orders attributes.
        attributes gather(const part21::entity &entity, std::initializer_list<type> chain) const noexcept;

        // Of a part only, or the whole simple instance.
        part21::parameter part(const part21::entity &entity, type type) const noexcept;

        // Geometry in the file's own units.
        std::optional<math::vec3d> point(const part21::parameter &reference) const;
        std::optional<math::vec3d> direction(const part21::parameter &reference) const;
        std::optional<frame> placement(const part21::parameter &reference) const;
        std::optional<math::mat4d> transformation(const part21::parameter &reference) const;

        // The units of a representation context, looked up once per context.
        units context_units(const part21::parameter &context);
        units context_units(const part21::entity &context);

        // The display name of a type, also for complex instances
        // ("(A B C)").
        std::string describe(const part21::entity &entity) const;

    private:
        const part21::file &m_file;
        load_report &m_report;
        std::vector<type> m_types; // per name id
        std::array<std::uint32_t, static_cast<std::size_t>(type::count)> m_ids{};
        std::unordered_map<std::uint32_t, units> m_units;

        double unit_factor(const part21::entity &unit, type kind, int depth) const;
    };

} // namespace volt::model::step
