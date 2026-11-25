#include <model/step.hpp>

#include "step_access.hpp"
#include "step_shapes.hpp"

#include <math/geometry.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <unordered_map>
#include <unordered_set>

// The product structure of a STEP file, and the reader's front: which
// products there are, which of them are assemblies of which others where,
// and which representation holds each one's shape.

namespace volt::model::step
{

    namespace
    {
        constexpr int max_depth = 64;
        constexpr std::size_t max_nodes = 1000000;

        // The inverse of a rigid transform, without a general inversion.
        math::mat4d rigid_inverse(const math::mat4d &m)
        {
            math::mat4d result = math::mat4d::identity();
            for (std::size_t i = 0; i < 3; ++i)
                for (std::size_t j = 0; j < 3; ++j)
                    result(i, j) = m(j, i);
            for (std::size_t i = 0; i < 3; ++i)
            {
                double t = 0.0;
                for (std::size_t k = 0; k < 3; ++k)
                    t -= result(i, k) * m(k, 3);
                result(i, 3) = t;
            }
            return result;
        }

        // A placement's matrix with its origin taken to millimeters.
        math::mat4d in_millimeters(math::mat4d m, double length)
        {
            for (std::size_t i = 0; i < 3; ++i)
                m(i, 3) *= length;
            return m;
        }

        class reader
        {
        public:
            reader(const part21::file &file, const load_options &options, load_report &report)
                : m_access(file, report), m_shapes(m_access, options), m_options(options), m_report(report)
            {
            }

            model run()
            {
                m_shapes.collect_styles();
                index();

                std::vector<std::uint32_t> roots;
                for (std::uint32_t pd : m_definitions)
                {
                    if (!m_child_definitions.contains(pd))
                        roots.push_back(pd);
                }

                m_model.nodes.push_back({});
                if (roots.size() == 1)
                {
                    build_definition(roots[0], 0, 0);
                    m_model.name = m_model.nodes[0].name;
                }
                else
                {
                    for (std::uint32_t pd : roots)
                    {
                        const auto child = new_node();
                        build_definition(pd, child, 1);
                        m_model.nodes[0].children.push_back(child);
                    }
                }

                // A file without products, or whose products lead to no
                // shape: whatever solids it has, in one node.
                if (m_model.meshes.empty())
                    fallback();
                return std::move(m_model);
            }

        private:
            access m_access;
            shape_builder m_shapes;
            load_options m_options;
            load_report &m_report;
            model m_model;

            std::vector<std::uint32_t> m_definitions;
            std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> m_representations; // of a definition
            std::unordered_map<std::uint32_t, std::uint32_t> m_definition_of;                // of a representation
            std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> m_usages;          // NAUOs under a definition
            std::unordered_set<std::uint32_t> m_child_definitions;
            std::unordered_map<std::uint32_t, std::uint32_t> m_placements; // NAUO -> representation relationship
            std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> m_links; // same-shape representations
            std::unordered_map<std::uint32_t, std::int64_t> m_item_meshes;         // item -> mesh, -1 for none

            std::uint32_t new_node()
            {
                m_model.nodes.push_back({});
                return static_cast<std::uint32_t>(m_model.nodes.size() - 1);
            }

            part21::entity entity(std::uint32_t index) const { return m_access.file()[index]; }

