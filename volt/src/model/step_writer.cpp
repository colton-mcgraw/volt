#include <model/measure.hpp>
#include <model/step.hpp>

#include <math/geometry.hpp>
#include <math/linear_algebra.hpp>
#include <math/transform.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <functional>
#include <cmath>
#include <map>
#include <unordered_map>

// A model as a new STEP file. Every mesh becomes a FACETED_BREP of one
// FACE_SURFACE per triangle (on a PLANE, bounded by a POLY_LOOP), or a
// SHELL_BASED_SURFACE_MODEL where the mesh is not closed; every node a
// product, placed in its parent by the CONTEXT_DEPENDENT_SHAPE_REPRESENTATION
// construction readers know from every CAD exporter.

namespace volt::model::step
{

    namespace
    {
        using id = std::uint64_t;

        // The shortest decimal that is the float, as a double: 0.1f writes
        // as 0.1 rather than 0.100000001490116.
        double tidy(float value)
        {
            char buffer[32];
            const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
            double result = static_cast<double>(value);
            if (error == std::errc{})
                std::from_chars(buffer, end, result);
            return result;
        }

        double tidy(double value)
        {
            // Rounding noise of float transforms (0.99999994) is noise.
            const double rounded = std::round(value * 1e9) / 1e9;
            return std::abs(rounded - value) < 1e-12 * std::max(1.0, std::abs(value)) ? rounded : value;
        }

        bool is_rigid(const math::mat4d &m)
        {
            const math::mat3d r = math::linear_part(m);
            const math::mat3d product = math::transpose(r) * r;
            for (std::size_t i = 0; i < 3; ++i)
                for (std::size_t j = 0; j < 3; ++j)
                    if (std::abs(product(i, j) - (i == j ? 1.0 : 0.0)) > 1e-5)
                        return false;
            return math::determinant(r) > 0.0 && std::abs(m(3, 0)) + std::abs(m(3, 1)) + std::abs(m(3, 2)) < 1e-9 &&
                   std::abs(m(3, 3) - 1.0) < 1e-9;
        }

        class step_writer
        {
        public:
            step_writer(const model &source, const write_options &options) : m_model(source), m_options(options) {}

            part21::file run()
            {
                header();
                m_application =
                    simple("APPLICATION_CONTEXT", [&] { m_b.string("core data for automotive mechanical design processes"); });
                simple("APPLICATION_PROTOCOL_DEFINITION", [&] {
                    m_b.string("international standard");
                    m_b.string("automotive_design");
                    m_b.integer(2000);
                    m_b.reference(m_application);
                });
                m_product_context = simple("PRODUCT_CONTEXT", [&] {
                    m_b.string("");
                    m_b.reference(m_application);
                    m_b.string("mechanical");
                });
                m_definition_context = simple("PRODUCT_DEFINITION_CONTEXT", [&] {
                    m_b.string("part definition");
                    m_b.reference(m_application);
                    m_b.string("design");
                });
                units();

                if (!m_model.nodes.empty())
                {
                    std::vector<char> visited(m_model.nodes.size(), 0);
                    product(0, math::mat4d::identity(), visited, 0);
                }

                if (!m_styled.empty())
                {
                    simple("MECHANICAL_DESIGN_GEOMETRIC_PRESENTATION_REPRESENTATION", [&] {
                        m_b.string("");
                        list(m_styled);
                        m_b.reference(m_context);
                    });
                }

                auto file = m_b.finish();
                return file ? std::move(*file) : part21::file{};
            }

        private:
            const model &m_model;
            write_options m_options;
            part21::builder m_b;
            id m_next = 1;

            id m_application = 0;
            id m_product_context = 0;
            id m_definition_context = 0;
            id m_context = 0;
            std::vector<id> m_styled;
            std::unordered_map<std::uint32_t, id> m_mesh_items; // mesh index -> item, for meshes not baked
            std::map<std::vector<std::uint32_t>, std::pair<id, id>> m_leaf_products; // meshes -> (definition, rep)

