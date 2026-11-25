#pragma once

#include <app/board/scene.hpp>
#include <model/model.hpp>

#include <math/matrix.hpp>
#include <math/vector.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// models on the board
// -------------------------------------------------------------------------
//
// What connects volt_model to the board view: a loaded model::model becomes
// meshes the renderer holds and instances in the scene. Kept apart from the
// renderer, whose header knows nothing of model files.
//
// Three steps, because they belong on different threads:
//
//   1. prepare(), on any thread (the one that loaded the file): the model's
//      meshes as the renderer takes them, and its instance tree walked into
//      a flat list. This is where the time goes for a large model, so it
//      runs off the render thread with the loading.
//   2. upload(), on the render thread: one renderer mesh per part.
//   3. add_model(), wherever the scene is changed: the instances, under a
//      placement on the board.
//
// A mesh whose vertices carry colours of their own (a part body with a
// coloured pin 1 marker, model::mesh::colors) is split into one part per
// colour: vertices are never shared between faces (model::vertex), so
// every triangle has one colour, and the renderer gets one colour per
// instance and a single pipeline.

namespace volt::board
{

    class renderer;

    // A mesh as the renderer takes it, in one colour.
    struct model_part
    {
        std::vector<vertex> vertices;
        std::vector<std::uint32_t> indices;
        math::vec3f color{1.0f, 1.0f, 1.0f}; // working space
    };

    // One placement of a part, in the model's coordinates.
    struct model_instance
    {
        std::uint32_t part = 0;
        math::mat4f transform = math::mat4f::identity();
    };

    struct prepared_model
    {
        std::string name;
        std::vector<model_part> parts;
        std::vector<model_instance> instances;

        // Around every placed triangle, model coordinates (millimeters).
        model::bounds bounds;

        // Triangles drawn when the whole model is placed once.
        std::size_t triangles = 0;
    };

    // Any thread. Triangles whose indices point past their mesh's vertices
    // are dropped, empty meshes too.
    prepared_model prepare(const model::model &model);

    // A prepared model on the GPU: the renderer's meshes for its parts, and
    // what add_model() needs to place them.
    struct model_meshes
    {
        std::string name;
        std::vector<mesh_handle> parts;
        std::vector<math::vec3f> colors;

        // Each part's triangles for picking (scene::meshes' shape).
        std::vector<std::shared_ptr<const pick_shape>> shapes;
        std::vector<model_instance> instances;
        model::bounds bounds;
        std::size_t triangles = 0;
    };

    // Render thread (see renderer::create_mesh()). On failure the meshes
    // made so far are released again.
    std::expected<model_meshes, std::string> upload(renderer &renderer, const prepared_model &prepared);

    // Releases the meshes (renderer::release_mesh()), and empties `meshes`.
    // Instances in a scene that still name them are skipped from then on;
    // remove_model() takes them out.
    void release(renderer &renderer, model_meshes &meshes);

    // The scene-level convenience: every placed part of the model, its
    // instance tree under `placement` (model coordinates to the board's), as
    // one new object of the scene (<app/board/scene.hpp>) of kind `kind`,
    // named after the model, from `file`. Returns the object.
    object_id add_model(scene &scene, const model_meshes &meshes, const math::mat4f &placement,
                        object_kind kind = object_kind::part_model, std::string file = {});

    // Takes out of the scene every instance of the model's meshes, and the
    // objects they made.
    void remove_model(scene &scene, const model_meshes &meshes);

    // The box around the model under `placement`: its own box's corners,
    // transformed, so a box around the model, if not always the tightest.
    model::bounds placed_bounds(const model::bounds &bounds, const math::mat4f &placement);

} // namespace volt::board
