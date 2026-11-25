#include <app/board/model.hpp>

#include <math/geometry.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_set>

namespace volt::board
{
    namespace
    {
        constexpr std::uint32_t unmapped = std::numeric_limits<std::uint32_t>::max();

        // The triangles of `mesh` whose first vertex has `color` (all of
        // them without one), as a part with only the vertices they use.
        model_part part_of(const model::mesh &mesh, const math::vec3f *color, std::vector<std::uint32_t> &remap)
        {
            model_part part;
            part.color = color != nullptr ? *color : mesh.material.color;

            std::fill(remap.begin(), remap.end(), unmapped);
            const std::size_t count = mesh.vertices.size();
            for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
            {
                const std::uint32_t a = mesh.indices[t];
                const std::uint32_t b = mesh.indices[t + 1];
                const std::uint32_t c = mesh.indices[t + 2];
                if (a >= count || b >= count || c >= count)
                    continue;
                if (color != nullptr && !(mesh.colors[a] == *color))
                    continue;

                for (const std::uint32_t index : {a, b, c})
                {
                    if (remap[index] == unmapped)
                    {
                        remap[index] = static_cast<std::uint32_t>(part.vertices.size());
                        const model::vertex &v = mesh.vertices[index];
                        part.vertices.push_back({v.position, v.normal});
                    }
                    part.indices.push_back(remap[index]);
                }
            }
            return part;
        }
    } // namespace

    prepared_model prepare(const model::model &model)
    {
        prepared_model prepared;
        prepared.name = model.name;

        // Model mesh -> the parts it became.
        std::vector<std::vector<std::uint32_t>> parts_of_mesh(model.meshes.size());
        std::vector<std::uint32_t> remap;

        for (std::size_t m = 0; m < model.meshes.size(); ++m)
        {
            const model::mesh &mesh = model.meshes[m];
            remap.assign(mesh.vertices.size(), unmapped);

            // The colours its triangles come in, in the order met.
            std::vector<math::vec3f> colors;
            const bool coloured = mesh.colors.size() == mesh.vertices.size() && !mesh.colors.empty();
            if (coloured)
            {
                for (std::size_t t = 0; t < mesh.indices.size(); t += 3)
                {
                    const std::uint32_t a = mesh.indices[t];
                    if (a < mesh.colors.size() && std::ranges::find(colors, mesh.colors[a]) == colors.end())
                        colors.push_back(mesh.colors[a]);
                }
            }

            const auto add = [&](const math::vec3f *color) {
                model_part part = part_of(mesh, color, remap);
                if (part.indices.empty())
                    return;
                parts_of_mesh[m].push_back(static_cast<std::uint32_t>(prepared.parts.size()));
                prepared.parts.push_back(std::move(part));
            };
            if (coloured)
            {
                for (const math::vec3f &color : colors)
                    add(&color);
            }
            else
            {
                add(nullptr);
            }
        }

        model::for_each_instance(model, [&](std::uint32_t mesh, const math::mat4f &transform) {
            for (const std::uint32_t part : parts_of_mesh[mesh])
            {
                prepared.instances.push_back({part, transform});
                prepared.triangles += prepared.parts[part].indices.size() / 3;
                for (const vertex &v : prepared.parts[part].vertices)
                {
                    const math::vec4f p = transform * math::vec4f{v.position[0], v.position[1], v.position[2], 1.0f};
                    prepared.bounds.add(math::vec3f{p[0], p[1], p[2]});
                }
            }
        });
        return prepared;
    }

    object_id add_model(scene &scene, const model_meshes &meshes, const math::mat4f &placement, object_kind kind,
                        std::string file)
    {
        object o;
        o.kind = kind;
        o.name = meshes.name.empty() ? std::string("Model") : meshes.name;
        o.file = std::move(file);
        o.position = {placement(0, 3), placement(1, 3), placement(2, 3)};
        o.rotation = std::atan2(placement(1, 0), placement(0, 0));
        o.bounds = placed_bounds(meshes.bounds, placement);
        o.triangles = meshes.triangles;
        const object_id id = scene.add_object(std::move(o));

        for (const model_instance &instance : meshes.instances)
        {
            if (instance.part >= meshes.parts.size())
                continue;
            scene.meshes.push_back({meshes.parts[instance.part], placement * instance.transform,
                                    meshes.colors[instance.part], id,
                                    instance.part < meshes.shapes.size() ? meshes.shapes[instance.part] : nullptr});
        }
        return id;
    }

    void remove_model(scene &scene, const model_meshes &meshes)
    {
        // The objects the model's instances belong to go with them.
        std::vector<object_id> owners;
        std::erase_if(scene.meshes, [&](const mesh_instance &instance) {
            if (std::ranges::find(meshes.parts, instance.mesh) == meshes.parts.end())
                return false;
            if (instance.object != 0 && std::ranges::find(owners, instance.object) == owners.end())
                owners.push_back(instance.object);
            return true;
        });
        for (const object_id id : owners)
            remove_object(scene, id);
    }

    model::bounds placed_bounds(const model::bounds &bounds, const math::mat4f &placement)
    {
        model::bounds placed;
        if (bounds.empty())
            return placed;
        for (int corner = 0; corner < 8; ++corner)
        {
            const math::vec4f p{(corner & 1) != 0 ? bounds.max[0] : bounds.min[0],
                                (corner & 2) != 0 ? bounds.max[1] : bounds.min[1],
                                (corner & 4) != 0 ? bounds.max[2] : bounds.min[2], 1.0f};
            const math::vec4f q = placement * p;
            placed.add(math::vec3f{q[0], q[1], q[2]});
        }
        return placed;
    }

} // namespace volt::board
