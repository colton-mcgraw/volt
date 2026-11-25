#include <font/outline.hpp>

#include <math/geometry.hpp>

#include <algorithm>
#include <cmath>

namespace volt::font
{

    namespace
    {
        float cross(const math::vec2f &a, const math::vec2f &b) noexcept
        {
            return a[0] * b[1] - a[1] * b[0];
        }

        // Where in (0, 1) the derivative of one coordinate of a curve is
        // zero, i.e. where the curve turns around in that axis.
        void extrema(const segment &s, std::size_t axis, float (&t)[2], int &count) noexcept
        {
            count = 0;
            const float p0 = s.from[axis];
            const float p1 = s.control_a[axis];
            if (s.kind == segment_kind::quadratic)
            {
                const float p2 = s.to[axis];
                const float denominator = p0 - 2.0f * p1 + p2;
                if (denominator != 0.0f)
                {
                    const float root = (p0 - p1) / denominator;
                    if (root > 0.0f && root < 1.0f)
                        t[count++] = root;
                }
                return;
            }

            // Cubic: the derivative is a quadratic a t^2 + b t + c.
            const float p2 = s.control_b[axis];
            const float p3 = s.to[axis];
            const float a = -p0 + 3.0f * p1 - 3.0f * p2 + p3;
            const float b = 2.0f * (p0 - 2.0f * p1 + p2);
            const float c = p1 - p0;
            const auto take = [&](float root) {
                if (root > 0.0f && root < 1.0f && count < 2)
                    t[count++] = root;
            };
            if (std::abs(a) < 1e-12f)
            {
                if (b != 0.0f)
                    take(-c / b);
                return;
            }
            const float discriminant = b * b - 4.0f * a * c;
            if (discriminant < 0.0f)
                return;
            const float root = std::sqrt(discriminant);
            take((-b + root) / (2.0f * a));
            take((-b - root) / (2.0f * a));
        }
    } // namespace

    // ---------------------------------------------------------------------
    // segment, box, polygon
    // ---------------------------------------------------------------------

    math::vec2f segment::at(float t) const noexcept
    {
        const float u = 1.0f - t;
        switch (kind)
        {
        case segment_kind::quadratic:
            return from * (u * u) + control_a * (2.0f * u * t) + to * (t * t);
        case segment_kind::cubic:
            return from * (u * u * u) + control_a * (3.0f * u * u * t) + control_b * (3.0f * u * t * t) +
                   to * (t * t * t);
        case segment_kind::line:
            break;
        }
        return from * u + to * t;
    }

    void box::add(const math::vec2f &point) noexcept
    {
        if (empty())
        {
            min = point;
            max = point;
            return;
        }
        min = {std::min(min[0], point[0]), std::min(min[1], point[1])};
        max = {std::max(max[0], point[0]), std::max(max[1], point[1])};
    }

    void box::merge(const box &other) noexcept
    {
        if (other.empty())
            return;
        add(other.min);
        add(other.max);
    }

    std::span<const math::vec2f> polygon::contour(std::size_t index) const noexcept
    {
        if (index >= ends.size())
            return {};
        const std::size_t begin = index == 0 ? 0 : ends[index - 1];
        const std::size_t end = std::min<std::size_t>(ends[index], points.size());
        if (begin >= end)
            return {};
        return std::span<const math::vec2f>(points).subspan(begin, end - begin);
    }

    std::uint32_t subdivisions(const segment &s, float tolerance) noexcept
    {
        if (s.kind == segment_kind::line)
            return 1;

        float deviation = 0.0f;
        float factor = 0.0f;
        if (s.kind == segment_kind::quadratic)
        {
            deviation = math::magnitude(s.from - s.control_a * 2.0f + s.to);
            factor = 0.25f; // n (n - 1) / 8 for degree 2
        }
        else
        {
            deviation = std::max(math::magnitude(s.from - s.control_a * 2.0f + s.control_b),
                                 math::magnitude(s.control_a - s.control_b * 2.0f + s.to));
            factor = 0.75f;
        }

        const float pieces = std::ceil(std::sqrt(factor * deviation / std::max(tolerance, 1e-6f)));
        // Also catches NaN and infinity from a broken transform.
        if (!(pieces >= 1.0f))
            return 1;
        if (pieces >= 1024.0f)
            return 1024;
        return static_cast<std::uint32_t>(pieces);
    }

    // ---------------------------------------------------------------------
    // outline
    // ---------------------------------------------------------------------

    void outline::move_to(const math::vec2f &point)
    {
        finish_contour();
        m_start = point;
        m_current = point;
        m_open = true;
    }

    void outline::line_to(const math::vec2f &point)
    {
        if (!m_open)
            move_to(m_current);
        m_segments.push_back({segment_kind::line, m_current, {}, {}, point});
        m_current = point;
    }

    void outline::quadratic_to(const math::vec2f &control, const math::vec2f &point)
    {
        if (!m_open)
            move_to(m_current);
        m_segments.push_back({segment_kind::quadratic, m_current, control, {}, point});
        m_current = point;
    }

    void outline::cubic_to(const math::vec2f &control_a, const math::vec2f &control_b, const math::vec2f &point)
    {
        if (!m_open)
            move_to(m_current);
        m_segments.push_back({segment_kind::cubic, m_current, control_a, control_b, point});
        m_current = point;
    }

