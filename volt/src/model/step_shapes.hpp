#pragma once

#include "curves.hpp"
#include "face_tessellator.hpp"
#include "step_access.hpp"
#include "surfaces.hpp"

#include <model/model.hpp>
#include <model/options.hpp>

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

// Private to volt_model: the solids and shells of a STEP file as meshes.
//
// Every geometric item a representation lists (a MANIFOLD_SOLID_BREP, a
// SHELL_BASED_SURFACE_MODEL, a TESSELLATED_SOLID, ...) becomes one mesh in
// millimeters. Edges are sampled once and shared by the two faces that use
// them, which is what makes the result watertight. A face that cannot be
// tessellated is left out and counted in the report.

namespace volt::model::step
{

    struct style
    {
        math::vec3f color = default_color; // working space
        float opacity = 1.0f;
    };

    class shape_builder
    {
    public:
        shape_builder(access &access, const load_options &options);

        // STYLED_ITEM and OVER_RIDING_STYLED_ITEM colors, by the item they
        // style. Called once, before any shape is built.
        void collect_styles();

        // Whether the instance is a geometric item this turns into a mesh.
        bool is_shape(const part21::entity &item) const;

        // The item as a mesh in millimeters, `units` being those of the
        // representation that lists it. Empty when nothing of it could be
        // tessellated.
        std::optional<mesh> build(const part21::entity &item, const units &units);

        // The style set on an item, if any.
        std::optional<style> style_of(const part21::entity &item) const;

    private:
        struct face_record
        {
            std::size_t first_vertex = 0;
            std::size_t vertex_count = 0;
            std::optional<style> style;
        };

        struct mesh_under_construction
        {
            mesh mesh;
            std::vector<face_record> faces;
            // Faces given as flat polygons, and faces with a surface.
            std::size_t polygons = 0;
            std::size_t surfaces = 0;
        };

        void add_shell(mesh_under_construction &out, const part21::entity &shell, bool flip, const units &units,
                       std::optional<style> inherited, int depth);
        void add_face(mesh_under_construction &out, const part21::entity &face, bool flip, const units &units,
                      std::optional<style> inherited, int depth);
        void add_tessellated(mesh_under_construction &out, const part21::entity &item, const units &units,
                             std::optional<style> inherited, int depth);
        void append(mesh_under_construction &out, const geometry::face_mesh &face, double scale,
                    std::optional<style> style);
        mesh finish(mesh_under_construction &&built, std::optional<style> item_style) const;

        const std::vector<math::vec3d> *edge_points(const part21::entity &edge, const units &units);
        std::shared_ptr<const geometry::surface> surface_of(const part21::entity &surface, const units &units,
                                                            std::string &why);
        std::optional<std::vector<math::vec3d>> coordinates(const part21::entity &list);

        void face_failed(const part21::entity &face, std::string_view why);

        access &m_access;
        load_options m_options;
        load_report &m_report;

        std::unordered_map<std::uint32_t, style> m_styles;
        std::unordered_map<std::uint32_t, std::vector<math::vec3d>> m_edges;
        std::unordered_map<std::uint32_t, std::pair<std::shared_ptr<const geometry::surface>, std::string>> m_surfaces;
        std::unordered_map<std::uint32_t, std::vector<math::vec3d>> m_coordinates;
    };

} // namespace volt::model::step
