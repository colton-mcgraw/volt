#pragma once

#include <model/model.hpp>
#include <model/options.hpp>
#include <model/part21.hpp>

#include <expected>
#include <string>
#include <string_view>

// -------------------------------------------------------------------------
// step
// -------------------------------------------------------------------------
//
// STEP models (ISO 10303) as meshes: what component manufacturers and
// KiCad's 3D library ship. Reads the product structure, units, colors and
// geometry of AP203, AP214 and AP242 files:
//
//   * Assemblies: PRODUCT_DEFINITIONs with their SHAPE_DEFINITION_
//     REPRESENTATIONs, NEXT_ASSEMBLY_USAGE_OCCURRENCEs placed by
//     CONTEXT_DEPENDENT_SHAPE_REPRESENTATION (ITEM_DEFINED_TRANSFORMATION
//     between two AXIS2_PLACEMENT_3Ds), MAPPED_ITEMs, and
//     SHAPE_REPRESENTATION_RELATIONSHIPs between representations. A part
//     used twice is one mesh placed twice.
//   * Units: each representation's GLOBAL_UNIT_ASSIGNED_CONTEXT, SI units
//     with prefixes and conversion based ones (inches, degrees).
//   * Colors: STYLED_ITEM and OVER_RIDING_STYLED_ITEM on solids, shells and
//     single faces, COLOUR_RGB and DRAUGHTING_PRE_DEFINED_COLOUR, and
//     SURFACE_STYLE_TRANSPARENT.
//   * Exact geometry, tessellated here: MANIFOLD_SOLID_BREP, BREP_WITH_VOIDS,
//     SHELL_BASED_SURFACE_MODEL, FACE_BASED_SURFACE_MODEL of ADVANCED_FACEs
//     on PLANE, CYLINDRICAL_, CONICAL_, SPHERICAL_, TOROIDAL_SURFACE,
//     SURFACE_OF_LINEAR_EXTRUSION, SURFACE_OF_REVOLUTION and (rational)
//     B_SPLINE_SURFACE, bounded by edges on LINE, CIRCLE, ELLIPSE, POLYLINE
//     and (rational) B_SPLINE_CURVE, through SURFACE_CURVE, SEAM_CURVE and
//     TRIMMED_CURVE.
//   * Faceted geometry: FACETED_BREP and faces bounded by POLY_LOOPs.
//   * AP242 tessellated geometry: TESSELLATED_SOLID and _SHELL of
//     TRIANGULATED_FACE and COMPLEX_TRIANGULATED_FACE, and the triangulated
//     surface sets.
//
// Colors in a STEP file are sRGB-encoded values in 0..1, as every exporter
// takes them from its color picker; they are converted to the linear working
// space (<model/model.hpp>) on the way in and back on the way out.
//
// What is not understood is skipped and counted in the load report, never
// fatal: a model with a face missing is more use than no model.

namespace volt::model::step
{

    // The model a parsed file describes.
    std::expected<model, std::string> read(const part21::file &file, const load_options &options,
                                           load_report &report);

    // Parses and reads a whole file held in memory.
    std::expected<model, std::string> read(std::string_view text, const load_options &options, load_report &report);

    struct write_options
    {
        // For the header: the file's name, who wrote it, with what.
        std::string file_name;
        std::string author;
        std::string organization;
        std::string originating_system = "Volt";

        // ISO 8601, for the header. Empty writes a fixed timestamp, which
        // keeps files written from the same model identical.
        std::string timestamp;
    };

    // A model as a new STEP file: AP214 (AUTOMOTIVE_DESIGN), each mesh a
    // FACETED_BREP (or an open shell where the mesh is not closed), with the
    // node tree as products and assembly occurrences, colors as STYLED_ITEMs
    // and lengths in millimeters.
    //
    // Why faceted B-rep in AP214 rather than AP242 tessellation: it is what
    // every STEP reader understands, from OpenCASCADE (FreeCAD, KiCad) to the
    // commercial ones, many of which still ignore TRIANGULATED_FACE. The
    // price is size: a triangle is a FACE, a FACE_OUTER_BOUND and a POLY_LOOP.
    part21::file build(const model &model, const write_options &options = {});
    std::string write(const model &model, const write_options &options = {});

} // namespace volt::model::step