            void index()
            {
                const part21::file &file = m_access.file();
                std::unordered_set<std::uint32_t> definitions;
                for (std::uint32_t i = 0; i < file.size(); ++i)
                {
                    const part21::entity e = file[i];
                    switch (m_access.type_of(e))
                    {
                    case type::shape_definition_representation:
                    {
                        // (definition: PRODUCT_DEFINITION_SHAPE, used_representation)
                        const part21::parameter a = e.parameters(0);
                        const part21::entity shape = m_access.resolve(a[0]);
                        const part21::entity definition = m_access.resolve(shape.parameters(0)[2]);
                        const part21::entity representation = m_access.resolve(a[1]);
                        if (!m_access.is(definition, type::product_definition) || !representation.valid())
                            break;
                        m_representations[definition.index()].push_back(representation.index());
                        m_definition_of.emplace(representation.index(), definition.index());
                        if (definitions.insert(definition.index()).second)
                            m_definitions.push_back(definition.index());
                        break;
                    }
                    case type::next_assembly_usage_occurrence:
                    {
                        // (id, name, description, relating, related, designator)
                        const part21::parameter a = e.parameters(0);
                        const part21::entity parent = m_access.resolve(a[3]);
                        const part21::entity child = m_access.resolve(a[4]);
                        if (!parent.valid() || !child.valid())
                            break;
                        m_usages[parent.index()].push_back(i);
                        m_child_definitions.insert(child.index());
                        for (const part21::entity &d : {parent, child})
                        {
                            if (definitions.insert(d.index()).second)
                                m_definitions.push_back(d.index());
                        }
                        break;
                    }
                    case type::context_dependent_shape_representation:
                    {
                        // (representation_relation, represented_product_relation)
                        const part21::parameter a = e.parameters(0);
                        const part21::entity relation = m_access.resolve(a[0]);
                        const part21::entity shape = m_access.resolve(a[1]);
                        const part21::entity usage = m_access.resolve(shape.parameters(0)[2]);
                        if (relation.valid() && m_access.is(usage, type::next_assembly_usage_occurrence))
                            m_placements.emplace(usage.index(), relation.index());
                        break;
                    }
                    default:
                        break;
                    }

                    // Representation relationships without a transformation:
                    // two representations of the same shape.
                    if ((m_access.is(e, type::shape_representation_relationship) ||
                         m_access.is(e, type::representation_relationship)) &&
                        !m_access.is(e, type::representation_relationship_with_transformation))
                    {
                        const attributes a = m_access.gather(e, {type::representation_relationship});
                        const part21::entity first = m_access.resolve(a[2]);
                        const part21::entity second = m_access.resolve(a[3]);
                        if (first.valid() && second.valid() && first.index() != second.index())
                        {
                            m_links[first.index()].push_back(second.index());
                            m_links[second.index()].push_back(first.index());
                        }
                    }
                }
            }

            std::string product_name(const part21::entity &definition) const
            {
                // PRODUCT_DEFINITION -> formation -> PRODUCT (id, name, ...)
                const part21::entity formation = m_access.resolve(definition.parameters(0)[2]);
                const part21::entity product = m_access.resolve(formation.parameters(0)[2]);
                if (!m_access.is(product, type::product))
                    return {};
                const part21::parameter a = product.parameters(0);
                const std::string_view name = a[1].text();
                return std::string(name.empty() ? a[0].text() : name);
            }

            // The placement of a NAUO's child in its parent, in millimeters.
            math::mat4d placement(std::uint32_t usage, std::uint32_t child_definition)
            {
                const auto found = m_placements.find(usage);
                if (found == m_placements.end())
                    return math::mat4d::identity();
                const part21::entity relation = entity(found->second);
                const attributes a = m_access.gather(relation, {type::representation_relationship});
                const part21::entity first = m_access.resolve(a[2]);
                const part21::entity second = m_access.resolve(a[3]);
                const part21::parameter transformation =
                    m_access.part(relation, type::representation_relationship_with_transformation)[0];
                const part21::entity operation = m_access.resolve(transformation);

                // The convention is rep_1 the child's, rep_2 the parent's.
                // Some exporters swap them; the child's own representation
                // says which.
                bool swapped = false;
                const auto child_reps = m_representations.find(child_definition);
                if (child_reps != m_representations.end() && second.valid() &&
                    std::find(child_reps->second.begin(), child_reps->second.end(), second.index()) !=
                        child_reps->second.end())
                    swapped = true;

                const units first_units = m_access.context_units(first.parameters(0)[2]);
                const units second_units = m_access.context_units(second.parameters(0)[2]);

                if (m_access.is(operation, type::item_defined_transformation))
                {
                    // (name, description, item_1, item_2)
                    const part21::parameter ia = operation.parameters(0);
                    const auto one = m_access.transformation(ia[2]);
                    const auto two = m_access.transformation(ia[3]);
                    if (!one || !two)
                        return math::mat4d::identity();
                    const math::mat4d m1 = in_millimeters(*one, first_units.length);
                    const math::mat4d m2 = in_millimeters(*two, second_units.length);
                    return swapped ? m1 * rigid_inverse(m2) : m2 * rigid_inverse(m1);
                }
                if (const auto direct = m_access.transformation(transformation))
                {
                    const math::mat4d m = in_millimeters(*direct, (swapped ? first_units : second_units).length);
                    return swapped ? rigid_inverse(m) : m;
                }
                return math::mat4d::identity();
            }

            struct pending_item
            {
                part21::entity item;
                units units;
            };