            template <typename Parameters>
            id simple(std::string_view type, Parameters &&parameters)
            {
                const id entity = m_next++;
                m_b.begin_instance(entity);
                m_b.begin_part(type);
                parameters();
                m_b.end_part();
                m_b.end_entity();
                return entity;
            }

            void list(const std::vector<id> &ids)
            {
                m_b.begin_list();
                for (id i : ids)
                    m_b.reference(i);
                m_b.end_list();
            }

            void header()
            {
                m_b.begin_header("FILE_DESCRIPTION");
                m_b.begin_list();
                m_b.string("Volt model");
                m_b.end_list();
                m_b.string("2;1");
                m_b.end_entity();

                m_b.begin_header("FILE_NAME");
                m_b.string(m_options.file_name.empty() ? m_model.name : m_options.file_name);
                m_b.string(m_options.timestamp.empty() ? "2000-01-01T00:00:00" : m_options.timestamp);
                m_b.begin_list();
                m_b.string(m_options.author);
                m_b.end_list();
                m_b.begin_list();
                m_b.string(m_options.organization);
                m_b.end_list();
                m_b.string("Volt");
                m_b.string(m_options.originating_system);
                m_b.string("");
                m_b.end_entity();

                m_b.begin_header("FILE_SCHEMA");
                m_b.begin_list();
                m_b.string("AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }");
                m_b.end_list();
                m_b.end_entity();
            }

            id complex(std::initializer_list<std::pair<std::string_view, std::function<void()>>> parts)
            {
                const id entity = m_next++;
                m_b.begin_instance(entity, 0, true);
                for (const auto &[type, parameters] : parts)
                {
                    m_b.begin_part(type);
                    parameters();
                    m_b.end_part();
                }
                m_b.end_entity();
                return entity;
            }

            void units()
            {
                const auto nothing = [] {};
                const id length = complex({{"LENGTH_UNIT", nothing},
                                           {"NAMED_UNIT", [&] { m_b.derived(); }},
                                           {"SI_UNIT", [&] {
                                                m_b.enumeration("MILLI");
                                                m_b.enumeration("METRE");
                                            }}});
                const id angle = complex({{"NAMED_UNIT", [&] { m_b.derived(); }},
                                          {"PLANE_ANGLE_UNIT", nothing},
                                          {"SI_UNIT", [&] {
                                               m_b.omitted();
                                               m_b.enumeration("RADIAN");
                                           }}});
                const id solid_angle = complex({{"NAMED_UNIT", [&] { m_b.derived(); }},
                                                {"SI_UNIT", [&] {
                                                     m_b.omitted();
                                                     m_b.enumeration("STERADIAN");
                                                 }},
                                                {"SOLID_ANGLE_UNIT", nothing}});
                const id uncertainty = simple("UNCERTAINTY_MEASURE_WITH_UNIT", [&] {
                    m_b.begin_typed("LENGTH_MEASURE");
                    m_b.real(1e-7);
                    m_b.end_typed();
                    m_b.reference(length);
                    m_b.string("distance_accuracy_value");
                    m_b.string("confusion accuracy");
                });
                m_context = complex({{"GEOMETRIC_REPRESENTATION_CONTEXT", [&] { m_b.integer(3); }},
                                     {"GLOBAL_UNCERTAINTY_ASSIGNED_CONTEXT", [&] { list({uncertainty}); }},
                                     {"GLOBAL_UNIT_ASSIGNED_CONTEXT", [&] { list({length, angle, solid_angle}); }},
                                     {"REPRESENTATION_CONTEXT", [&] {
                                          m_b.string("Context #1");
                                          m_b.string("3D Context with UNIT and UNCERTAINTY");
                                      }}});
            }

