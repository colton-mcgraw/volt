#include <model/measure.hpp>

#include <math/geometry.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace volt::model
{

    namespace
    {
        struct position_hash
        {
            std::size_t operator()(const math::vec3f &p) const noexcept { return std::hash<math::vec3f>{}(p); }
        };

        struct edge_use
        {
            std::uint32_t forward = 0;  // uses running from the smaller id to the larger
            std::uint32_t backward = 0; // and the other way
        };
    } // namespace

    mesh_measure measure(const mesh &mesh)
    {
        mesh_measure result;

        // Weld by exact position.
        std::unordered_map<math::vec3f, std::uint32_t, position_hash> ids;
        std::vector<std::uint32_t> welded(mesh.vertices.size());
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
            welded[i] = ids.try_emplace(mesh.vertices[i].position, static_cast<std::uint32_t>(ids.size())).first->second;

        std::unordered_map<std::uint64_t, edge_use> edges;
        edges.reserve(mesh.indices.size());

        const std::size_t triangles = mesh.indices.size() / 3;
        for (std::size_t t = 0; t < triangles; ++t)
        {
            const std::uint32_t corner[3] = {mesh.indices[3 * t], mesh.indices[3 * t + 1], mesh.indices[3 * t + 2]};
            if (corner[0] >= mesh.vertices.size() || corner[1] >= mesh.vertices.size() ||
                corner[2] >= mesh.vertices.size())
                continue;
            ++result.triangles;

            const math::vec3d a = mesh.vertices[corner[0]].position.cast<double>();
            const math::vec3d b = mesh.vertices[corner[1]].position.cast<double>();
            const math::vec3d c = mesh.vertices[corner[2]].position.cast<double>();
            result.area += 0.5 * math::magnitude(math::cross(b - a, c - a));
            result.volume += math::dot(a, math::cross(b, c)) / 6.0;

            const std::uint32_t w[3] = {welded[corner[0]], welded[corner[1]], welded[corner[2]]};
            if (w[0] == w[1] || w[1] == w[2] || w[2] == w[0])
            {
                ++result.degenerate_triangles;
                continue;
            }
            for (std::size_t k = 0; k < 3; ++k)
            {
                const std::uint32_t from = w[k];
                const std::uint32_t to = w[(k + 1) % 3];
                const std::uint64_t key = (static_cast<std::uint64_t>(std::min(from, to)) << 32) | std::max(from, to);
                edge_use &use = edges[key];
                if (from < to)
                    ++use.forward;
                else
                    ++use.backward;
            }
        }

        result.edges = edges.size();
        for (const auto &[key, use] : edges)
        {
            const std::uint32_t total = use.forward + use.backward;
            if (total == 1)
                ++result.boundary_edges;
            else if (total > 2)
                ++result.non_manifold_edges;
            else if (use.forward != 1)
                ++result.inconsistent_edges;
        }
        return result;
    }

    double worst_normal_deviation(const mesh &mesh)
    {
        double worst = 0.0;
        const std::size_t triangles = mesh.indices.size() / 3;
        for (std::size_t t = 0; t < triangles; ++t)
        {
            const vertex &a = mesh.vertices[mesh.indices[3 * t]];
            const vertex &b = mesh.vertices[mesh.indices[3 * t + 1]];
            const vertex &c = mesh.vertices[mesh.indices[3 * t + 2]];
            const math::vec3d n =
                math::cross((b.position - a.position).cast<double>(), (c.position - a.position).cast<double>());
            const double length = math::magnitude(n);
            // Slivers have no reliable normal of their own.
            if (length < 1e-12)
                continue;
            for (const vertex *v : {&a, &b, &c})
            {
                const double cosine = math::dot(n / length, v->normal.cast<double>()) /
                                      std::max(1e-30, math::magnitude(v->normal.cast<double>()));
                worst = std::max(worst, std::acos(std::clamp(cosine, -1.0, 1.0)));
            }
        }
        return worst;
    }

} // namespace volt::model
