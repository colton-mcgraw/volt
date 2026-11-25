#include "triangulate.hpp"

#include <math/geometry.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace volt::model::geometry
{

    // ---------------------------------------------------------------------
    // Ear clipping
    // ---------------------------------------------------------------------
    //
    // A port of earcut's linked-list algorithm. The ring that is outer runs
    // counter-clockwise (area() < 0 at a convex corner, in earcut's sign
    // convention), holes the other way; holes are joined to the outer ring by
    // bridges, after which there is one ring to clip ears from.

    namespace
    {
        constexpr int none = -1;

        struct node
        {
            std::uint32_t i = 0; // vertex index
            double x = 0.0;
            double y = 0.0;
            int prev = none;
            int next = none;
            std::int32_t z = 0;
            int prev_z = none;
            int next_z = none;
            bool steiner = false;
        };

        struct removal
        {
            std::uint32_t vertex;
            std::uint32_t a;
            std::uint32_t b;
        };

        class earcut
        {
        public:
            earcut(const std::vector<math::vec2d> &points, std::vector<triangle_indices> &out)
                : m_points(points), m_out(out)
            {
            }

            bool clean = true;
            std::vector<removal> removed;

            void run(const std::vector<std::vector<std::uint32_t>> &rings)
            {
                std::size_t total = 0;
                for (const auto &ring : rings)
                    total += ring.size();
                m_nodes.reserve(total * 2 + 16);

                int outer = linked_list(rings[0], true);
                if (outer == none || at(outer).next == at(outer).prev)
                    return;
                if (rings.size() > 1)
                    outer = eliminate_holes(rings, outer);

                if (total > 80)
                {
                    m_min_x = m_min_y = std::numeric_limits<double>::max();
                    double max_x = std::numeric_limits<double>::lowest();
                    double max_y = std::numeric_limits<double>::lowest();
                    for (const auto &ring : rings)
                    {
                        for (std::uint32_t index : ring)
                        {
                            m_min_x = std::min(m_min_x, m_points[index][0]);
                            m_min_y = std::min(m_min_y, m_points[index][1]);
                            max_x = std::max(max_x, m_points[index][0]);
                            max_y = std::max(max_y, m_points[index][1]);
                        }
                    }
                    const double size = std::max(max_x - m_min_x, max_y - m_min_y);
                    m_inv_size = size != 0.0 ? 32767.0 / size : 0.0;
                }

                earcut_linked(outer, 0);
            }

        private:
            const std::vector<math::vec2d> &m_points;
            std::vector<triangle_indices> &m_out;
            std::vector<node> m_nodes;
            double m_min_x = 0.0;
            double m_min_y = 0.0;
            double m_inv_size = 0.0;

            node &at(int index) { return m_nodes[static_cast<std::size_t>(index)]; }

            // Points come in normalized to about the unit square. Three
            // points this close to a line are on it: boundaries sampled along
            // a straight line in parameter space carry rounding noise, and
            // ears of noise are slivers.
            static double area(const node &p, const node &q, const node &r)
            {
                const double a = (q.y - p.y) * (r.x - q.x) - (q.x - p.x) * (r.y - q.y);
                return std::abs(a) < 1e-14 ? 0.0 : a;
            }

            static bool equals(const node &a, const node &b) { return a.x == b.x && a.y == b.y; }

            int insert_node(std::uint32_t i, int last)
            {
                node n;
                n.i = i;
                n.x = m_points[i][0];
                n.y = m_points[i][1];
                const int index = static_cast<int>(m_nodes.size());
                m_nodes.push_back(n);
                if (last == none)
                {
                    at(index).prev = index;
                    at(index).next = index;
                }
                else
                {
                    at(index).next = at(last).next;
                    at(index).prev = last;
                    at(at(last).next).prev = index;
                    at(last).next = index;
                }
                return index;
            }

            void remove_node(int p)
            {
                node &n = at(p);
                at(n.next).prev = n.prev;
                at(n.prev).next = n.next;
                if (n.prev_z != none)
                    at(n.prev_z).next_z = n.next_z;
                if (n.next_z != none)
                    at(n.next_z).prev_z = n.prev_z;
            }

            int linked_list(const std::vector<std::uint32_t> &ring, bool outer)
            {
                double sum = 0.0;
                for (std::size_t k = 0, j = ring.size() - 1; k < ring.size(); j = k++)
                {
                    const math::vec2d &a = m_points[ring[j]];
                    const math::vec2d &b = m_points[ring[k]];
                    sum += (a[0] - b[0]) * (b[1] + a[1]);
                }
                int last = none;
                if (outer == (sum > 0.0))
                {
                    for (std::uint32_t index : ring)
                        last = insert_node(index, last);
                }
                else
                {
                    for (auto it = ring.rbegin(); it != ring.rend(); ++it)
                        last = insert_node(*it, last);
                }
                if (last != none && equals(at(last), at(at(last).next)) && at(last).i == at(at(last).next).i)
                {
                    const int next = at(last).next;
                    remove_node(last);
                    last = next;
                }
                return last;
            }

            // Drops nodes that repeat their neighbor or lie on a straight line
            // between their neighbors, remembering where they were.
            int filter_points(int start, int end = none)
            {
                if (start == none)
                    return start;
                if (end == none)
                    end = start;

                int p = start;
                bool again = false;
                do
                {
                    again = false;
                    node &n = at(p);
                    if (!n.steiner && (equals(n, at(n.next)) || area(at(n.prev), n, at(n.next)) == 0.0))
                    {
                        const std::uint32_t a = at(n.prev).i;
                        const std::uint32_t b = at(n.next).i;
                        if (n.i != a && n.i != b && a != b)
                            removed.push_back({n.i, a, b});
                        remove_node(p);
                        p = end = n.prev;
                        if (p == at(p).next)
                            break;
                        again = true;
                    }
                    else
                        p = n.next;
                } while (again || p != end);
                return end;
            }

            void emit(int a, int b, int c) { m_out.push_back({at(a).i, at(b).i, at(c).i}); }

            void earcut_linked(int ear, int pass)
            {
                if (ear == none)
                    return;
                if (pass == 0 && m_inv_size != 0.0)
                    index_curve(ear);

                int stop = ear;
                while (at(ear).prev != at(ear).next)
                {
                    const int prev = at(ear).prev;
                    const int next = at(ear).next;
                    if (m_inv_size != 0.0 ? is_ear_hashed(ear) : is_ear(ear))
                    {
                        emit(prev, ear, next);
                        remove_node(ear);
                        ear = at(next).next;
                        stop = at(next).next;
                        continue;
                    }
                    ear = next;
                    if (ear == stop)
                    {
                        if (pass == 0)
                            earcut_linked(filter_points(ear), 1);
                        else if (pass == 1)
                        {
                            ear = cure_local_intersections(filter_points(ear));
                            earcut_linked(ear, 2);
                        }
                        else
                            split_earcut(ear);
                        break;
                    }
                }
            }

            bool point_in_triangle(double ax, double ay, double bx, double by, double cx, double cy, double px,
                                   double py) const
            {
                return (cx - px) * (ay - py) >= (ax - px) * (cy - py) && (ax - px) * (by - py) >= (bx - px) * (ay - py) &&
                       (bx - px) * (cy - py) >= (cx - px) * (by - py);
            }

            bool is_ear(int ear)
            {
                const node &a = at(at(ear).prev);
                const node &b = at(ear);
                const node &c = at(at(ear).next);
                if (area(a, b, c) >= 0.0)
                    return false;

                const double x0 = std::min({a.x, b.x, c.x});
                const double y0 = std::min({a.y, b.y, c.y});
                const double x1 = std::max({a.x, b.x, c.x});
                const double y1 = std::max({a.y, b.y, c.y});

                int p = c.next;
                while (p != b.prev)
                {
                    const node &n = at(p);
                    if (n.x >= x0 && n.x <= x1 && n.y >= y0 && n.y <= y1 &&
                        point_in_triangle(a.x, a.y, b.x, b.y, c.x, c.y, n.x, n.y) &&
                        area(at(n.prev), n, at(n.next)) >= 0.0)
                        return false;
                    p = n.next;
                }
                return true;
            }

            bool blocks(const node &n, int a_index, int c_index, const node &a, const node &b, const node &c, double x0,
                        double y0, double x1, double y1, int self)
            {
                return self != a_index && self != c_index && n.x >= x0 && n.x <= x1 && n.y >= y0 && n.y <= y1 &&
                       point_in_triangle(a.x, a.y, b.x, b.y, c.x, c.y, n.x, n.y) &&
                       area(at(n.prev), n, at(n.next)) >= 0.0;
            }

            bool is_ear_hashed(int ear)
            {
                const int a_index = at(ear).prev;
                const int c_index = at(ear).next;
                const node a = at(a_index);
                const node b = at(ear);
                const node c = at(c_index);
                if (area(a, b, c) >= 0.0)
                    return false;

                const double x0 = std::min({a.x, b.x, c.x});
                const double y0 = std::min({a.y, b.y, c.y});
                const double x1 = std::max({a.x, b.x, c.x});
                const double y1 = std::max({a.y, b.y, c.y});
                const std::int32_t min_z = z_order(x0, y0);
                const std::int32_t max_z = z_order(x1, y1);

                int p = b.prev_z;
                int n = b.next_z;
                while (p != none && at(p).z >= min_z && n != none && at(n).z <= max_z)
                {
                    if (blocks(at(p), a_index, c_index, a, b, c, x0, y0, x1, y1, p))
                        return false;
                    p = at(p).prev_z;
                    if (blocks(at(n), a_index, c_index, a, b, c, x0, y0, x1, y1, n))
                        return false;
                    n = at(n).next_z;
                }
                while (p != none && at(p).z >= min_z)
                {
                    if (blocks(at(p), a_index, c_index, a, b, c, x0, y0, x1, y1, p))
                        return false;
                    p = at(p).prev_z;
                }
                while (n != none && at(n).z <= max_z)
                {
                    if (blocks(at(n), a_index, c_index, a, b, c, x0, y0, x1, y1, n))
                        return false;
                    n = at(n).next_z;
                }
                return true;
            }

            static int sign(double v) { return v > 0.0 ? 1 : (v < 0.0 ? -1 : 0); }

            static bool on_segment(const node &p, const node &q, const node &r)
            {
                return q.x <= std::max(p.x, r.x) && q.x >= std::min(p.x, r.x) && q.y <= std::max(p.y, r.y) &&
                       q.y >= std::min(p.y, r.y);
            }

            static bool intersects(const node &p1, const node &q1, const node &p2, const node &q2)
            {
                const int o1 = sign(area(p1, q1, p2));
                const int o2 = sign(area(p1, q1, q2));
                const int o3 = sign(area(p2, q2, p1));
                const int o4 = sign(area(p2, q2, q1));
                if (o1 != o2 && o3 != o4)
                    return true;
                if (o1 == 0 && on_segment(p1, p2, q1))
                    return true;
                if (o2 == 0 && on_segment(p1, q2, q1))
                    return true;
                if (o3 == 0 && on_segment(p2, p1, q2))
                    return true;
                if (o4 == 0 && on_segment(p2, q1, q2))
                    return true;
                return false;
            }

            bool locally_inside(int a_index, int b_index)
            {
                const node &a = at(a_index);
                const node &b = at(b_index);
                return area(at(a.prev), a, at(a.next)) < 0.0
                           ? area(a, b, at(a.next)) >= 0.0 && area(a, at(a.prev), b) >= 0.0
                           : area(a, b, at(a.prev)) < 0.0 || area(a, at(a.next), b) < 0.0;
            }

            int cure_local_intersections(int start)
            {
                if (start == none)
                    return start;
                int p = start;
                do
                {
                    const int a = at(p).prev;
                    const int b = at(at(p).next).next;
                    if (!equals(at(a), at(b)) && intersects(at(a), at(p), at(at(p).next), at(b)) &&
                        locally_inside(a, b) && locally_inside(b, a))
                    {
                        emit(a, p, b);
                        const int after = at(p).next;
                        remove_node(p);
                        remove_node(after);
                        p = start = b;
                    }
                    p = at(p).next;
                } while (p != start);
                return filter_points(p);
            }

            bool intersects_polygon(int a_index, int b_index)
            {
                const node &a = at(a_index);
                const node &b = at(b_index);
                int p = a_index;
                do
                {
                    const node &n = at(p);
                    const node &m = at(n.next);
                    if (n.i != a.i && m.i != a.i && n.i != b.i && m.i != b.i && intersects(n, m, a, b))
                        return true;
                    p = n.next;
                } while (p != a_index);
                return false;
            }

            bool middle_inside(int a_index, int b_index)
            {
                const node &a = at(a_index);
                const node &b = at(b_index);
                int p = a_index;
                bool inside = false;
                const double px = (a.x + b.x) / 2.0;
                const double py = (a.y + b.y) / 2.0;
                do
                {
                    const node &n = at(p);
                    const node &m = at(n.next);
                    if (((n.y > py) != (m.y > py)) && m.y != n.y && (px < (m.x - n.x) * (py - n.y) / (m.y - n.y) + n.x))
                        inside = !inside;
                    p = n.next;
                } while (p != a_index);
                return inside;
            }

            bool is_valid_diagonal(int a_index, int b_index)
            {
                const node &a = at(a_index);
                const node &b = at(b_index);
                return at(a.next).i != b.i && at(a.prev).i != b.i && !intersects_polygon(a_index, b_index) &&
                       ((locally_inside(a_index, b_index) && locally_inside(b_index, a_index) &&
                         middle_inside(a_index, b_index) &&
                         (area(at(a.prev), a, at(b.prev)) != 0.0 || area(a, at(b.prev), b) != 0.0)) ||
                        (equals(a, b) && area(at(a.prev), a, at(a.next)) > 0.0 && area(at(b.prev), b, at(b.next)) > 0.0));
            }

            // Joins a and b with a bridge: two rings out of one, or one out of
            // two. Returns the copy of b.
            int split_polygon(int a, int b)
            {
                node a_copy = at(a);
                node b_copy = at(b);
                a_copy.prev_z = a_copy.next_z = none;
                b_copy.prev_z = b_copy.next_z = none;
                a_copy.z = b_copy.z = 0;
                const int a2 = static_cast<int>(m_nodes.size());
                m_nodes.push_back(a_copy);
                const int b2 = static_cast<int>(m_nodes.size());
                m_nodes.push_back(b_copy);

                const int an = at(a).next;
                const int bp = at(b).prev;

                at(a).next = b;
                at(b).prev = a;

                at(a2).next = an;
                at(an).prev = a2;

                at(b2).next = a2;
                at(a2).prev = b2;

                at(bp).next = b2;
                at(b2).prev = bp;
                return b2;
            }

            void split_earcut(int start)
            {
                int a = start;
                do
                {
                    int b = at(at(a).next).next;
                    while (b != at(a).prev)
                    {
                        if (at(a).i != at(b).i && is_valid_diagonal(a, b))
                        {
                            int c = split_polygon(a, b);
                            a = filter_points(a, at(a).next);
                            c = filter_points(c, at(c).next);
                            earcut_linked(a, 0);
                            earcut_linked(c, 0);
                            return;
                        }
                        b = at(b).next;
                    }
                    a = at(a).next;
                } while (a != start);

                // Nothing left that is a proper polygon: fill what remains as
                // a fan, so that no vertex is lost, and say so.
                clean = false;
                const int first = start;
                int p = at(first).next;
                while (at(p).next != first)
                {
                    emit(first, p, at(p).next);
                    p = at(p).next;
                }
            }

            int leftmost(int start)
            {
                int p = start;
                int best = start;
                do
                {
                    if (at(p).x < at(best).x || (at(p).x == at(best).x && at(p).y < at(best).y))
                        best = p;
                    p = at(p).next;
                } while (p != start);
                return best;
            }

            int eliminate_holes(const std::vector<std::vector<std::uint32_t>> &rings, int outer)
            {
                std::vector<int> queue;
                for (std::size_t r = 1; r < rings.size(); ++r)
                {
                    if (rings[r].empty())
                        continue;
                    const int list = linked_list(rings[r], false);
                    if (list == none)
                        continue;
                    if (list == at(list).next)
                        at(list).steiner = true;
                    queue.push_back(leftmost(list));
                }
                std::sort(queue.begin(), queue.end(), [this](int a, int b) {
                    if (at(a).x != at(b).x)
                        return at(a).x < at(b).x;
                    return at(a).y < at(b).y;
                });
                for (int hole : queue)
                    outer = eliminate_hole(hole, outer);
                return outer;
            }

            bool sector_contains_sector(int m, int p)
            {
                return area(at(at(m).prev), at(m), at(at(p).prev)) < 0.0 &&
                       area(at(at(p).next), at(m), at(at(m).next)) < 0.0;
            }

            int find_hole_bridge(int hole, int outer)
            {
                int p = outer;
                const double hx = at(hole).x;
                const double hy = at(hole).y;
                double qx = -std::numeric_limits<double>::infinity();
                int m = none;
                do
                {
                    const node &n = at(p);
                    const node &next = at(n.next);
                    if (hy <= n.y && hy >= next.y && next.y != n.y)
                    {
                        const double x = n.x + (hy - n.y) * (next.x - n.x) / (next.y - n.y);
                        if (x <= hx && x > qx)
                        {
                            qx = x;
                            m = n.x < next.x ? p : n.next;
                            if (x == hx)
                                return m;
                        }
                    }
                    p = n.next;
                } while (p != outer);
                if (m == none)
                    return none;

                const int stop = m;
                const double mx = at(m).x;
                const double my = at(m).y;
                double tan_min = std::numeric_limits<double>::infinity();
                p = m;
                do
                {
                    const node &n = at(p);
                    if (hx >= n.x && n.x >= mx && hx != n.x &&
                        point_in_triangle(hy < my ? hx : qx, hy, mx, my, hy < my ? qx : hx, hy, n.x, n.y))
                    {
                        const double tan = std::abs(hy - n.y) / (hx - n.x);
                        if (locally_inside(p, hole) &&
                            (tan < tan_min ||
                             (tan == tan_min && (n.x > at(m).x || (n.x == at(m).x && sector_contains_sector(m, p))))))
                        {
                            m = p;
                            tan_min = tan;
                        }
                    }
                    p = n.next;
                } while (p != stop);
                return m;
            }

            int eliminate_hole(int hole, int outer)
            {
                const int bridge = find_hole_bridge(hole, outer);
                if (bridge == none)
                {
                    // A hole outside the outer ring, or touching it in a way
                    // no bridge resolves: its triangles are missing.
                    clean = false;
                    return outer;
                }
                const int reverse = split_polygon(bridge, hole);
                filter_points(reverse, at(reverse).next);
                return filter_points(bridge, at(bridge).next);
            }

            std::int32_t z_order(double x, double y) const
            {
                auto ix = static_cast<std::uint32_t>(std::clamp((x - m_min_x) * m_inv_size, 0.0, 32767.0));
                auto iy = static_cast<std::uint32_t>(std::clamp((y - m_min_y) * m_inv_size, 0.0, 32767.0));
                ix = (ix | (ix << 8)) & 0x00FF00FFu;
                ix = (ix | (ix << 4)) & 0x0F0F0F0Fu;
                ix = (ix | (ix << 2)) & 0x33333333u;
                ix = (ix | (ix << 1)) & 0x55555555u;
                iy = (iy | (iy << 8)) & 0x00FF00FFu;
                iy = (iy | (iy << 4)) & 0x0F0F0F0Fu;
                iy = (iy | (iy << 2)) & 0x33333333u;
                iy = (iy | (iy << 1)) & 0x55555555u;
                return static_cast<std::int32_t>(ix | (iy << 1));
            }

            void index_curve(int start)
            {
                int p = start;
                do
                {
                    node &n = at(p);
                    if (n.z == 0)
                        n.z = z_order(n.x, n.y);
                    n.prev_z = n.prev;
                    n.next_z = n.next;
                    p = n.next;
                } while (p != start);
                at(at(p).prev_z).next_z = none;
                at(p).prev_z = none;
                sort_linked(p);
            }

            // Simon Tatham's merge sort of a linked list, by z.
            int sort_linked(int list)
            {
                int in_size = 1;
                int merges = 0;
                do
                {
                    int p = list;
                    list = none;
                    int tail = none;
                    merges = 0;
                    while (p != none)
                    {
                        ++merges;
                        int q = p;
                        int p_size = 0;
                        for (int i = 0; i < in_size; ++i)
                        {
                            ++p_size;
                            q = at(q).next_z;
                            if (q == none)
                                break;
                        }
                        int q_size = in_size;
                        while (p_size > 0 || (q_size > 0 && q != none))
                        {
                            int e = none;
                            if (p_size != 0 && (q_size == 0 || q == none || at(p).z <= at(q).z))
                            {
                                e = p;
                                p = at(p).next_z;
                                --p_size;
                            }
                            else
                            {
                                e = q;
                                q = at(q).next_z;
                                --q_size;
                            }
                            if (tail != none)
                                at(tail).next_z = e;
                            else
                                list = e;
                            at(e).prev_z = tail;
                            tail = e;
                        }
                        p = q;
                    }
                    if (tail != none)
                        at(tail).next_z = none;
                    in_size *= 2;
                } while (merges > 1);
                return list;
            }
        };

        std::uint64_t edge_key(std::uint32_t a, std::uint32_t b)
        {
            return (static_cast<std::uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
        }
    } // namespace

    std::vector<triangle_indices> triangulate_polygon(const std::vector<math::vec2d> &points,
                                                      const std::vector<std::vector<std::uint32_t>> &rings, bool &clean)
    {
        std::vector<triangle_indices> triangles;
        clean = true;
        if (rings.empty() || rings[0].size() < 3)
        {
            clean = false;
            return triangles;
        }

        earcut cutter(points, triangles);
        cutter.run(rings);
        clean = cutter.clean;

        // Put back the vertices earcut skipped, latest first: each lies on
        // the edge between the two it was skipped between, and every edge of
        // the ring ends up as an edge of a triangle.
        if (!cutter.removed.empty())
        {
            std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> edges;
            const auto add = [&](std::uint32_t t) {
                for (int k = 0; k < 3; ++k)
                    edges[edge_key(triangles[t][static_cast<std::size_t>(k)], triangles[t][static_cast<std::size_t>((k + 1) % 3)])].push_back(t);
            };
            const auto drop = [&](std::uint32_t t) {
                for (int k = 0; k < 3; ++k)
                {
                    auto &list = edges[edge_key(triangles[t][static_cast<std::size_t>(k)], triangles[t][static_cast<std::size_t>((k + 1) % 3)])];
                    list.erase(std::remove(list.begin(), list.end(), t), list.end());
                }
            };
            for (std::uint32_t t = 0; t < triangles.size(); ++t)
                add(t);

            for (auto it = cutter.removed.rbegin(); it != cutter.removed.rend(); ++it)
            {
                const auto found = edges.find(edge_key(it->a, it->b));
                if (found == edges.end() || found->second.empty())
                {
                    clean = false;
                    continue;
                }
                const std::vector<std::uint32_t> users = found->second;
                for (std::uint32_t t : users)
                {
                    const triangle_indices old = triangles[t];
                    int k = 0;
                    while (k < 3 && edge_key(old[static_cast<std::size_t>(k)], old[static_cast<std::size_t>((k + 1) % 3)]) != edge_key(it->a, it->b))
                        ++k;
                    if (k == 3)
                        continue;
                    const std::uint32_t p = old[static_cast<std::size_t>(k)];
                    const std::uint32_t q = old[static_cast<std::size_t>((k + 1) % 3)];
                    const std::uint32_t r = old[static_cast<std::size_t>((k + 2) % 3)];
                    drop(t);
                    triangles[t] = {p, it->vertex, r};
                    add(t);
                    triangles.push_back({it->vertex, q, r});
                    add(static_cast<std::uint32_t>(triangles.size() - 1));
                }
            }
        }
        return triangles;
    }

    // ---------------------------------------------------------------------
    // mesh2d
    // ---------------------------------------------------------------------

    void mesh2d::connect()
    {
        std::unordered_map<std::uint64_t, std::pair<std::int32_t, int>> directed;
        directed.reserve(triangles.size() * 3);
        for (std::size_t t = 0; t < triangles.size(); ++t)
        {
            triangles[t].n = {-1, -1, -1};
            for (int k = 0; k < 3; ++k)
            {
                const std::uint32_t a = triangles[t].v[static_cast<std::size_t>(k)];
                const std::uint32_t b = triangles[t].v[static_cast<std::size_t>((k + 1) % 3)];
                directed[(static_cast<std::uint64_t>(a) << 32) | b] = {static_cast<std::int32_t>(t), k};
            }
        }
        for (std::size_t t = 0; t < triangles.size(); ++t)
        {
            for (int k = 0; k < 3; ++k)
            {
                const std::uint32_t a = triangles[t].v[static_cast<std::size_t>(k)];
                const std::uint32_t b = triangles[t].v[static_cast<std::size_t>((k + 1) % 3)];
                const auto it = directed.find((static_cast<std::uint64_t>(b) << 32) | a);
                if (it != directed.end() && it->second.first != static_cast<std::int32_t>(t))
                    triangles[t].n[static_cast<std::size_t>(k)] = it->second.first;
            }
        }
        // Edges used more than twice, or twice the same way, cannot be
        // trusted as interior: make sure neighbor links are mutual.
        for (std::size_t t = 0; t < triangles.size(); ++t)
        {
            for (int k = 0; k < 3; ++k)
            {
                const std::int32_t u = triangles[t].n[static_cast<std::size_t>(k)];
                if (u < 0)
                    continue;
                const auto &other = triangles[static_cast<std::size_t>(u)];
                if (std::find(other.n.begin(), other.n.end(), static_cast<std::int32_t>(t)) == other.n.end())
                    triangles[t].n[static_cast<std::size_t>(k)] = -1;
            }
        }
    }

    void mesh2d::relink(std::int32_t triangle, std::int32_t from, std::int32_t to)
    {
        if (triangle < 0)
            return;
        for (std::int32_t &n : triangles[static_cast<std::size_t>(triangle)].n)
        {
            if (n == from)
            {
                n = to;
                return;
            }
        }
    }

    namespace
    {
        // Positive when d lies inside the circle through the
        // counter-clockwise triangle abc.
        double incircle(const math::vec2d &a, const math::vec2d &b, const math::vec2d &c, const math::vec2d &d)
        {
            const double adx = a[0] - d[0];
            const double ady = a[1] - d[1];
            const double bdx = b[0] - d[0];
            const double bdy = b[1] - d[1];
            const double cdx = c[0] - d[0];
            const double cdy = c[1] - d[1];
            return (adx * adx + ady * ady) * (bdx * cdy - cdx * bdy) - (bdx * bdx + bdy * bdy) * (adx * cdy - cdx * ady) +
                   (cdx * cdx + cdy * cdy) * (adx * bdy - bdx * ady);
        }
    } // namespace

    bool mesh2d::flip(std::uint32_t t, int edge)
    {
        triangle &tri = triangles[t];
        const std::int32_t u_index = tri.n[static_cast<std::size_t>(edge)];
        if (u_index < 0)
            return false;
        triangle &other = triangles[static_cast<std::size_t>(u_index)];

        const std::uint32_t a = tri.v[static_cast<std::size_t>(edge)];
        const std::uint32_t b = tri.v[static_cast<std::size_t>((edge + 1) % 3)];
        const std::uint32_t c = tri.v[static_cast<std::size_t>((edge + 2) % 3)];
        int j = 0;
        while (j < 3 && !(other.v[static_cast<std::size_t>(j)] == b && other.v[static_cast<std::size_t>((j + 1) % 3)] == a))
            ++j;
        if (j == 3)
            return false;
        const std::uint32_t d = other.v[static_cast<std::size_t>((j + 2) % 3)];
        if (d == c)
            return false;

        // The two new triangles must both turn the right way: the quad is
        // convex (or c, d see each other past a collinear a or b).
        if (!proper(points[a], points[d], points[c]) || !proper(points[d], points[b], points[c]))
            return false;

        const std::int32_t n_bc = tri.n[static_cast<std::size_t>((edge + 1) % 3)];
        const std::int32_t n_ca = tri.n[static_cast<std::size_t>((edge + 2) % 3)];
        const std::int32_t n_ad = other.n[static_cast<std::size_t>((j + 1) % 3)];
        const std::int32_t n_db = other.n[static_cast<std::size_t>((j + 2) % 3)];
        const auto t_index = static_cast<std::int32_t>(t);

        tri.v = {a, d, c};
        tri.n = {n_ad, u_index, n_ca};
        other.v = {d, b, c};
        other.n = {n_db, n_bc, t_index};
        relink(n_ad, u_index, t_index);
        relink(n_bc, t_index, u_index);
        return true;
    }

    void mesh2d::legalize(std::vector<std::pair<std::uint32_t, int>> &stack, std::vector<std::uint32_t> *touched)
    {
        std::size_t budget = 64 * triangles.size() + 1024;
        while (!stack.empty() && budget-- > 0)
        {
            const auto [t, edge] = stack.back();
            stack.pop_back();
            const triangle &tri = triangles[t];
            const std::int32_t u = tri.n[static_cast<std::size_t>(edge)];
            if (u < 0)
                continue;
            const triangle &other = triangles[static_cast<std::size_t>(u)];
            const std::uint32_t a = tri.v[static_cast<std::size_t>(edge)];
            const std::uint32_t b = tri.v[static_cast<std::size_t>((edge + 1) % 3)];
            const std::uint32_t c = tri.v[static_cast<std::size_t>((edge + 2) % 3)];
            std::uint32_t d = c;
            for (int j = 0; j < 3; ++j)
            {
                if (other.v[static_cast<std::size_t>(j)] == b && other.v[static_cast<std::size_t>((j + 1) % 3)] == a)
                    d = other.v[static_cast<std::size_t>((j + 2) % 3)];
            }
            if (d == c)
                continue;
            // A collinear (zero area) triangle always yields to a flip.
            // A flat triangle on either side always yields to a flip. Else
            // the circle test, relative to the size of the two triangles
            // (it grows with the fourth power of it).
            const bool degenerate = !proper(points[a], points[b], points[c]) || !proper(points[b], points[a], points[d]);
            if (!degenerate)
            {
                const double size = std::max({math::distance_squared(points[a], points[b]),
                                              math::distance_squared(points[a], points[c]),
                                              math::distance_squared(points[b], points[c]),
                                              math::distance_squared(points[a], points[d]),
                                              math::distance_squared(points[b], points[d])});
                if (incircle(points[a], points[b], points[c], points[d]) <= 1e-12 * size * size)
                    continue;
            }
            if (!flip(t, edge))
                continue;
            if (touched != nullptr)
            {
                touched->push_back(t);
                touched->push_back(static_cast<std::uint32_t>(u));
            }
            // After the flip t = (a, d, c) and u = (d, b, c): their outer
            // edges may now be the illegal ones.
            stack.push_back({t, 0});
            stack.push_back({t, 2});
            stack.push_back({static_cast<std::uint32_t>(u), 0});
            stack.push_back({static_cast<std::uint32_t>(u), 1});
        }
    }

    void mesh2d::make_delaunay()
    {
        std::vector<std::pair<std::uint32_t, int>> stack;
        stack.reserve(triangles.size() * 3);
        for (std::uint32_t t = 0; t < triangles.size(); ++t)
            for (int k = 0; k < 3; ++k)
                if (triangles[t].n[static_cast<std::size_t>(k)] > static_cast<std::int32_t>(t))
                    stack.push_back({t, k});
        legalize(stack, nullptr);
    }

    bool mesh2d::can_insert(std::uint32_t t, const math::vec2d &p) const
    {
        const triangle &tri = triangles[t];
        const math::vec2d &a = points[tri.v[0]];
        const math::vec2d &b = points[tri.v[1]];
        const math::vec2d &c = points[tri.v[2]];
        return proper(a, b, p) && proper(b, c, p) && proper(c, a, p);
    }

    bool mesh2d::can_split(std::uint32_t t, int edge, const math::vec2d &p) const
    {
        const triangle &tri = triangles[t];
        const math::vec2d &a = points[tri.v[static_cast<std::size_t>(edge)]];
        const math::vec2d &b = points[tri.v[static_cast<std::size_t>((edge + 1) % 3)]];
        const math::vec2d &c = points[tri.v[static_cast<std::size_t>((edge + 2) % 3)]];
        if (!proper(a, p, c) || !proper(p, b, c))
            return false;
        const std::int32_t u = tri.n[static_cast<std::size_t>(edge)];
        if (u < 0)
            return true;
        const triangle &other = triangles[static_cast<std::size_t>(u)];
        for (std::size_t j = 0; j < 3; ++j)
        {
            if (other.v[j] == tri.v[static_cast<std::size_t>((edge + 1) % 3)] &&
                other.v[(j + 1) % 3] == tri.v[static_cast<std::size_t>(edge)])
            {
                const math::vec2d &d = points[other.v[(j + 2) % 3]];
                return proper(b, p, d) && proper(p, a, d);
            }
        }
        return false;
    }

    std::uint32_t mesh2d::insert(std::uint32_t t, const math::vec2d &p, std::vector<std::uint32_t> &touched)
    {
        const auto m = static_cast<std::uint32_t>(points.size());
        points.push_back(p);

        const triangle old = triangles[t];
        const std::uint32_t a = old.v[0];
        const std::uint32_t b = old.v[1];
        const std::uint32_t c = old.v[2];
        const auto t1 = static_cast<std::int32_t>(t);
        const auto t2 = static_cast<std::int32_t>(triangles.size());
        const auto t3 = t2 + 1;

        triangles[t] = {{a, b, m}, {old.n[0], t2, t3}};
        triangles.push_back({{b, c, m}, {old.n[1], t3, t1}});
        triangles.push_back({{c, a, m}, {old.n[2], t1, t2}});
        relink(old.n[1], t1, t2);
        relink(old.n[2], t1, t3);

        touched.push_back(t);
        touched.push_back(static_cast<std::uint32_t>(t2));
        touched.push_back(static_cast<std::uint32_t>(t3));
        std::vector<std::pair<std::uint32_t, int>> stack{
            {t, 0}, {static_cast<std::uint32_t>(t2), 0}, {static_cast<std::uint32_t>(t3), 0}};
        legalize(stack, &touched);
        return m;
    }

    std::uint32_t mesh2d::split(std::uint32_t t, int edge, const math::vec2d &p, std::vector<std::uint32_t> &touched)
    {
        const std::int32_t u_signed = triangles[t].n[static_cast<std::size_t>(edge)];
        if (u_signed < 0)
            return insert(t, p, touched);
        const auto u = static_cast<std::uint32_t>(u_signed);

        const triangle tri = triangles[t];
        const triangle other = triangles[u];
        const std::uint32_t a = tri.v[static_cast<std::size_t>(edge)];
        const std::uint32_t b = tri.v[static_cast<std::size_t>((edge + 1) % 3)];
        const std::uint32_t c = tri.v[static_cast<std::size_t>((edge + 2) % 3)];
        int j = 0;
        while (j < 3 && !(other.v[static_cast<std::size_t>(j)] == b && other.v[static_cast<std::size_t>((j + 1) % 3)] == a))
            ++j;
        if (j == 3)
            return insert(t, p, touched);
        const std::uint32_t d = other.v[static_cast<std::size_t>((j + 2) % 3)];

        const std::int32_t n_bc = tri.n[static_cast<std::size_t>((edge + 1) % 3)];
        const std::int32_t n_ca = tri.n[static_cast<std::size_t>((edge + 2) % 3)];
        const std::int32_t n_ad = other.n[static_cast<std::size_t>((j + 1) % 3)];
        const std::int32_t n_db = other.n[static_cast<std::size_t>((j + 2) % 3)];

        const auto m = static_cast<std::uint32_t>(points.size());
        points.push_back(p);

        const auto t1 = static_cast<std::int32_t>(t);
        const auto u1 = static_cast<std::int32_t>(u);
        const auto t2 = static_cast<std::int32_t>(triangles.size());
        const auto u2 = t2 + 1;

        triangles[t] = {{a, m, c}, {u2, t2, n_ca}};
        triangles[u] = {{b, m, d}, {t2, u2, n_db}};
        triangles.push_back({{m, b, c}, {u1, n_bc, t1}});
        triangles.push_back({{m, a, d}, {t1, n_ad, u1}});
        relink(n_bc, t1, t2);
        relink(n_ad, u1, u2);

        touched.push_back(t);
        touched.push_back(u);
        touched.push_back(static_cast<std::uint32_t>(t2));
        touched.push_back(static_cast<std::uint32_t>(u2));
        std::vector<std::pair<std::uint32_t, int>> stack{{t, 2},
                                                         {u, 2},
                                                         {static_cast<std::uint32_t>(t2), 1},
                                                         {static_cast<std::uint32_t>(u2), 1}};
        legalize(stack, &touched);
        return m;
    }

} // namespace volt::model::geometry
