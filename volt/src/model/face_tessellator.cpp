#include "face_tessellator.hpp"

#include "triangulate.hpp"

#include <math/geometry.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace volt::model::geometry
{

    namespace
    {
        // A point of a loop in parameter space, with the exact position it
        // stands for in 3D.
        struct vtx
        {
            math::vec2d uv;
            math::vec3d position;
        };

        // A loop in parameter space, unwrapped so that consecutive points are
        // close: on a periodic surface the coordinates run on past the
        // period instead of jumping back. `closure` is where the first point
        // is reached again after going round; it differs from the first point
        // by a whole number of periods when the loop wraps around the surface.
        struct uv_loop
        {
            std::vector<vtx> v;
            math::vec2d closure{};

            math::vec2d net() const { return closure - v.front().uv; }

            // The step from the previous point to point k.
            math::vec2d delta(std::size_t k) const
            {
                return k == 0 ? closure - v.back().uv : v[k].uv - v[k - 1].uv;
            }
        };

        double at(const math::vec2d &uv, int axis)
        {
            return uv[static_cast<std::size_t>(axis)];
        }

        double &at(math::vec2d &uv, int axis)
        {
            return uv[static_cast<std::size_t>(axis)];
        }

        // The same loop starting from point `start`, keeping the steps.
        uv_loop rotate(const uv_loop &loop, std::size_t start)
        {
            const std::size_t n = loop.v.size();
            uv_loop result;
            result.v.reserve(n);
            result.v.push_back(loop.v[start]);
            for (std::size_t k = 1; k < n; ++k)
            {
                const std::size_t index = (start + k) % n;
                result.v.push_back({result.v.back().uv + loop.delta(index), loop.v[index].position});
            }
            result.closure = result.v.back().uv + loop.delta(start);
            return result;
        }

        // The same loop the other way round, from the same first point.
        uv_loop reverse(const uv_loop &loop)
        {
            const std::size_t n = loop.v.size();
            uv_loop result;
            result.v.reserve(n);
            result.v.push_back(loop.v[0]);
            for (std::size_t k = 1; k < n; ++k)
            {
                const std::size_t index = n - k;
                result.v.push_back({result.v.back().uv - loop.delta((index + 1) % n), loop.v[index].position});
            }
            result.closure = result.v.back().uv - loop.delta(1 % n);
            return result;
        }

        void shift(uv_loop &loop, int axis, double by)
        {
            if (by == 0.0)
                return;
            for (vtx &p : loop.v)
                at(p.uv, axis) += by;
            at(loop.closure, axis) += by;
        }

        double signed_area(const uv_loop &loop)
        {
            double sum = 0.0;
            const std::size_t n = loop.v.size();
            for (std::size_t k = 0; k < n; ++k)
            {
                const math::vec2d &a = loop.v[k].uv;
                const math::vec2d &b = k + 1 < n ? loop.v[k + 1].uv : loop.closure;
                sum += a[0] * b[1] - b[0] * a[1];
            }
            return 0.5 * sum;
        }

        math::vec2d centroid(const uv_loop &loop)
        {
            math::vec2d sum{};
            for (const vtx &p : loop.v)
                sum += p.uv;
            return sum / static_cast<double>(loop.v.size());
        }

        double positive_modulo(double value, double period)
        {
            const double r = std::fmod(value, period);
            return r < 0.0 ? r + period : r;
        }

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
            return std::acos(std::clamp(math::dot(a, b), -1.0, 1.0));
        }

        class tessellator
        {
        public:
            tessellator(const surface &surface, bool same_sense, const face_settings &settings)
                : m_surface(surface), m_sense(same_sense ? 1 : -1), m_settings(settings)
            {
            }

            std::expected<face_mesh, std::string> run(const std::vector<std::vector<math::vec3d>> &loops)
            {
                if (auto built = build_rings(loops); !built)
                    return std::unexpected(built.error());
                return triangulate();
            }

        private:
            const surface &m_surface;
            int m_sense;
            face_settings m_settings;

            // How far the farthest boundary point is from the surface.
            double m_mismatch = 0.0;

            std::vector<vtx> m_vertices;
            std::vector<std::vector<std::uint32_t>> m_rings;

            // ---- loops into parameter space --------------------------------

            std::optional<uv_loop> map_loop(const std::vector<math::vec3d> &points)
            {
                const std::size_t n = points.size();
                std::vector<math::vec2d> raw(n);
                std::vector<int> pole_axis(n, -1);
                for (std::size_t k = 0; k < n; ++k)
                {
                    raw[k] = m_surface.project(points[k], k > 0 ? &raw[k - 1] : nullptr);
                    for (int axis = 0; axis < 2; ++axis)
                    {
                        if (m_surface.singular(raw[k], axis))
                        {
                            pole_axis[k] = axis;
                            break;
                        }
                    }
                }

                std::size_t first = n;
                for (std::size_t k = 0; k < n; ++k)
                {
                    if (pole_axis[k] < 0)
                    {
                        first = k;
                        break;
                    }
                }
                if (first == n)
                    return std::nullopt; // nothing but a pole: a vertex loop

                uv_loop loop;
                math::vec2d previous = m_surface.project(points[first], nullptr);
                m_mismatch = std::max(m_mismatch, math::distance(m_surface.point(previous), points[first]));
                loop.v.push_back({previous, points[first]});

                std::size_t step = 1;
                while (step <= n)
                {
                    const std::size_t index = (first + step) % n;
                    if (index != first && pole_axis[index] >= 0)
                    {
                        // Through a pole: the coordinate along the collapsed
                        // axis is free there. In parameter space the boundary
                        // runs along the pole line, from where it arrived to
                        // where it leaves, in the direction that keeps the
                        // face on its left (right for a reversed face).
                        const int a = pole_axis[index];
                        const int o = 1 - a;
                        std::size_t run = 0;
                        while (run < n && pole_axis[(index + run) % n] >= 0 && (index + run) % n != first)
                            ++run;
                        const std::size_t after = (index + run) % n;

                        double pole_at = at(raw[index], o);
                        for (bool upper : {true, false})
                        {
                            const auto known = m_surface.pole(a, upper);
                            if (known && std::abs(*known - pole_at) < 1e-6 * (1.0 + std::abs(*known)))
                                pole_at = *known;
                        }
                        const bool upper = pole_at > at(previous, o);

                        math::vec2d next = m_surface.project(points[after], &previous);
                        if (m_surface.periodic[a])
                        {
                            const double period = m_surface.period[a];
                            bool increase = a == 0 ? !upper : upper;
                            if (m_sense < 0)
                                increase = !increase;
                            double d = positive_modulo(increase ? at(next, a) - at(previous, a)
                                                                : at(previous, a) - at(next, a),
                                                       period);
                            if (d < 1e-9 * period)
                                d = period;
                            at(next, a) = at(previous, a) + (increase ? d : -d);
                        }

                        math::vec2d arrive = previous;
                        at(arrive, o) = pole_at;
                        math::vec2d leave = next;
                        at(leave, o) = pole_at;
                        // Along the pole line in steps, so that the triangles
                        // fanning into the pole stay narrow.
                        loop.v.push_back({arrive, points[index]});
                        std::vector<vtx> along;
                        sample_line({arrive, points[index]}, {leave, points[index]}, true, along);
                        loop.v.insert(loop.v.end(), along.begin(), along.end());
                        loop.v.push_back({leave, points[index]});

                        step += run;
                        if (after == first)
                        {
                            loop.closure = next;
                            return loop;
                        }
                        loop.v.push_back({next, points[after]});
                        previous = next;
                        ++step;
                        continue;
                    }

                    const math::vec2d uv = m_surface.project(points[index], &previous);
                    m_mismatch = std::max(m_mismatch, math::distance(m_surface.point(uv), points[index]));
                    if (index == first)
                    {
                        loop.closure = uv;
                        return loop;
                    }
                    loop.v.push_back({uv, points[index]});
                    previous = uv;
                    ++step;
                }
                loop.closure = loop.v.front().uv;
                return loop;
            }

            // ---- chains and rings ------------------------------------------

            // Points strictly between a and b along the straight line between
            // them in parameter space, as many as the surface's bending asks.
            void sample_line(const vtx &a, const vtx &b, bool fixed_position, std::vector<vtx> &out) const
            {
                struct span
                {
                    double t0;
                    double t1;
                    math::vec3d p0;
                    math::vec3d p1;
                    int depth;
                };
                const auto uv_at = [&](double t) { return a.uv + (b.uv - a.uv) * t; };
                const auto position = [&](double t) { return fixed_position ? a.position : m_surface.point(uv_at(t)); };

                std::vector<span> stack;
                const math::vec3d middle = position(0.5);
                stack.push_back({0.5, 1.0, middle, b.position, 1});
                stack.push_back({0.0, 0.5, a.position, middle, 1});
                bool first = true;
                while (!stack.empty())
                {
                    const span s = stack.back();
                    stack.pop_back();
                    bool split = false;
                    if (s.depth < 10 && out.size() < 4096)
                    {
                        const double tm = 0.5 * (s.t0 + s.t1);
                        const math::vec3d pm = position(tm);
                        split = distance_to_segment(pm, s.p0, s.p1) > m_settings.chord ||
                                angle_between(m_surface.normal(uv_at(s.t0)), m_surface.normal(uv_at(s.t1))) >
                                    m_settings.angle;
                        if (split)
                        {
                            stack.push_back({tm, s.t1, pm, s.p1, s.depth + 1});
                            stack.push_back({s.t0, tm, s.p0, pm, s.depth + 1});
                            continue;
                        }
                    }
                    if (!first)
                        out.push_back({uv_at(s.t0), s.p0});
                    first = false;
                }
            }

            // A chain along a pole line or a seam of the domain, from `from`
            // to `to` (both included).
            std::vector<vtx> line_chain(const math::vec2d &from, const math::vec2d &to, bool pole) const
            {
                const math::vec3d p0 = m_surface.point(from);
                const vtx a{from, p0};
                const vtx b{to, pole ? p0 : m_surface.point(to)};
                std::vector<vtx> chain{a};
                sample_line(a, b, pole, chain);
                chain.push_back(b);
                return chain;
            }

            // A loop as a chain that runs once around along axis w, in the
            // direction `increasing`, starting at the point closest to
            // `target` (modulo the period) and shifted to it; the chain ends
            // with the first point again, one period on.
            std::vector<vtx> loop_chain(uv_loop loop, int w, bool increasing, double target) const
            {
                const double period = m_surface.period[w];
                if ((at(loop.net(), w) > 0.0) != increasing)
                    loop = reverse(loop);

                std::size_t best = 0;
                double best_distance = std::numeric_limits<double>::infinity();
                for (std::size_t k = 0; k < loop.v.size(); ++k)
                {
                    const double d = positive_modulo(at(loop.v[k].uv, w) - target + 0.5 * period, period) - 0.5 * period;
                    if (std::abs(d) < best_distance)
                    {
                        best_distance = std::abs(d);
                        best = k;
                    }
                }
                loop = rotate(loop, best);
                shift(loop, w, period * std::round((target - at(loop.v[0].uv, w)) / period));

                std::vector<vtx> chain = loop.v;
                chain.push_back({loop.closure, loop.v[0].position});
                return chain;
            }

            std::uint32_t add_vertex(const vtx &v)
            {
                m_vertices.push_back(v);
                return static_cast<std::uint32_t>(m_vertices.size() - 1);
            }

            void add_loop_ring(const uv_loop &loop)
            {
                std::vector<std::uint32_t> ring;
                for (const vtx &v : loop.v)
                    ring.push_back(add_vertex(v));
                m_rings.push_back(std::move(ring));
            }

            // The outer ring of a band around axis w: the bottom chain (going
            // up along w), the seam on the right, the top chain (going down),
            // the seam on the left. The two seams are the same points one
            // period apart, so they meet in 3D.
            void close_band(const std::vector<vtx> &bottom, const std::vector<vtx> &top, int w)
            {
                const double period = m_surface.period[w];
                std::vector<vtx> right;
                sample_line(bottom.back(), top.front(), false, right);

                std::vector<std::uint32_t> ring;
                for (const vtx &v : bottom)
                    ring.push_back(add_vertex(v));
                for (const vtx &v : right)
                    ring.push_back(add_vertex(v));
                for (const vtx &v : top)
                    ring.push_back(add_vertex(v));
                for (auto it = right.rbegin(); it != right.rend(); ++it)
                {
                    vtx twin = *it;
                    at(twin.uv, w) -= period;
                    ring.push_back(add_vertex(twin));
                }
                m_rings.insert(m_rings.begin(), std::move(ring));
            }

            // Moves holes into the window [start, start + period) along each
            // periodic axis, so that they lie inside the outer ring.
            void place_holes(std::vector<uv_loop> &holes, const math::vec2d &start) const
            {
                for (uv_loop &hole : holes)
                {
                    const math::vec2d c = centroid(hole);
                    for (int axis = 0; axis < 2; ++axis)
                    {
                        if (!m_surface.periodic[axis])
                            continue;
                        const double period = m_surface.period[axis];
                        shift(hole, axis, -period * std::floor((at(c, axis) - at(start, axis)) / period));
                    }
                }
            }

            bool closed_surface() const
            {
                if (m_surface.periodic[0] && m_surface.periodic[1])
                    return true;
                for (int a = 0; a < 2; ++a)
                {
                    if (m_surface.periodic[a] && m_surface.pole(a, true) && m_surface.pole(a, false))
                        return true;
                }
                return false;
            }

            // The whole parameter domain of a closed surface as the outer
            // ring, placed so that `centre` is in the middle of it.
            std::expected<void, std::string> full_domain(const math::vec2d &centre)
            {
                int w = m_surface.periodic[0] ? 0 : 1;
                const int o = 1 - w;
                const double pw = m_surface.period[w];
                const double start_w = at(centre, w) - 0.5 * pw;

                math::vec2d b0{};
                math::vec2d b1{};
                at(b0, w) = start_w;
                at(b1, w) = start_w + pw;
                std::vector<vtx> bottom;
                std::vector<vtx> top;
                if (m_surface.periodic[o])
                {
                    const double po = m_surface.period[o];
                    const double start_o = at(centre, o) - 0.5 * po;
                    at(b0, o) = start_o;
                    at(b1, o) = start_o;
                    bottom = line_chain(b0, b1, false);
                    bottom.back().position = bottom.front().position;
                    for (auto it = bottom.rbegin(); it != bottom.rend(); ++it)
                    {
                        vtx twin = *it;
                        at(twin.uv, o) += po;
                        top.push_back(twin);
                    }
                }
                else
                {
                    const auto lower = m_surface.pole(w, false);
                    const auto upper = m_surface.pole(w, true);
                    if (!lower || !upper)
                        return std::unexpected("a face without boundary on a surface that is not closed");
                    at(b0, o) = *lower;
                    at(b1, o) = *lower;
                    bottom = line_chain(b0, b1, true);
                    math::vec2d t0 = b1;
                    math::vec2d t1 = b0;
                    at(t0, o) = *upper;
                    at(t1, o) = *upper;
                    top = line_chain(t0, t1, true);
                }
                close_band(bottom, top, w);
                return {};
            }

            std::expected<void, std::string> build_rings(const std::vector<std::vector<math::vec3d>> &loops)
            {
                std::vector<uv_loop> closed;
                std::vector<uv_loop> wrapping;
                int wrap_axis = -1;

                for (const std::vector<math::vec3d> &input : loops)
                {
                    // Consecutive repeats say nothing and confuse the
                    // triangulation.
                    std::vector<math::vec3d> points;
                    points.reserve(input.size());
                    for (const math::vec3d &p : input)
                    {
                        if (points.empty() || points.back() != p)
                            points.push_back(p);
                    }
                    while (points.size() > 1 && points.back() == points.front())
                        points.pop_back();
                    if (points.size() < 2)
                        continue;

                    auto mapped = map_loop(points);
                    if (!mapped || mapped->v.size() < 2)
                        continue;

                    const math::vec2d net = mapped->net();
                    int turns[2] = {0, 0};
                    for (int axis = 0; axis < 2; ++axis)
                    {
                        const double period = m_surface.period[axis];
                        if (m_surface.periodic[axis])
                        {
                            const double k = std::round(at(net, axis) / period);
                            if (std::abs(at(net, axis) - k * period) > 0.25 * period)
                                return std::unexpected("a loop does not close in parameter space");
                            turns[axis] = static_cast<int>(k);
                            // Exactly whole periods from here on.
                            at(mapped->closure, axis) = at(mapped->v.front().uv, axis) + k * period;
                        }
                        else
                            at(mapped->closure, axis) = at(mapped->v.front().uv, axis);
                    }

                    if (turns[0] == 0 && turns[1] == 0)
                    {
                        if (mapped->v.size() >= 3)
                            closed.push_back(std::move(*mapped));
                        continue;
                    }
                    if ((turns[0] != 0 && turns[1] != 0) || std::abs(turns[0]) > 1 || std::abs(turns[1]) > 1)
                        return std::unexpected("a loop winds around the surface more than once");
                    const int axis = turns[0] != 0 ? 0 : 1;
                    if (wrap_axis >= 0 && wrap_axis != axis)
                        return std::unexpected("loops wind around the surface in two directions");
                    wrap_axis = axis;
                    wrapping.push_back(std::move(*mapped));
                }

                if (!wrapping.empty())
                    return build_band(wrapping, closed, wrap_axis);
                return build_closed(closed);
            }

            std::expected<void, std::string> build_closed(std::vector<uv_loop> &closed)
            {
                const bool closed_domain = closed_surface();
                if (closed.empty())
                {
                    if (!closed_domain)
                        return std::unexpected("a face without a usable boundary");
                    math::vec2d centre{};
                    for (int axis = 0; axis < 2; ++axis)
                    {
                        const double lo = m_surface.lo[axis];
                        const double hi = m_surface.hi[axis];
                        at(centre, axis) = std::isfinite(lo) && std::isfinite(hi) ? 0.5 * (lo + hi) : 0.0;
                    }
                    return full_domain(centre);
                }

                std::vector<double> areas;
                for (const uv_loop &loop : closed)
                    areas.push_back(signed_area(loop) * m_sense);

                std::optional<std::size_t> outer;
                if (!closed_domain)
                {
                    outer = 0;
                    for (std::size_t k = 1; k < closed.size(); ++k)
                    {
                        if (std::abs(areas[k]) > std::abs(areas[*outer]))
                            outer = k;
                    }
                }
                else
                {
                    for (std::size_t k = 0; k < closed.size(); ++k)
                    {
                        if (areas[k] > 0.0 && (!outer || areas[k] > areas[*outer]))
                            outer = k;
                    }
                }

                std::vector<uv_loop> holes;
                if (!outer)
                {
                    // Only holes: the face is the rest of a closed surface.
                    holes = std::move(closed);
                    const math::vec2d centre = centroid(holes.front());
                    if (auto made = full_domain(centre); !made)
                        return made;
                    math::vec2d start{};
                    for (int axis = 0; axis < 2; ++axis)
                        at(start, axis) = at(centre, axis) - 0.5 * m_surface.period[axis];
                    place_holes(holes, start);
                }
                else
                {
                    add_loop_ring(closed[*outer]);
                    math::vec2d start{};
                    for (int axis = 0; axis < 2; ++axis)
                    {
                        double lowest = std::numeric_limits<double>::infinity();
                        for (const vtx &v : closed[*outer].v)
                            lowest = std::min(lowest, at(v.uv, axis));
                        at(start, axis) = lowest;
                    }
                    for (std::size_t k = 0; k < closed.size(); ++k)
                    {
                        if (k != *outer)
                            holes.push_back(std::move(closed[k]));
                    }
                    place_holes(holes, start);
                }
                for (const uv_loop &hole : holes)
                    add_loop_ring(hole);
                return {};
            }

            std::expected<void, std::string> build_band(std::vector<uv_loop> &wrapping, std::vector<uv_loop> &holes,
                                                        int w)
            {
                const int o = 1 - w;
                const double period = m_surface.period[w];
                // Going along +w, the face lies towards +o when this is
                // positive: a counter-clockwise boundary (seen with the
                // surface's normal) in (u, v), mirrored when w is v.
                const int above_sign = (w == 0 ? 1 : -1) * m_sense;

                const auto mean_o = [&](const uv_loop &loop) {
                    double sum = 0.0;
                    for (const vtx &v : loop.v)
                        sum += at(v.uv, o);
                    return sum / static_cast<double>(loop.v.size());
                };
                const auto extreme_o = [&](const uv_loop &loop, bool highest) {
                    double value = highest ? -std::numeric_limits<double>::infinity()
                                           : std::numeric_limits<double>::infinity();
                    for (const vtx &v : loop.v)
                        value = highest ? std::max(value, at(v.uv, o)) : std::min(value, at(v.uv, o));
                    return value;
                };

                const uv_loop *bottom = nullptr;
                const uv_loop *top = nullptr;
                std::optional<double> bottom_pole;
                std::optional<double> top_pole;

                if (wrapping.size() == 2)
                {
                    uv_loop &first = wrapping[0];
                    uv_loop &second = wrapping[1];
                    const bool first_below = (at(first.net(), w) > 0.0 ? 1 : -1) * above_sign > 0;
                    const bool second_below = (at(second.net(), w) > 0.0 ? 1 : -1) * above_sign > 0;
                    if (m_surface.periodic[o] && first_below != second_below)
                    {
                        // On a torus the band between two circles can go
                        // either way round; the loops' directions say which.
                        bottom = first_below ? &first : &second;
                        uv_loop &upper = first_below ? second : first;
                        const double po = m_surface.period[o];
                        const double gap = mean_o(upper) - mean_o(*bottom);
                        shift(upper, o, -po * std::floor(gap / po));
                        top = &upper;
                    }
                    else
                    {
                        const bool first_lower = mean_o(first) <= mean_o(second);
                        bottom = first_lower ? &first : &second;
                        top = first_lower ? &second : &first;
                    }
                }
                else if (wrapping.size() == 1)
                {
                    const uv_loop &only = wrapping[0];
                    const bool below = (at(only.net(), w) > 0.0 ? 1 : -1) * above_sign > 0;
                    if (below)
                    {
                        bottom = &only;
                        const auto pole = m_surface.pole(w, true);
                        if (pole && *pole >= extreme_o(only, true) - 1e-9)
                            top_pole = pole;
                    }
                    else
                    {
                        top = &only;
                        const auto pole = m_surface.pole(w, false);
                        if (pole && *pole <= extreme_o(only, false) + 1e-9)
                            bottom_pole = pole;
                    }
                    if (!top_pole && !bottom_pole)
                        return std::unexpected("one loop around a surface that does not close beyond it");
                }
                else
                    return std::unexpected("more than two loops around the surface");

                // Where to cut the band open: at a point of one of its loops
                // that no hole lies across.
                std::vector<std::pair<double, double>> hole_ranges;
                for (const uv_loop &hole : holes)
                {
                    double lo = std::numeric_limits<double>::infinity();
                    double hi = -lo;
                    for (const vtx &v : hole.v)
                    {
                        lo = std::min(lo, at(v.uv, w));
                        hi = std::max(hi, at(v.uv, w));
                    }
                    hole_ranges.emplace_back(lo, hi);
                }
                const uv_loop &reference = bottom != nullptr ? *bottom : *top;
                double cut = at(reference.v[0].uv, w);
                for (const vtx &v : reference.v)
                {
                    const double candidate = at(v.uv, w);
                    bool clear = true;
                    for (const auto &[lo, hi] : hole_ranges)
                    {
                        const double margin = 1e-6 * period;
                        const double from = positive_modulo(candidate - lo + margin, period);
                        if (from <= hi - lo + 2.0 * margin)
                            clear = false;
                    }
                    if (clear)
                    {
                        cut = candidate;
                        break;
                    }
                }

                std::vector<vtx> bottom_chain;
                std::vector<vtx> top_chain;
                if (bottom != nullptr)
                {
                    bottom_chain = loop_chain(*bottom, w, true, cut);
                    cut = at(bottom_chain.front().uv, w);
                }
                if (top != nullptr)
                {
                    top_chain = loop_chain(*top, w, false, cut + period);
                    if (bottom == nullptr)
                        cut = at(top_chain.front().uv, w) - period;
                }
                if (bottom == nullptr)
                {
                    math::vec2d from{};
                    math::vec2d to{};
                    at(from, w) = cut;
                    at(to, w) = cut + period;
                    at(from, o) = *bottom_pole;
                    at(to, o) = *bottom_pole;
                    bottom_chain = line_chain(from, to, true);
                }
                if (top == nullptr)
                {
                    math::vec2d from{};
                    math::vec2d to{};
                    at(from, w) = cut + period;
                    at(to, w) = cut;
                    at(from, o) = *top_pole;
                    at(to, o) = *top_pole;
                    top_chain = line_chain(from, to, true);
                }

                close_band(bottom_chain, top_chain, w);

                math::vec2d start{};
                at(start, w) = cut;
                if (m_surface.periodic[o])
                    at(start, o) = 0.5 * (at(bottom_chain.front().uv, o) + at(top_chain.front().uv, o)) -
                                   0.5 * m_surface.period[o];
                place_holes(holes, start);
                for (const uv_loop &hole : holes)
                    add_loop_ring(hole);
                return {};
            }

            // ---- triangulation ---------------------------------------------

            std::expected<face_mesh, std::string> triangulate()
            {
                if (m_rings.empty() || m_rings[0].size() < 3)
                    return std::unexpected("no boundary left to triangulate");

                // Parameter space stretched to roughly the surface's own
                // lengths, so that triangles shaped well there are shaped well
                // on the surface: an angle of a pipe 0.2 mm across is not as
                // long as a millimeter along it.
                math::vec2d lo{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
                math::vec2d hi = -lo;
                for (const vtx &v : m_vertices)
                {
                    for (int axis = 0; axis < 2; ++axis)
                    {
                        at(lo, axis) = std::min(at(lo, axis), at(v.uv, axis));
                        at(hi, axis) = std::max(at(hi, axis), at(v.uv, axis));
                    }
                }
                double scale[2] = {1.0, 1.0};
                if (!m_surface.planar)
                {
                    double sum[2] = {0.0, 0.0};
                    int count[2] = {0, 0};
                    for (int i = 0; i <= 4; ++i)
                    {
                        for (int j = 0; j <= 4; ++j)
                        {
                            const math::vec2d uv{lo[0] + (hi[0] - lo[0]) * i / 4.0, lo[1] + (hi[1] - lo[1]) * j / 4.0};
                            math::vec3d p;
                            math::vec3d du;
                            math::vec3d dv;
                            m_surface.derivatives(uv, p, du, dv);
                            const double lu = math::magnitude(du);
                            const double lv = math::magnitude(dv);
                            if (lu > 1e-12 * m_surface.size && std::isfinite(lu))
                            {
                                sum[0] += lu;
                                ++count[0];
                            }
                            if (lv > 1e-12 * m_surface.size && std::isfinite(lv))
                            {
                                sum[1] += lv;
                                ++count[1];
                            }
                        }
                    }
                    for (int axis = 0; axis < 2; ++axis)
                        scale[axis] = count[axis] > 0 ? sum[axis] / count[axis] : 0.0;
                    if (scale[0] <= 0.0)
                        scale[0] = scale[1] > 0.0 ? scale[1] : 1.0;
                    if (scale[1] <= 0.0)
                        scale[1] = scale[0];
                }
                const double extent = std::max((hi[0] - lo[0]) * scale[0], (hi[1] - lo[1]) * scale[1]);
                if (!(extent > 0.0) || !std::isfinite(extent))
                    return std::unexpected("the face has no extent in parameter space");

                const auto to_xy = [&](const math::vec2d &uv) {
                    return math::vec2d{(uv[0] - lo[0]) * scale[0] / extent, (uv[1] - lo[1]) * scale[1] / extent};
                };
                const auto to_uv = [&](const math::vec2d &xy) {
                    return math::vec2d{xy[0] * extent / scale[0] + lo[0], xy[1] * extent / scale[1] + lo[1]};
                };

                mesh2d mesh;
                mesh.points.reserve(m_vertices.size());
                for (const vtx &v : m_vertices)
                    mesh.points.push_back(to_xy(v.uv));

                bool clean = true;
                const std::vector<triangle_indices> triangles = triangulate_polygon(mesh.points, m_rings, clean);
                if (triangles.empty())
                    return std::unexpected("the boundary could not be triangulated");
                mesh.triangles.reserve(triangles.size() * 4);
                for (const triangle_indices &t : triangles)
                    mesh.triangles.push_back({{t[0], t[1], t[2]}, {-1, -1, -1}});
                mesh.connect();
                mesh.make_delaunay();

                std::vector<math::vec2d> uvs;
                std::vector<math::vec3d> positions;
                std::vector<math::vec3d> normals;
                uvs.reserve(m_vertices.size());
                for (const vtx &v : m_vertices)
                {
                    uvs.push_back(v.uv);
                    positions.push_back(v.position);
                    normals.push_back(m_surface.normal(v.uv));
                }

                // A boundary far off its surface means the file's geometry
                // disagrees with itself; points inside, taken from that
                // surface, would only make it worse.
                const bool trusted = m_mismatch <= 20.0 * m_settings.chord;
                if (!m_surface.planar && trusted)
                    refine(mesh, uvs, positions, normals, to_uv);

                face_mesh result;
                result.clean = clean;
                result.mismatch = m_mismatch;
                result.positions = std::move(positions);
                result.normals = std::move(normals);
                result.parameters = std::move(uvs);
                if (m_sense < 0)
                {
                    for (math::vec3d &n : result.normals)
                        n = -n;
                }
                for (const mesh2d::triangle &t : mesh.triangles)
                {
                    const math::vec3d &a = result.positions[t.v[0]];
                    const math::vec3d &b = result.positions[t.v[1]];
                    const math::vec3d &c = result.positions[t.v[2]];
                    // A triangle with two corners at one point (the fans
                    // around a pole, the two sides of a seam) is not there.
                    if (a == b || b == c || c == a)
                        continue;
                    result.indices.push_back(t.v[0]);
                    result.indices.push_back(m_sense > 0 ? t.v[1] : t.v[2]);
                    result.indices.push_back(m_sense > 0 ? t.v[2] : t.v[1]);
                }
                if (result.indices.empty())
                    return std::unexpected("the face has no area");
                return result;
            }

            template <typename ToUv>
            void refine(mesh2d &mesh, std::vector<math::vec2d> &uvs, std::vector<math::vec3d> &positions,
                        std::vector<math::vec3d> &normals, const ToUv &to_uv)
            {
                // Where the boundary is off the surface (a file whose edges and
                // surfaces disagree), nothing inside can be closer than that.
                const double chord = std::max(m_settings.chord, 1.5 * m_mismatch);
                const double angle = m_settings.angle;
                const std::size_t limit = std::max(m_settings.max_points, uvs.size());

                std::vector<std::uint32_t> queue(mesh.triangles.size());
                for (std::uint32_t t = 0; t < queue.size(); ++t)
                    queue[t] = t;
                std::vector<std::uint32_t> touched;

                const auto add_point = [&](const math::vec2d &uv) {
                    uvs.push_back(uv);
                    positions.push_back(m_surface.point(uv));
                    normals.push_back(m_surface.normal(uv));
                };

                std::size_t guard = 0;
                const std::size_t guard_limit = 20 * limit + 1000;
                while (!queue.empty() && uvs.size() < limit && guard++ < guard_limit)
                {
                    const std::uint32_t t = queue.back();
                    queue.pop_back();
                    if (t >= mesh.triangles.size())
                        continue;
                    const mesh2d::triangle tri = mesh.triangles[t];
                    const std::uint32_t a = tri.v[0];
                    const std::uint32_t b = tri.v[1];
                    const std::uint32_t c = tri.v[2];
                    const math::vec3d &pa = positions[a];
                    const math::vec3d &pb = positions[b];
                    const math::vec3d &pc = positions[c];
                    // Two corners at one point: a fan into a pole (which is
                    // fine as it is), or the two sides of a seam (which spans
                    // the whole surface and has to be split). The edges tell.
                    const bool collapsed = pa == pb || pb == pc || pc == pa;
                    const double area_xy = orient(mesh.points[a], mesh.points[b], mesh.points[c]);
                    if (area_xy < 1e-13)
                        continue;

                    // Boundary edges are never split (the neighboring face
                    // has the same points), so whatever error they carry is
                    // the least this triangle can have. Holding it to less
                    // would refine towards that edge without end.
                    double floor_chord = 0.0;
                    double floor_angle = 0.0;
                    int worst_edge = -1;
                    double worst_length = 0.0;
                    for (int k = 0; k < 3; ++k)
                    {
                        const std::uint32_t i = tri.v[static_cast<std::size_t>(k)];
                        const std::uint32_t j = tri.v[static_cast<std::size_t>((k + 1) % 3)];
                        const math::vec3d middle = m_surface.point((uvs[i] + uvs[j]) * 0.5);
                        const double deviation = distance_to_segment(middle, positions[i], positions[j]);
                        const double turn = angle_between(normals[i], normals[j]);
                        if (tri.n[static_cast<std::size_t>(k)] < 0)
                        {
                            floor_chord = std::max(floor_chord, deviation);
                            floor_angle = std::max(floor_angle, turn);
                            continue;
                        }
                        // Interior edges that are off the surface in the
                        // middle, or along which the surface turns too much.
                        const double length = math::distance(mesh.points[i], mesh.points[j]);
                        // Nothing finer than the chord tolerance itself: below
                        // that no error can show, however the surface turns.
                        if (length < 1e-7 || math::distance(positions[i], positions[j]) < m_settings.chord)
                            continue;
                        if ((deviation > chord || turn > angle) && length > worst_length)
                        {
                            worst_edge = k;
                            worst_length = length;
                        }
                    }

                    if (worst_edge < 0)
                    {
                        const double local_chord = std::max(chord, 1.25 * floor_chord);
                        const double local_angle = std::max(angle, 1.25 * floor_angle);

                        // How far the surface's middle is from the triangle's
                        // plane, and whether the triangle faces the way the
                        // surface does at its corners (it does not when it is
                        // a sliver, typically near a pole, where parameter
                        // space is squeezed).
                        bool off = false;
                        bool askew = false;
                        const math::vec3d cross = math::cross(pb - pa, pc - pa);
                        const double length = math::magnitude(cross);
                        if (!collapsed && length > 0.0)
                        {
                            const math::vec3d n = cross / length;
                            const math::vec3d centre = m_surface.point((uvs[a] + uvs[b] + uvs[c]) / 3.0);
                            off = std::abs(math::dot(centre - pa, n)) > local_chord;
                            const double facing = std::max(1.5 * local_angle, 0.6);
                            askew = angle_between(n, normals[a]) > facing || angle_between(n, normals[b]) > facing ||
                                    angle_between(n, normals[c]) > facing;
                        }
                        // Only away from the boundary: a triangle on a
                        // boundary edge near a pole is skewed by the edge
                        // itself, and splitting it only moves points closer
                        // to that edge.
                        const bool on_boundary = tri.n[0] < 0 || tri.n[1] < 0 || tri.n[2] < 0;
                        if (askew && !on_boundary)
                        {
                            // The longest interior edge that can be split
                            // without a sliver.
                            double best = 0.0;
                            for (int k = 0; k < 3; ++k)
                            {
                                if (tri.n[static_cast<std::size_t>(k)] < 0)
                                    continue;
                                const std::uint32_t i = tri.v[static_cast<std::size_t>(k)];
                                const std::uint32_t j = tri.v[static_cast<std::size_t>((k + 1) % 3)];
                                const double l = math::distance(mesh.points[i], mesh.points[j]);
                                if (l > best && l > 1e-7 && math::distance(positions[i], positions[j]) >= m_settings.chord &&
                                    mesh.can_split(t, k, (mesh.points[i] + mesh.points[j]) * 0.5))
                                {
                                    best = l;
                                    worst_edge = k;
                                }
                            }
                        }
                        if (worst_edge < 0 && off)
                        {
                            const math::vec2d xy = (mesh.points[a] + mesh.points[b] + mesh.points[c]) / 3.0;
                            const double size = std::max({math::distance(pa, pb), math::distance(pb, pc), math::distance(pc, pa)});
                            if (area_xy < 1e-11 || size < m_settings.chord || !mesh.can_insert(t, xy))
                                continue;
                            add_point(to_uv(xy));
                            touched.clear();
                            mesh.insert(t, xy, touched);
                            queue.insert(queue.end(), touched.begin(), touched.end());
                            continue;
                        }
                        if (worst_edge < 0)
                            continue;
                    }

                    const std::uint32_t i = tri.v[static_cast<std::size_t>(worst_edge)];
                    const std::uint32_t j = tri.v[static_cast<std::size_t>((worst_edge + 1) % 3)];
                    const math::vec2d xy = (mesh.points[i] + mesh.points[j]) * 0.5;
                    // Never into a sliver: refining a flat triangle only
                    // makes flatter ones.
                    if (!mesh.can_split(t, worst_edge, xy))
                        continue;
                    // The middle in parameters directly, not back through the
                    // stretched coordinates, which would round.
                    add_point((uvs[i] + uvs[j]) * 0.5);
                    touched.clear();
                    mesh.split(t, worst_edge, xy, touched);
                    queue.insert(queue.end(), touched.begin(), touched.end());
                }
            }
        };
    } // namespace

    std::expected<face_mesh, std::string> tessellate_face(const surface &surface,
                                                          const std::vector<std::vector<math::vec3d>> &loops,
                                                          bool same_sense, const face_settings &settings)
    {
        tessellator worker(surface, same_sense, settings);
        return worker.run(loops);
    }

    std::expected<face_mesh, std::string> tessellate_polygon(const std::vector<std::vector<math::vec3d>> &loops,
                                                             const math::vec3d &normal)
    {
        // Newell's normal of each loop; the largest is the outer one.
        std::size_t outer = loops.size();
        math::vec3d outer_normal{};
        double largest = 0.0;
        for (std::size_t k = 0; k < loops.size(); ++k)
        {
            const std::vector<math::vec3d> &loop = loops[k];
            math::vec3d n{};
            for (std::size_t i = 0; i < loop.size(); ++i)
            {
                const math::vec3d &p = loop[i];
                const math::vec3d &q = loop[(i + 1) % loop.size()];
                n[0] += (p[1] - q[1]) * (p[2] + q[2]);
                n[1] += (p[2] - q[2]) * (p[0] + q[0]);
                n[2] += (p[0] - q[0]) * (p[1] + q[1]);
            }
            const double length = math::magnitude(n);
            if (loop.size() >= 3 && length > largest)
            {
                largest = length;
                outer = k;
                outer_normal = n / length;
            }
        }
        if (outer == loops.size() || !(largest > 0.0))
            return std::unexpected("a polygon without area");

        math::vec3d z = outer_normal;
        if (math::magnitude(normal) > 0.0 && math::dot(z, normal) < 0.0)
            z = -z;

        face_mesh result;
        if (loops.size() == 1 && loops[0].size() == 3)
        {
            result.positions = loops[0];
            result.normals.assign(3, z);
            const bool flipped = math::dot(outer_normal, z) < 0.0;
            result.indices = {0, flipped ? 2u : 1u, flipped ? 1u : 2u};
            return result;
        }

        const math::vec3d helper = std::abs(z[0]) < 0.9 ? math::vec3d{1.0, 0.0, 0.0} : math::vec3d{0.0, 1.0, 0.0};
        const math::vec3d x = math::normalized(math::cross(helper, z));
        const math::vec3d y = math::cross(z, x);

        std::vector<math::vec2d> points;
        std::vector<std::vector<std::uint32_t>> rings;
        const auto add = [&](const std::vector<math::vec3d> &loop) {
            std::vector<std::uint32_t> ring;
            for (const math::vec3d &p : loop)
            {
                ring.push_back(static_cast<std::uint32_t>(points.size()));
                points.push_back({math::dot(p, x), math::dot(p, y)});
                result.positions.push_back(p);
            }
            rings.push_back(std::move(ring));
        };
        add(loops[outer]);
        for (std::size_t k = 0; k < loops.size(); ++k)
        {
            if (k != outer && loops[k].size() >= 3)
                add(loops[k]);
        }

        bool clean = true;
        const std::vector<triangle_indices> triangles = triangulate_polygon(points, rings, clean);
        result.clean = clean;
        result.normals.assign(result.positions.size(), z);
        for (const triangle_indices &t : triangles)
        {
            result.indices.push_back(t[0]);
            result.indices.push_back(t[1]);
            result.indices.push_back(t[2]);
        }
        if (result.indices.empty())
            return std::unexpected("the polygon could not be triangulated");
        return result;
    }

} // namespace volt::model::geometry
