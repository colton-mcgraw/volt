#include <model/model.hpp>

#include <math/geometry.hpp>
#include <math/linear_algebra.hpp>
#include <math/transform.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace volt::model
{

    // The same curve as <gpu/color.hpp>, which this library does not depend
    // on: model data stays free of the renderer.
    float srgb_to_linear(float encoded) noexcept
    {
        return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
    }

    float linear_to_srgb(float linear) noexcept
    {
        return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    }

    math::vec3f srgb_to_linear(const math::vec3f &encoded) noexcept
    {
        return {srgb_to_linear(encoded[0]), srgb_to_linear(encoded[1]), srgb_to_linear(encoded[2])};
    }

    math::vec3f linear_to_srgb(const math::vec3f &linear) noexcept
    {
        return {linear_to_srgb(linear[0]), linear_to_srgb(linear[1]), linear_to_srgb(linear[2])};
    }

    // ---------------------------------------------------------------------
    // bounds
    // ---------------------------------------------------------------------

    void bounds::add(const math::vec3f &point) noexcept
    {
        for (std::size_t i = 0; i < 3; ++i)
        {
            min[i] = std::min(min[i], point[i]);
            max[i] = std::max(max[i], point[i]);
        }
    }

    void bounds::add(const bounds &other) noexcept
    {
        if (other.empty())
            return;
        add(other.min);
        add(other.max);
    }

    math::vec3f bounds::size() const noexcept
    {
        return empty() ? math::vec3f{} : max - min;
    }

    math::vec3f bounds::center() const noexcept
    {
        return empty() ? math::vec3f{} : (min + max) * 0.5f;
    }

    std::size_t model::triangle_count() const noexcept
    {
        std::size_t total = 0;
        for_each_instance(*this, [&](std::uint32_t mesh, const math::mat4f &) { total += meshes[mesh].triangle_count(); });
        return total;
    }

    bounds bounds_of(const mesh &mesh, const math::mat4f &transform)
    {
        bounds result;
        for (const vertex &vertex : mesh.vertices)
            result.add(math::transform_point(transform, vertex.position));
        return result;
    }

    bounds bounds_of(const model &model)
    {
        bounds result;
        for_each_instance(model, [&](std::uint32_t mesh, const math::mat4f &transform) {
            result.add(bounds_of(model.meshes[mesh], transform));
        });
        return result;
    }

    // ---------------------------------------------------------------------
    // flatten
    // ---------------------------------------------------------------------

    void transform_mesh(mesh &mesh, const math::mat4d &transform)
    {
        const math::mat3d linear = math::linear_part(transform);
        const auto inverse = math::try_inverse(linear);
        const math::mat3d normal_matrix = inverse ? math::transpose(*inverse) : linear;
        const bool mirrored = math::determinant(linear) < 0.0;

        for (vertex &vertex : mesh.vertices)
        {
            const math::vec3d position = math::transform_point(transform, vertex.position.cast<double>());
            math::vec3d normal = normal_matrix * vertex.normal.cast<double>();
            const double length = math::magnitude(normal);
            if (length > 0.0)
                normal = normal / length;
            vertex.position = position.cast<float>();
            vertex.normal = normal.cast<float>();
        }

        if (mirrored)
        {
            for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
                std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
        }
    }

    model flatten(const model &source)
    {
        model result;
        result.name = source.name;
        node root;
        root.name = source.name;

        for_each_instance(source, [&](std::uint32_t index, const math::mat4f &transform) {
            mesh copy = source.meshes[index];
            if (transform != math::mat4f::identity())
                transform_mesh(copy, transform.cast<double>());
            root.meshes.push_back(static_cast<std::uint32_t>(result.meshes.size()));
            result.meshes.push_back(std::move(copy));
        });

        result.nodes.push_back(std::move(root));
        return result;
    }

    // ---------------------------------------------------------------------
    // normals
    // ---------------------------------------------------------------------

    namespace
    {
        struct position_hash
        {
            std::size_t operator()(const math::vec3f &p) const noexcept { return std::hash<math::vec3f>{}(p); }
        };
    } // namespace

    void compute_normals(mesh &mesh, float crease_angle)
    {
        const std::size_t triangles = mesh.triangle_count();
        const bool has_colors = mesh.colors.size() == mesh.vertices.size();

        // Face normals, area weighted (the cross product's length is twice
        // the area), and the corners that share each position.
        std::vector<math::vec3d> face_normals(triangles);
        std::unordered_map<math::vec3f, std::vector<std::uint32_t>, position_hash> corners;
        for (std::size_t t = 0; t < triangles; ++t)
        {
            const math::vec3d a = mesh.vertices[mesh.indices[3 * t]].position.cast<double>();
            const math::vec3d b = mesh.vertices[mesh.indices[3 * t + 1]].position.cast<double>();
            const math::vec3d c = mesh.vertices[mesh.indices[3 * t + 2]].position.cast<double>();
            face_normals[t] = math::cross(b - a, c - a);
            for (std::size_t k = 0; k < 3; ++k)
                corners[mesh.vertices[mesh.indices[3 * t + k]].position].push_back(static_cast<std::uint32_t>(3 * t + k));
        }

        const double cos_crease = std::cos(static_cast<double>(crease_angle));
        const auto unit = [](const math::vec3d &v) {
            const double length = math::magnitude(v);
            return length > 0.0 ? math::vec3d(v / length) : math::vec3d{};
        };

        std::vector<vertex> vertices;
        std::vector<math::vec3f> colors;
        std::vector<std::uint32_t> indices(mesh.indices.size());
        vertices.reserve(mesh.indices.size());

        // Corners at one position get the same vertex when their averaged
        // normals come out the same, which they do for a smooth region.
        for (auto &[position, list] : corners)
        {
            std::unordered_map<math::vec3f, std::uint32_t, position_hash> made;
            for (std::uint32_t corner : list)
            {
                const std::size_t t = corner / 3;
                const math::vec3d own = unit(face_normals[t]);
                math::vec3d sum{};
                for (std::uint32_t other : list)
                {
                    const math::vec3d theirs = unit(face_normals[other / 3]);
                    if (math::dot(own, theirs) >= cos_crease)
                        sum += face_normals[other / 3];
                }
                math::vec3d normal = unit(sum);
                if (math::magnitude(normal) == 0.0)
                    normal = own;
                const math::vec3f key = normal.cast<float>();
                const std::uint32_t source = mesh.indices[corner];
                auto found = made.find(key);
                if (found == made.end() || (has_colors && colors[found->second] != mesh.colors[source]))
                {
                    const auto index = static_cast<std::uint32_t>(vertices.size());
                    vertices.push_back({position, key});
                    if (has_colors)
                        colors.push_back(mesh.colors[source]);
                    found = made.insert_or_assign(key, index).first;
                }
                indices[corner] = found->second;
            }
        }

        mesh.vertices = std::move(vertices);
        mesh.indices = std::move(indices);
        if (has_colors)
            mesh.colors = std::move(colors);
        else
            mesh.colors.clear();
    }

    bool is_valid(const model &model)
    {
        for (const mesh &mesh : model.meshes)
        {
            if (mesh.indices.size() % 3 != 0)
                return false;
            if (!mesh.colors.empty() && mesh.colors.size() != mesh.vertices.size())
                return false;
            for (std::uint32_t index : mesh.indices)
            {
                if (index >= mesh.vertices.size())
                    return false;
            }
        }
        for (const node &node : model.nodes)
        {
            for (std::uint32_t mesh : node.meshes)
            {
                if (mesh >= model.meshes.size())
                    return false;
            }
            for (std::uint32_t child : node.children)
            {
                if (child >= model.nodes.size() || child == 0)
                    return false;
            }
        }
        return true;
    }

} // namespace volt::model
