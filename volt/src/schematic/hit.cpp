#include <schematic/hit.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <numeric>

namespace volt::schematic
{

    namespace
    {
        math::vec2f to_float(point p) noexcept
        {
            return math::vec2f{static_cast<float>(p.x()), static_cast<float>(p.y())};
        }

        float distance(math::vec2f a, math::vec2f b) noexcept
        {
            const math::vec2f d = a - b;
            return std::sqrt(d.x() * d.x() + d.y() * d.y());
        }

        // Where along the segment the point projects to, 0 at a and 1 at
        // b, clamped.
        float along(math::vec2f p, math::vec2f a, math::vec2f b) noexcept
        {
            const math::vec2f d = b - a;
            const float length2 = d.x() * d.x() + d.y() * d.y();
            if (!(length2 > 0.0f))
                return 0.0f;
            const math::vec2f e = p - a;
            return std::clamp((e.x() * d.x() + e.y() * d.y()) / length2, 0.0f, 1.0f);
        }

        float distance_to_segment(math::vec2f p, math::vec2f a, math::vec2f b) noexcept
        {
            return distance(p, a + (b - a) * along(p, a, b));
        }

        // A box in floats, for the tests on it.
        struct fbox
        {
            math::vec2f min{};
            math::vec2f max{};

            bool contains(math::vec2f p, float grow) const noexcept
            {
                return p.x() >= min.x() - grow && p.x() <= max.x() + grow && p.y() >= min.y() - grow &&
                       p.y() <= max.y() + grow;
            }
            bool inside(const fbox &outer) const noexcept
            {
                return min.x() >= outer.min.x() && max.x() <= outer.max.x() && min.y() >= outer.min.y() &&
                       max.y() <= outer.max.y();
            }
            bool overlaps(const fbox &other) const noexcept
            {
                return min.x() <= other.max.x() && max.x() >= other.min.x() && min.y() <= other.max.y() &&
                       max.y() >= other.min.y();
            }
            float area() const noexcept { return (max.x() - min.x()) * (max.y() - min.y()); }
        };

        fbox part_box(const part &p)
        {
            point lo, hi;
            part_bounds(p, lo, hi);
            return {to_float(lo), to_float(hi)};
        }

        fbox label_box(const label &l, const draw_style &style)
        {
            fbox b;
            label_bounds(l, style, b.min, b.max);
            return b;
        }

        // Whether the segment touches the box (Liang-Barsky clipping).
        bool segment_touches(math::vec2f a, math::vec2f b, const fbox &box) noexcept
        {
            float t0 = 0.0f;
            float t1 = 1.0f;
            const math::vec2f d = b - a;
            const float p[] = {-d.x(), d.x(), -d.y(), d.y()};
            const float q[] = {a.x() - box.min.x(), box.max.x() - a.x(), a.y() - box.min.y(), box.max.y() - a.y()};
            for (int i = 0; i < 4; ++i)
            {
                if (p[i] == 0.0f)
                {
                    if (q[i] < 0.0f)
                        return false;
                    continue;
                }
                const float t = q[i] / p[i];
                if (p[i] < 0.0f)
                    t0 = std::max(t0, t);
                else
                    t1 = std::min(t1, t);
                if (t0 > t1)
                    return false;
            }
            return true;
        }

        rect outwards(const fbox &b)
        {
            return rect{point{static_cast<int>(std::floor(b.min.x())), static_cast<int>(std::floor(b.min.y()))},
                        point{static_cast<int>(std::ceil(b.max.x())), static_cast<int>(std::ceil(b.max.y()))}};
        }

        rect join(const rect &a, const rect &b)
        {
            return rect{point{std::min(a.min.x(), b.min.x()), std::min(a.min.y(), b.min.y())},
                        point{std::max(a.max.x(), b.max.x()), std::max(a.max.y(), b.max.y())}};
        }

        int kind_rank(connection_kind kind) noexcept
        {
            switch (kind)
            {
            case connection_kind::pin:
                return 0;
            case connection_kind::wire_end:
                return 1;
            case connection_kind::label:
                return 2;
            case connection_kind::wire_inside:
                return 3;
            }
            return 3;
        }