            id point(const math::vec3d &p)
            {
                return simple("CARTESIAN_POINT", [&] {
                    m_b.string("");
                    m_b.begin_list();
                    for (std::size_t i = 0; i < 3; ++i)
                        m_b.real(tidy(p[i]));
                    m_b.end_list();
                });
            }

            id direction(const math::vec3d &d)
            {
                return simple("DIRECTION", [&] {
                    m_b.string("");
                    m_b.begin_list();
                    for (std::size_t i = 0; i < 3; ++i)
                        m_b.real(tidy(d[i]));
                    m_b.end_list();
                });
            }

            id placement(const math::mat4d &m)
            {
                const id origin = point({m(0, 3), m(1, 3), m(2, 3)});
                const id axis = direction({m(0, 2), m(1, 2), m(2, 2)});
                const id reference = direction({m(0, 0), m(1, 0), m(2, 0)});
                return simple("AXIS2_PLACEMENT_3D", [&] {
                    m_b.string("");
                    m_b.reference(origin);
                    m_b.reference(axis);
                    m_b.reference(reference);
                });
            }

            id colour(const math::vec3f &linear)
            {
                const math::vec3f srgb = linear_to_srgb(linear);
                const id rgb = simple("COLOUR_RGB", [&] {
                    m_b.string("");
                    for (std::size_t i = 0; i < 3; ++i)
                        m_b.real(tidy(static_cast<double>(std::clamp(srgb[i], 0.0f, 1.0f))));
                });
                const id fill_colour = simple("FILL_AREA_STYLE_COLOUR", [&] {
                    m_b.string("");
                    m_b.reference(rgb);
                });
                const id fill = simple("FILL_AREA_STYLE", [&] {
                    m_b.string("");
                    list({fill_colour});
                });
                const id area = simple("SURFACE_STYLE_FILL_AREA", [&] { m_b.reference(fill); });
                const id side = simple("SURFACE_SIDE_STYLE", [&] {
                    m_b.string("");
                    list({area});
                });
                const id usage = simple("SURFACE_STYLE_USAGE", [&] {
                    m_b.enumeration("BOTH");
                    m_b.reference(side);
                });
                return simple("PRESENTATION_STYLE_ASSIGNMENT", [&] { list({usage}); });
            }

            void style(id item, const math::vec3f &linear, float opacity)
            {
                id assignment = colour(linear);
                if (opacity < 1.0f)
                {
                    // The transparency rides along in a rendering style.
                    const id transparent = simple("SURFACE_STYLE_TRANSPARENT",
                                                  [&] { m_b.real(tidy(static_cast<double>(1.0f - opacity))); });
                    const id properties = simple("SURFACE_STYLE_RENDERING_WITH_PROPERTIES", [&] {
                        m_b.enumeration("NORMAL_SHADING");
                        m_b.omitted();
                        list({transparent});
                    });
                    const id side = simple("SURFACE_SIDE_STYLE", [&] {
                        m_b.string("");
                        list({properties});
                    });
                    const id usage = simple("SURFACE_STYLE_USAGE", [&] {
                        m_b.enumeration("BOTH");
                        m_b.reference(side);
                    });
                    const id second = simple("PRESENTATION_STYLE_ASSIGNMENT", [&] { list({usage}); });
                    m_styled.push_back(simple("STYLED_ITEM", [&] {
                        m_b.string("transparency");
                        list({second});
                        m_b.reference(item);
                    }));
                }
                m_styled.push_back(simple("STYLED_ITEM", [&] {
                    m_b.string("color");
                    list({assignment});
                    m_b.reference(item);
                }));
            }

