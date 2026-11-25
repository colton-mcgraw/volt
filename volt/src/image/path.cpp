#include <image/path.hpp>

#include "flatten.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace volt::image
{

    namespace
    {
        // The handle length that makes a cubic follow a quarter circle.
        constexpr float kappa = 0.5522847498f;

        math::vec2f lerp(math::vec2f a, math::vec2f b, float t)
        {
            return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t};
        }

        float length(math::vec2f v)
        {
            return std::sqrt(v[0] * v[0] + v[1] * v[1]);
        }
    } // namespace

    math::vec2f apply(const math::mat3f &m, math::vec2f p) noexcept
    {
        return {m(0, 0) * p[0] + m(0, 1) * p[1] + m(0, 2), m(1, 0) * p[0] + m(1, 1) * p[1] + m(1, 2)};
    }

    float max_scale(const math::mat3f &m) noexcept
    {
        // The largest singular value of the linear part.
        const float a = m(0, 0);
        const float b = m(0, 1);
        const float c = m(1, 0);
        const float d = m(1, 1);
        const float s = a * a + b * b + c * c + d * d;
        const float det = a * d - b * c;
        const float root = std::sqrt(std::max(0.0f, s * s - 4.0f * det * det));
        return std::sqrt(std::max(0.0f, (s + root) / 2.0f));
    }

    // ---- building -----------------------------------------------------------

    void path::ensure_subpath()
    {
        if (!m_open)
            move_to(m_current);
    }

    void path::move_to(math::vec2f point)
    {
        // Two moves in a row: the first one draws nothing.
        if (!m_verbs.empty() && m_verbs.back() == path_verb::move)
            m_points.back() = point;
        else
        {
            m_verbs.push_back(path_verb::move);
            m_points.push_back(point);
        }
        m_current = point;
        m_start = point;
        m_open = true;
    }

    void path::line_to(math::vec2f point)
    {
        ensure_subpath();
        m_verbs.push_back(path_verb::line);
        m_points.push_back(point);
        m_current = point;
    }

    void path::quad_to(math::vec2f control, math::vec2f point)
    {
        ensure_subpath();
        m_verbs.push_back(path_verb::quad);
        m_points.push_back(control);
        m_points.push_back(point);
        m_current = point;
    }

    void path::cubic_to(math::vec2f control1, math::vec2f control2, math::vec2f point)
    {
        ensure_subpath();
        m_verbs.push_back(path_verb::cubic);
        m_points.push_back(control1);
        m_points.push_back(control2);
        m_points.push_back(point);
        m_current = point;
    }

    void path::close()
    {
        if (!m_open)
            return;
        m_verbs.push_back(path_verb::close);
        m_current = m_start;
        m_open = false;
    }

    void path::clear() noexcept
    {
        m_verbs.clear();
        m_points.clear();
        m_current = {};
        m_start = {};
        m_open = false;
    }

    void path::arc_to(math::vec2f radii, float rotation_degrees, bool large_arc, bool sweep, math::vec2f point)
    {
        // The endpoint to center conversion of SVG's implementation notes
        // (F.6.5), in double precision.
        const math::vec2f from = m_current;
        if (from[0] == point[0] && from[1] == point[1])
            return;
        double rx = std::abs(static_cast<double>(radii[0]));
        double ry = std::abs(static_cast<double>(radii[1]));
        if (rx == 0.0 || ry == 0.0 || !std::isfinite(rx) || !std::isfinite(ry))
        {
            line_to(point);
            return;
        }

        const double from_x = static_cast<double>(from[0]);
        const double from_y = static_cast<double>(from[1]);
        const double to_x = static_cast<double>(point[0]);
        const double to_y = static_cast<double>(point[1]);
        const double phi = static_cast<double>(rotation_degrees) * std::numbers::pi / 180.0;
        const double cos_phi = std::cos(phi);
        const double sin_phi = std::sin(phi);
        const double dx = (from_x - to_x) / 2.0;
        const double dy = (from_y - to_y) / 2.0;
        const double x1 = cos_phi * dx + sin_phi * dy;
        const double y1 = -sin_phi * dx + cos_phi * dy;

        const double lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
        if (lambda > 1.0)
        {
            rx *= std::sqrt(lambda);
            ry *= std::sqrt(lambda);
        }

        const double numerator = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
        const double denominator = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        double coefficient = denominator > 0.0 ? std::sqrt(std::max(0.0, numerator / denominator)) : 0.0;
        if (large_arc == sweep)
            coefficient = -coefficient;
        const double cx1 = coefficient * rx * y1 / ry;
        const double cy1 = -coefficient * ry * x1 / rx;
        const double cx = cos_phi * cx1 - sin_phi * cy1 + (from_x + to_x) / 2.0;
        const double cy = sin_phi * cx1 + cos_phi * cy1 + (from_y + to_y) / 2.0;

        const auto angle = [](double ux, double uy, double vx, double vy) {
            return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        };
        const double start = angle(1.0, 0.0, (x1 - cx1) / rx, (y1 - cy1) / ry);
        double sweep_angle = angle((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
        if (!sweep && sweep_angle > 0.0)
            sweep_angle -= 2.0 * std::numbers::pi;
        else if (sweep && sweep_angle < 0.0)
            sweep_angle += 2.0 * std::numbers::pi;

        const auto segments = static_cast<int>(std::ceil(std::abs(sweep_angle) / (std::numbers::pi / 2.0) - 1e-9));
        const double step = sweep_angle / std::max(segments, 1);
        const double handle = 4.0 / 3.0 * std::tan(step / 4.0);
        const auto map = [&](double ux, double uy) -> math::vec2f {
            const double x = rx * ux;
            const double y = ry * uy;
            return {static_cast<float>(cos_phi * x - sin_phi * y + cx), static_cast<float>(sin_phi * x + cos_phi * y + cy)};
        };

        double a0 = start;
        for (int i = 0; i < segments; ++i)
        {
            const double a1 = a0 + step;
            const double c0 = std::cos(a0);
            const double s0 = std::sin(a0);
            const double c1 = std::cos(a1);
            const double s1 = std::sin(a1);
            const math::vec2f end = i + 1 == segments ? point : map(c1, s1);
            cubic_to(map(c0 - handle * s0, s0 + handle * c0), map(c1 + handle * s1, s1 - handle * c1), end);
            a0 = a1;
        }
    }

    void path::add_rectangle(math::vec2f corner, math::vec2f size)
    {
        move_to(corner);
        line_to({corner[0] + size[0], corner[1]});
        line_to({corner[0] + size[0], corner[1] + size[1]});
        line_to({corner[0], corner[1] + size[1]});
        close();
    }

    void path::add_rounded_rectangle(math::vec2f corner, math::vec2f size, math::vec2f radii)
    {
        const float rx = std::clamp(radii[0], 0.0f, std::abs(size[0]) / 2.0f);
        const float ry = std::clamp(radii[1], 0.0f, std::abs(size[1]) / 2.0f);
        if (rx <= 0.0f || ry <= 0.0f)
        {
            add_rectangle(corner, size);
            return;
        }
        const float x0 = corner[0];
        const float y0 = corner[1];
        const float x1 = corner[0] + size[0];
        const float y1 = corner[1] + size[1];
        const float kx = rx * kappa;
        const float ky = ry * kappa;
        move_to({x0 + rx, y0});
        line_to({x1 - rx, y0});
        cubic_to({x1 - rx + kx, y0}, {x1, y0 + ry - ky}, {x1, y0 + ry});
        line_to({x1, y1 - ry});
        cubic_to({x1, y1 - ry + ky}, {x1 - rx + kx, y1}, {x1 - rx, y1});
        line_to({x0 + rx, y1});
        cubic_to({x0 + rx - kx, y1}, {x0, y1 - ry + ky}, {x0, y1 - ry});
        line_to({x0, y0 + ry});
        cubic_to({x0, y0 + ry - ky}, {x0 + rx - kx, y0}, {x0 + rx, y0});
        close();
    }

    void path::add_ellipse(math::vec2f center, math::vec2f radii)
    {
        const float cx = center[0];
        const float cy = center[1];
        const float rx = radii[0];
        const float ry = radii[1];
        const float kx = rx * kappa;
        const float ky = ry * kappa;
        move_to({cx + rx, cy});
        cubic_to({cx + rx, cy + ky}, {cx + kx, cy + ry}, {cx, cy + ry});
        cubic_to({cx - kx, cy + ry}, {cx - rx, cy + ky}, {cx - rx, cy});
        cubic_to({cx - rx, cy - ky}, {cx - kx, cy - ry}, {cx, cy - ry});
        cubic_to({cx + kx, cy - ry}, {cx + rx, cy - ky}, {cx + rx, cy});
        close();
    }

    void path::append(const path &other, const math::mat3f &transform)
    {
        std::size_t at = 0;
        for (const path_verb verb : other.m_verbs)
        {
            const auto p = [&](std::size_t k) { return apply(transform, other.m_points[at + k]); };
            switch (verb)
            {
            case path_verb::move:
                move_to(p(0));
                at += 1;
                break;
            case path_verb::line:
                line_to(p(0));
                at += 1;
                break;
            case path_verb::quad:
                quad_to(p(0), p(1));
                at += 2;
                break;
            case path_verb::cubic:
                cubic_to(p(0), p(1), p(2));
                at += 3;
                break;
            case path_verb::close:
                close();
                break;
            }
        }
    }

    // ---- bounds ---------------------------------------------------------------

    path::bounding_box path::bounds() const
    {
        if (m_points.empty())
            return {};
        bounding_box box{m_points[0], m_points[0]};
        const auto include = [&](math::vec2f p) {
            box.min = {std::min(box.min[0], p[0]), std::min(box.min[1], p[1])};
            box.max = {std::max(box.max[0], p[0]), std::max(box.max[1], p[1])};
        };

        math::vec2f current{};
        std::size_t at = 0;
        for (const path_verb verb : m_verbs)
        {
            switch (verb)
            {
            case path_verb::move:
            case path_verb::line:
                current = m_points[at++];
                include(current);
                break;
            case path_verb::quad:
            {
                const math::vec2f c = m_points[at];
                const math::vec2f e = m_points[at + 1];
                for (std::size_t axis = 0; axis < 2; ++axis)
                {
                    // B'(t) = 0 at t = (p0 - p1) / (p0 - 2 p1 + p2).
                    const float den = current[axis] - 2.0f * c[axis] + e[axis];
                    if (den != 0.0f)
                    {
                        const float t = (current[axis] - c[axis]) / den;
                        if (t > 0.0f && t < 1.0f)
                            include(lerp(lerp(current, c, t), lerp(c, e, t), t));
                    }
                }
                include(e);
                current = e;
                at += 2;
                break;
            }
            case path_verb::cubic:
            {
                const math::vec2f p0 = current;
                const math::vec2f p1 = m_points[at];
                const math::vec2f p2 = m_points[at + 1];
                const math::vec2f p3 = m_points[at + 2];
                for (std::size_t axis = 0; axis < 2; ++axis)
                {
                    // B'(t) / 3 = a t^2 + b t + c.
                    const double q0 = static_cast<double>(p0[axis]);
                    const double q1 = static_cast<double>(p1[axis]);
                    const double q2 = static_cast<double>(p2[axis]);
                    const double q3 = static_cast<double>(p3[axis]);
                    const double a = -q0 + 3.0 * q1 - 3.0 * q2 + q3;
                    const double b = 2.0 * (q0 - 2.0 * q1 + q2);
                    const double c = q1 - q0;
                    double roots[2];
                    int count = 0;
                    if (std::abs(a) < 1e-12)
                    {
                        if (std::abs(b) > 1e-12)
                            roots[count++] = -c / b;
                    }
                    else
                    {
                        const double discriminant = b * b - 4.0 * a * c;
                        if (discriminant >= 0.0)
                        {
                            const double root = std::sqrt(discriminant);
                            roots[count++] = (-b + root) / (2.0 * a);
                            roots[count++] = (-b - root) / (2.0 * a);
                        }
                    }
                    for (int k = 0; k < count; ++k)
                    {
                        const auto t = static_cast<float>(roots[k]);
                        if (t > 0.0f && t < 1.0f)
                        {
                            const math::vec2f a1 = lerp(p0, p1, t);
                            const math::vec2f b1 = lerp(p1, p2, t);
                            const math::vec2f c1 = lerp(p2, p3, t);
                            include(lerp(lerp(a1, b1, t), lerp(b1, c1, t), t));
                        }
                    }
                }
                include(p3);
                current = p3;
                at += 3;
                break;
            }
            case path_verb::close:
                break;
            }
        }
        return box;
    }

    // ---- flattening -------------------------------------------------------------

    namespace detail
    {
        float arc_step(float radius, float tolerance) noexcept
        {
            if (!(radius > tolerance) || !(tolerance > 0.0f))
                return std::numbers::pi_v<float> / 2.0f;
            return std::max(2.0f * std::acos(1.0f - tolerance / radius), 0.01f);
        }

        namespace
        {
            // How far the control polygon turns: a bound on how far the
            // curve does.
            float turning(std::span<const math::vec2f> points)
            {
                float total = 0.0f;
                math::vec2f previous{};
                bool have = false;
                for (std::size_t i = 1; i < points.size(); ++i)
                {
                    const math::vec2f d = points[i] - points[i - 1];
                    if (d[0] == 0.0f && d[1] == 0.0f)
                        continue;
                    if (have)
                        total += std::abs(std::atan2(previous[0] * d[1] - previous[1] * d[0],
                                                     previous[0] * d[0] + previous[1] * d[1]));
                    previous = d;
                    have = true;
                }
                return total;
            }

            int segment_count(float distance_estimate, float tolerance, float turn, float max_turn)
            {
                float n = std::sqrt(std::max(distance_estimate, 0.0f) / tolerance);
                if (max_turn > 0.0f)
                    n = std::max(n, turn / max_turn);
                if (!std::isfinite(n))
                    return 1;
                return std::clamp(static_cast<int>(std::ceil(n)), 1, 4096);
            }

            void finish(std::vector<contour> &out, contour &current, bool drawn, bool closed)
            {
                if (drawn && !current.points.empty())
                {
                    current.closed = closed;
                    if (closed && current.points.size() > 1)
                    {
                        const math::vec2f first = current.points.front();
                        const math::vec2f last = current.points.back();
                        if (first[0] == last[0] && first[1] == last[1])
                            current.points.pop_back();
                    }
                    out.push_back(std::move(current));
                }
                current = {};
            }

            void push(contour &current, math::vec2f point)
            {
                if (!current.points.empty())
                {
                    const math::vec2f last = current.points.back();
                    if (last[0] == point[0] && last[1] == point[1])
                        return;
                }
                if (std::isfinite(point[0]) && std::isfinite(point[1]))
                    current.points.push_back(point);
            }
        } // namespace

        std::vector<contour> flatten(const path &source, const math::mat3f &transform, float tolerance, float max_turn)
        {
            tolerance = std::max(tolerance, 1e-6f);
            std::vector<contour> out;
            contour current;
            bool drawn = false;

            const auto points = source.points();
            std::size_t at = 0;
            math::vec2f last{};
            for (const path_verb verb : source.verbs())
            {
                switch (verb)
                {
                case path_verb::move:
                    finish(out, current, drawn, false);
                    drawn = false;
                    last = apply(transform, points[at++]);
                    push(current, last);
                    break;
                case path_verb::line:
                    last = apply(transform, points[at++]);
                    push(current, last);
                    drawn = true;
                    break;
                case path_verb::quad:
                {
                    const math::vec2f p0 = last;
                    const math::vec2f p1 = apply(transform, points[at]);
                    const math::vec2f p2 = apply(transform, points[at + 1]);
                    at += 2;
                    const math::vec2f dd = p0 - 2.0f * p1 + p2;
                    const math::vec2f control[] = {p0, p1, p2};
                    // Error of n chords: |B''| / (8 n^2), with |B''| = 2 |dd|.
                    const int n = segment_count(length(dd) / 4.0f, tolerance, turning(control), max_turn);
                    for (int i = 1; i <= n; ++i)
                    {
                        const float t = static_cast<float>(i) / static_cast<float>(n);
                        push(current, i == n ? p2 : lerp(lerp(p0, p1, t), lerp(p1, p2, t), t));
                    }
                    last = p2;
                    drawn = true;
                    break;
                }
                case path_verb::cubic:
                {
                    const math::vec2f p0 = last;
                    const math::vec2f p1 = apply(transform, points[at]);
                    const math::vec2f p2 = apply(transform, points[at + 1]);
                    const math::vec2f p3 = apply(transform, points[at + 2]);
                    at += 3;
                    const float dd = std::max(length(p0 - 2.0f * p1 + p2), length(p1 - 2.0f * p2 + p3));
                    const math::vec2f control[] = {p0, p1, p2, p3};
                    // |B''| <= 6 dd, so n chords are within 6 dd / (8 n^2).
                    const int n = segment_count(0.75f * dd, tolerance, turning(control), max_turn);
                    for (int i = 1; i <= n; ++i)
                    {
                        const float t = static_cast<float>(i) / static_cast<float>(n);
                        const float u = 1.0f - t;
                        const math::vec2f p = i == n ? p3
                                                     : (u * u * u) * p0 + (3.0f * u * u * t) * p1 +
                                                           (3.0f * u * t * t) * p2 + (t * t * t) * p3;
                        push(current, p);
                    }
                    last = p3;
                    drawn = true;
                    break;
                }
                case path_verb::close:
                {
                    // "M x y Z" draws a subpath of no length, which still
                    // gets round or square caps.
                    const math::vec2f start = current.points.empty() ? last : current.points.front();
                    finish(out, current, true, true);
                    drawn = false;
                    last = start;
                    break;
                }
                }
            }
            finish(out, current, drawn, false);
            return out;
        }
    } // namespace detail

    std::vector<contour> flatten(const path &source, const math::mat3f &transform, float tolerance)
    {
        return detail::flatten(source, transform, tolerance, 0.0f);
    }

} // namespace volt::image