            // The shape items of a representation and of those linked to it
            // as the same shape, and the representations it maps in.
            void gather(std::uint32_t representation, std::uint32_t owner, std::unordered_set<std::uint32_t> &seen,
                        std::vector<pending_item> &items, std::vector<std::pair<part21::entity, units>> &mapped,
                        int depth)
            {
                if (depth > max_depth || !seen.insert(representation).second)
                    return;
                const part21::entity rep = entity(representation);
                const part21::parameter a = rep.parameters(0);
                const units units = m_access.context_units(a[2]);
                const part21::parameter list = a[1];
                for (std::size_t i = 0; i < list.size(); ++i)
                {
                    const part21::entity item = m_access.resolve(list[i]);
                    if (!item.valid())
                        continue;
                    if (m_shapes.is_shape(item))
                        items.push_back({item, units});
                    else if (m_access.is(item, type::mapped_item))
                        mapped.push_back({item, units});
                }

                const auto links = m_links.find(representation);
                if (links == m_links.end())
                    return;
                for (std::uint32_t other : links->second)
                {
                    // Not into the representation of another product: that
                    // one is placed by the assembly structure.
                    const auto owner_of = m_definition_of.find(other);
                    if (owner_of != m_definition_of.end() && owner_of->second != owner)
                        continue;
                    gather(other, owner, seen, items, mapped, depth + 1);
                }
            }

            std::int64_t mesh_of(const pending_item &pending, const std::string &fallback_name)
            {
                const auto found = m_item_meshes.find(pending.item.index());
                if (found != m_item_meshes.end())
                    return found->second;
                std::optional<mesh> built = m_shapes.build(pending.item, pending.units);
                std::int64_t index = -1;
                if (built)
                {
                    if (built->name.empty())
                        built->name = fallback_name;
                    index = static_cast<std::int64_t>(m_model.meshes.size());
                    m_model.meshes.push_back(std::move(*built));
                }
                m_item_meshes.emplace(pending.item.index(), index);
                return index;
            }

            // Fills node `target` with the shape of representation set
            // `representations` belonging to definition `owner`.
            void fill(std::uint32_t target, const std::vector<std::uint32_t> &representations, std::uint32_t owner,
                      const std::unordered_set<std::uint32_t> &placed_children, int depth)
            {
                std::unordered_set<std::uint32_t> seen;
                std::vector<pending_item> items;
                std::vector<std::pair<part21::entity, units>> mapped;
                for (std::uint32_t rep : representations)
                    gather(rep, owner, seen, items, mapped, depth);

                // Where a tessellation and the B-rep it stands for are both
                // here, keep one of them.
                std::unordered_set<std::uint32_t> listed;
                for (const pending_item &p : items)
                    listed.insert(p.item.index());
                std::unordered_set<std::uint32_t> skip;
                for (const pending_item &p : items)
                {
                    if (!m_access.is(p.item, type::tessellated_solid))
                        continue;
                    const part21::entity link = m_access.resolve(m_access.part(p.item, type::tessellated_solid)[2]);
                    if (!link.valid() || !listed.contains(link.index()))
                        continue;
                    if (m_options.prefer_tessellation)
                        skip.insert(link.index());
                    else
                        skip.insert(p.item.index());
                }

                const std::string name = m_model.nodes[target].name;
                for (const pending_item &p : items)
                {
                    if (skip.contains(p.item.index()))
                        continue;
                    const std::int64_t mesh = mesh_of(p, name);
                    if (mesh >= 0)
                        m_model.nodes[target].meshes.push_back(static_cast<std::uint32_t>(mesh));
                }

                // MAPPED_ITEM: another representation placed in this one.
                for (const auto &[item, item_units] : mapped)
                {
                    // (name, mapping_source: REPRESENTATION_MAP(origin, rep), target)
                    const part21::parameter a = m_access.part(item, type::mapped_item);
                    const part21::entity map = m_access.resolve(a[1]);
                    const part21::parameter ma = m_access.part(map, type::representation_map);
                    const part21::entity mapped_rep = m_access.resolve(ma[1]);
                    if (!mapped_rep.valid())
                        continue;
                    // Placed already through the assembly structure.
                    if (const auto owner_of = m_definition_of.find(mapped_rep.index());
                        owner_of != m_definition_of.end() && placed_children.contains(owner_of->second))
                        continue;

                    const units mapped_units = m_access.context_units(mapped_rep.parameters(0)[2]);
                    const auto origin = m_access.transformation(ma[0]);
                    const auto destination = m_access.transformation(a[2]);
                    math::mat4d transform = math::mat4d::identity();
                    if (origin && destination)
                        transform = in_millimeters(*destination, item_units.length) *
                                    rigid_inverse(in_millimeters(*origin, mapped_units.length));
                    else if (destination)
                        transform = in_millimeters(*destination, item_units.length);

                    if (m_model.nodes.size() >= max_nodes || depth >= max_depth)
                        continue;
                    const std::uint32_t child = new_node();
                    m_model.nodes[child].name = std::string(a[0].text());
                    m_model.nodes[child].transform = transform.cast<float>();
                    m_model.nodes[target].children.push_back(child);

                    std::uint32_t child_owner = owner;
                    if (const auto owner_of = m_definition_of.find(mapped_rep.index());
                        owner_of != m_definition_of.end())
                    {
                        child_owner = owner_of->second;
                        if (m_model.nodes[child].name.empty())
                            m_model.nodes[child].name = product_name(entity(child_owner));
                    }
                    fill(child, {mapped_rep.index()}, child_owner, {}, depth + 1);
                }
            }

