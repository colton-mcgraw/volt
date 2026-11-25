#include "curves.hpp"

#include <math/geometry.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace volt::model::geometry
{

    namespace
    {
        constexpr double two_pi = 2.0 * std::numbers::pi;

        double distance_to_segment(const math::vec3d &p, const math::vec3d &a, const math::vec3d &b)
        {
            const math::vec3d ab = b - a;
            const double length2 = math::dot(ab, ab);
            double t = length2 > 0.0 ? math::dot(p - a, ab) / length2 : 0.0;
            t = std::clamp(t, 0.0, 1.0);
            return math::distance(p, a + ab * t);
        }

        double angle_between(const math::vec3d &a, const math::vec3d &b)
        {
            const double la = math::magnitude(a);
            const double lb = math::magnitude(b);
            if (la <= 0.0 || lb <= 0.0)
                return 0.0;
            return std::acos(std::clamp(math::dot(a, b) / (la * lb), -1.0, 1.0));
        }
    } // namespace

    // ---------------------------------------------------------------------
    // curve
    // ---------------------------------------------------------------------

    double curve::newton(const math::vec3d &p, double t) const
    {
        for (int iteration = 0; iteration < 32; ++iteration)
        {
            const math::vec3d r = point(t) - p;
            const math::vec3d d = derivative(t);
            const double denominator = math::dot(d, d);
            if (!(denominator > 1e-300))
                break;
            double step = -math::dot(r, d) / denominator;
            double next = t + step;
            if (!periodic && std::isfinite(start) && std::isfinite(end))
                next = std::clamp(next, start, end);
            step = next - t;
            t = next;
            if (std::abs(step) <= 1e-14 * (1.0 + std::abs(t)))
                break;
        }
        return t;
    }

    double curve::project(const math::vec3d &p) const
    {
        if (!std::isfinite(start) || !std::isfinite(end))
            return newton(p, 0.0);
        constexpr int samples = 64;
        double best_t = start;
        double best = std::numeric_limits<double>::infinity();
        for (int i = 0; i <= samples; ++i)
        {
            const double t = start + (end - start) * i / samples;
            const double d = math::distance_squared(point(t), p);
            if (d < best)
            {
                best = d;
                best_t = t;
            }
        }
        return newton(p, best_t);
    }

    void curve::adaptive(double t0, double t1, int initial, const sampling &sampling, std::vector<double> &out) const
    {
        struct span
        {
            double a;
            double b;
            int depth;
        };

        initial = std::max(1, initial);
        std::vector<span> stack;
        for (int i = initial - 1; i >= 0; --i)
            stack.push_back({t0 + (t1 - t0) * i / initial, t0 + (t1 - t0) * (i + 1) / initial, 0});

        // Depth first, latest pieces last, so the parameters come out in order.
        bool first = true;
        while (!stack.empty())
        {
            const span s = stack.back();
            stack.pop_back();

            const math::vec3d pa = point(s.a);
            const math::vec3d pb = point(s.b);
            bool split = false;
            if (s.depth < 20 && out.size() < sampling.max_points)
            {
                for (double f : {0.25, 0.5, 0.75})
                {
                    if (distance_to_segment(point(s.a + (s.b - s.a) * f), pa, pb) > sampling.chord)
                    {
                        split = true;
                        break;
                    }
                }
                if (!split && angle_between(derivative(s.a), derivative(s.b)) > sampling.angle)
                    split = true;
            }

            if (split)
            {
                const double m = 0.5 * (s.a + s.b);
                stack.push_back({m, s.b, s.depth + 1});
                stack.push_back({s.a, m, s.depth + 1});
                continue;
            }

            if (!first)
                out.push_back(s.a);
            first = false;
        }
    }

    void curve::sample(double t0, double t1, const sampling &sampling, std::vector<double> &out) const
    {
        adaptive(t0, t1, 4, sampling, out);
    }

    // ---------------------------------------------------------------------
    // line
    // ---------------------------------------------------------------------

    line_curve::line_curve(const math::vec3d &origin, const math::vec3d &direction)
        : m_origin(origin), m_direction(direction)
    {
    }

    double line_curve::project(const math::vec3d &p) const
    {
        const double length2 = math::dot(m_direction, m_direction);
        return length2 > 0.0 ? math::dot(p - m_origin, m_direction) / length2 : 0.0;
    }

    // ---------------------------------------------------------------------
    // ellipse
    // ---------------------------------------------------------------------

    ellipse_curve::ellipse_curve(const step::frame &frame, double a, double b) : m_frame(frame), m_a(a), m_b(b)
    {
        start = 0.0;
        end = two_pi;
        periodic = true;
        period = two_pi;
    }

    math::vec3d ellipse_curve::point(double t) const
    {
        return m_frame.origin + m_frame.x * (m_a * std::cos(t)) + m_frame.y * (m_b * std::sin(t));
    }

    math::vec3d ellipse_curve::derivative(double t) const
    {
        return m_frame.x * (-m_a * std::sin(t)) + m_frame.y * (m_b * std::cos(t));
    }

    double ellipse_curve::project(const math::vec3d &p) const
    {
        const math::vec3d d = p - m_frame.origin;
        const double x = math::dot(d, m_frame.x);
        const double y = math::dot(d, m_frame.y);
        const double t = std::atan2(y / m_b, x / m_a);
        return m_a == m_b ? t : newton(p, t);
    }

    void ellipse_curve::sample(double t0, double t1, const sampling &sampling, std::vector<double> &out) const
    {
        // In parameter steps: the tangent turns at most a / b times as fast
        // as the parameter (at the ends of the major axis), and the chord
        // error of a step dt is at most dt^2 a / 8.
        const double big = std::max(m_a, m_b);
        const double small = std::min(m_a, m_b);
        const double step = std::max(std::min(sampling.angle * small / big, std::sqrt(8.0 * sampling.chord / big)), 1e-3);
        const double span = t1 - t0;
        const auto segments =
            std::min(static_cast<std::size_t>(std::ceil(std::abs(span) / step)), sampling.max_points);
        for (std::size_t i = 1; i < segments; ++i)
            out.push_back(t0 + span * static_cast<double>(i) / static_cast<double>(segments));
    }

    // ---------------------------------------------------------------------
    // polyline
    // ---------------------------------------------------------------------

    polyline_curve::polyline_curve(std::vector<math::vec3d> points) : m_points(std::move(points))
    {
        start = 1.0;
        end = static_cast<double>(m_points.size());
    }

    math::vec3d polyline_curve::point(double t) const
    {
        if (m_points.empty())
            return {};
        const double clamped = std::clamp(t - 1.0, 0.0, static_cast<double>(m_points.size() - 1));
        const auto i = std::min(static_cast<std::size_t>(clamped), m_points.size() - 1);
        if (i + 1 >= m_points.size())
            return m_points.back();
        const double f = clamped - static_cast<double>(i);
        return m_points[i] + (m_points[i + 1] - m_points[i]) * f;
    }

    math::vec3d polyline_curve::derivative(double t) const
    {
        if (m_points.size() < 2)
            return {1.0, 0.0, 0.0};
        const double clamped = std::clamp(t - 1.0, 0.0, static_cast<double>(m_points.size() - 1));
        const auto i = std::min(static_cast<std::size_t>(clamped), m_points.size() - 2);
        return m_points[i + 1] - m_points[i];
    }

    double polyline_curve::project(const math::vec3d &p) const
    {
        double best = std::numeric_limits<double>::infinity();
        double best_t = 1.0;
        for (std::size_t i = 0; i + 1 < m_points.size(); ++i)
        {
            const math::vec3d ab = m_points[i + 1] - m_points[i];
            const double length2 = math::dot(ab, ab);
            const double f = length2 > 0.0 ? std::clamp(math::dot(p - m_points[i], ab) / length2, 0.0, 1.0) : 0.0;
            const double d = math::distance_squared(p, m_points[i] + ab * f);
            if (d < best)
            {
                best = d;
                best_t = 1.0 + static_cast<double>(i) + f;
            }
        }
        return best_t;
    }

    void polyline_curve::sample(double t0, double t1, const sampling &, std::vector<double> &out) const
    {
        if (t1 >= t0)
        {
            for (double k = std::floor(t0) + 1.0; k < t1; k += 1.0)
                out.push_back(k);
        }
        else
        {
            for (double k = std::ceil(t0) - 1.0; k > t1; k -= 1.0)
                out.push_back(k);
        }
    }

    // ---------------------------------------------------------------------
    // B-spline
    // ---------------------------------------------------------------------

    bspline_curve::bspline_curve(bspline_basis basis, std::vector<math::vec3d> points, std::vector<double> weights)
        : m_basis(std::move(basis)), m_points(std::move(points)), m_weights(std::move(weights))
    {
        if (!m_basis.valid() || m_points.size() != static_cast<std::size_t>(m_basis.count()) ||
            (!m_weights.empty() && m_weights.size() != m_points.size()))
        {
            m_basis = {};
            return;
        }
        for (double w : m_weights)
        {
            if (!(w > 0.0))
            {
                m_basis = {};
                return;
            }
        }
        start = m_basis.start();
        end = m_basis.end();

        // Closed in position: treat as periodic, so that edges may run
        // across the joint.
        const math::vec3d a = point(start);
        const math::vec3d b = point(end);
        double size = 0.0;
        for (const math::vec3d &p : m_points)
            size = std::max(size, math::distance(p, m_points.front()));
        if (math::distance(a, b) <= 1e-9 * std::max(size, 1e-9))
        {
            periodic = true;
            period = end - start;
        }
    }

    void bspline_curve::evaluate(double t, math::vec3d &position, math::vec3d *tangent) const
    {
        const int span = m_basis.span(t);
        const int p = m_basis.degree();
        double values[max_degree + 1];
        double derivatives[max_degree + 1];
        m_basis.evaluate(span, t, values, derivatives);

        math::vec3d a{};
        math::vec3d da{};
        double w = 0.0;
        double dw = 0.0;
        for (int j = 0; j <= p; ++j)
        {
            const auto index = static_cast<std::size_t>(span - p + j);
            const double weight = m_weights.empty() ? 1.0 : m_weights[index];
            a += m_points[index] * (values[j] * weight);
            da += m_points[index] * (derivatives[j] * weight);
            w += values[j] * weight;
            dw += derivatives[j] * weight;
        }
        if (w == 0.0)
            w = 1.0;
        position = a / w;
        if (tangent != nullptr)
            *tangent = (da - position * dw) / w;
    }

    math::vec3d bspline_curve::point(double t) const
    {
        if (!valid())
            return {};
        math::vec3d p;
        evaluate(t, p, nullptr);
        return p;
    }

    math::vec3d bspline_curve::derivative(double t) const
    {
        if (!valid())
            return {1.0, 0.0, 0.0};
        math::vec3d p;
        math::vec3d d;
        evaluate(t, p, &d);
        return d;
    }

    double bspline_curve::project(const math::vec3d &p) const
    {
        if (!valid())
            return 0.0;
        const std::vector<double> breaks = m_basis.breaks();
        double best = std::numeric_limits<double>::infinity();
        double best_t = start;
        for (std::size_t i = 0; i + 1 < breaks.size(); ++i)
        {
            for (int k = 0; k < 8; ++k)
            {
                const double t = breaks[i] + (breaks[i + 1] - breaks[i]) * k / 8.0;
                const double d = math::distance_squared(point(t), p);
                if (d < best)
                {
                    best = d;
                    best_t = t;
                }
            }
        }
        if (math::distance_squared(point(end), p) < best)
            best_t = end;
        return newton(p, best_t);
    }

    void bspline_curve::sample(double t0, double t1, const sampling &sampling, std::vector<double> &out) const
    {
        if (!valid())
            return;

        // Each knot span separately: the curve is smooth within one, and
        // the knots are where it may turn.
        std::vector<double> cuts{t0};
        const double lo = std::min(t0, t1);
        const double hi = std::max(t0, t1);
        std::vector<double> inside;
        // For a periodic curve sampled across its joint, the knots repeat
        // every period.
        const std::vector<double> breaks = m_basis.breaks();
        const double shift_from = periodic ? std::floor((lo - start) / period) : 0.0;
        const double shift_to = periodic ? std::floor((hi - start) / period) : 0.0;
        for (double k = shift_from; k <= shift_to && k - shift_from < 1000.0; k += 1.0)
        {
            for (double b : breaks)
            {
                const double t = b + k * period;
                if (t > lo + 1e-12 * (hi - lo) && t < hi - 1e-12 * (hi - lo))
                    inside.push_back(t);
            }
        }
        std::sort(inside.begin(), inside.end());
        if (t1 < t0)
            std::reverse(inside.begin(), inside.end());
        cuts.insert(cuts.end(), inside.begin(), inside.end());
        cuts.push_back(t1);

        const int initial = std::max(2, m_basis.degree());
        for (std::size_t i = 0; i + 1 < cuts.size(); ++i)
        {
            if (i != 0)
                out.push_back(cuts[i]);
            adaptive(cuts[i], cuts[i + 1], initial, sampling, out);
        }
    }

    // ---------------------------------------------------------------------
    // From STEP entities
    // ---------------------------------------------------------------------

    namespace
    {
        std::vector<double> expand_knots(const part21::parameter &multiplicities, const part21::parameter &knots)
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

        // The knots of the B-spline forms that do not list them.
        std::vector<double> implicit_knots(step::type type, int degree, int count)
        {
            std::vector<double> knots;
            const int p = degree;
            const int n = count;
            if (type == step::type::uniform_curve || type == step::type::uniform_surface)
            {
                for (int i = 0; i < n + p + 1; ++i)
                    knots.push_back(static_cast<double>(i - p));
            }
            else if (type == step::type::quasi_uniform_curve || type == step::type::quasi_uniform_surface)
            {
                for (int i = 0; i <= p; ++i)
                    knots.push_back(0.0);
                for (int i = 1; i < n - p; ++i)
                    knots.push_back(static_cast<double>(i));
                for (int i = 0; i <= p; ++i)
                    knots.push_back(static_cast<double>(n - p));
            }
            else
            {
                // Bezier: segments of degree p joined end to end.
                if ((n - 1) % p != 0)
                    return {};
                const int segments = (n - 1) / p;
                for (int i = 0; i <= p; ++i)
                    knots.push_back(0.0);
                for (int s = 1; s < segments; ++s)
                    for (int i = 0; i < p; ++i)
                        knots.push_back(static_cast<double>(s));
                for (int i = 0; i <= p; ++i)
                    knots.push_back(static_cast<double>(segments));
            }
            return knots;
        }

        curve_result unsupported(const step::access &access, const part21::entity &entity)
        {
            return {nullptr, access.describe(entity)};
        }
    } // namespace

    curve_result make_curve(const step::access &access, const part21::entity &entity, int depth)
    {
        using step::type;
        if (!entity.valid())
            return {nullptr, "(missing curve)"};
        if (depth > 16)
            return {nullptr, "(curves nested too deeply)"};

        if (access.is(entity, type::line))
        {
            const part21::parameter a = access.part(entity, type::line);
            const auto origin = access.point(a[1]);
            const part21::entity vector = access.resolve(a[2]);
            const part21::parameter v = access.part(vector, type::vector);
            const auto direction = access.direction(v[1]);
            const double magnitude = v[2].value().number();
            if (!origin || !direction || !(magnitude > 0.0))
                return {nullptr, "LINE (invalid)"};
            return {std::make_shared<line_curve>(*origin, *direction * magnitude), {}};
        }

        if (access.is(entity, type::circle) || access.is(entity, type::ellipse))
        {
            const bool circle = access.is(entity, type::circle);
            const part21::parameter a = access.part(entity, circle ? type::circle : type::ellipse);
            const auto frame = access.placement(a[1]);
            const double r1 = a[2].value().number();
            const double r2 = circle ? r1 : a[3].value().number();
            if (!frame || !(r1 > 0.0) || !(r2 > 0.0))
                return {nullptr, circle ? "CIRCLE (invalid)" : "ELLIPSE (invalid)"};
            return {std::make_shared<ellipse_curve>(*frame, r1, r2), {}};
        }

        if (access.is(entity, type::polyline))
        {
            const part21::parameter list = access.part(entity, type::polyline)[1];
            std::vector<math::vec3d> points;
            for (std::size_t i = 0; i < list.size(); ++i)
            {
                const auto p = access.point(list[i]);
                if (!p)
                    return {nullptr, "POLYLINE (invalid)"};
                points.push_back(*p);
            }
            if (points.size() < 2)
                return {nullptr, "POLYLINE (invalid)"};
            return {std::make_shared<polyline_curve>(std::move(points)), {}};
        }

        if (access.is(entity, type::b_spline_curve) || access.is(entity, type::b_spline_curve_with_knots) ||
            access.is(entity, type::bezier_curve) || access.is(entity, type::uniform_curve) ||
            access.is(entity, type::quasi_uniform_curve))
        {
            // Simple: (name, degree, points, form, closed, self_intersect
            // [, multiplicities, knots, knot_spec]). Complex: those split over
            // the B_SPLINE_CURVE and B_SPLINE_CURVE_WITH_KNOTS parts.
            part21::parameter degree_value;
            part21::parameter point_list;
            part21::parameter multiplicities;
            part21::parameter knot_list;
            type form = type::b_spline_curve_with_knots;
            if (!entity.is_complex())
            {
                const part21::parameter a = entity.parameters();
                degree_value = a[1];
                point_list = a[2];
                multiplicities = a[6];
                knot_list = a[7];
                form = access.type_of(entity);
            }
            else
            {
                const part21::parameter curve = access.part(entity, type::b_spline_curve);
                degree_value = curve[0];
                point_list = curve[1];
                const part21::parameter knots = access.part(entity, type::b_spline_curve_with_knots);
                multiplicities = knots[0];
                knot_list = knots[1];
                for (type t : {type::b_spline_curve_with_knots, type::bezier_curve, type::uniform_curve,
                               type::quasi_uniform_curve})
                {
                    if (access.is(entity, t))
                    {
                        form = t;
                        break;
                    }
                }
            }

            const std::int64_t degree = degree_value.value().integer();
            if (degree < 1 || degree > max_degree)
                return {nullptr, "B_SPLINE_CURVE (invalid degree)"};

            std::vector<math::vec3d> points;
            points.reserve(point_list.size());
            for (std::size_t i = 0; i < point_list.size(); ++i)
            {
                const auto p = access.point(point_list[i]);
                if (!p)
                    return {nullptr, "B_SPLINE_CURVE (invalid control point)"};
                points.push_back(*p);
            }

            const int count = static_cast<int>(points.size());
            std::vector<double> knots = form == type::b_spline_curve_with_knots
                                            ? expand_knots(multiplicities, knot_list)
                                            : implicit_knots(form, static_cast<int>(degree), count);

            std::vector<double> weights;
            if (const part21::parameter rational = access.part(entity, type::rational_b_spline_curve);
                rational.is(part21::kind::list))
            {
                const part21::parameter list = rational[0];
                for (std::size_t i = 0; i < list.size(); ++i)
                    weights.push_back(list[i].value().number());
            }

            auto curve = std::make_shared<bspline_curve>(bspline_basis(static_cast<int>(degree), std::move(knots), count),
                                                         std::move(points), std::move(weights));
            if (!curve->valid())
                return {nullptr, "B_SPLINE_CURVE (invalid knots or weights)"};
            return {std::move(curve), {}};
        }

        if (access.is(entity, type::trimmed_curve))
            return make_curve(access, access.resolve(access.part(entity, type::trimmed_curve)[1]), depth + 1);

        for (type t : {type::surface_curve, type::seam_curve, type::intersection_curve, type::bounded_surface_curve})
        {
            if (access.is(entity, t))
                return make_curve(access, access.resolve(access.part(entity, t)[1]), depth + 1);
        }

        return unsupported(access, entity);
    }

} // namespace volt::model::geometry