        // The point inside the wire (not an end) a pointer at `at` would
        // snap to; nullopt when the wire has no point inside it.
        std::optional<point> inside_point(const wire &w, math::vec2f at, float tolerance)
        {
            const math::vec2f a = to_float(w.a);
            const math::vec2f b = to_float(w.b);
            const float t = along(at, a, b);
            const math::vec2f projected = a + (b - a) * t;

            // The nearest grid point inside the wire, of those around the
            // projection.
            const point near{static_cast<int>(std::lround(projected.x())), static_cast<int>(std::lround(projected.y()))};
            const point grid = snap_to_grid(near);
            std::optional<point> best;
            float best_distance = tolerance;
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                {
                    const point q = grid + point{dx, dy} * units_per_grid;
                    const float d = distance(to_float(q), at);
                    if (inside_wire(w, q) && d <= best_distance)
                    {
                        best = q;
                        best_distance = d;
                    }
                }
            if (best)
                return best;

            // A wire drawn on the grid offers nothing else; one off it (an
            // end off the grid) the nearest point of integer coordinates.
            if (snap_to_grid(w.a) == w.a && snap_to_grid(w.b) == w.b)
                return std::nullopt;
            const point d = w.b - w.a;
            const int steps = std::gcd(std::abs(d.x()), std::abs(d.y()));
            if (steps < 2)
                return std::nullopt;
            const point step{d.x() / steps, d.y() / steps};
            const int k = std::clamp(static_cast<int>(std::lround(t * static_cast<float>(steps))), 1, steps - 1);
            return w.a + step * k;
        }
    } // namespace

    std::vector<item_ref> hit_test_all(const sheet &s, math::vec2f at, float tolerance, const draw_style &style)
    {
        std::vector<item_ref> hits;

        // Labels, the later first.
        const auto labels = s.labels.entries();
        for (std::size_t i = labels.size(); i-- > 0;)
            if (label_box(labels[i].record, style).contains(at, tolerance))
                hits.push_back({item_kind::label, labels[i].id});

        // Wires, the nearest first (the later of equals).
        struct near_wire
        {
            float distance;
            std::size_t index;
            item_id id;
        };
        std::vector<near_wire> wires;
        const auto wire_entries = s.wires.entries();
        for (std::size_t i = 0; i < wire_entries.size(); ++i)
        {
            const wire &w = wire_entries[i].record;
            const float d = distance_to_segment(at, to_float(w.a), to_float(w.b));
            if (d <= tolerance)
                wires.push_back({d, i, wire_entries[i].id});
        }
        std::ranges::sort(wires, [](const near_wire &x, const near_wire &y) {
            return x.distance != y.distance ? x.distance < y.distance : x.index > y.index;
        });
        for (const near_wire &w : wires)
            hits.push_back({item_kind::wire, w.id});

        // Parts, the smallest first (the later of equals).
        struct under_part
        {
            float area;
            std::size_t index;
            item_id id;
        };
        std::vector<under_part> parts;
        const auto part_entries = s.parts.entries();
        for (std::size_t i = 0; i < part_entries.size(); ++i)
        {
            const fbox b = part_box(part_entries[i].record);
            if (b.contains(at, tolerance))
                parts.push_back({b.area(), i, part_entries[i].id});
        }
        std::ranges::sort(parts, [](const under_part &x, const under_part &y) {
            return x.area != y.area ? x.area < y.area : x.index > y.index;
        });
        for (const under_part &p : parts)
            hits.push_back({item_kind::part, p.id});

        return hits;
    }

    std::optional<item_ref> hit_test(const sheet &s, math::vec2f at, float tolerance, const draw_style &style)
    {
        const std::vector<item_ref> hits = hit_test_all(s, at, tolerance, style);
        if (hits.empty())
            return std::nullopt;
        return hits.front();
    }

    std::vector<item_ref> items_in_rect(const sheet &s, point a, point b, bool crossing, const draw_style &style)
    {
        const fbox area{to_float(point{std::min(a.x(), b.x()), std::min(a.y(), b.y())}),
                        to_float(point{std::max(a.x(), b.x()), std::max(a.y(), b.y())})};
        const auto take = [&](const fbox &box) { return crossing ? box.overlaps(area) : box.inside(area); };

        std::vector<item_ref> found;
        for (const auto &e : s.parts)
            if (take(part_box(e.record)))
                found.push_back({item_kind::part, e.id});
        for (const auto &e : s.wires)
        {
            const math::vec2f wa = to_float(e.record.a);
            const math::vec2f wb = to_float(e.record.b);
            const bool in = crossing ? segment_touches(wa, wb, area) : area.contains(wa, 0.0f) && area.contains(wb, 0.0f);
            if (in)
                found.push_back({item_kind::wire, e.id});
        }
        for (const auto &e : s.labels)
            if (take(label_box(e.record, style)))
                found.push_back({item_kind::label, e.id});
        return found;
    }

