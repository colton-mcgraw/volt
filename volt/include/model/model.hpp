#pragma once

#include <math/matrix.hpp>
#include <math/vector.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// model
// -------------------------------------------------------------------------
//
// What a 3D model file turns into: triangle meshes, and a tree of instances
// that places them. Plain data, free of the GPU and the window, shaped so
// that the board view can copy it into vertex and index buffers as it is.
//
// Units and conventions, the same for every format that loads into this:
//
//   * Lengths are millimeters. Each loader converts from what its file says
//     (STEP carries its units; OBJ and STL carry none and are taken to be
//     millimeters already).
//   * Coordinates are those of <math/transform.hpp>: right-handed, column
//     vectors, radians.
//   * Triangles are counter-clockwise seen from the side their normals point
//     to, which for a closed solid is the outside.
//   * Colors are in the renderer's working space: linear light with the sRGB
//     primaries, 1.0 being SDR white (<gpu/color.hpp>). Files store colors
//     sRGB encoded, and the loaders convert.

namespace volt::model
{

    // One vertex as the GPU reads it: 24 bytes, position at offset 0 and
    // normal at offset 12, both three 32-bit floats. Vertices are not shared
    // between the faces of a solid, so that every face keeps its own normals:
    // smooth across a face, a hard edge where two faces meet.
    struct vertex
    {
        math::vec3f position;
        math::vec3f normal; // unit length

        friend bool operator==(const vertex &, const vertex &) = default;
    };

    static_assert(sizeof(vertex) == 24);

    // Color as sRGB-encoded values in 0..1, and back.
    float srgb_to_linear(float encoded) noexcept;
    float linear_to_srgb(float linear) noexcept;
    math::vec3f srgb_to_linear(const math::vec3f &encoded) noexcept;
    math::vec3f linear_to_srgb(const math::vec3f &linear) noexcept;

    // What a part looks like when its file says nothing: a light gray, sRGB
    // 0.8, which is what most CAD programs show.
    inline constexpr math::vec3f default_color{0.6038273f, 0.6038273f, 0.6038273f};

    struct material
    {
        math::vec3f color = default_color; // working space
        float opacity = 1.0f;              // 1 is opaque

        friend bool operator==(const material &, const material &) = default;
    };

    // ---------------------------------------------------------------------
    // mesh
    // ---------------------------------------------------------------------
    //
    // An indexed triangle list in the coordinates of whatever node refers to
    // it. Usually one solid of a part.
    struct mesh
    {
        std::string name;

        std::vector<vertex> vertices;

        // Three per triangle, each an index into vertices.
        std::vector<std::uint32_t> indices;

        // Empty, or one color per vertex (working space), for meshes whose
        // faces are not all the same color: a part body with a colored pin 1
        // marker. Where present it replaces material.color.
        std::vector<math::vec3f> colors;

        volt::model::material material;

        std::size_t triangle_count() const noexcept { return indices.size() / 3; }
        bool empty() const noexcept { return indices.empty(); }
    };

    // ---------------------------------------------------------------------
    // node
    // ---------------------------------------------------------------------
    //
    // One instance in the assembly tree. Nodes and meshes are referred to by
    // index into the model's arrays, so one mesh can be placed many times (the
    // same resistor twelve times on a board) without being copied.
    struct node
    {
        std::string name;

        // From this node's coordinates to its parent's. Rigid for everything
        // a STEP file describes; other formats may scale or mirror.
        math::mat4f transform = math::mat4f::identity();

        std::vector<std::uint32_t> meshes;   // into model::meshes
        std::vector<std::uint32_t> children; // into model::nodes
    };

    // An axis-aligned box. Default constructed it is empty, and adding the
    // first point makes it that point.
    struct bounds
    {
        math::vec3f min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max()};
        math::vec3f max{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                        std::numeric_limits<float>::lowest()};

        bool empty() const noexcept { return min[0] > max[0]; }
        void add(const math::vec3f &point) noexcept;
        void add(const bounds &other) noexcept;
        math::vec3f size() const noexcept;
        math::vec3f center() const noexcept;
    };

    // ---------------------------------------------------------------------
    // model
    // ---------------------------------------------------------------------
    //
    // A tree of nodes, nodes[0] being the root, that refer to meshes. Every
    // node other than the root is the child of exactly one other; a node
    // tree that is not a tree (a cycle, a node listed twice) is cut where the
    // repetition starts by everything that walks it.
    struct model
    {
        std::string name;
        std::vector<mesh> meshes;
        std::vector<node> nodes;

        bool empty() const noexcept { return meshes.empty(); }

        std::size_t triangle_count() const noexcept;
    };

    // Where every instance of every mesh ends up, as the box around them.
    bounds bounds_of(const model &model);

    // The box around the mesh's vertices after `transform`.
    bounds bounds_of(const mesh &mesh, const math::mat4f &transform = math::mat4f::identity());

    // Calls visit(mesh_index, world_transform) for every placed mesh, depth
    // first from the root. Stops descending at depth 256 and never visits a
    // node twice, so a malformed tree cannot make it loop.
    template <typename Visit>
    void for_each_instance(const model &model, Visit &&visit);

    // The model as one root node holding one mesh per placed instance, every
    // mesh in world coordinates: what a format without an instance tree (OBJ,
    // STL) stores, and what is simplest to draw. Normals go through the
    // inverse transpose and are renormalized; a mirroring transform reverses
    // the triangles so they stay counter-clockwise.
    model flatten(const model &model);

    // Applies `transform` to the mesh in place, as flatten() does.
    void transform_mesh(mesh &mesh, const math::mat4d &transform);

    // Normals for a mesh whose file does not have them (STL, some OBJ).
    // Vertices at the same position are averaged over the triangles around
    // them that meet at less than `crease_angle` (radians); across sharper
    // edges they are split. Rebuilds vertices, so the vertex count changes.
    void compute_normals(mesh &mesh, float crease_angle = 0.5235988f);

    // Checks the indices: in range, a multiple of three, and nodes pointing
    // at nodes and meshes that exist. What a renderer wants to know before it
    // uploads anything.
    bool is_valid(const model &model);

    // ---------------------------------------------------------------------

    template <typename Visit>
    void for_each_instance(const model &model, Visit &&visit)
    {
        if (model.nodes.empty())
            return;

        struct pending
        {
            std::uint32_t node;
            math::mat4f transform;
            int depth;
        };

        std::vector<char> visited(model.nodes.size(), 0);
        std::vector<pending> stack{{0, model.nodes[0].transform, 0}};
        visited[0] = 1;
        while (!stack.empty())
        {
            const pending current = stack.back();
            stack.pop_back();

            const node &node = model.nodes[current.node];
            for (std::uint32_t mesh : node.meshes)
            {
                if (mesh < model.meshes.size())
                    visit(mesh, current.transform);
            }

            if (current.depth >= 256)
                continue;
            for (auto it = node.children.rbegin(); it != node.children.rend(); ++it)
            {
                const std::uint32_t child = *it;
                if (child >= model.nodes.size() || visited[child] != 0)
                    continue;
                visited[child] = 1;
                stack.push_back({child, current.transform * model.nodes[child].transform, current.depth + 1});
            }
        }
    }

} // namespace volt::model