    void outline::close()
    {
        finish_contour();
    }

    void outline::finish_contour()
    {
        if (!m_open)
            return;
        m_open = false;

        const std::size_t begin = m_ends.empty() ? 0 : m_ends.back();
        if (m_segments.size() == begin)
            return;
        if (m_current != m_start)
            m_segments.push_back({segment_kind::line, m_current, {}, {}, m_start});
        m_current = m_start;
        m_ends.push_back(static_cast<std::uint32_t>(m_segments.size()));
    }

    void outline::append(const outline &other, const affine &transform)
    {
        finish_contour();
        const auto base = static_cast<std::uint32_t>(m_segments.size());
        m_segments.reserve(m_segments.size() + other.m_segments.size());
        for (const segment &s : other.m_segments)
        {
            m_segments.push_back({s.kind, transform.apply(s.from), transform.apply(s.control_a),
                                  transform.apply(s.control_b), transform.apply(s.to)});
        }
        for (const std::uint32_t end : other.m_ends)
            m_ends.push_back(base + end);

        // An unfinished contour in `other` is closed here rather than left
        // dangling between two outlines.
        const std::size_t finished = m_ends.empty() ? 0 : m_ends.back();
        if (m_segments.size() > finished)
            m_ends.push_back(static_cast<std::uint32_t>(m_segments.size()));
    }

    void outline::clear()
    {
        m_segments.clear();
        m_ends.clear();
        m_start = {};
        m_current = {};
        m_open = false;
    }

    std::span<const segment> outline::contour(std::size_t index) const noexcept
    {
        if (index >= m_ends.size())
            return {};
        const std::size_t begin = index == 0 ? 0 : m_ends[index - 1];
        const std::size_t end = m_ends[index];
        return std::span<const segment>(m_segments).subspan(begin, end - begin);
    }

    box outline::control_bounds() const noexcept
    {
        box result;
        for (const segment &s : m_segments)
        {
            result.add(s.from);
            if (s.kind != segment_kind::line)
                result.add(s.control_a);
            if (s.kind == segment_kind::cubic)
                result.add(s.control_b);
            result.add(s.to);
        }
        return result;
    }

    box outline::bounds() const noexcept
    {
        box result;
        for (const segment &s : m_segments)
        {
            result.add(s.from);
            result.add(s.to);
            if (s.kind == segment_kind::line)
                continue;

            // Only a control point outside the box of the ends can pull the
            // curve out, and then only as far as the curve's extremum.
            for (std::size_t axis = 0; axis < 2; ++axis)
            {
                float t[2];
                int count = 0;
                extrema(s, axis, t, count);
                for (int i = 0; i < count; ++i)
                    result.add(s.at(t[i]));
            }
        }
        return result;
    }

    float outline::signed_area() const noexcept
    {
        // Green's theorem, A = 1/2 * closed integral of (x dy - y dx), which
        // has a closed form per segment in terms of its control points.
        double twice = 0.0;
        for (const segment &s : m_segments)
        {
            switch (s.kind)
            {
            case segment_kind::line:
                twice += static_cast<double>(cross(s.from, s.to));
                break;
            case segment_kind::quadratic:
                twice += (2.0 * static_cast<double>(cross(s.from, s.control_a)) +
                          2.0 * static_cast<double>(cross(s.control_a, s.to)) +
                          static_cast<double>(cross(s.from, s.to))) /
                         3.0;
                break;
            case segment_kind::cubic:
                twice += (6.0 * static_cast<double>(cross(s.from, s.control_a)) +
                          3.0 * static_cast<double>(cross(s.from, s.control_b)) +
                          static_cast<double>(cross(s.from, s.to)) +
                          3.0 * static_cast<double>(cross(s.control_a, s.control_b)) +
                          3.0 * static_cast<double>(cross(s.control_a, s.to)) +
                          6.0 * static_cast<double>(cross(s.control_b, s.to))) /
                         10.0;
                break;
            }
        }
        return static_cast<float>(twice * 0.5);
    }

    outline outline::transformed(const affine &transform) const
    {
        outline result;
        result.append(*this, transform);
        return result;
    }

    polygon outline::flatten(float tolerance) const
    {
        polygon result;
        flatten(tolerance, result);
        return result;
    }

    void outline::flatten(float tolerance, polygon &out) const
    {
        out.points.clear();
        out.ends.clear();
        for (std::size_t c = 0; c < m_ends.size(); ++c)
        {
            const std::span<const segment> segments = contour(c);
            if (segments.empty())
                continue;
            const std::size_t begin = out.points.size();
            for (const segment &s : segments)
            {
                // Each segment adds its start and its inner points; its end
                // is the next segment's start, and the last one's is the
                // contour's first point again.
                out.points.push_back(s.from);
                const std::uint32_t pieces = subdivisions(s, tolerance);
                for (std::uint32_t i = 1; i < pieces; ++i)
                    out.points.push_back(s.at(static_cast<float>(i) / static_cast<float>(pieces)));
            }
            if (out.points.size() > begin)
                out.ends.push_back(static_cast<std::uint32_t>(out.points.size()));
        }
    }

} // namespace volt::font
