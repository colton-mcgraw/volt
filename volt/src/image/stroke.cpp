#include <image/path.hpp>

#include "flatten.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace volt::image
{

    namespace
    {
        math::vec2f lerp(math::vec2f a, math::vec2f b, float t)
        {
            return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t};
        }

        float distance(math::vec2f a, math::vec2f b)
        {
            return std::hypot(b[0] - a[0], b[1] - a[1]);
        }

        // Builds the stroke outline as convex pieces that all wind the same
        // way (positive area): segments, joins and caps. Where two pieces
        // share an edge they compute its end points identically, so the edge
        // cancels exactly when filled.
        class outline
        {
        public:
            outline(const stroke_style &style, float tolerance)
                : m_half(style.width / 2.0f), m_style(style), m_step(detail::arc_step(style.width / 2.0f, tolerance))
            {
            }

            void add(const contour &line)
            {
                // Consecutive duplicates are gone already (flatten and dash
                // see to it); a closed contour's last point is not its first.
                const std::vector<math::vec2f> &p = line.points;
                const std::size_t n = p.size();
                if (n == 1 || (n == 2 && line.closed && p[0][0] == p[1][0] && p[0][1] == p[1][1]))
                {
                    dot(p[0]);
                    return;
                }
                if (n < 2)
                    return;

                // Each segment's rectangle, by its corners on the left (+normal)
                // and right side; joins move inner corners before they are drawn.
                const bool closed = line.closed && n > 2;
                const std::size_t count = closed ? n : n - 1;
                std::vector<segment> segments(count);
                for (std::size_t i = 0; i < count; ++i)
                {
                    segment &g = segments[i];
                    const math::vec2f a = p[i];
                    const math::vec2f b = p[(i + 1) % n];
                    g.length = distance(a, b);
                    g.direction = g.length > 0.0f ? math::vec2f{(b[0] - a[0]) / g.length, (b[1] - a[1]) / g.length}
                                                  : math::vec2f{1.0f, 0.0f};
                    const math::vec2f normal = m_half * perpendicular(g.direction);
                    g.start_left = a + normal;
                    g.end_left = b + normal;
                    g.start_right = a - normal;
                    g.end_right = b - normal;
                }

                if (closed)
                    for (std::size_t i = 0; i < n; ++i)
                        join(p[i], segments[(i + count - 1) % count], segments[i]);
                else
                    for (std::size_t i = 1; i + 1 < n; ++i)
                        join(p[i], segments[i - 1], segments[i]);

                for (const segment &g : segments)
                    piece({g.start_left, g.end_left, g.end_right, g.start_right});
                if (!closed)
                {
                    cap(p[0], -1.0f * segments.front().direction, segments.front().start_right,
                        segments.front().start_left);
                    cap(p[n - 1], segments.back().direction, segments.back().end_left, segments.back().end_right);
                }
            }

            std::vector<contour> take() { return std::move(m_pieces); }

        private:
            struct segment
            {
                math::vec2f direction;
                float length = 0.0f;
                math::vec2f start_left, end_left, start_right, end_right;
            };

            static math::vec2f perpendicular(math::vec2f d) { return {-d[1], d[0]}; }

            // A simple polygon, turned to positive area if it is not.
            void piece(std::vector<math::vec2f> points)
            {
                float area = 0.0f;
                for (std::size_t i = 0; i < points.size(); ++i)
                {
                    const math::vec2f a = points[i];
                    const math::vec2f b = points[(i + 1) % points.size()];
                    area += a[0] * b[1] - b[0] * a[1];
                }
                if (area == 0.0f)
                    return;
                if (area < 0.0f)
                    std::reverse(points.begin(), points.end());
                m_pieces.push_back({std::move(points), true});
            }

            // Points along the circle of the half width around `center`,
            // from angle `from` turning by `sweep`, the end points included.
            void arc(std::vector<math::vec2f> &points, math::vec2f center, float from, float sweep)
            {
                const int steps = std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / m_step)));
                for (int i = 0; i <= steps; ++i)
                {
                    const float angle = from + sweep * static_cast<float>(i) / static_cast<float>(steps);
                    points.push_back({center[0] + m_half * std::cos(angle), center[1] + m_half * std::sin(angle)});
                }
            }

            // The join where segment `in` meets segment `out` at `vertex`.
            //
            // On the inside of the turn the two rectangles overlap. Filled
            // with the nonzero rule that is still the right area, but along
            // the inner edge the overlap is counted twice in the pixels it
            // touches, which shows as beads along every flattened curve. So
            // when the turn is gentle enough, both rectangles are cut back to
            // where their inner sides cross, and the outer join piece pivots
            // on that point: then nothing overlaps, and every shared edge has
            // identical end points on both sides.
            void join(math::vec2f vertex, segment &in, segment &out)
            {
                const float cross = in.direction[0] * out.direction[1] - in.direction[1] * out.direction[0];
                const float dot = in.direction[0] * out.direction[0] + in.direction[1] * out.direction[1];
                if (std::abs(cross) < 1e-6f && dot > 0.0f)
                {
                    // Straight on: make the shared edge one edge.
                    out.start_left = in.end_left;
                    out.start_right = in.end_right;
                    return;
                }

                // The outside of the turn: left when turning right.
                const bool left_outside = cross <= 0.0f;
                const math::vec2f outer_in = left_outside ? in.end_left : in.end_right;
                const math::vec2f outer_out = left_outside ? out.start_left : out.start_right;
                const float side = left_outside ? 1.0f : -1.0f;

                // The cosine of half the turn, and how far back along each
                // segment the inner sides cross.
                const float half_cos = std::sqrt(std::max(0.0f, (1.0f + dot) / 2.0f));
                math::vec2f bisector = perpendicular(in.direction) + perpendicular(out.direction);
                const float bisector_length = std::hypot(bisector[0], bisector[1]);
                if (bisector_length > 0.0f)
                    bisector = (1.0f / bisector_length) * bisector;

                math::vec2f pivot = vertex;
                const float setback = dot > 0.0f ? m_half * std::abs(cross) / (1.0f + dot) : INFINITY;
                if (bisector_length > 0.0f && half_cos > 0.0f && setback <= 0.5f * in.length &&
                    setback <= 0.5f * out.length)
                {
                    pivot = vertex - (side * m_half / half_cos) * bisector;
                    if (left_outside)
                    {
                        in.end_right = pivot;
                        out.start_right = pivot;
                    }
                    else
                    {
                        in.end_left = pivot;
                        out.start_left = pivot;
                    }
                }

                switch (m_style.join)
                {
                case line_join::round:
                {
                    std::vector<math::vec2f> points{pivot};
                    const float from = std::atan2(outer_in[1] - vertex[1], outer_in[0] - vertex[0]);
                    float sweep = std::atan2(cross, dot);
                    if (std::abs(cross) < 1e-6f)
                        sweep = -side * std::numbers::pi_v<float>; // a U-turn: round the far side, ahead
                    arc(points, vertex, from, sweep);
                    // The arc starts and ends on the segments' corners, exactly.
                    points[1] = outer_in;
                    points.back() = outer_out;
                    piece(std::move(points));
                    return;
                }
                case line_join::miter:
                {
                    // The miter's length over the width is 1 / sin(theta / 2),
                    // theta the angle between the segments: 1 / half_cos.
                    if (bisector_length > 0.0f && half_cos > 0.0f && 1.0f / half_cos <= m_style.miter_limit)
                    {
                        const math::vec2f tip = vertex + (side * m_half / half_cos) * bisector;
                        piece({pivot, outer_in, tip, outer_out});
                        return;
                    }
                    [[fallthrough]];
                }
                case line_join::bevel:
                    piece({pivot, outer_in, outer_out});
                    return;
                }
            }

            // At an open end, `direction` pointing away from the line, with
            // the segment's corners there: `left` and `right` as seen looking
            // along `direction`.
            void cap(math::vec2f end, math::vec2f direction, math::vec2f left, math::vec2f right)
            {
                switch (m_style.cap)
                {
                case line_cap::butt:
                    return;
                case line_cap::square:
                {
                    const math::vec2f ahead = m_half * direction;
                    piece({left, left + ahead, right + ahead, right});
                    return;
                }
                case line_cap::round:
                {
                    std::vector<math::vec2f> points;
                    const math::vec2f normal = perpendicular(direction);
                    const float from = std::atan2(normal[1], normal[0]);
                    arc(points, end, from, -std::numbers::pi_v<float>);
                    points.front() = left;
                    points.back() = right;
                    piece(std::move(points));
                    return;
                }
                }
            }

            // A subpath of no length: a dot of the cap's shape.
            void dot(math::vec2f at)
            {
                if (m_style.cap == line_cap::round)
                {
                    std::vector<math::vec2f> points;
                    arc(points, at, 0.0f, 2.0f * std::numbers::pi_v<float>);
                    points.pop_back();
                    piece(std::move(points));
                }
                else if (m_style.cap == line_cap::square)
                    piece({{at[0] - m_half, at[1] - m_half},
                           {at[0] + m_half, at[1] - m_half},
                           {at[0] + m_half, at[1] + m_half},
                           {at[0] - m_half, at[1] + m_half}});
            }

            float m_half;
            const stroke_style &m_style;
            float m_step;
            std::vector<contour> m_pieces;
        };

        // The dash pattern as SVG means it, or empty for a solid line.
        std::vector<float> effective_pattern(std::span<const float> dashes)
        {
            std::vector<float> pattern(dashes.begin(), dashes.end());
            float total = 0.0f;
            for (const float d : pattern)
            {
                if (!(d >= 0.0f) || !std::isfinite(d))
                    return {};
                total += d;
            }
            if (!(total > 0.0f))
                return {};
            if (pattern.size() % 2 != 0)
                pattern.insert(pattern.end(), dashes.begin(), dashes.end());
            return pattern;
        }
    } // namespace

    std::vector<contour> dash(std::span<const contour> lines, std::span<const float> dashes, float offset)
    {
        const std::vector<float> pattern = effective_pattern(dashes);
        if (pattern.empty())
            return std::vector<contour>(lines.begin(), lines.end());

        float period = 0.0f;
        for (const float d : pattern)
            period += d;
        float total_length = 0.0f;
        for (const contour &line : lines)
            for (std::size_t i = 0; i + 1 < line.points.size() + (line.closed ? 1 : 0); ++i)
                total_length += distance(line.points[i], line.points[(i + 1) % line.points.size()]);
        // A pattern far finer than the path would make millions of pieces;
        // such a line is drawn solid.
        if (total_length / period * static_cast<float>(pattern.size()) > 1e6f)
            return std::vector<contour>(lines.begin(), lines.end());

        // Where in the pattern the offset starts.
        float phase = std::fmod(offset, period);
        if (phase < 0.0f)
            phase += period;
        std::size_t first_index = 0;
        while (phase > 0.0f && phase >= pattern[first_index])
        {
            phase -= pattern[first_index];
            first_index = (first_index + 1) % pattern.size();
        }
        const float first_remaining = pattern[first_index] - phase;

        std::vector<contour> out;
        for (const contour &line : lines)
        {
            if (line.points.empty())
                continue;
            std::vector<math::vec2f> points = line.points;
            if (line.closed)
                points.push_back(points.front());

            std::size_t index = first_index;
            float remaining = first_remaining;
            bool on = index % 2 == 0;
            const std::size_t first_dash = out.size();
            const bool starts_on = on;

            contour current;
            if (on)
                current.points.push_back(points[0]);
            if (points.size() == 1 && on)
                out.push_back(current);

            for (std::size_t i = 0; i + 1 < points.size(); ++i)
            {
                const math::vec2f a = points[i];
                const math::vec2f b = points[i + 1];
                const float length = distance(a, b);
                float position = 0.0f;
                while (length - position > remaining)
                {
                    position += remaining;
                    const math::vec2f cut = lerp(a, b, length > 0.0f ? position / length : 0.0f);
                    if (on)
                    {
                        current.points.push_back(cut);
                        out.push_back(std::move(current));
                        current = {};
                    }
                    else
                        current.points = {cut};
                    on = !on;
                    index = (index + 1) % pattern.size();
                    remaining = pattern[index];
                }
                remaining -= length - position;
                if (on)
                    current.points.push_back(b);
            }
            if (on && current.points.size() >= 2)
            {
                // A closed line that ends in a dash and began in one: the two
                // are one dash across the starting point.
                if (line.closed && starts_on && out.size() > first_dash)
                {
                    contour &head = out[first_dash];
                    current.points.insert(current.points.end(), head.points.begin() + 1, head.points.end());
                    head = std::move(current);
                }
                else
                    out.push_back(std::move(current));
            }
        }
        return out;
    }

    std::vector<contour> stroke(const path &source, const stroke_style &style, float tolerance)
    {
        if (!(style.width > 0.0f) || !std::isfinite(style.width))
            return {};
        tolerance = std::max(tolerance, style.width * 1e-4f);
        const float half = style.width / 2.0f;
        std::vector<contour> lines =
            detail::flatten(source, math::mat3f::identity(), tolerance, detail::arc_step(half, tolerance));
        if (!style.dashes.empty())
            lines = dash(lines, style.dashes, style.dash_offset);

        outline builder(style, tolerance);
        for (contour &line : lines)
        {
            // Dashing can leave repeated points; the outline wants none.
            std::vector<math::vec2f> unique;
            for (const math::vec2f p : line.points)
                if (unique.empty() || unique.back()[0] != p[0] || unique.back()[1] != p[1])
                    unique.push_back(p);
            if (line.closed && unique.size() > 1 && unique.front()[0] == unique.back()[0] &&
                unique.front()[1] == unique.back()[1])
                unique.pop_back();
            line.points = std::move(unique);
            builder.add(line);
        }
        return builder.take();
    }

} // namespace volt::image
