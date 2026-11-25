#pragma once

#include <model/model.hpp>

#include <cstddef>

// -------------------------------------------------------------------------
// measure
// -------------------------------------------------------------------------
//
// Properties of a triangle mesh that say whether it is a sound solid: is it
// closed, are its triangles oriented consistently, how large is it. The
// loaders' tests lean on these, and so can anything that wants to warn about
// a broken part model.
//
// Vertices are matched by exact position. Faces do not share vertices (each
// keeps its own normals), but the tessellator places the vertices along an
// edge at exactly the same coordinates in both faces, so welding by position
// recovers the solid's topology.

namespace volt::model
{

    struct mesh_measure
    {
        std::size_t triangles = 0;
        std::size_t degenerate_triangles = 0; // two corners at the same position

        // Undirected edges after welding by position.
        std::size_t edges = 0;
        std::size_t boundary_edges = 0;    // used by one triangle: a hole or an open shell
        std::size_t non_manifold_edges = 0; // used by three or more
        // Used by two triangles that run along it in the same direction:
        // one of them faces the wrong way.
        std::size_t inconsistent_edges = 0;

        // Enclosed volume by the divergence theorem, mm^3. Meaningful for a
        // closed mesh; positive when the normals point outward.
        double volume = 0.0;
        double area = 0.0; // mm^2

        // Every edge shared by exactly two triangles running opposite ways.
        bool closed() const noexcept
        {
            return triangles > 0 && boundary_edges == 0 && non_manifold_edges == 0 && inconsistent_edges == 0;
        }
    };

    // In the mesh's own coordinates.
    mesh_measure measure(const mesh &mesh);

    // The largest angle in radians between a vertex normal and the normal of
    // a triangle that uses it, over the whole mesh: how far the normals are
    // from pointing the way the triangles face. Smooth shading of a curved
    // face keeps this small (well below 90 degrees); a normal pointing
    // inward makes it close to 180.
    double worst_normal_deviation(const mesh &mesh);

} // namespace volt::model
