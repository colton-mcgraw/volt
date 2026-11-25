#include "step_access.hpp"

#include <math/geometry.hpp>

#include <cmath>
#include <numbers>

namespace volt::model::step
{

    namespace
    {
        constexpr std::string_view names[] = {
#define VOLT_STEP_NAME(identifier, name) name,
            VOLT_STEP_TYPES(VOLT_STEP_NAME)
#undef VOLT_STEP_NAME
        };

        constexpr std::uint32_t absent = 0xffffffffu;

        std::optional<math::vec3d> unit(const math::vec3d &v)
        {
            const double length = math::magnitude(v);
            if (!(length > 1e-300) || !std::isfinite(length))
                return std::nullopt;
            return v / length;
        }

        double si_prefix(std::string_view prefix)
        {
            struct entry
            {
                std::string_view name;
                double factor;
            };
            static constexpr entry prefixes[] = {
                {"EXA", 1e18},   {"PETA", 1e15},  {"TERA", 1e12}, {"GIGA", 1e9},   {"MEGA", 1e6},  {"KILO", 1e3},
                {"HECTO", 1e2},  {"DECA", 1e1},   {"DECI", 1e-1}, {"CENTI", 1e-2}, {"MILLI", 1e-3}, {"MICRO", 1e-6},
                {"NANO", 1e-9},  {"PICO", 1e-12}, {"FEMTO", 1e-15}, {"ATTO", 1e-18},
            };
            for (const entry &e : prefixes)
            {
                if (e.name == prefix)
                    return e.factor;
            }
            return 1.0;
        }
    } // namespace

    std::string_view type_name(type type) noexcept
    {
        const auto index = static_cast<std::size_t>(type);
        return index < std::size(names) ? names[index] : std::string_view("?");
    }

    math::mat4d frame::matrix() const noexcept
    {
        math::mat4d m = math::mat4d::identity();
        for (std::size_t i = 0; i < 3; ++i)
        {
            m(i, 0) = x[i];
            m(i, 1) = y[i];
            m(i, 2) = z[i];
            m(i, 3) = origin[i];
        }
        return m;
    }

    access::access(const part21::file &file, load_report &report) : m_file(file), m_report(report)
    {
        m_types.assign(file.name_count(), type::unknown);
        for (std::size_t t = 0; t < static_cast<std::size_t>(type::count); ++t)
        {
            const auto id = file.name_id(names[t]);
            m_ids[t] = id ? *id : absent;
            if (id)
                m_types[*id] = static_cast<type>(t);
        }
    }

    type access::type_of(const part21::entity &entity) const noexcept
    {
        const std::size_t parts = entity.part_count();
        for (std::size_t part = 0; part < parts; ++part)
        {
            const std::uint32_t id = entity.name_id(part);
            if (id < m_types.size() && m_types[id] != type::unknown)
                return m_types[id];
        }
        return type::unknown;
    }

    bool access::is(const part21::entity &entity, type type) const noexcept
    {
        const std::uint32_t id = m_ids[static_cast<std::size_t>(type)];
        if (id == absent)
            return false;
        if (!entity.is_complex())
            return entity.name_id() == id;
        return entity.find_part(id).has_value();
    }

    part21::entity access::resolve(const part21::parameter &parameter) const noexcept
    {
        const part21::parameter value = parameter.value();
        if (!value.is(part21::kind::reference))
            return {};
        return m_file.get(value.reference());
    }

    attributes access::gather(const part21::entity &entity, std::initializer_list<type> chain) const noexcept
    {
        attributes result;
        if (!entity.valid())
            return result;
        if (!entity.is_complex())
        {
            const part21::parameter list = entity.parameters();
            for (std::size_t i = 0; i < list.size(); ++i)
                result.push(list[i]);
            return result;
        }
        for (type t : chain)
        {
            const std::uint32_t id = m_ids[static_cast<std::size_t>(t)];
            if (id == absent)
                continue;
            const auto part = entity.find_part(id);
            if (!part)
                continue;
            const part21::parameter list = entity.parameters(*part);
            for (std::size_t i = 0; i < list.size(); ++i)
                result.push(list[i]);
        }
        return result;
    }

