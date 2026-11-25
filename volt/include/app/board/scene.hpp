#pragma once

#include <model/model.hpp>

#include <math/matrix.hpp>
#include <math/vector.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// What the board view draws, as plain data: the renderer (<app/board/
// renderer.hpp>) turns it into commands, the application fills it. And
// what the view's pixels land on: the board's top, and the things on it.
// Nothing here touches the GPU, so the code that builds scenes builds and is
// tested without one (src/app/board/scene.cpp).
//
// Objects. Boxes and mesh instances are what the renderer draws; an object
// is what the user sees and picks: the board itself, a part, a model
// dropped on it. Every box and instance names the object it belongs to (0:
// none), and the scene keeps the objects in a table, by id, with what the
// interface shows of them (name, file, bounds, triangles) and whether they
// are drawn. A model placed on the board becomes one object, however many
// instances its parts make (add_model(), <app/board/model.hpp>); boxes put
// in the scene without an object get one from adopt_boxes().

namespace volt::board
{

    // Where the board is looked at from. World units are whatever the board
    // is laid out in (millimeters, here). The board lies in the xy plane and
    // z points up, away from its top side; the conventions are those of
    // <math/transform.hpp>.
    struct camera
    {
        math::vec3f eye{0.0f, -100.0f, 100.0f};
        math::vec3f target{};
        math::vec3f up{0.0f, 0.0f, 1.0f};

        // Vertical field of view in radians.
        float fov_y = 0.6f;

        // Distances in front of the eye between which things are visible.
        float z_near = 1.0f;
        float z_far = 1000.0f;

        // A parallel projection instead of a perspective one. The view is
        // then as tall as the perspective view is at the target's distance
        // (ortho_height()), so switching keeps what is at the target the
        // same size, and dollying in and out still zooms. Depth then runs
        // from z_far behind the eye to z_far in front of it, so that nothing
        // is cut by coming close.
        bool orthographic = false;

        float ortho_height() const noexcept;
    };

    // The matrix that takes world points to the clip space of an image
    // `aspect` (width / height) wide, y down as images are: what the
    // renderer draws with, and what picking undoes.
    math::mat4f view_projection(const camera &camera, float aspect) noexcept;

    // An object on the board, see the top of this file. 0 names none.
    using object_id = std::uint32_t;

    enum class object_kind : std::uint8_t
    {
        board,         // the board itself
        part_body,     // a part drawn as a box, having no model
        part_model,    // a part's 3D model
        dropped_model, // a model the user put on the board
    };

    inline constexpr std::size_t object_kind_count = 4;

    // "Board", "Part body", ...
    const char *kind_name(object_kind kind) noexcept;

    struct object
    {
        object_id id = 0;
        object_kind kind = object_kind::part_model;

        // What lists call it, and the file it came from ("" for none).
        std::string name;
        std::string file;

        // Where it is: its origin on the board and its turn about z, in
        // radians, as placed; and the box around it, world units.
        math::vec3f position{};
        float rotation = 0.0f;
        model::bounds bounds;

        // Triangles drawn for it; a box's 12.
        std::size_t triangles = 0;

        // The user's own switch; the kind's (scene::shown) applies as well.
        bool visible = true;
    };

    // The simplest shape the view draws: a box. The board itself is one,
    // and so is the body of a part that has no model.
    struct box
    {
        math::vec3f center;
        math::vec3f size;

        // Turn about the z axis in radians, counter-clockwise seen from
        // above: how a part is rotated on the board.
        float rotation = 0.0f;

        // Working space (<gpu/color.hpp>).
        math::vec3f color{1.0f, 1.0f, 1.0f};

        object_id object = 0;

        // What the object made of it is called (adopt_boxes()); empty
        // names it after its kind ("Part body").
        std::string name{};
    };

    // A box around `b`, axis aligned: its corners, turned.
    model::bounds bounds_of(const box &b) noexcept;

    // A vertex of a mesh as the GPU reads it: position and unit normal,
    // three floats each, 24 bytes. The same layout as model::vertex, which
    // is what meshes are made from (<app/board/model.hpp>).
    struct vertex
    {
        math::vec3f position;
        math::vec3f normal;
    };

    static_assert(sizeof(vertex) == 24);

    // A mesh the renderer holds on the GPU (renderer::create_mesh()). A
    // handle stays cheap to copy and safe to keep: once its mesh is released
    // it names nothing, and instances that use it are skipped, even if the
    // slot has been reused by a newer mesh.
    struct mesh_handle
    {
        std::uint32_t index = 0;
        std::uint32_t generation = 0; // 0: no mesh

        constexpr bool valid() const noexcept { return generation != 0; }
        friend constexpr bool operator==(const mesh_handle &, const mesh_handle &) = default;
    };

    // One placement of a mesh.
    // A mesh's triangles as picking tests them: positions and indices in
    // the mesh's coordinates, on the CPU, beside the renderer's copy (which
    // the CPU cannot read). Shared by every instance of the mesh. Made by
    // upload() (<app/board/model.hpp>) from what it uploads.
    struct pick_shape
    {
        std::vector<math::vec3f> positions;
        std::vector<std::uint32_t> indices; // three per triangle
    };

    struct mesh_instance
    {
        mesh_handle mesh;

