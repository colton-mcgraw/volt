#include "step_shapes.hpp"

#include "face_tessellator.hpp"

#include <math/geometry.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace volt::model::step
{

    namespace
    {
        // References the style search follows at most from a styled item.
        constexpr int max_style_depth = 16;

        // Solids, shells and faces nest no deeper than this in any sane file;
        // the limit keeps a cyclic one from recursing forever.
        constexpr int max_shape_depth = 32;

        std::optional<math::vec3f> predefined_colour(std::string_view name)
        {
            struct entry
            {
                std::string_view name;
                math::vec3f color;
            };
            static constexpr entry colours[] = {
                {"black", {0.0f, 0.0f, 0.0f}},   {"red", {1.0f, 0.0f, 0.0f}},     {"green", {0.0f, 1.0f, 0.0f}},
                {"blue", {0.0f, 0.0f, 1.0f}},    {"yellow", {1.0f, 1.0f, 0.0f}}, {"magenta", {1.0f, 0.0f, 1.0f}},
                {"cyan", {0.0f, 1.0f, 1.0f}},    {"white", {1.0f, 1.0f, 1.0f}},
            };
            for (const entry &e : colours)
            {
                if (e.name.size() != name.size())
                    continue;
                bool same = true;
                for (std::size_t i = 0; i < name.size() && same; ++i)
                {
                    const char c = name[i] >= 'A' && name[i] <= 'Z' ? static_cast<char>(name[i] - 'A' + 'a') : name[i];
                    same = c == e.name[i];
                }
                if (same)
                    return e.color;
            }
            return std::nullopt;
        }

        struct style_search
        {
            const access &access;
            std::optional<math::vec3f> color;
            std::optional<float> transparency;
            int visited = 0;

            void walk_parameter(const part21::parameter &p, int depth)
            {
                if (depth > max_style_depth || visited > 256 || color)
                    return;
                if (p.is(part21::kind::list))
                {
                    for (std::size_t i = 0; i < p.size(); ++i)
                        walk_parameter(p[i], depth);
                    return;
                }
                const part21::entity e = access.resolve(p);
                if (e.valid())
                    walk(e, depth + 1);
            }

            void walk(const part21::entity &e, int depth)
            {
                if (depth > max_style_depth || ++visited > 256)
                    return;

                // Colors of edges, points and text are not colors of faces.
                for (type t : {type::curve_style, type::point_style, type::text_style, type::surface_style_boundary,
                               type::surface_style_silhouette, type::surface_style_segmentation_curve,
                               type::surface_style_control_grid, type::surface_style_parameter_line})
                {
                    if (access.is(e, t))
                        return;
                }

                if (access.is(e, type::colour_rgb))
                {
                    const part21::parameter a = access.part(e, type::colour_rgb);
                    math::vec3f c{static_cast<float>(a[1].value().number()), static_cast<float>(a[2].value().number()),
                                  static_cast<float>(a[3].value().number())};
                    // Some exporters write 0..255.
                    if (c[0] > 1.0f || c[1] > 1.0f || c[2] > 1.0f)
                        c = c / 255.0f;
                    for (float &channel : c)
                        channel = std::clamp(channel, 0.0f, 1.0f);
                    color = c;
                    return;
                }
                if (access.is(e, type::draughting_pre_defined_colour))
                {
                    color = predefined_colour(access.part(e, type::draughting_pre_defined_colour)[0].text());
                    return;
                }
                if (access.is(e, type::surface_style_transparent))
                {
                    transparency = static_cast<float>(access.part(e, type::surface_style_transparent)[0].value().number());
                    return;
                }

                const std::size_t parts = e.part_count();
                for (std::size_t part = 0; part < parts; ++part)
                    walk_parameter(e.parameters(part), depth);
            }
        };

        std::optional<math::vec3d> unit_normal_of(const std::vector<math::vec3d> &points,
                                                  const std::vector<std::uint32_t> &triangle)
        {
            const math::vec3d n = math::cross(points[triangle[1]] - points[triangle[0]],
                                              points[triangle[2]] - points[triangle[0]]);
            const double length = math::magnitude(n);
            if (!(length > 0.0))
                return std::nullopt;
            return n / length;
        }
    } // namespace

    shape_builder::shape_builder(access &access, const load_options &options)
        : m_access(access), m_options(options), m_report(access.report())
    {
    }

    void shape_builder::collect_styles()
    {
        const part21::file &file = m_access.file();
        std::vector<char> overriding;
        for (std::size_t i = 0; i < file.size(); ++i)
        {
            const part21::entity e = file[i];
            const bool overrides =
                m_access.is(e, type::over_riding_styled_item) || m_access.is(e, type::context_dependent_over_riding_styled_item);

            if (!overrides && !m_access.is(e, type::styled_item))
                continue;
            // (name, styles, item[, over_ridden_style]); for complex
            // instances the STYLED_ITEM part carries all three.
            const attributes a = m_access.gather(e, {type::styled_item, type::over_riding_styled_item});
            const part21::entity item = m_access.resolve(a[2]);
            if (!item.valid())
                continue;

            style_search search{m_access, {}, {}, 0};
            search.walk_parameter(a[1], 0);
            if (!search.color)
                continue;

            style s;
            s.color = srgb_to_linear(*search.color);
            if (search.transparency)
                s.opacity = std::clamp(1.0f - *search.transparency, 0.0f, 1.0f);

            // A plain style does not replace an overriding one.
            const std::uint32_t key = item.index();
            if (overriding.size() <= key)
                overriding.resize(static_cast<std::size_t>(key) + 1, 0);
            if (!overrides && overriding[key] != 0)
                continue;
            m_styles[key] = s;
            if (overrides)
                overriding[key] = 1;
        }
    }

    std::optional<style> shape_builder::style_of(const part21::entity &item) const
    {
        if (!item.valid())
            return std::nullopt;
        const auto it = m_styles.find(item.index());
        if (it == m_styles.end())
            return std::nullopt;
        return it->second;
    }

    bool shape_builder::is_shape(const part21::entity &item) const
    {
        for (type t : {type::manifold_solid_brep, type::brep_with_voids, type::faceted_brep,
                       type::shell_based_surface_model, type::face_based_surface_model, type::tessellated_solid,
                       type::tessellated_shell, type::triangulated_surface_set, type::complex_triangulated_surface_set,
                       type::closed_shell, type::open_shell})
        {
            if (m_access.is(item, t))
                return true;
        }
        return false;
    }

    void shape_builder::face_failed(const part21::entity &face, std::string_view why)
    {
        ++m_report.faces_failed;
        m_report.warn(std::format("#{} {} (line {}): {}", face.id(), m_access.describe(face), face.line(), why));
    }

    // ---------------------------------------------------------------------
    // Building a mesh
    // ---------------------------------------------------------------------

    void shape_builder::append(mesh_under_construction &out, const geometry::face_mesh &face, double scale,
                               std::optional<style> style)
    {
        const std::size_t first = out.mesh.vertices.size();
        if (first + face.positions.size() > 0xffffffffu)
            return;
        for (std::size_t i = 0; i < face.positions.size(); ++i)
        {
            const math::vec3d p = face.positions[i] * scale;
            out.mesh.vertices.push_back({p.cast<float>(), face.normals[i].cast<float>()});
        }
        for (std::uint32_t index : face.indices)
            out.mesh.indices.push_back(static_cast<std::uint32_t>(first) + index);
        out.faces.push_back({first, face.positions.size(), style});
    }

    mesh shape_builder::finish(mesh_under_construction &&built, std::optional<style> item_style) const
    {
        mesh result = std::move(built.mesh);
        const style base = item_style.value_or(style{});

        // One color for the whole mesh when every face agrees; colors per
        // vertex when they do not.
        std::optional<style> common;
        bool uniform = true;
        for (const face_record &face : built.faces)
        {
            const style s = face.style.value_or(base);
            if (!common)
                common = s;
            else if (common->color != s.color || common->opacity != s.opacity)
                uniform = false;
        }
        const style chosen = common.value_or(base);
        result.material.color = chosen.color;
        result.material.opacity = chosen.opacity;
        if (!uniform)
        {
            result.colors.assign(result.vertices.size(), base.color);
            for (const face_record &face : built.faces)
            {
                const math::vec3f c = face.style.value_or(base).color;
                std::fill_n(result.colors.begin() + static_cast<std::ptrdiff_t>(face.first_vertex),
                            static_cast<std::ptrdiff_t>(face.vertex_count), c);
            }
        }
        return result;
    }

    std::optional<mesh> shape_builder::build(const part21::entity &item, const units &units)
    {
        if (!item.valid())
            return std::nullopt;

        mesh_under_construction built;
        const std::optional<style> item_style = style_of(item);
        built.mesh.name = std::string(item.parameters(0)[0].text());

        if (m_access.is(item, type::manifold_solid_brep) || m_access.is(item, type::faceted_brep) ||
            m_access.is(item, type::brep_with_voids))
        {
            // (name, outer[, voids])
            const attributes a = m_access.gather(item, {type::manifold_solid_brep, type::brep_with_voids});
            add_shell(built, m_access.resolve(a[1]), false, units, item_style, 0);
            const part21::parameter voids = a[2];
            for (std::size_t i = 0; i < voids.size(); ++i)
            {
                const part21::entity shell = m_access.resolve(voids[i]);
                // A void is an oriented shell turned inside out; a plain
                // closed shell listed as a void is turned here.
                add_shell(built, shell, !m_access.is(shell, type::oriented_closed_shell), units, item_style, 0);
            }
        }
        else if (m_access.is(item, type::shell_based_surface_model) || m_access.is(item, type::face_based_surface_model))
        {
            const type t = m_access.is(item, type::shell_based_surface_model) ? type::shell_based_surface_model
                                                                               : type::face_based_surface_model;
            const part21::parameter shells = m_access.part(item, t)[1];
            for (std::size_t i = 0; i < shells.size(); ++i)
                add_shell(built, m_access.resolve(shells[i]), false, units, item_style, 0);
        }
        else if (m_access.is(item, type::closed_shell) || m_access.is(item, type::open_shell))
            add_shell(built, item, false, units, item_style, 0);
        else if (m_access.is(item, type::tessellated_solid) || m_access.is(item, type::tessellated_shell) ||
                 m_access.is(item, type::triangulated_surface_set) ||
                 m_access.is(item, type::complex_triangulated_surface_set))
            add_tessellated(built, item, units, item_style, 0);
        else
            return std::nullopt;

        if (built.mesh.indices.empty())
            return std::nullopt;
        ++m_report.solids;
        const bool faceted = built.polygons > 0 && built.surfaces == 0;
        mesh result = finish(std::move(built), item_style);

        // A solid made only of flat polygons is either faceted by nature
        // (a box) or a curved part someone tessellated (most faceted files
        // are): smooth across shallow creases, sharp across real edges.
        if (faceted)
            compute_normals(result);
        return result;
    }

    void shape_builder::add_shell(mesh_under_construction &out, const part21::entity &shell, bool flip,
                                  const units &units, std::optional<style> inherited, int depth)
    {
        if (!shell.valid() || depth > max_shape_depth)
            return;
        if (const auto own = style_of(shell))
            inherited = own;

        if (m_access.is(shell, type::oriented_closed_shell) || m_access.is(shell, type::oriented_open_shell))
        {
            const type t = m_access.is(shell, type::oriented_closed_shell) ? type::oriented_closed_shell
                                                                            : type::oriented_open_shell;
            // (name, *, shell, orientation)
            const part21::parameter a = m_access.part(shell, t);
            const bool orientation = a[3].logical(true);
            add_shell(out, m_access.resolve(a[2]), orientation ? flip : !flip, units, inherited, depth + 1);
            return;
        }

        if (m_access.is(shell, type::closed_shell) || m_access.is(shell, type::open_shell) ||
            m_access.is(shell, type::connected_face_set))
        {
            const part21::parameter faces = shell.parameters(0)[1];
            for (std::size_t i = 0; i < faces.size(); ++i)
                add_face(out, m_access.resolve(faces[i]), flip, units, inherited, depth + 1);
            return;
        }

        m_report.unsupported_entity(m_access.describe(shell));
    }

    const std::vector<math::vec3d> *shape_builder::edge_points(const part21::entity &edge, const units &units)
    {
        if (const auto it = m_edges.find(edge.index()); it != m_edges.end())
            return &it->second;

        if (!m_access.is(edge, type::edge_curve))
        {
            m_report.unsupported_entity(m_access.describe(edge));
            return nullptr;
        }
        // (name, edge_start, edge_end, edge_geometry, same_sense)
        const part21::parameter a = m_access.part(edge, type::edge_curve);
        const part21::entity start_vertex = m_access.resolve(a[1]);
        const part21::entity end_vertex = m_access.resolve(a[2]);
        const auto start = m_access.point(m_access.part(start_vertex, type::vertex_point)[1]);
        const auto end = m_access.point(m_access.part(end_vertex, type::vertex_point)[1]);
        if (!start || !end)
            return nullptr;

        std::vector<math::vec3d> points{*start};
        const geometry::curve_result curve = geometry::make_curve(m_access, m_access.resolve(a[3]));
        if (!curve.curve)
        {
            // Straight across: the faces stay closed, if not exactly round.
            m_report.unsupported_entity(curve.unsupported);
        }
        else
        {
            const geometry::curve &c = *curve.curve;
            const bool same_sense = a[4].logical(true);
            const bool closed = start_vertex.index() == end_vertex.index();
            double t0 = c.project(*start);
            double t1 = c.project(*end);
            if (c.periodic)
            {
                const double period = c.period;
                const double eps = 1e-9 * period;
                if (closed)
                    t1 = same_sense ? t0 + period : t0 - period;
                else if (same_sense)
                {
                    while (t1 <= t0 + eps)
                        t1 += period;
                    while (t1 > t0 + period + eps)
                        t1 -= period;
                }
                else
                {
                    while (t1 >= t0 - eps)
                        t1 -= period;
                    while (t1 < t0 - period - eps)
                        t1 += period;
                }
            }
            else if (closed)
            {
                t0 = same_sense ? c.start : c.end;
                t1 = same_sense ? c.end : c.start;
            }

            geometry::sampling sampling;
            sampling.chord = m_options.tessellation.chord_tolerance / units.length;
            sampling.angle = m_options.tessellation.angle_tolerance;
            sampling.max_points = std::min<std::size_t>(m_options.tessellation.max_points_per_face, 100000);
            std::vector<double> parameters;
            if (std::isfinite(t0) && std::isfinite(t1))
                c.sample(t0, t1, sampling, parameters);
            for (double t : parameters)
                points.push_back(c.point(t));
        }
        points.push_back(*end);
        return &m_edges.emplace(edge.index(), std::move(points)).first->second;
    }

    std::shared_ptr<const geometry::surface> shape_builder::surface_of(const part21::entity &surface,
                                                                        const units &units, std::string &why)
    {
        if (const auto it = m_surfaces.find(surface.index()); it != m_surfaces.end())
        {
            why = it->second.second;
            return it->second.first;
        }
        geometry::surface_result made = geometry::make_surface(m_access, surface, units.angle);
        if (!made.surface)
            m_report.unsupported_entity(made.unsupported);
        why = made.unsupported;
        m_surfaces.emplace(surface.index(), std::make_pair(made.surface, made.unsupported));
        return made.surface;
    }

    void shape_builder::add_face(mesh_under_construction &out, const part21::entity &face, bool flip,
                                 const units &units, std::optional<style> inherited, int depth)
    {
        if (!face.valid() || depth > max_shape_depth)
            return;
        if (const auto own = style_of(face))
            inherited = own;

        if (m_access.is(face, type::oriented_face))
        {
            // (name, bounds*, face, orientation)
            const part21::parameter a = m_access.part(face, type::oriented_face);
            add_face(out, m_access.resolve(a[2]), a[3].logical(true) ? flip : !flip, units, inherited, depth + 1);
            return;
        }

        const bool advanced = m_access.is(face, type::advanced_face) || m_access.is(face, type::face_surface);
        if (!advanced && !m_access.is(face, type::face))
        {
            m_report.unsupported_entity(m_access.describe(face));
            ++m_report.faces_failed;
            return;
        }
        ++m_report.faces;

        // (name, bounds[, face_geometry, same_sense])
        const part21::parameter a = face.parameters(0);
        const part21::parameter bounds = a[1];
        bool same_sense = advanced ? a[3].logical(true) : true;
        if (flip)
            same_sense = !same_sense;

        std::vector<std::vector<math::vec3d>> loops;
        bool polygonal = false;
        for (std::size_t b = 0; b < bounds.size(); ++b)
        {
            const part21::entity bound = m_access.resolve(bounds[b]);
            if (!m_access.is(bound, type::face_bound) && !m_access.is(bound, type::face_outer_bound))
            {
                m_report.unsupported_entity(m_access.describe(bound));
                continue;
            }
            // (name, bound, orientation)
            const part21::parameter ba = bound.parameters(0);
            const part21::entity loop = m_access.resolve(ba[1]);
            const bool orientation = ba[2].logical(true);

            std::vector<math::vec3d> points;
            if (m_access.is(loop, type::edge_loop))
            {
                const part21::parameter edges = m_access.part(loop, type::edge_loop)[1];
                bool broken = false;
                for (std::size_t e = 0; e < edges.size() && !broken; ++e)
                {
                    part21::entity oriented = m_access.resolve(edges[e]);
                    bool forward = true;
                    // An oriented edge may name another oriented edge.
                    for (int guard = 0; guard < 8 && m_access.is(oriented, type::oriented_edge); ++guard)
                    {
                        const part21::parameter oa = m_access.part(oriented, type::oriented_edge);
                        if (!oa[4].logical(true))
                            forward = !forward;
                        oriented = m_access.resolve(oa[3]);
                    }
                    const std::vector<math::vec3d> *sampled = oriented.valid() ? edge_points(oriented, units) : nullptr;
                    if (sampled == nullptr)
                    {
                        broken = true;
                        break;
                    }
                    const auto add = [&](const math::vec3d &p) {
                        if (points.empty() || points.back() != p)
                            points.push_back(p);
                    };
                    if (forward)
                        for (const math::vec3d &p : *sampled)
                            add(p);
                    else
                        for (auto it = sampled->rbegin(); it != sampled->rend(); ++it)
                            add(*it);
                }
                if (broken)
                {
                    face_failed(face, "an edge of its boundary could not be read");
                    return;
                }
            }
            else if (m_access.is(loop, type::poly_loop))
            {
                polygonal = true;
                const part21::parameter list = m_access.part(loop, type::poly_loop)[1];
                for (std::size_t i = 0; i < list.size(); ++i)
                {
                    const auto p = m_access.point(list[i]);
                    if (p && (points.empty() || points.back() != *p))
                        points.push_back(*p);
                }
            }
            else if (m_access.is(loop, type::vertex_loop))
            {
                // A single point: it bounds nothing that the rest of the
                // face does not already say.
                continue;
            }
            else
            {
                m_report.unsupported_entity(m_access.describe(loop));
                continue;
            }

            while (points.size() > 1 && points.back() == points.front())
                points.pop_back();
            if (!orientation)
                std::reverse(points.begin(), points.end());
            if (!points.empty())
                loops.push_back(std::move(points));
        }

        std::expected<geometry::face_mesh, std::string> result = std::unexpected(std::string("no boundary"));
        if (polygonal || !advanced)
            ++out.polygons;
        else
            ++out.surfaces;
        if (polygonal || !advanced)
        {
            // A flat polygon. Its plane, where the face names one, says which
            // way it faces; otherwise the outer loop's direction does.
            math::vec3d facing{};
            if (advanced)
            {
                const part21::entity geometry = m_access.resolve(a[2]);
                if (m_access.is(geometry, type::plane))
                {
                    if (const auto frame = m_access.placement(m_access.part(geometry, type::plane)[1]))
                        facing = same_sense ? frame->z : -frame->z;
                }
            }
            if (math::magnitude(facing) == 0.0 && flip)
            {
                for (auto &loop : loops)
                    std::reverse(loop.begin(), loop.end());
            }
            result = geometry::tessellate_polygon(loops, facing);
        }
        else
        {
            const part21::entity geometry = m_access.resolve(a[2]);
            std::string why;
            const std::shared_ptr<const geometry::surface> surface = surface_of(geometry, units, why);
            if (!surface)
            {
                face_failed(face, std::format("its surface cannot be used: {}", why));
                return;
            }
            geometry::face_settings settings;
            settings.chord = m_options.tessellation.chord_tolerance / units.length;
            settings.angle = m_options.tessellation.angle_tolerance;
            settings.max_points = m_options.tessellation.max_points_per_face;
            result = geometry::tessellate_face(*surface, loops, same_sense, settings);
        }

        if (!result)
        {
            face_failed(face, result.error());
            return;
        }
        if (!result->clean)
            m_report.warn(std::format("#{} (line {}): its boundary intersects itself; triangulated as well as possible",
                                      face.id(), face.line()));
        if (result->mismatch * units.length > 10.0 * m_options.tessellation.chord_tolerance)
            m_report.warn(std::format("#{} (line {}): its edges lie up to {:.3g} mm off its surface", face.id(),
                                      face.line(), result->mismatch * units.length));
        append(out, *result, units.length, inherited);
    }

    // ---------------------------------------------------------------------
    // Tessellated geometry (AP242)
    // ---------------------------------------------------------------------

    std::optional<std::vector<math::vec3d>> shape_builder::coordinates(const part21::entity &list)
    {
        if (const auto it = m_coordinates.find(list.index()); it != m_coordinates.end())
            return it->second;
        if (!m_access.is(list, type::coordinates_list))
            return std::nullopt;
        // (name, npoints, position_coords)
        const part21::parameter coords = m_access.part(list, type::coordinates_list)[2];
        std::vector<math::vec3d> points;
        points.reserve(coords.size());
        for (std::size_t i = 0; i < coords.size(); ++i)
        {
            const part21::parameter p = coords[i];
            points.push_back({p[0].value().number(), p[1].value().number(), p[2].value().number()});
        }
        return m_coordinates.emplace(list.index(), std::move(points)).first->second;
    }

    void shape_builder::add_tessellated(mesh_under_construction &out, const part21::entity &item, const units &units,
                                        std::optional<style> inherited, int depth)
    {
        if (!item.valid() || depth > max_shape_depth)
            return;
        if (const auto own = style_of(item))
            inherited = own;

        if (m_access.is(item, type::tessellated_solid) || m_access.is(item, type::tessellated_shell))
        {
            // (name, items, link)
            const part21::parameter items = item.parameters(0)[1];
            for (std::size_t i = 0; i < items.size(); ++i)
                add_tessellated(out, m_access.resolve(items[i]), units, inherited, depth + 1);
            return;
        }

        const bool face = m_access.is(item, type::triangulated_face) || m_access.is(item, type::complex_triangulated_face);
        const bool set =
            m_access.is(item, type::triangulated_surface_set) || m_access.is(item, type::complex_triangulated_surface_set);
        if (!face && !set)
        {
            m_report.unsupported_entity(m_access.describe(item));
            return;
        }
        ++m_report.faces;

        // Faces: (name, coordinates, pnmax, normals, geometric_link, pnindex,
        // ...); surface sets have no geometric_link.
        const part21::parameter a = item.parameters(0);
        const auto points = coordinates(m_access.resolve(a[1]));
        if (!points)
        {
            face_failed(item, "its COORDINATES_LIST is missing");
            return;
        }
        const part21::parameter normals = a[3];
        const std::size_t base = face ? 5 : 4;
        const part21::parameter pnindex = a[base];
        const bool complex = m_access.is(item, type::complex_triangulated_face) ||
                             m_access.is(item, type::complex_triangulated_surface_set);

        // Point k (1-based, through pnindex when there is one) to a
        // coordinate.
        const std::size_t count = pnindex.size() > 0 ? pnindex.size() : points->size();
        const auto coordinate = [&](std::int64_t k) -> std::optional<std::size_t> {
            if (k < 1 || static_cast<std::size_t>(k) > count)
                return std::nullopt;
            std::int64_t c = k;
            if (pnindex.size() > 0)
                c = pnindex[static_cast<std::size_t>(k - 1)].value().integer();
            if (c < 1 || static_cast<std::size_t>(c) > points->size())
                return std::nullopt;
            return static_cast<std::size_t>(c - 1);
        };

        std::vector<std::array<std::int64_t, 3>> triangles;
        const auto add_triangle = [&](std::int64_t i, std::int64_t j, std::int64_t k) {
            if (i != j && j != k && k != i)
                triangles.push_back({i, j, k});
        };
        if (!complex)
        {
            const part21::parameter list = a[base + 1];
            for (std::size_t t = 0; t < list.size(); ++t)
                add_triangle(list[t][0].integer(), list[t][1].integer(), list[t][2].integer());
        }
        else
        {
            const part21::parameter strips = a[base + 1];
            for (std::size_t s = 0; s < strips.size(); ++s)
            {
                const part21::parameter strip = strips[s];
                for (std::size_t k = 0; k + 2 < strip.size(); ++k)
                {
                    const std::int64_t p = strip[k].integer();
                    const std::int64_t q = strip[k + 1].integer();
                    const std::int64_t r = strip[k + 2].integer();
                    if (k % 2 == 0)
                        add_triangle(p, q, r);
                    else
                        add_triangle(q, p, r);
                }
            }
            const part21::parameter fans = a[base + 2];
            for (std::size_t f = 0; f < fans.size(); ++f)
            {
                const part21::parameter fan = fans[f];
                for (std::size_t k = 1; k + 1 < fan.size(); ++k)
                    add_triangle(fan[0].integer(), fan[k].integer(), fan[k + 1].integer());
            }
        }

        // Vertices for the points used; normals as given, or averaged over
        // the face where the file has none (a face is smooth).
        geometry::face_mesh mesh;
        std::vector<std::int64_t> vertex_of(count + 1, -1);
        for (const auto &t : triangles)
        {
            std::array<std::uint32_t, 3> corners{};
            bool valid = true;
            for (std::size_t c = 0; c < 3; ++c)
            {
                const auto index = coordinate(t[c]);
                if (!index)
                {
                    valid = false;
                    break;
                }
                std::int64_t &v = vertex_of[static_cast<std::size_t>(t[c])];
                if (v < 0)
                {
                    v = static_cast<std::int64_t>(mesh.positions.size());
                    mesh.positions.push_back((*points)[*index]);
                    math::vec3d n{};
                    if (normals.size() == 1)
                        n = {normals[0][0].value().number(), normals[0][1].value().number(), normals[0][2].value().number()};
                    else if (normals.size() >= count)
                    {
                        const part21::parameter given = normals[static_cast<std::size_t>(t[c] - 1)];
                        n = {given[0].value().number(), given[1].value().number(), given[2].value().number()};
                    }
                    mesh.normals.push_back(n);
                }
                corners[c] = static_cast<std::uint32_t>(v);
            }
            if (valid)
                mesh.indices.insert(mesh.indices.end(), corners.begin(), corners.end());
        }
        if (mesh.indices.empty())
        {
            face_failed(item, "no valid triangles");
            return;
        }

        const bool given_normals = normals.size() == 1 || normals.size() >= count;
        if (!given_normals)
        {
            std::vector<math::vec3d> sums(mesh.positions.size());
            for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
            {
                const std::vector<std::uint32_t> tri{mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2]};
                const math::vec3d n = math::cross(mesh.positions[tri[1]] - mesh.positions[tri[0]],
                                                  mesh.positions[tri[2]] - mesh.positions[tri[0]]);
                for (std::uint32_t v : tri)
                    sums[v] += n;
            }
            for (std::size_t v = 0; v < sums.size(); ++v)
            {
                const double length = math::magnitude(sums[v]);
                mesh.normals[v] = length > 0.0 ? math::vec3d(sums[v] / length) : math::vec3d{0.0, 0.0, 1.0};
            }
        }
        else
        {
            for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
            {
                // Normals that disagree with the winding: trust the normals,
                // they are what the exporter computed from the surface.
                const std::vector<std::uint32_t> tri{mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2]};
                const auto n = unit_normal_of(mesh.positions, tri);
                const math::vec3d given = mesh.normals[tri[0]] + mesh.normals[tri[1]] + mesh.normals[tri[2]];
                if (n && math::dot(*n, given) < 0.0)
                    std::swap(mesh.indices[t + 1], mesh.indices[t + 2]);
            }
            for (math::vec3d &n : mesh.normals)
            {
                const double length = math::magnitude(n);
                n = length > 0.0 ? math::vec3d(n / length) : math::vec3d{0.0, 0.0, 1.0};
            }
        }
        append(out, mesh, units.length, inherited);
    }

} // namespace volt::model::step
