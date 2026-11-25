#pragma once

#include "surfaces.hpp"

#include <math/vector.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

// Private to volt_model: one trimmed face of a B-rep as triangles.
//
// The face is a region of a surface bounded by loops of points in 3D: the
// sampled edges, in the exact positions the neighboring faces use as well,
// so that the two meshes meet without cracks. The loops are taken into the
// surface's parameter space, where the region is a polygon with holes: a
// loop running once around a cylinder becomes a horizontal line across its
// unrolled parameter rectangle, a seam edge used twice becomes the left and
// right side of it, a pole of a sphere becomes a line along its top. That
// polygon is triangulated, then refined with points inside until every
// triangle is within the chord and angle tolerances of the surface.
//
// Loop orientation is only trusted where nothing else decides: on surfaces
// that are closed in every direction (sphere, torus) a single loop can bound
// either side of itself, and there the loop's direction against same_sense
// says which.

namespace volt::model::geometry
{

    struct face_settings
    {
        double chord = 0.01; // in the surface's length unit
        double angle = 0.35;
        std::size_t max_points = 50000;
    };

    struct face_mesh
    {
        std::vector<math::vec3d> positions;
        std::vector<math::vec3d> normals; // unit, facing the way the face does
        std::vector<math::vec2d> parameters; // where on the surface each point is
        std::vector<std::uint32_t> indices; // counter-clockwise seen from the side the normals point to
        bool clean = true; // false when the triangulation needed a fallback

        // How far the boundary lies off the surface: about the file's own
        // tolerance for a sound face, more where its edges and its surface
        // disagree. The tessellation holds to no less than this.
        double mismatch = 0.0;
    };

    // `loops` are closed (the last point is not repeated) and may run either
    // way. The result faces along the surface normal when same_sense is set,
    // against it otherwise.
    std::expected<face_mesh, std::string> tessellate_face(const surface &surface,
                                                          const std::vector<std::vector<math::vec3d>> &loops,
                                                          bool same_sense, const face_settings &settings);

    // A flat polygon with holes given only by its loops (the faces of a
    // FACETED_BREP, POLY_LOOPs): the plane is fitted through the outer loop.
    // `normal`, when not zero, is the direction the face must face.
    std::expected<face_mesh, std::string> tessellate_polygon(const std::vector<std::vector<math::vec3d>> &loops,
                                                             const math::vec3d &normal);

} // namespace volt::model::geometry