        // From the mesh's coordinates to the world's. Rigid motions with a
        // uniform scale, possibly mirrored: what model files place parts
        // with. The renderer turns normals by this matrix as it is, which is
        // only right for those (a non-uniform scale would need its inverse
        // transpose).
        math::mat4f transform = math::mat4f::identity();

        // Working space (<gpu/color.hpp>), as model colours are.
        math::vec3f color{1.0f, 1.0f, 1.0f};

        object_id object = 0;

        // The mesh's triangles for picking; without them its object is
        // picked by its bounds.
        std::shared_ptr<const pick_shape> shape{};
    };

    // What to draw this frame. Whoever owns the board fills one; the
    // renderer only turns it into commands.
    struct scene
    {
        camera view;

        math::vec3f background{0.0f, 0.0f, 0.0f};

        // The direction the light comes from. Need not be normalized.
        math::vec3f light{0.3f, -0.5f, 1.0f};

        std::vector<box> boxes;
        std::vector<mesh_instance> meshes;

        // Boxes drawn in their colour as it is, unlit, over nothing in
        // particular: marks the interface puts in the view (the selection's
        // outline, a measurement). They belong to no object.
        std::vector<box> marks;

        // ---- objects -----------------------------------------------------

        // By id, ascending; ids are never used twice.
        std::vector<object> objects;
        object_id next_object = 1;

        // Each kind of object drawn or not, by object_kind.
        std::array<bool, object_kind_count> shown{true, true, true, true};

        // The selected object, drawn tinted towards selection_color.
        object_id selected = 0;
        math::vec3f selection_color{1.0f, 0.45f, 0.1f};

        // Adds `o` to the table under a new id, which it returns.
        object_id add_object(object o);

        object *find(object_id id) noexcept;
        const object *find(object_id id) const noexcept;

        // Whether what belongs to `id` is drawn: the object is visible and so
        // is its kind. What belongs to no object always is.
        bool drawn(object_id id) const noexcept;

        // How many objects of the kind there are.
        std::size_t count(object_kind kind) const noexcept;
    };

    // Gives every box that belongs to no object one of its own: the one
    // whose top is the board's top (z = 0) and that covers the most is the
    // board, the rest are part bodies. Boxes that have an object keep it.
    // Returns how many objects it made.
    std::size_t adopt_boxes(scene &scene);

    // Moves what belongs to `id` by `offset` in the board's plane: its boxes,
    // its instances, its place and its bounds. False if there is no such
    // object.
    bool move_object(scene &scene, object_id id, const math::vec2f &offset);

    // Takes the object out of the table, and its boxes and instances out of
    // the scene (the renderer's meshes are the caller's to release). Clears
    // the selection if it was selected.
    bool remove_object(scene &scene, object_id id);

    // The twelve edges of `bounds` as boxes `width` thick, in `color`, for
    // scene::marks: an outline.
    void outline(std::vector<box> &marks, const model::bounds &bounds, float width, const math::vec3f &color);

    // A bar from `a` to `b` in the plane z = `height`, `width` wide, for
    // scene::marks: a measurement.
    void bar(std::vector<box> &marks, const math::vec2f &a, const math::vec2f &b, float height, float width,
             const math::vec3f &color);

    // ---- picking ---------------------------------------------------------

    // A ray: from `origin` along `direction` (unit length).
    struct ray
    {
        math::vec3f origin;
        math::vec3f direction;
    };

    // The ray through the centre of `pixel` of a view `size` pixels big (y
    // down, from the top-left, as viewports count them), as the renderer
    // projects from `camera`. For a parallel projection it starts far
    // behind the eye, so that what is behind the eye is hit as well. Nothing
    // for a view without a size.
    std::optional<ray> view_ray(const camera &camera, const math::vec2f &pixel, const math::vec2f &size) noexcept;

    // How far along `r` it enters `bounds` (0 if it starts inside), or
    // nothing if it misses.
    std::optional<float> hit(const ray &r, const model::bounds &bounds) noexcept;

    // How far along `r` it meets the box as drawn, turned about z (0 if it
    // starts inside), or nothing.
    std::optional<float> hit(const ray &r, const box &b) noexcept;

    // How far along `r` it meets a triangle of the instance's shape, either
    // side, or nothing (also for an instance without a shape). The
    // instance's transform is taken to be what mesh_instance says it is: a
    // rigid motion with a uniform scale, possibly mirrored.
    std::optional<float> hit(const ray &r, const mesh_instance &instance) noexcept;

    // The drawn object `r` meets first; 0 for none. The objects' bounds
    // find the candidates; each is then tested as it is drawn: a box as the
    // turned box it is, a model by the triangles of its instances' shapes.
    // So a turned part, or the hole in a ring, is not picked from outside
    // its shape. An object with neither (a model uploaded without shapes)
    // is picked by its bounds.
    object_id pick(const scene &scene, const ray &r) noexcept;

    // Where the ray through `pixel` meets the board's top, the plane z = 0.
    // Nothing where the ray runs above the horizon or away from the board.
    std::optional<math::vec2f> point_on_board(const camera &camera, const math::vec2f &pixel,
                                              const math::vec2f &size) noexcept;

} // namespace volt::board