    // ---- wire ends ----------------------------------------------------------------------

    std::optional<wire_end_hit> hit_wire_end(const sheet &s, math::vec2f at, float tolerance)
    {
        std::optional<wire_end_hit> best;
        float best_distance = tolerance;
        for (const auto &e : s.wires)
        {
            for (const bool end_b : {false, true})
            {
                const float d = distance(at, to_float(end_b ? e.record.b : e.record.a));
                if (d <= best_distance)
                {
                    best_distance = d;
                    best = wire_end_hit{e.id, end_b};
                }
            }
        }
        return best;
    }

    std::vector<wire_end_hit> wire_ends_at(const sheet &s, point at)
    {
        std::vector<wire_end_hit> ends;
        for (const auto &e : s.wires)
        {
            if (e.record.a == at)
                ends.push_back({e.id, false});
            if (e.record.b == at)
                ends.push_back({e.id, true});
        }
        return ends;
    }

    // ---- snapping -----------------------------------------------------------------------

    std::optional<connection_point> nearest_connection(const sheet &s, math::vec2f at, float tolerance,
                                                       std::span<const item_ref> exclude)
    {
        std::optional<connection_point> best;
        float best_distance = 0.0f;
        const auto excluded = [&](item_ref item) { return std::ranges::find(exclude, item) != exclude.end(); };
        const auto offer = [&](const connection_point &c) {
            const float d = distance(at, to_float(c.at));
            if (d > tolerance)
                return;
            if (best && (d > best_distance || (d == best_distance && kind_rank(c.kind) >= kind_rank(best->kind))))
                return;
            best = c;
            best_distance = d;
        };

        for (const auto &e : s.parts)
        {
            // Only parts whose box is near can have a pin near.
            if (!part_box(e.record).contains(at, tolerance) || excluded({item_kind::part, e.id}))
                continue;
            for (const placed_pin &p : placed_pins(e.record))
                offer({p.position, connection_kind::pin, {item_kind::part, e.id}, p.pin, false});
        }
        for (const auto &e : s.wires)
        {
            if (excluded({item_kind::wire, e.id}))
                continue;
            const wire &w = e.record;
            offer({w.a, connection_kind::wire_end, {item_kind::wire, e.id}, nullptr, false});
            offer({w.b, connection_kind::wire_end, {item_kind::wire, e.id}, nullptr, true});
            if (distance_to_segment(at, to_float(w.a), to_float(w.b)) > tolerance)
                continue;
            if (const auto inside = inside_point(w, at, tolerance))
                offer({*inside, connection_kind::wire_inside, {item_kind::wire, e.id}, nullptr, false});
        }
        for (const auto &e : s.labels)
            if (e.record.kind != label_kind::note && !excluded({item_kind::label, e.id}))
                offer({e.record.position, connection_kind::label, {item_kind::label, e.id}, nullptr, false});
        return best;
    }

    // ---- boxes ----------------------------------------------------------------------------

    std::optional<rect> item_bounds(const sheet &s, item_ref item, const draw_style &style)
    {
        switch (item.kind)
        {
        case item_kind::part:
            if (const part *p = s.parts.find(item.id))
            {
                rect r;
                part_bounds(*p, r.min, r.max);
                return r;
            }
            break;
        case item_kind::wire:
            if (const wire *w = s.wires.find(item.id))
                return rect{point{std::min(w->a.x(), w->b.x()), std::min(w->a.y(), w->b.y())},
                            point{std::max(w->a.x(), w->b.x()), std::max(w->a.y(), w->b.y())}};
            break;
        case item_kind::label:
            if (const label *l = s.labels.find(item.id))
                return outwards(label_box(*l, style));
            break;
        }
        return std::nullopt;
    }

    std::optional<rect> selection_bounds(const sheet &s, std::span<const item_ref> items, const draw_style &style)
    {
        std::optional<rect> all;
        for (const item_ref item : items)
            if (const auto r = item_bounds(s, item, style))
                all = all ? join(*all, *r) : *r;
        return all;
    }

    std::optional<point> rotation_pivot(const sheet &s, std::span<const item_ref> items, const draw_style &style)
    {
        const auto r = selection_bounds(s, items, style);
        if (!r)
            return std::nullopt;
        return snap_to_grid(r->center());
    }

} // namespace volt::schematic