            // One mesh as a faceted solid (or an open shell model).
            id mesh_item(const mesh &mesh)
            {
                // Vertices welded by position: a CARTESIAN_POINT each.
                std::unordered_map<math::vec3f, id> points;
                std::vector<id> corner(mesh.vertices.size());
                for (std::size_t v = 0; v < mesh.vertices.size(); ++v)
                {
                    const math::vec3f &p = mesh.vertices[v].position;
                    auto it = points.find(p);
                    if (it == points.end())
                    {
                        const math::vec3d q{tidy(p[0]), tidy(p[1]), tidy(p[2])};
                        it = points.emplace(p, point(q)).first;
                    }
                    corner[v] = it->second;
                }

                const bool per_vertex = mesh.colors.size() == mesh.vertices.size();
                std::map<std::array<std::int64_t, 4>, id> planes;
                std::vector<id> faces;
                std::vector<std::pair<id, math::vec3f>> face_colours;
                for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
                {
                    const std::uint32_t a = mesh.indices[t];
                    const std::uint32_t b = mesh.indices[t + 1];
                    const std::uint32_t c = mesh.indices[t + 2];
                    if (corner[a] == corner[b] || corner[b] == corner[c] || corner[c] == corner[a])
                        continue;
                    const math::vec3d pa = mesh.vertices[a].position.cast<double>();
                    const math::vec3d n = math::cross(mesh.vertices[b].position.cast<double>() - pa,
                                                      mesh.vertices[c].position.cast<double>() - pa);
                    const double length = math::magnitude(n);
                    if (!(length > 0.0))
                        continue;
                    const math::vec3d unit = n / length;

                    // Coplanar triangles share their plane.
                    const std::array<std::int64_t, 4> key{std::llround(unit[0] * 1e6), std::llround(unit[1] * 1e6),
                                                          std::llround(unit[2] * 1e6),
                                                          std::llround(math::dot(unit, pa) * 1e6)};
                    auto plane = planes.find(key);
                    if (plane == planes.end())
                    {
                        math::vec3d x = math::cross(std::abs(unit[0]) < 0.9 ? math::vec3d{1, 0, 0} : math::vec3d{0, 1, 0}, unit);
                        x = math::normalized(x);
                        math::mat4d frame = math::mat4d::identity();
                        const math::vec3d y = math::cross(unit, x);
                        for (std::size_t i = 0; i < 3; ++i)
                        {
                            frame(i, 0) = x[i];
                            frame(i, 1) = y[i];
                            frame(i, 2) = unit[i];
                            frame(i, 3) = pa[i];
                        }
                        const id position = placement(frame);
                        plane = planes.emplace(key, simple("PLANE", [&] {
                                          m_b.string("");
                                          m_b.reference(position);
                                      })).first;
                    }

                    const id loop = simple("POLY_LOOP", [&] {
                        m_b.string("");
                        list({corner[a], corner[b], corner[c]});
                    });
                    const id bound = simple("FACE_OUTER_BOUND", [&] {
                        m_b.string("");
                        m_b.reference(loop);
                        m_b.enumeration("T");
                    });
                    const id face = simple("FACE_SURFACE", [&] {
                        m_b.string("");
                        list({bound});
                        m_b.reference(plane->second);
                        m_b.enumeration("T");
                    });
                    faces.push_back(face);
                    if (per_vertex)
                        face_colours.emplace_back(face, (mesh.colors[a] + mesh.colors[b] + mesh.colors[c]) / 3.0f);
                }

                const bool closed = measure(mesh).closed();
                const id shell = simple(closed ? "CLOSED_SHELL" : "OPEN_SHELL", [&] {
                    m_b.string("");
                    list(faces);
                });
                const id item = closed ? simple("FACETED_BREP",
                                                [&] {
                                                    m_b.string(mesh.name);
                                                    m_b.reference(shell);
                                                })
                                       : simple("SHELL_BASED_SURFACE_MODEL", [&] {
                                             m_b.string(mesh.name);
                                             list({shell});
                                         });

                style(item, mesh.material.color, mesh.material.opacity);
                if (per_vertex)
                {
                    // Faces of another color than the mesh's get their own
                    // style, one per color.
                    std::map<std::array<int, 3>, std::vector<id>> groups;
                    for (const auto &[face, c] : face_colours)
                    {
                        const std::array<int, 3> key{static_cast<int>(std::lround(c[0] * 1000.0f)),
                                                     static_cast<int>(std::lround(c[1] * 1000.0f)),
                                                     static_cast<int>(std::lround(c[2] * 1000.0f))};
                        groups[key].push_back(face);
                    }
                    for (const auto &[key, members] : groups)
                    {
                        const math::vec3f c{static_cast<float>(key[0]) / 1000.0f, static_cast<float>(key[1]) / 1000.0f,
                                            static_cast<float>(key[2]) / 1000.0f};
                        if (math::distance(c.cast<double>(), mesh.material.color.cast<double>()) < 2e-3)
                            continue;
                        const id assignment = colour(c);
                        for (id face : members)
                        {
                            m_styled.push_back(simple("STYLED_ITEM", [&] {
                                m_b.string("color");
                                list({assignment});
                                m_b.reference(face);
                            }));
                        }
                    }
                }
                return item;
            }

