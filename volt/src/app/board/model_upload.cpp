#include <app/board/model.hpp>

#include <app/board/renderer.hpp>

namespace volt::board
{

    std::expected<model_meshes, std::string> upload(renderer &renderer, const prepared_model &prepared)
    {
        model_meshes meshes;
        meshes.name = prepared.name;
        meshes.instances = prepared.instances;
        meshes.bounds = prepared.bounds;
        meshes.triangles = prepared.triangles;

        for (const model_part &part : prepared.parts)
        {
            auto made = renderer.create_mesh(part.vertices, part.indices);
            if (!made)
            {
                release(renderer, meshes);
                return std::unexpected(std::move(made).error());
            }
            meshes.parts.push_back(*made);
            meshes.colors.push_back(part.color);

            // The CPU's copy, for picking: positions and indices only.
            auto shape = std::make_shared<pick_shape>();
            shape->positions.reserve(part.vertices.size());
            for (const vertex &v : part.vertices)
                shape->positions.push_back(v.position);
            shape->indices = part.indices;
            meshes.shapes.push_back(std::move(shape));
        }
        return meshes;
    }

    void release(renderer &renderer, model_meshes &meshes)
    {
        for (const mesh_handle mesh : meshes.parts)
            renderer.release_mesh(mesh);
        meshes = {};
    }

} // namespace volt::board
