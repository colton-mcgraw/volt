#include "surfaces.hpp"

#include <math/geometry.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace volt::model::geometry
{

    namespace
    {
        constexpr double pi = std::numbers::pi;
        constexpr double two_pi = 2.0 * std::numbers::pi;

        // value + k * period closest to `near`.
        double unwrap(double value, double period, double near)
        {
            return value + period * std::round((near - value) / period);
        }

        math::vec3d radial(const step::frame &frame, double u)
        {
            return frame.x * std::cos(u) + frame.y * std::sin(u);
        }

        math::vec3d tangential(const step::frame &frame, double u)
        {
            return frame.x * -std::sin(u) + frame.y * std::cos(u);
        }

        math::vec3d unit_or(const math::vec3d &v, const math::vec3d &fallback)
        {
            const double length = math::magnitude(v);
            return length > 1e-300 ? math::vec3d(v / length) : fallback;
        }
    } // namespace

    // ---------------------------------------------------------------------
    // surface
    // ---------------------------------------------------------------------

    math::vec2d surface::clamp(math::vec2d uv) const
    {
        for (int axis = 0; axis < 2; ++axis)
        {
            const auto a = static_cast<std::size_t>(axis);
            if (!periodic[axis] && std::isfinite(lo[axis]) && std::isfinite(hi[axis]))
                uv[a] = std::clamp(uv[a], lo[axis], hi[axis]);
        }
        return uv;
    }

    math::vec3d surface::normal(const math::vec2d &uv) const
    {
        math::vec3d p;
        math::vec3d du;
        math::vec3d dv;
        derivatives(uv, p, du, dv);
        math::vec3d n = math::cross(du, dv);
        const double scale = math::magnitude(du) * math::magnitude(dv);
        if (math::magnitude(n) > 1e-10 * scale && scale > 0.0)
            return math::normalized(n);

        // Degenerate here: step towards the middle of the domain until the
        // normal is defined, which gives its limit from inside.
        math::vec2d centre = uv;
        for (int axis = 0; axis < 2; ++axis)
        {
            const auto a = static_cast<std::size_t>(axis);
            if (std::isfinite(lo[axis]) && std::isfinite(hi[axis]))
                centre[a] = 0.5 * (lo[axis] + hi[axis]);
            else
                centre[a] = uv[a] + 1.0;
        }
        for (double f : {1e-7, 1e-5, 1e-3, 1e-2})
        {
            const math::vec2d moved = uv + (centre - uv) * f;
            derivatives(moved, p, du, dv);
            n = math::cross(du, dv);
            const double s = math::magnitude(du) * math::magnitude(dv);
            if (s > 0.0 && math::magnitude(n) > 1e-10 * s)
                return math::normalized(n);
        }
        return {0.0, 0.0, 1.0};
    }

    math::vec2d surface::newton(const math::vec3d &p, math::vec2d uv) const
    {
        uv = clamp(uv);
        for (int iteration = 0; iteration < 50; ++iteration)
        {
            math::vec3d s;
            math::vec3d su;
            math::vec3d sv;
            derivatives(uv, s, su, sv);
            const math::vec3d r = s - p;
            const double a = math::dot(su, su);
            const double b = math::dot(su, sv);
            const double c = math::dot(sv, sv);
            const double g1 = math::dot(r, su);
            const double g2 = math::dot(r, sv);
            const double damping = 1e-12 * (a + c) + 1e-300;
            const double det = (a + damping) * (c + damping) - b * b;
            if (!(std::abs(det) > 1e-300))
                break;
            const math::vec2d step{-((c + damping) * g1 - b * g2) / det, -((a + damping) * g2 - b * g1) / det};
            if (!std::isfinite(step[0]) || !std::isfinite(step[1]))
                break;
            const math::vec2d next = clamp(uv + step);
            const math::vec2d moved = next - uv;
            uv = next;
            if (std::abs(moved[0]) <= 1e-14 * (1.0 + std::abs(uv[0])) &&
                std::abs(moved[1]) <= 1e-14 * (1.0 + std::abs(uv[1])))
                break;
        }
        return uv;
    }

    math::vec2d surface::nearest_on_grid(const math::vec3d &p, int resolution) const
    {
        double u0 = std::isfinite(lo[0]) ? lo[0] : -1.0;
        double u1 = std::isfinite(hi[0]) ? hi[0] : 1.0;
        double v0 = std::isfinite(lo[1]) ? lo[1] : -1.0;
        double v1 = std::isfinite(hi[1]) ? hi[1] : 1.0;
        math::vec2d best_uv{u0, v0};
        double best = std::numeric_limits<double>::infinity();
        for (int i = 0; i <= resolution; ++i)
        {
            for (int j = 0; j <= resolution; ++j)
            {
                const math::vec2d uv{u0 + (u1 - u0) * i / resolution, v0 + (v1 - v0) * j / resolution};
                const double d = math::distance_squared(point(uv), p);
                if (d < best)
                {
                    best = d;
                    best_uv = uv;
                }
            }
        }
        return best_uv;
    }

    math::vec2d surface::project(const math::vec3d &p, const math::vec2d *hint) const
    {
        math::vec2d uv = hint != nullptr ? *hint : nearest_on_grid(p, 16);
        if (hint != nullptr)
        {
            for (int axis = 0; axis < 2; ++axis)
            {
                // Bring a hint from an unwrapped neighbour into the domain.
                const auto a = static_cast<std::size_t>(axis);
                if (periodic[axis])
                    uv[a] = lo[axis] + std::fmod(std::fmod(uv[a] - lo[axis], period[axis]) + period[axis], period[axis]);
            }
        }
        uv = newton(p, uv);
        if (hint != nullptr)
        {
            // A hint that led astray: start over from the grid, keep the better.
            const double d = math::distance(point(uv), p);
            if (d > 1e-6 * size)
            {
                const math::vec2d other = newton(p, nearest_on_grid(p, 24));
                if (math::distance(point(other), p) < d)
                    uv = other;
            }
            for (int axis = 0; axis < 2; ++axis)
            {
                const auto a = static_cast<std::size_t>(axis);
                if (periodic[axis])
                    uv[a] = unwrap(uv[a], period[axis], (*hint)[a]);
            }
        }
        return uv;
    }

    bool surface::singular(const math::vec2d &uv, int axis) const
    {
        math::vec3d p;
        math::vec3d du;
        math::vec3d dv;
        derivatives(uv, p, du, dv);
        double extent = 1.0;
        if (periodic[axis])
            extent = period[axis];
        else if (std::isfinite(lo[axis]) && std::isfinite(hi[axis]))
            extent = hi[axis] - lo[axis];
        return math::magnitude(axis == 0 ? du : dv) * extent < 1e-7 * size;
    }

    std::optional<double> surface::pole(int axis, bool upper) const
    {
        const int other = 1 - axis;
        if (!std::isfinite(lo[axis]) || !std::isfinite(hi[axis]) || !std::isfinite(lo[other]) ||
            !std::isfinite(hi[other]) || periodic[other])
            return std::nullopt;
        const double at = upper ? hi[other] : lo[other];
        math::vec2d uv{};
        uv[static_cast<std::size_t>(other)] = at;
        uv[static_cast<std::size_t>(axis)] = lo[axis];
        const math::vec3d first = point(uv);
        for (int i = 1; i <= 8; ++i)
        {
            uv[static_cast<std::size_t>(axis)] = lo[axis] + (hi[axis] - lo[axis]) * i / 8.0;
            if (math::distance(point(uv), first) > 1e-7 * size)
                return std::nullopt;
        }
        return at;
    }

    // ---------------------------------------------------------------------
    // plane
    // ---------------------------------------------------------------------

    plane_surface::plane_surface(const step::frame &frame) : m_frame(frame)
    {
        planar = true;
    }

    math::vec3d plane_surface::point(const math::vec2d &uv) const
    {
        return m_frame.origin + m_frame.x * uv[0] + m_frame.y * uv[1];
    }

    void plane_surface::derivatives(const math::vec2d &uv, math::vec3d &p, math::vec3d &du, math::vec3d &dv) const
    {
        p = point(uv);
        du = m_frame.x;
        dv = m_frame.y;
    }

    math::vec2d plane_surface::project(const math::vec3d &p, const math::vec2d *) const
    {
        const math::vec3d d = p - m_frame.origin;
        return {math::dot(d, m_frame.x), math::dot(d, m_frame.y)};
    }

    // ---------------------------------------------------------------------
    // cylinder and cone
    // ---------------------------------------------------------------------

    cone_surface::cone_surface(const step::frame &frame, double radius, double semi_angle)
        : m_frame(frame), m_radius(radius), m_tan(std::tan(semi_angle))
    {
        periodic[0] = true;
        period[0] = two_pi;
        lo[0] = 0.0;
        hi[0] = two_pi;
        size = std::max(radius, 1e-9);
    }

    math::vec3d cone_surface::point(const math::vec2d &uv) const
    {
        const double rho = m_radius + uv[1] * m_tan;
        return m_frame.origin + radial(m_frame, uv[0]) * rho + m_frame.z * uv[1];
    }

    void cone_surface::derivatives(const math::vec2d &uv, math::vec3d &p, math::vec3d &du, math::vec3d &dv) const
    {
        const double rho = m_radius + uv[1] * m_tan;
        const math::vec3d r = radial(m_frame, uv[0]);
        p = m_frame.origin + r * rho + m_frame.z * uv[1];
        du = tangential(m_frame, uv[0]) * rho;
        dv = r * m_tan + m_frame.z;
    }

    math::vec3d cone_surface::normal(const math::vec2d &uv) const
    {
        const double rho = m_radius + uv[1] * m_tan;
        const math::vec3d n = math::normalized(radial(m_frame, uv[0]) - m_frame.z * m_tan);
        return rho < -1e-9 * size ? math::vec3d(-n) : n;
    }

    math::vec2d cone_surface::project(const math::vec3d &p, const math::vec2d *hint) const
    {
        const math::vec3d d = p - m_frame.origin;
        const double x = math::dot(d, m_frame.x);
        const double y = math::dot(d, m_frame.y);
        const double v = math::dot(d, m_frame.z);
        double u = std::atan2(y, x);
        // Beyond the apex the radius is negative and the angle opposite.
        if (m_radius + v * m_tan < 0.0)
            u = std::atan2(-y, -x);
        if (hint != nullptr)
            u = unwrap(u, two_pi, (*hint)[0]);
        return {u, v};
    }

    bool cone_surface::singular(const math::vec2d &uv, int axis) const
    {
        return axis == 0 && std::abs(m_radius + uv[1] * m_tan) <= 1e-9 * std::max(size, 1.0);
    }

    std::optional<double> cone_surface::pole(int axis, bool) const
    {
        if (axis != 0 || std::abs(m_tan) < 1e-12)
            return std::nullopt;
        return -m_radius / m_tan;
    }

    // ---------------------------------------------------------------------
    // sphere
    // ---------------------------------------------------------------------

    sphere_surface::sphere_surface(const step::frame &frame, double radius) : m_frame(frame), m_radius(radius)
    {
        periodic[0] = true;
        period[0] = two_pi;
        lo[0] = 0.0;
        hi[0] = two_pi;
        lo[1] = -pi / 2.0;
        hi[1] = pi / 2.0;
        size = radius;
    }

    math::vec3d sphere_surface::point(const math::vec2d &uv) const
    {
        return m_frame.origin + radial(m_frame, uv[0]) * (m_radius * std::cos(uv[1])) +
               m_frame.z * (m_radius * std::sin(uv[1]));
    }

    void sphere_surface::derivatives(const math::vec2d &uv, math::vec3d &p, math::vec3d &du, math::vec3d &dv) const
    {
        const math::vec3d r = radial(m_frame, uv[0]);
        const double c = std::cos(uv[1]);
        const double s = std::sin(uv[1]);
        p = m_frame.origin + r * (m_radius * c) + m_frame.z * (m_radius * s);
        du = tangential(m_frame, uv[0]) * (m_radius * c);
        dv = r * (-m_radius * s) + m_frame.z * (m_radius * c);
    }

    math::vec3d sphere_surface::normal(const math::vec2d &uv) const
    {
        return radial(m_frame, uv[0]) * std::cos(uv[1]) + m_frame.z * std::sin(uv[1]);
    }

    math::vec2d sphere_surface::project(const math::vec3d &p, const math::vec2d *hint) const
    {
        const math::vec3d d = p - m_frame.origin;
        const double x = math::dot(d, m_frame.x);
        const double y = math::dot(d, m_frame.y);
        const double z = math::dot(d, m_frame.z);
        const double horizontal = std::sqrt(x * x + y * y);
        const double v = std::atan2(z, horizontal);
        double u = horizontal > 1e-12 * m_radius ? std::atan2(y, x) : (hint != nullptr ? (*hint)[0] : 0.0);
        if (hint != nullptr)
            u = unwrap(u, two_pi, (*hint)[0]);
        return {u, v};
    }

    bool sphere_surface::singular(const math::vec2d &uv, int axis) const
    {
        return axis == 0 && std::abs(std::cos(uv[1])) < 1e-9;
    }

    std::optional<double> sphere_surface::pole(int axis, bool upper) const
    {
        if (axis != 0)
            return std::nullopt;
        return upper ? pi / 2.0 : -pi / 2.0;
    }

    // ---------------------------------------------------------------------
    // torus
    // ---------------------------------------------------------------------

    torus_surface::torus_surface(const step::frame &frame, double major, double minor)
        : m_frame(frame), m_major(major), m_minor(minor)
    {
        for (int axis = 0; axis < 2; ++axis)
        {
            periodic[axis] = true;
            period[axis] = two_pi;
            lo[axis] = 0.0;
            hi[axis] = two_pi;
        }
        size = major + minor;
    }

    math::vec3d torus_surface::point(const math::vec2d &uv) const
    {
        return m_frame.origin + radial(m_frame, uv[0]) * (m_major + m_minor * std::cos(uv[1])) +
               m_frame.z * (m_minor * std::sin(uv[1]));
    }

    void torus_surface::derivatives(const math::vec2d &uv, math::vec3d &p, math::vec3d &du, math::vec3d &dv) const
    {
        const math::vec3d r = radial(m_frame, uv[0]);
        const double c = std::cos(uv[1]);
        const double s = std::sin(uv[1]);
        p = m_frame.origin + r * (m_major + m_minor * c) + m_frame.z * (m_minor * s);
        du = tangential(m_frame, uv[0]) * (m_major + m_minor * c);
        dv = r * (-m_minor * s) + m_frame.z * (m_minor * c);
    }

    math::vec3d torus_surface::normal(const math::vec2d &uv) const
    {
        const math::vec3d n = radial(m_frame, uv[0]) * std::cos(uv[1]) + m_frame.z * std::sin(uv[1]);
        return m_major + m_minor * std::cos(uv[1]) < 0.0 ? math::vec3d(-n) : n;
    }

    math::vec2d torus_surface::project(const math::vec3d &p, const math::vec2d *hint) const
    {
        const math::vec3d d = p - m_frame.origin;
        const double x = math::dot(d, m_frame.x);
        const double y = math::dot(d, m_frame.y);
        const double z = math::dot(d, m_frame.z);
        double u = std::atan2(y, x);
        double v = std::atan2(z, std::sqrt(x * x + y * y) - m_major);
        if (hint != nullptr)
        {
            u = unwrap(u, two_pi, (*hint)[0]);
            v = unwrap(v, two_pi, (*hint)[1]);
        }
        return {u, v};
    }

    bool torus_surface::singular(const math::vec2d &uv, int axis) const
    {
        return axis == 0 && std::abs(m_major + m_minor * std::cos(uv[1])) < 1e-9 * size;
    }

    // ---------------------------------------------------------------------
    // linear extrusion
    // ---------------------------------------------------------------------

    extrusion_surface::extrusion_surface(std::shared_ptr<const curve> curve, const math::vec3d &direction)
        : m_curve(std::move(curve)), m_direction(direction)
    {
        lo[0] = m_curve->start;
        hi[0] = m_curve->end;
        periodic[0] = m_curve->periodic;
        period[0] = m_curve->period;
        if (std::isfinite(lo[0]) && std::isfinite(hi[0]))
        {
            size = 0.0;
            const math::vec3d first = m_curve->point(lo[0]);
            for (int i = 1; i <= 8; ++i)
                size = std::max(size, math::distance(first, m_curve->point(lo[0] + (hi[0] - lo[0]) * i / 8.0)));
        }
        size = std::max(size, math::magnitude(direction));
    }

    math::vec3d extrusion_surface::point(const math::vec2d &uv) const
    {
        return m_curve->point(uv[0]) + m_direction * uv[1];
    }

    void extrusion_surface::derivatives(const math::vec2d &uv, math::vec3d &p, math::vec3d &du, math::vec3d &dv) const
    {
        p = point(uv);
        du = m_curve->derivative(uv[0]);
        dv = m_direction;
    }

    math::vec2d extrusion_surface::project(const math::vec3d &p, const math::vec2d *hint) const
    {
        // Along the extrusion first: the curve parameter of p moved back
        // onto the curve's own plane is a good start.
        const double length2 = math::dot(m_direction, m_direction);
        double u = hint != nullptr ? (*hint)[0] : m_curve->project(p);
        if (hint == nullptr)
        {
            for (int i = 0; i < 2; ++i)
            {
                const double v = math::dot(p - m_curve->point(u), m_direction) / length2;
                u = m_curve->project(p - m_direction * v);
            }
        }
        const double v = math::dot(p - m_curve->point(u), m_direction) / length2;
        math::vec2d uv = newton(p, {u, v});
        if (hint != nullptr && periodic[0])
            uv[0] = unwrap(uv[0], period[0], (*hint)[0]);
        return uv;
    }

    // ---------------------------------------------------------------------
    // revolution
    // ---------------------------------------------------------------------

    revolution_surface::revolution_surface(std::shared_ptr<const curve> curve, const math::vec3d &origin,
                                           const math::vec3d &axis)
        : m_curve(std::move(curve)), m_origin(origin), m_axis(math::normalized(axis))
    {
        periodic[0] = true;
        period[0] = two_pi;
        lo[0] = 0.0;
        hi[0] = two_pi;
        lo[1] = m_curve->start;
        hi[1] = m_curve->end;
        periodic[1] = m_curve->periodic;
        period[1] = m_curve->period;

        // The half-plane the curve lies in, as seen from the axis.
        m_reference = {};
        size = 0.0;
        const double a = std::isfinite(lo[1]) ? lo[1] : -1.0;
        const double b = std::isfinite(hi[1]) ? hi[1] : 1.0;
        for (int i = 0; i <= 16; ++i)
        {
            const math::vec3d w = m_curve->point(a + (b - a) * i / 16.0) - m_origin;
            const math::vec3d away = w - m_axis * math::dot(w, m_axis);
            m_reference += away;
            size = std::max(size, math::magnitude(w));
        }
        math::vec3d fallback = std::abs(m_axis[0]) < 0.9 ? math::vec3d{1.0, 0.0, 0.0} : math::vec3d{0.0, 1.0, 0.0};
        fallback = fallback - m_axis * math::dot(fallback, m_axis);
        m_reference = unit_or(m_reference, math::normalized(fallback));
        size = std::max(size, 1e-9);
    }

    math::vec3d revolution_surface::rotate(const math::vec3d &v, double angle) const
    {
        const double c = std::cos(angle);
        const double s = std::sin(angle);
        return v * c + math::cross(m_axis, v) * s + m_axis * (math::dot(m_axis, v) * (1.0 - c));
    }

    math::vec3d revolution_surface::point(const math::vec2d &uv) const
    {
        return m_origin + rotate(m_curve->point(uv[1]) - m_origin, uv[0]);
    }

    void revolution_surface::derivatives(const math::vec2d &uv, math::vec3d &p, math::vec3d &du,
                                         math::vec3d &dv) const
    {
        const math::vec3d w = rotate(m_curve->point(uv[1]) - m_origin, uv[0]);
        p = m_origin + w;
        du = math::cross(m_axis, w);
        dv = rotate(m_curve->derivative(uv[1]), uv[0]);
    }

    math::vec2d revolution_surface::project(const math::vec3d &p, const math::vec2d *hint) const
    {
        const math::vec3d w = p - m_origin;
        const math::vec3d side = math::cross(m_axis, m_reference);
        double u = std::atan2(math::dot(w, side), math::dot(w, m_reference));
        const math::vec3d back = m_origin + rotate(w, -u);
        double v = m_curve->project(back);
        if (hint != nullptr)
        {
            u = unwrap(u, two_pi, (*hint)[0]);
            if (periodic[1])
                v = unwrap(v, period[1], (*hint)[1]);
        }
        math::vec2d uv = newton(p, {u, v});
        if (hint != nullptr)
        {
            uv[0] = unwrap(uv[0], two_pi, (*hint)[0]);
            if (periodic[1])
                uv[1] = unwrap(uv[1], period[1], (*hint)[1]);
        }
        return uv;
    }

    std::optional<double> revolution_surface::pole(int axis, bool upper) const
    {
        if (axis != 0 || periodic[1] || !std::isfinite(lo[1]) || !std::isfinite(hi[1]))
            return std::nullopt;
        const double at = upper ? hi[1] : lo[1];
        const math::vec3d w = m_curve->point(at) - m_origin;
        const double away = math::magnitude(w - m_axis * math::dot(w, m_axis));
        if (away > 1e-7 * size)
            return std::nullopt;
        return at;
    }

    // ---------------------------------------------------------------------
    // B-spline surface
    // ---------------------------------------------------------------------

    bspline_surface::bspline_surface(bspline_basis u, bspline_basis v, std::vector<math::vec3d> points,
                                     std::vector<double> weights)
        : m_u(std::move(u)), m_v(std::move(v)), m_points(std::move(points)), m_weights(std::move(weights))
    {
        m_valid = m_u.valid() && m_v.valid() &&
                  m_points.size() == static_cast<std::size_t>(m_u.count()) * static_cast<std::size_t>(m_v.count()) &&
                  (m_weights.empty() || m_weights.size() == m_points.size());
        for (double w : m_weights)
            m_valid = m_valid && w > 0.0;
        if (!m_valid)
            return;

        lo[0] = m_u.start();
        hi[0] = m_u.end();
        lo[1] = m_v.start();
        hi[1] = m_v.end();

        size = 0.0;
        for (const math::vec3d &p : m_points)
            size = std::max(size, math::distance(p, m_points.front()));
        size = std::max(size, 1e-9);

        // Closed along an axis: opposite edges coincide.
        for (int axis = 0; axis < 2; ++axis)
        {
            const int other = 1 - axis;
            bool closed = true;
            for (int i = 0; i <= 8 && closed; ++i)
            {
                math::vec2d a{};
                a[static_cast<std::size_t>(other)] = lo[other] + (hi[other] - lo[other]) * i / 8.0;
                math::vec2d b = a;
                a[static_cast<std::size_t>(axis)] = lo[axis];
                b[static_cast<std::size_t>(axis)] = hi[axis];
                closed = math::distance(point(a), point(b)) <= 1e-7 * size;
            }
            if (closed)
            {
                periodic[axis] = true;
                period[axis] = hi[axis] - lo[axis];
            }
        }

        const auto spans = [](const bspline_basis &basis) { return static_cast<int>(basis.breaks().size()) - 1; };
        m_grid = std::clamp(4 * std::max(spans(m_u), spans(m_v)), 12, 48);
    }

    void bspline_surface::derivatives(const math::vec2d &uv, math::vec3d &p, math::vec3d &du, math::vec3d &dv) const
    {
        if (!m_valid)
        {
            p = du = dv = {};
            return;
        }
        // Outside the domain only through a periodic wrap.
        math::vec2d at = uv;
        for (int axis = 0; axis < 2; ++axis)
        {
            const auto a = static_cast<std::size_t>(axis);
            if (periodic[axis])
                at[a] = lo[axis] + std::fmod(std::fmod(at[a] - lo[axis], period[axis]) + period[axis], period[axis]);
        }

        const int su = m_u.span(at[0]);
        const int sv = m_v.span(at[1]);
        const int pu = m_u.degree();
        const int pv = m_v.degree();
        double nu[max_degree + 1];
        double dnu[max_degree + 1];
        double nv[max_degree + 1];
        double dnv[max_degree + 1];
        m_u.evaluate(su, at[0], nu, dnu);
        m_v.evaluate(sv, at[1], nv, dnv);

        math::vec3d a{};
        math::vec3d au{};
        math::vec3d av{};
        double w = 0.0;
        double wu = 0.0;
        double wv = 0.0;
        const auto columns = static_cast<std::size_t>(m_v.count());
        for (int i = 0; i <= pu; ++i)
        {
            const auto row = static_cast<std::size_t>(su - pu + i) * columns;
            for (int j = 0; j <= pv; ++j)
            {
                const std::size_t index = row + static_cast<std::size_t>(sv - pv + j);
                const double weight = m_weights.empty() ? 1.0 : m_weights[index];
                const math::vec3d &cp = m_points[index];
                const double b = nu[i] * nv[j] * weight;
                const double bu = dnu[i] * nv[j] * weight;
                const double bv = nu[i] * dnv[j] * weight;
                a += cp * b;
                au += cp * bu;
                av += cp * bv;
                w += b;
                wu += bu;
                wv += bv;
            }
        }
        if (w == 0.0)
            w = 1.0;
        p = a / w;
        du = (au - p * wu) / w;
        dv = (av - p * wv) / w;
    }

    math::vec3d bspline_surface::point(const math::vec2d &uv) const
    {
        math::vec3d p;
        math::vec3d du;
        math::vec3d dv;
        derivatives(uv, p, du, dv);
        return p;
    }

    math::vec2d bspline_surface::project(const math::vec3d &p, const math::vec2d *hint) const
    {
        if (hint != nullptr)
            return surface::project(p, hint);
        return newton(p, nearest_on_grid(p, m_grid));
    }

    // ---------------------------------------------------------------------
    // From STEP entities
    // ---------------------------------------------------------------------

    namespace
    {
        std::vector<double> expand(const part21::parameter &multiplicities, const part21::parameter &knots)
        {
            std::vector<double> result;
            if (multiplicities.size() != knots.size())
                return result;
            for (std::size_t i = 0; i < knots.size(); ++i)
            {
                const std::int64_t m = multiplicities[i].value().integer();
                if (m < 1 || m > max_degree + 1 || result.size() > 1000000)
                    return {};
                for (std::int64_t k = 0; k < m; ++k)
                    result.push_back(knots[i].value().number());
            }
            return result;
        }

        std::vector<double> bezier_knots(int degree, int count)
        {
            std::vector<double> knots;
            if ((count - 1) % degree != 0)
                return knots;
            const int segments = (count - 1) / degree;
            for (int i = 0; i <= degree; ++i)
                knots.push_back(0.0);
            for (int s = 1; s < segments; ++s)
                for (int i = 0; i < degree; ++i)
                    knots.push_back(static_cast<double>(s));
            for (int i = 0; i <= degree; ++i)
                knots.push_back(static_cast<double>(segments));
            return knots;
        }

        std::shared_ptr<const curve> swept_curve(const step::access &access, const part21::parameter &reference,
                                                 int depth, std::string &unsupported)
        {
            curve_result result = make_curve(access, access.resolve(reference), depth + 1);
            if (!result.curve)
                unsupported = result.unsupported;
            return result.curve;
        }
    } // namespace

    surface_result make_surface(const step::access &access, const part21::entity &entity, double angle_unit,
                                int depth)
    {
        using step::type;
        if (!entity.valid())
            return {nullptr, "(missing surface)"};
        if (depth > 16)
            return {nullptr, "(surfaces nested too deeply)"};

        if (access.is(entity, type::plane))
        {
            const auto frame = access.placement(access.part(entity, type::plane)[1]);
            if (!frame)
                return {nullptr, "PLANE (invalid)"};
            return {std::make_shared<plane_surface>(*frame), {}};
        }

        if (access.is(entity, type::cylindrical_surface))
        {
            const part21::parameter a = access.part(entity, type::cylindrical_surface);
            const auto frame = access.placement(a[1]);
            const double radius = a[2].value().number();
            if (!frame || !(radius > 0.0))
                return {nullptr, "CYLINDRICAL_SURFACE (invalid)"};
            return {std::make_shared<cone_surface>(*frame, radius, 0.0), {}};
        }

        if (access.is(entity, type::conical_surface))
        {
            const part21::parameter a = access.part(entity, type::conical_surface);
            const auto frame = access.placement(a[1]);
            const double radius = a[2].value().number();
            const double semi_angle = a[3].value().number() * angle_unit;
            if (!frame || !(radius >= 0.0) || !(std::abs(semi_angle) > 1e-12) || !(std::abs(semi_angle) < pi / 2.0))
                return {nullptr, "CONICAL_SURFACE (invalid)"};
            return {std::make_shared<cone_surface>(*frame, radius, semi_angle), {}};
        }

        if (access.is(entity, type::spherical_surface))
        {
            const part21::parameter a = access.part(entity, type::spherical_surface);
            const auto frame = access.placement(a[1]);
            const double radius = a[2].value().number();
            if (!frame || !(radius > 0.0))
                return {nullptr, "SPHERICAL_SURFACE (invalid)"};
            return {std::make_shared<sphere_surface>(*frame, radius), {}};
        }

        if (access.is(entity, type::toroidal_surface) || access.is(entity, type::degenerate_toroidal_surface))
        {
            const type t =
                access.is(entity, type::toroidal_surface) ? type::toroidal_surface : type::degenerate_toroidal_surface;
            const part21::parameter a = access.part(entity, t);
            const auto frame = access.placement(a[1]);
            const double major = a[2].value().number();
            const double minor = a[3].value().number();
            if (!frame || !(major >= 0.0) || !(minor > 0.0))
                return {nullptr, "TOROIDAL_SURFACE (invalid)"};
            return {std::make_shared<torus_surface>(*frame, major, minor), {}};
        }

        if (access.is(entity, type::surface_of_linear_extrusion))
        {
            const part21::parameter a = access.part(entity, type::surface_of_linear_extrusion);
            std::string unsupported;
            auto curve = swept_curve(access, a[1], depth, unsupported);
            if (!curve)
                return {nullptr, unsupported};
            const part21::parameter v = access.part(access.resolve(a[2]), type::vector);
            const auto direction = access.direction(v[1]);
            const double magnitude = v[2].value().number();
            if (!direction || !(magnitude > 0.0))
                return {nullptr, "SURFACE_OF_LINEAR_EXTRUSION (invalid)"};
            return {std::make_shared<extrusion_surface>(std::move(curve), *direction * magnitude), {}};
        }

        if (access.is(entity, type::surface_of_revolution))
        {
            const part21::parameter a = access.part(entity, type::surface_of_revolution);
            std::string unsupported;
            auto curve = swept_curve(access, a[1], depth, unsupported);
            if (!curve)
                return {nullptr, unsupported};
            const auto axis = access.placement(a[2]);
            if (!axis)
                return {nullptr, "SURFACE_OF_REVOLUTION (invalid)"};
            return {std::make_shared<revolution_surface>(std::move(curve), axis->origin, axis->z), {}};
        }

        if (access.is(entity, type::b_spline_surface) || access.is(entity, type::b_spline_surface_with_knots) ||
            access.is(entity, type::bezier_surface) || access.is(entity, type::uniform_surface) ||
            access.is(entity, type::quasi_uniform_surface))
        {
            part21::parameter u_degree;
            part21::parameter v_degree;
            part21::parameter grid;
            part21::parameter u_multiplicities;
            part21::parameter v_multiplicities;
            part21::parameter u_knots;
            part21::parameter v_knots;
            const bool with_knots = access.is(entity, type::b_spline_surface_with_knots);
            if (!entity.is_complex())
            {
                const part21::parameter a = entity.parameters();
                u_degree = a[1];
                v_degree = a[2];
                grid = a[3];
                u_multiplicities = a[8];
                v_multiplicities = a[9];
                u_knots = a[10];
                v_knots = a[11];
            }
            else
            {
                const part21::parameter s = access.part(entity, type::b_spline_surface);
                u_degree = s[0];
                v_degree = s[1];
                grid = s[2];
                const part21::parameter k = access.part(entity, type::b_spline_surface_with_knots);
                u_multiplicities = k[0];
                v_multiplicities = k[1];
                u_knots = k[2];
                v_knots = k[3];
            }

            const std::int64_t pu = u_degree.value().integer();
            const std::int64_t pv = v_degree.value().integer();
            if (pu < 1 || pu > max_degree || pv < 1 || pv > max_degree)
                return {nullptr, "B_SPLINE_SURFACE (invalid degree)"};

            const std::size_t rows = grid.size();
            const std::size_t columns = rows > 0 ? grid[0].size() : 0;
            if (rows == 0 || columns == 0 || rows * columns > 4000000)
                return {nullptr, "B_SPLINE_SURFACE (invalid control points)"};
            std::vector<math::vec3d> points;
            points.reserve(rows * columns);
            for (std::size_t i = 0; i < rows; ++i)
            {
                if (grid[i].size() != columns)
                    return {nullptr, "B_SPLINE_SURFACE (ragged control points)"};
                for (std::size_t j = 0; j < columns; ++j)
                {
                    const auto p = access.point(grid[i][j]);
                    if (!p)
                        return {nullptr, "B_SPLINE_SURFACE (invalid control point)"};
                    points.push_back(*p);
                }
            }

            std::vector<double> weights;
            if (const part21::parameter rational = access.part(entity, type::rational_b_spline_surface);
                rational.is(part21::kind::list))
            {
                const part21::parameter w = rational[0];
                if (w.size() != rows)
                    return {nullptr, "B_SPLINE_SURFACE (invalid weights)"};
                for (std::size_t i = 0; i < rows; ++i)
                {
                    if (w[i].size() != columns)
                        return {nullptr, "B_SPLINE_SURFACE (invalid weights)"};
                    for (std::size_t j = 0; j < columns; ++j)
                        weights.push_back(w[i][j].value().number());
                }
            }

            const int nu = static_cast<int>(rows);
            const int nv = static_cast<int>(columns);
            std::vector<double> ku;
            std::vector<double> kv;
            if (with_knots)
            {
                ku = expand(u_multiplicities, u_knots);
                kv = expand(v_multiplicities, v_knots);
            }
            else if (access.is(entity, type::bezier_surface))
            {
                ku = bezier_knots(static_cast<int>(pu), nu);
                kv = bezier_knots(static_cast<int>(pv), nv);
            }
            else
            {
                const bool uniform = access.is(entity, type::uniform_surface);
                const auto knots = [uniform](int p, int n) {
                    std::vector<double> k;
                    if (uniform)
                    {
                        for (int i = 0; i < n + p + 1; ++i)
                            k.push_back(static_cast<double>(i - p));
                    }
                    else
                    {
                        for (int i = 0; i <= p; ++i)
                            k.push_back(0.0);
                        for (int i = 1; i < n - p; ++i)
                            k.push_back(static_cast<double>(i));
                        for (int i = 0; i <= p; ++i)
                            k.push_back(static_cast<double>(n - p));
                    }
                    return k;
                };
                ku = knots(static_cast<int>(pu), nu);
                kv = knots(static_cast<int>(pv), nv);
            }

            auto surface = std::make_shared<bspline_surface>(bspline_basis(static_cast<int>(pu), std::move(ku), nu),
                                                             bspline_basis(static_cast<int>(pv), std::move(kv), nv),
                                                             std::move(points), std::move(weights));
            if (!surface->valid())
                return {nullptr, "B_SPLINE_SURFACE (invalid knots or weights)"};
            return {std::move(surface), {}};
        }

        return {nullptr, access.describe(entity)};
    }

} // namespace volt::model::geometry