            struct product_ids
            {
                id definition = 0;
                id representation = 0;
                id origin = 0;
            };

            product_ids new_product(const std::string &name, std::vector<id> items)
            {
                const std::string label = name.empty() ? "part" : name;
                const id product = simple("PRODUCT", [&] {
                    m_b.string(label);
                    m_b.string(label);
                    m_b.string("");
                    list({m_product_context});
                });
                simple("PRODUCT_RELATED_PRODUCT_CATEGORY", [&] {
                    m_b.string("part");
                    m_b.omitted();
                    list({product});
                });
                const id formation = simple("PRODUCT_DEFINITION_FORMATION", [&] {
                    m_b.string("");
                    m_b.string("");
                    m_b.reference(product);
                });
                product_ids ids;
                ids.definition = simple("PRODUCT_DEFINITION", [&] {
                    m_b.string("design");
                    m_b.string("");
                    m_b.reference(formation);
                    m_b.reference(m_definition_context);
                });
                const id shape = simple("PRODUCT_DEFINITION_SHAPE", [&] {
                    m_b.string("");
                    m_b.string("");
                    m_b.reference(ids.definition);
                });
                ids.origin = placement(math::mat4d::identity());
                items.insert(items.begin(), ids.origin);
                ids.representation = simple("SHAPE_REPRESENTATION", [&] {
                    m_b.string(label);
                    list(items);
                    m_b.reference(m_context);
                });
                simple("SHAPE_DEFINITION_REPRESENTATION", [&] {
                    m_b.reference(shape);
                    m_b.reference(ids.representation);
                });
                return ids;
            }

            // The items for the node's own meshes; baked into place when the
            // node is placed by something a STEP placement cannot say
            // (scaling, mirroring).
            std::vector<id> items_for(std::uint32_t node, const math::mat4d *bake)
            {
                std::vector<id> items;
                for (std::uint32_t index : m_model.nodes[node].meshes)
                {
                    if (index >= m_model.meshes.size())
                        continue;
                    if (bake == nullptr)
                    {
                        auto it = m_mesh_items.find(index);
                        if (it == m_mesh_items.end())
                            it = m_mesh_items.emplace(index, mesh_item(m_model.meshes[index])).first;
                        items.push_back(it->second);
                    }
                    else
                    {
                        mesh copy = m_model.meshes[index];
                        transform_mesh(copy, *bake);
                        items.push_back(mesh_item(copy));
                    }
                }
                return items;
            }