    part21::parameter access::part(const part21::entity &entity, type type) const noexcept
    {
        if (!entity.is_complex())
            return is(entity, type) ? entity.parameters() : part21::parameter{};
        const std::uint32_t id = m_ids[static_cast<std::size_t>(type)];
        if (id == absent)
            return {};
        const auto part = entity.find_part(id);
        return part ? entity.parameters(*part) : part21::parameter{};
    }

    std::optional<math::vec3d> access::point(const part21::parameter &reference) const
    {
        const part21::entity entity = resolve(reference);
        if (!is(entity, type::cartesian_point))
            return std::nullopt;
        const part21::parameter coordinates = part(entity, type::cartesian_point)[1];
        const std::size_t count = coordinates.size();
        if (count < 1 || count > 3)
            return std::nullopt;
        math::vec3d p{};
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!coordinates[i].value().is_number())
                return std::nullopt;
            p[i] = coordinates[i].value().number();
        }
        return p;
    }

    std::optional<math::vec3d> access::direction(const part21::parameter &reference) const
    {
        const part21::entity entity = resolve(reference);
        if (!is(entity, type::direction))
            return std::nullopt;
        const part21::parameter ratios = part(entity, type::direction)[1];
        const std::size_t count = ratios.size();
        if (count < 1 || count > 3)
            return std::nullopt;
        math::vec3d d{};
        for (std::size_t i = 0; i < count; ++i)
            d[i] = ratios[i].value().number();
        return unit(d);
    }

    std::optional<frame> access::placement(const part21::parameter &reference) const
    {
        const part21::entity entity = resolve(reference);
        if (!entity.valid())
            return std::nullopt;

        frame result;
        std::optional<math::vec3d> axis;
        std::optional<math::vec3d> reference_direction;

        if (is(entity, type::axis2_placement_3d))
        {
            const part21::parameter a = part(entity, type::axis2_placement_3d);
            const auto location = point(a[1]);
            if (!location)
                return std::nullopt;
            result.origin = *location;
            axis = direction(a[2]);
            reference_direction = direction(a[3]);
        }
        else if (is(entity, type::axis2_placement_2d))
        {
            const part21::parameter a = part(entity, type::axis2_placement_2d);
            const auto location = point(a[1]);
            if (!location)
                return std::nullopt;
            result.origin = *location;
            reference_direction = direction(a[2]);
        }
        else if (is(entity, type::axis1_placement))
        {
            const part21::parameter a = part(entity, type::axis1_placement);
            const auto location = point(a[1]);
            if (!location)
                return std::nullopt;
            result.origin = *location;
            axis = direction(a[2]);
        }
        else
            return std::nullopt;

        result.z = axis.value_or(math::vec3d{0.0, 0.0, 1.0});

        // The reference direction projected into the plane normal to the
        // axis; any direction there if it is missing or parallel.
        math::vec3d x = reference_direction.value_or(math::vec3d{1.0, 0.0, 0.0});
        x = x - result.z * math::dot(x, result.z);
        auto ux = unit(x);
        if (!ux || math::magnitude(x) < 1e-9)
        {
            const math::vec3d trial =
                std::abs(result.z[0]) < 0.9 ? math::vec3d{1.0, 0.0, 0.0} : math::vec3d{0.0, 1.0, 0.0};
            ux = unit(trial - result.z * math::dot(trial, result.z));
        }
        result.x = *ux;
        result.y = math::cross(result.z, result.x);
        return result;
    }

    std::optional<math::mat4d> access::transformation(const part21::parameter &reference) const
    {
        const part21::entity entity = resolve(reference);
        if (!entity.valid())
            return std::nullopt;
        if (is(entity, type::cartesian_transformation_operator_3d))
        {
            // The leading name and description attributes are written
            // differently by different exporters, the last five are always
            // axis1, axis2, local_origin, scale, axis3.
            const part21::parameter a = part(entity, type::cartesian_transformation_operator_3d);
            if (a.size() < 5)
                return std::nullopt;
            const std::size_t n = a.size();
            const std::optional<math::vec3d> origin = point(a[n - 3]);
            const double scale = a[n - 2].value().is_number() ? a[n - 2].value().number() : 1.0;
            const math::vec3d z = direction(a[n - 1]).value_or(math::vec3d{0.0, 0.0, 1.0});
            math::vec3d x = direction(a[n - 5]).value_or(math::vec3d{1.0, 0.0, 0.0});
            x = x - z * math::dot(x, z);
            const auto ux = unit(x);
            frame f;
            f.origin = origin.value_or(math::vec3d{});
            f.z = z;
            f.x = ux.value_or(math::vec3d{1.0, 0.0, 0.0});
            f.y = math::cross(f.z, f.x);
            math::mat4d m = f.matrix();
            if (scale > 0.0 && scale != 1.0)
            {
                for (std::size_t i = 0; i < 3; ++i)
                    for (std::size_t j = 0; j < 3; ++j)
                        m(i, j) *= scale;
            }
            return m;
        }
        const auto f = placement(reference);
        if (!f)
            return std::nullopt;
        return f->matrix();
    }

    double access::unit_factor(const part21::entity &unit, type kind, int depth) const
    {
        if (!unit.valid() || depth > 8)
            return 0.0;

        if (const part21::parameter si = part(unit, type::si_unit); si.is(part21::kind::list))
        {
            const double prefix = si[0].is(part21::kind::enumeration) ? si_prefix(si[0].text()) : 1.0;
            const std::string_view name = si[1].text();
            if (name == "METRE")
                return prefix * 1000.0;
            if (name == "RADIAN")
                return prefix;
            if (name == "STERADIAN")
                return prefix;
            return 0.0;
        }

        if (const part21::parameter cbu = part(unit, type::conversion_based_unit); cbu.is(part21::kind::list))
        {
            const part21::entity measure = resolve(cbu[1]);
            const attributes m = gather(measure, {type::measure_with_unit, type::length_measure_with_unit,
                                                  type::plane_angle_measure_with_unit});
            const double value = m[0].value().number();
            const double base = unit_factor(resolve(m[1]), kind, depth + 1);
            if (value > 0.0 && base > 0.0)
                return value * base;

            // A conversion without a usable factor: go by the name.
            const std::string_view name = cbu[0].text();
            if (name == "INCH" || name == "inch")
                return 25.4;
            if (name == "FOOT" || name == "foot")
                return 304.8;
            if (name == "DEGREE" || name == "degree" || name == "DEGREES")
                return std::numbers::pi / 180.0;
            return 0.0;
        }
        return 0.0;
    }

    units access::context_units(const part21::parameter &context)
    {
        return context_units(resolve(context));
    }

    units access::context_units(const part21::entity &entity)
    {
        if (!entity.valid())
            return {};
        if (const auto it = m_units.find(entity.index()); it != m_units.end())
            return it->second;

        units result;
        const part21::parameter assigned = part(entity, type::global_unit_assigned_context);
        const part21::parameter list = assigned[0];
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            const part21::entity unit = resolve(list[i]);
            if (is(unit, type::length_unit))
            {
                const double factor = unit_factor(unit, type::length_unit, 0);
                if (factor > 0.0 && std::isfinite(factor))
                    result.length = factor;
            }
            else if (is(unit, type::plane_angle_unit))
            {
                const double factor = unit_factor(unit, type::plane_angle_unit, 0);
                if (factor > 0.0 && std::isfinite(factor))
                    result.angle = factor;
            }
        }

        const part21::parameter uncertainties = part(entity, type::global_uncertainty_assigned_context)[0];
        for (std::size_t i = 0; i < uncertainties.size(); ++i)
        {
            const part21::entity measure = resolve(uncertainties[i]);
            const attributes m = gather(measure, {type::measure_with_unit, type::uncertainty_measure_with_unit});
            const double value = m[0].value().number();
            const double factor = unit_factor(resolve(m[1]), type::length_unit, 0);
            if (value > 0.0 && factor > 0.0 && is(resolve(m[1]), type::length_unit))
                result.uncertainty = value * factor / result.length;
        }

        m_units.emplace(entity.index(), result);
        return result;
    }

    std::string access::describe(const part21::entity &entity) const
    {
        if (!entity.valid())
            return "(missing)";
        if (!entity.is_complex())
            return std::string(entity.name());
        std::string text = "(";
        for (std::size_t part = 0; part < entity.part_count(); ++part)
        {
            if (part != 0)
                text += ' ';
            text += entity.name(part);
        }
        return text + ")";
    }

} // namespace volt::model::step