            std::vector<std::uint32_t> m_stack;

            void build_definition(std::uint32_t definition, std::uint32_t target, int depth)
            {
                m_stack.push_back(definition);
                m_model.nodes[target].name = product_name(entity(definition));

                std::unordered_set<std::uint32_t> placed;
                const auto usages = m_usages.find(definition);
                if (usages != m_usages.end())
                {
                    for (std::uint32_t usage : usages->second)
                    {
                        const part21::entity child = m_access.resolve(entity(usage).parameters(0)[4]);
                        if (m_placements.contains(usage))
                            placed.insert(child.index());
                    }
                }

                const auto reps = m_representations.find(definition);
                if (reps != m_representations.end())
                    fill(target, reps->second, definition, placed, depth);

                if (usages != m_usages.end())
                {
                    for (std::uint32_t usage : usages->second)
                    {
                        const part21::entity child = m_access.resolve(entity(usage).parameters(0)[4]);
                        if (!child.valid())
                            continue;
                        if (std::find(m_stack.begin(), m_stack.end(), child.index()) != m_stack.end())
                        {
                            m_report.warn(std::format("#{} makes an assembly contain itself; not followed",
                                                      entity(usage).id()));
                            continue;
                        }
                        if (depth >= max_depth || m_model.nodes.size() >= max_nodes)
                        {
                            m_report.warn("the assembly is too deep or too large; cut short");
                            continue;
                        }
                        const math::mat4d transform = placement(usage, child.index());
                        const std::uint32_t node = new_node();
                        m_model.nodes[node].transform = transform.cast<float>();
                        m_model.nodes[target].children.push_back(node);
                        build_definition(child.index(), node, depth + 1);
                    }
                }
                m_stack.pop_back();
            }

            void fallback()
            {
                const part21::file &file = m_access.file();

                // Items that are part of other items are not solids of their
                // own: the shells of a solid, the faces of a tessellated one.
                std::unordered_set<std::uint32_t> contained;
                for (std::uint32_t i = 0; i < file.size(); ++i)
                {
                    const part21::entity e = file[i];
                    if (!m_shapes.is_shape(e))
                        continue;
                    const part21::parameter a = e.parameters(0);
                    for (std::size_t k = 1; k < a.size(); ++k)
                    {
                        const part21::parameter p = a[k];
                        const auto mark = [&](const part21::parameter &r) {
                            const part21::entity target = m_access.resolve(r);
                            if (target.valid())
                                contained.insert(target.index());
                        };
                        mark(p);
                        for (std::size_t j = 0; j < p.size(); ++j)
                            mark(p[j]);
                    }
                }

                // The units of the first context that has any.
                units units;
                for (std::uint32_t i = 0; i < file.size(); ++i)
                {
                    if (m_access.is(file[i], type::global_unit_assigned_context))
                    {
                        units = m_access.context_units(file[i]);
                        break;
                    }
                }

                m_model.nodes.assign(1, node{});
                for (std::uint32_t i = 0; i < file.size(); ++i)
                {
                    const part21::entity e = file[i];
                    if (!m_shapes.is_shape(e) || contained.contains(i))
                        continue;
                    const std::int64_t mesh = mesh_of({e, units}, "solid");
                    if (mesh >= 0)
                        m_model.nodes[0].meshes.push_back(static_cast<std::uint32_t>(mesh));
                }
            }

        };
    } // namespace

    std::expected<model, std::string> read(const part21::file &file, const load_options &options, load_report &report)
    {
        const auto start = std::chrono::steady_clock::now();
        report.entities = file.size();
        reader r(file, options, report);
        model result = r.run();
        report.build_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (result.meshes.empty())
        {
            std::string message = "the file has no shape that could be read";
            if (!report.warnings.empty())
                message += " (" + report.warnings.front() + ")";
            return std::unexpected(message);
        }
        return result;
    }

    std::expected<model, std::string> read(std::string_view text, const load_options &options, load_report &report)
    {
        const auto start = std::chrono::steady_clock::now();
        auto file = part21::parse(text);
        report.parse_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (!file)
            return std::unexpected(file.error());
        return read(*file, options, report);
    }

} // namespace volt::model::step