            product_ids product(std::uint32_t index, const math::mat4d &bake, std::vector<char> &visited, int depth)
            {
                visited[index] = 1;
                const node &n = m_model.nodes[index];
                const bool baking = bake != math::mat4d::identity();

                // A part used again as it is: one product, placed twice.
                if (n.children.empty() && !baking)
                {
                    const auto found = m_leaf_products.find(n.meshes);
                    if (found != m_leaf_products.end())
                    {
                        product_ids ids;
                        ids.definition = found->second.first;
                        ids.representation = found->second.second;
                        ids.origin = m_leaf_origins[ids.definition];
                        return ids;
                    }
                }

                std::vector<id> items = items_for(index, baking ? &bake : nullptr);

                struct child_record
                {
                    product_ids ids;
                    math::mat4d transform;
                    std::string name;
                };
                std::vector<child_record> children;
                for (std::uint32_t child : n.children)
                {
                    if (child >= m_model.nodes.size() || visited[child] != 0 || depth > 256)
                        continue;
                    const math::mat4d local = m_model.nodes[child].transform.cast<double>();
                    // Placements are rigid; anything else is baked into the
                    // geometry below it.
                    const math::mat4d combined = bake * local;
                    if (is_rigid(combined))
                        children.push_back({product(child, math::mat4d::identity(), visited, depth + 1), combined,
                                            m_model.nodes[child].name});
                    else
                        children.push_back({product(child, combined, visited, depth + 1), math::mat4d::identity(),
                                            m_model.nodes[child].name});
                }

                std::vector<math::mat4d> transforms;
                for (const child_record &c : children)
                    transforms.push_back(c.transform);

                product_ids ids;
                if (children.empty())
                {
                    ids = new_product(n.name, items);
                    if (!baking)
                    {
                        m_leaf_products.emplace(n.meshes, std::make_pair(ids.definition, ids.representation));
                        m_leaf_origins[ids.definition] = ids.origin;
                    }
                    return ids;
                }

                // Assemblies: the placements are created first and listed in
                // the representation with the node's own items.
                std::vector<id> wheres;
                for (const math::mat4d &t : transforms)
                    wheres.push_back(placement(t));
                std::vector<id> all = items;
                all.insert(all.end(), wheres.begin(), wheres.end());
                ids = new_product(n.name, all);
                for (std::size_t c = 0; c < children.size(); ++c)
                    occurrence(ids, children[c].ids, wheres[c], children[c].name.empty() ? std::to_string(c + 1)
                                                                                           : children[c].name);
                return ids;
            }

            void occurrence(const product_ids &parent, const product_ids &child, id where, const std::string &name)
            {
                const id usage = simple("NEXT_ASSEMBLY_USAGE_OCCURRENCE", [&] {
                    m_b.string(name);
                    m_b.string(name);
                    m_b.string("");
                    m_b.reference(parent.definition);
                    m_b.reference(child.definition);
                    m_b.omitted();
                });
                const id shape = simple("PRODUCT_DEFINITION_SHAPE", [&] {
                    m_b.string("Placement");
                    m_b.string("Placement of an item");
                    m_b.reference(usage);
                });
                const id transformation = simple("ITEM_DEFINED_TRANSFORMATION", [&] {
                    m_b.string("");
                    m_b.string("");
                    m_b.reference(child.origin);
                    m_b.reference(where);
                });
                const id relation = complex({{"REPRESENTATION_RELATIONSHIP", [&] {
                                                  m_b.string("");
                                                  m_b.string("");
                                                  m_b.reference(child.representation);
                                                  m_b.reference(parent.representation);
                                              }},
                                             {"REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION",
                                              [&] { m_b.reference(transformation); }},
                                             {"SHAPE_REPRESENTATION_RELATIONSHIP", [] {}}});
                simple("CONTEXT_DEPENDENT_SHAPE_REPRESENTATION", [&] {
                    m_b.reference(relation);
                    m_b.reference(shape);
                });
            }

            std::unordered_map<id, id> m_leaf_origins;
        };
    } // namespace

    part21::file build(const model &model, const write_options &options)
    {
        step_writer writer(model, options);
        return writer.run();
    }

    std::string write(const model &model, const write_options &options)
    {
        const part21::file file = build(model, options);
        part21::write_options text;
        text.newline = "\n";
        return part21::write(file, text);
    }

} // namespace volt::model::step
