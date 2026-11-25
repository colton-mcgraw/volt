#include <schematic/follow.hpp>

#include <schematic/netlist.hpp>
#include <schematic/symbol.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace volt::schematic
{

    namespace
    {
        long long dot(point u, point v) noexcept
        {
            return static_cast<long long>(u.x()) * v.x() + static_cast<long long>(u.y()) * v.y();
        }

        enum class axis : std::uint8_t
        {
            none, // diagonal, or of no length
            horizontal,
            vertical,
        };

        axis axis_of(const wire &w) noexcept
        {
            if (w.a == w.b)
                return axis::none;
            if (w.a.y() == w.b.y())
                return axis::horizontal;
            if (w.a.x() == w.b.x())
                return axis::vertical;
            return axis::none;
        }

        // What leaves a point: where it goes, and what kinds of thing go.
        struct departure
        {
            point to{};
            bool pin = false;
            bool wire_end = false;
            bool label = false;
        };

        // A moved wire, before and after.
        struct moved_wire
        {
            wire from;
            wire to;
        };

        // Where a point strictly inside a moved wire goes: as many steps
        // along the moved wire from its first end as it was along the wire.
        point carried(const moved_wire &m, point p) noexcept
        {
            const point d = m.from.b - m.from.a;
            const int steps = std::gcd(std::abs(d.x()), std::abs(d.y()));
            const point step{d.x() / steps, d.y() / steps};
            const int k = step.x() != 0 ? (p.x() - m.from.a.x()) / step.x() : (p.y() - m.from.a.y()) / step.y();
            const point e = m.to.b - m.to.a;
            if (std::gcd(std::abs(e.x()), std::abs(e.y())) != steps)
                return p + (m.to.a - m.from.a); // not a rigid move: as its first end moved
            return m.to.a + point{e.x() / steps, e.y() / steps} * k;
        }

        // An orthogonal path from p to q, appended: one wire when they line
        // up, else two by a corner, vertical first or horizontal first.
        void add_route(std::vector<wire> &out, point p, point q, bool vertical_first)
        {
            if (p == q)
                return;
            if (p.x() == q.x() || p.y() == q.y())
            {
                out.push_back(wire{p, q, {}});
                return;
            }
            const point corner = vertical_first ? point{p.x(), q.y()} : point{q.x(), p.y()};
            out.push_back(wire{p, corner, {}});
            out.push_back(wire{corner, q, {}});
        }

        // Wires by the line they lie on: the horizontal ones by y, the
        // vertical ones by x, the rest in a list, so that the wires a point
        // can be inside of are a few to look at.
        class wire_lines
        {
        public:
            void add(const wire &w, std::size_t index)
            {
                if (w.a == w.b)
                    return;
                if (w.a.y() == w.b.y())
                    m_rows[w.a.y()].push_back(index);
                else if (w.a.x() == w.b.x())
                    m_columns[w.a.x()].push_back(index);
                else
                    m_others.push_back(index);
            }

            // Calls f(index) for every wire added whose line passes through
            // p (and the diagonal ones).
            template <typename F>
            void visit(point p, F &&f) const
            {
                if (const auto row = m_rows.find(p.y()); row != m_rows.end())
                    for (const std::size_t i : row->second)
                        f(i);
                if (const auto column = m_columns.find(p.x()); column != m_columns.end())
                    for (const std::size_t i : column->second)
                        f(i);
                for (const std::size_t i : m_others)
                    f(i);
            }

        private:
            std::unordered_map<int, std::vector<std::size_t>> m_rows;
            std::unordered_map<int, std::vector<std::size_t>> m_columns;
            std::vector<std::size_t> m_others;
        };

        // An unmoved wire with an end on a moving point: where its ends go
        // (none for an end that stays).
        struct attached
        {
            item_id id = 0;
            wire w;
            std::optional<point> to_a;
            std::optional<point> to_b;
        };
    } // namespace

    namespace
    {
        // The plan; `direct`: no slides and no jogs, every attached end
        // straight to where its point went (diagonal if need be).
        follow_plan route_followers(const sheet &before, const sheet &after, std::span<const item_ref> moved,
                                    const connectivity &rules, bool direct);

        // `after` with the plan applied, new wires numbered past every id.
        sheet applied(const sheet &after, const follow_plan &plan)
        {
            sheet s = after;
            item_id next = 0;
            for (const auto &e : s.parts)
                next = std::max(next, e.id);
            for (const auto &e : s.wires)
                next = std::max(next, e.id);
            for (const auto &e : s.labels)
                next = std::max(next, e.id);
            for (const auto &[id, w] : plan.wires)
                (void)s.wires.replace(id, w);
            for (const wire &w : plan.added)
                if (w.a != w.b)
                    (void)s.wires.insert(++next, w);
            for (const auto &[id, l] : plan.labels)
                (void)s.labels.replace(id, l);
            return s;
        }

        // Whether the plan joins pins that were on different nets before
        // the move: what a wire routed over another moved pin, or two
        // routes meeting, would do.
        bool joins_nets(const netlist &was, const sheet &result, const connectivity &rules)
        {
            const netlist now = build_netlist(result, rules);
            std::unordered_map<std::size_t, std::size_t> before_of; // net now -> a net before
            for (const auto &e : result.parts)
            {
                for (const pin &p : find_symbol(e.record.symbol).pins)
                {
                    const auto then = was.net_of_pin(e.id, p.number);
                    const auto at = now.net_of_pin(e.id, p.number);
                    if (!then || !at)
                        continue;
                    const auto [it, added] = before_of.try_emplace(*at, *then);
                    if (!added && it->second != *then)
                        return true;
                }
            }
            return false;
        }

        // Whether a moved pin, wire end or label lands on something of
        // another net that stays: a connection the user makes by moving,
        // which the check above must not take for the routing's.
        bool lands_elsewhere(const sheet &before, const sheet &after, const netlist &was,
                             std::span<const item_ref> moved, const connectivity &rules)
        {
            const std::unordered_set<item_ref> moving(moved.begin(), moved.end());
            std::vector<std::pair<point, std::optional<std::size_t>>> arrivals; // where, of which net
            for (const auto &e : after.parts)
            {
                if (!moving.contains(item_ref{item_kind::part, e.id}))
                    continue;
                for (const placed_pin &p : placed_pins(e.record))
                    arrivals.emplace_back(p.position, was.net_of_pin(e.id, p.pin->number));
            }
            for (const auto &e : after.wires)
                if (moving.contains(item_ref{item_kind::wire, e.id}))
                {
                    arrivals.emplace_back(e.record.a, was.net_of_wire(e.id));
                    arrivals.emplace_back(e.record.b, was.net_of_wire(e.id));
                }
            for (const auto &e : after.labels)
                if (moving.contains(item_ref{item_kind::label, e.id}) && e.record.kind != label_kind::note)
                    arrivals.emplace_back(e.record.position, was.net_of_label(e.id));

            for (const auto &[at, net] : arrivals)
            {
                for (const auto &e : before.wires)
                    if (!moving.contains(item_ref{item_kind::wire, e.id}) && pin_on_wire(e.record, at, rules) &&
                        was.net_of_wire(e.id) != net)
                        return true;
                for (const auto &e : before.parts)
                    if (!moving.contains(item_ref{item_kind::part, e.id}))
                        for (const placed_pin &p : placed_pins(e.record))
                            if (p.position == at && was.net_of_pin(e.id, p.pin->number) != net)
                                return true;
                for (const auto &e : before.labels)
                    if (!moving.contains(item_ref{item_kind::label, e.id}) && e.record.kind != label_kind::note &&
                        e.record.position == at && was.net_of_label(e.id) != net)
                        return true;
            }
            return false;
        }
    } // namespace

    follow_plan plan_follow(const sheet &before, const sheet &after, std::span<const item_ref> moved,
                            const connectivity &rules)
    {
        follow_plan plan = route_followers(before, after, moved, rules, false);
        if (plan.empty())
            return plan;
        // The routing must not join what was apart (see the top of
        // follow.hpp): then the ends go straight to their pins; and if
        // even that joins nets, the wires stay where they are.
        const netlist was = build_netlist(before, rules);
        if (!joins_nets(was, applied(after, plan), rules) || lands_elsewhere(before, after, was, moved, rules))
            return plan;
        plan = route_followers(before, after, moved, rules, true);
        if (!joins_nets(was, applied(after, plan), rules))
            return plan;
        return {};
    }

    namespace
    {
        follow_plan route_followers(const sheet &before, const sheet &after, std::span<const item_ref> moved,
                                    const connectivity &rules, bool direct)
        {
            follow_plan plan;
            const std::unordered_set<item_ref> moving(moved.begin(), moved.end());
            const auto is_moving = [&](item_kind kind, item_id id) { return moving.contains(item_ref{kind, id}); };

            // ---- what moves, from where to where

            std::unordered_map<point, departure> leaving;
            std::vector<point> leaving_order;
            const auto leave = [&](point from, point to) -> departure & {
                const auto [it, added] = leaving.try_emplace(from, departure{to});
                if (added)
                    leaving_order.push_back(from);
                return it->second;
            };
            std::vector<moved_wire> moved_wires;
            for (const auto &e : before.parts)
            {
                const part *now = is_moving(item_kind::part, e.id) ? after.parts.find(e.id) : nullptr;
                if (now == nullptr)
                    continue;
                const std::vector<point> from = pin_positions(e.record);
                const std::vector<point> to = pin_positions(*now);
                for (std::size_t i = 0; i < from.size() && i < to.size(); ++i)
                    leave(from[i], to[i]).pin = true;
            }
            for (const auto &e : before.wires)
            {
                const wire *now = is_moving(item_kind::wire, e.id) ? after.wires.find(e.id) : nullptr;
                if (now == nullptr)
                    continue;
                leave(e.record.a, now->a).wire_end = true;
                leave(e.record.b, now->b).wire_end = true;
                if (e.record.a != e.record.b)
                    moved_wires.push_back({e.record, *now});
            }
            for (const auto &e : before.labels)
            {
                const label *now = is_moving(item_kind::label, e.id) ? after.labels.find(e.id) : nullptr;
                if (now == nullptr || e.record.kind == label_kind::note)
                    continue;
                leave(e.record.position, now->position).label = true;
            }
            if (leaving.empty())
                return plan;

            wire_lines moved_lines;
            for (std::size_t i = 0; i < moved_wires.size(); ++i)
                moved_lines.add(moved_wires[i].from, i);
            const auto target_of = [&](point p) -> std::optional<point> {
                if (const auto it = leaving.find(p); it != leaving.end())
                    return it->second.to;
                std::optional<point> found;
                moved_lines.visit(p, [&](std::size_t i) {
                    if (!found && inside_wire(moved_wires[i].from, p))
                        found = carried(moved_wires[i], p);
                });
                return found;
            };

            // ---- what stays, and what is attached

            std::unordered_set<point> fixed_pins;
            for (const auto &e : before.parts)
                if (!is_moving(item_kind::part, e.id))
                    for (const point p : pin_positions(e.record))
                        fixed_pins.insert(p);

            std::vector<attached> followers;
            std::vector<std::pair<item_id, wire>> still; // unmoved and not attached
            std::unordered_set<point> attached_ends;     // where attached wires' ends are
            std::unordered_map<point, std::vector<std::pair<item_id, bool>>> ends_at; // every wire end
            for (const auto &e : before.wires)
            {
                ends_at[e.record.a].emplace_back(e.id, false);
                ends_at[e.record.b].emplace_back(e.id, true);
                if (is_moving(item_kind::wire, e.id))
                    continue;
                attached f{e.id, e.record, target_of(e.record.a), target_of(e.record.b)};
                if (f.to_a)
                    attached_ends.insert(e.record.a);
                if (f.to_b)
                    attached_ends.insert(e.record.b);
                if (f.to_a || f.to_b)
                    followers.push_back(std::move(f));
                else
                    still.emplace_back(e.id, e.record);
            }

            // ---- held points

            wire_lines still_lines;
            for (std::size_t i = 0; i < still.size(); ++i)
                still_lines.add(still[i].second, i);
            std::unordered_map<point, bool> held_cache;
            const auto held = [&](point p, point q) {
                if (p == q)
                    return false;
                if (const auto it = held_cache.find(p); it != held_cache.end())
                    return it->second;
                bool result = fixed_pins.contains(p);
                if (!result)
                {
                    const auto it = leaving.find(p);
                    const bool connects = attached_ends.contains(p) ||
                                          (it != leaving.end() && (it->second.wire_end || it->second.label ||
                                                                   (it->second.pin && rules.pins_inside_wires)));
                    if (connects)
                    {
                        still_lines.visit(p, [&](std::size_t i) {
                            const wire &w = still[i].second;
                            result = result || (inside_wire(w, p) && !on_wire(w, q));
                        });
                    }
                }
                held_cache.emplace(p, result);
                return result;
            };

            // Whether a horizontal wire is at p, unmoved: a route from p then
            // leaves across it.
            const auto horizontal_at = [&](point p) {
                for (const auto &e : before.wires)
                    if (!is_moving(item_kind::wire, e.id) && axis_of(e.record) == axis::horizontal &&
                        on_wire(e.record, p))
                        return true;
                return false;
            };

            // Attached ends on held points stay; the points inside moved wires
            // they stay on get a wire to where that point went.
            std::vector<std::pair<point, point>> held_inside;
            for (attached &f : followers)
            {
                for (const bool end_b : {false, true})
                {
                    std::optional<point> &to = end_b ? f.to_b : f.to_a;
                    const point from = end_b ? f.w.b : f.w.a;
                    if (!to || !held(from, *to))
                        continue;
                    if (!leaving.contains(from) &&
                        std::ranges::find(held_inside, from, &std::pair<point, point>::first) == held_inside.end())
                        held_inside.emplace_back(from, *to);
                    to.reset();
                }
            }

            // ---- what a slide must not leave, cross or land on

            std::unordered_set<point> marks = fixed_pins; // unmoved pins, labels that stay, unmoved wire ends
            std::unordered_set<point> label_points;       // every net label and power port
            for (const auto &e : before.labels)
            {
                if (e.record.kind == label_kind::note)
                    continue;
                label_points.insert(e.record.position);
                if (is_moving(item_kind::label, e.id))
                    continue;
                const auto to = target_of(e.record.position);
                if (!to || held(e.record.position, *to))
                    marks.insert(e.record.position);
            }
            for (const auto &[id, w] : still)
            {
                marks.insert(w.a);
                marks.insert(w.b);
            }
            for (const attached &f : followers)
            {
                if (!f.to_a)
                    marks.insert(f.w.a);
                if (!f.to_b)
                    marks.insert(f.w.b);
            }
            std::unordered_set<item_id> following;
            for (const attached &f : followers)
                if (f.to_a || f.to_b)
                    following.insert(f.id);
            const auto is_follower = [&](item_id id) { return following.contains(id); };

            // ---- the attached wires, in the sheet's order

            std::unordered_map<item_id, wire> changed;
            std::unordered_set<point> slid;
            const auto entries = before.wires.entries();
            wire_lines all_lines;
            for (std::size_t i = 0; i < entries.size(); ++i)
                all_lines.add(entries[i].record, i);

            // Slides wire f (its end at p going to q, its other end `far`
            // staying) across by `across`, if everything at `far` can take it
            // (see the top of follow.hpp); false, and nothing changed, if not.
            const auto try_slide = [&](const attached &f, bool end_b, point q, point far, point across) {
                const axis along = axis_of(f.w);
                const point to = far + across;
                if (slid.contains(far) || fixed_pins.contains(far) || label_points.contains(far))
                    return false;
                bool tee = false;
                all_lines.visit(far, [&](std::size_t i) {
                    tee = tee || (entries[i].id != f.id && inside_wire(entries[i].record, far));
                });
                if (tee)
                    return false;

                // The wires ending at `far` bend: each must run across f.
                std::vector<std::pair<item_id, wire>> bent;
                std::unordered_set<point> excused{far};
                for (const auto &[id, other_end_b] : ends_at[far])
                {
                    if (id == f.id)
                        continue;
                    if (is_moving(item_kind::wire, id) || is_follower(id) || changed.contains(id))
                        return false;
                    const wire &v = *before.wires.find(id);
                    const axis va = axis_of(v);
                    if (va == axis::none || va == along)
                        return false;
                    const point other = other_end_b ? v.a : v.b;
                    if (dot(to - other, far - other) < 0)
                        return false; // past its other end
                    wire moved_v = v;
                    (other_end_b ? moved_v.b : moved_v.a) = to;
                    bent.emplace_back(id, moved_v);
                    excused.insert(other);
                }

                wire slid_wire = f.w;
                (end_b ? slid_wire.b : slid_wire.a) = q;
                (end_b ? slid_wire.a : slid_wire.b) = to;
                const wire stretch{far, to, {}};
                for (const point m : marks)
                {
                    if (excused.contains(m))
                        continue;
                    if (inside_wire(f.w, m))
                        return false; // left behind
                    if (m != q && on_wire(slid_wire, m))
                        return false; // reached
                    if (far != to && on_wire(stretch, m))
                        return false; // passed over by a bent wire
                }
                bool lands = false;
                still_lines.visit(to, [&](std::size_t i) {
                    const item_id id = still[i].first;
                    const bool bends = std::ranges::find(bent, id, &std::pair<item_id, wire>::first) != bent.end();
                    lands = lands || (!bends && on_wire(still[i].second, to));
                });
                if (lands)
                    return false; // landing on a wire

                for (auto &[id, w] : bent)
                    changed[id] = std::move(w);
                changed[f.id] = slid_wire;
                slid.insert(far);
                return true;
            };

            for (const attached &f : followers)
            {
                if (!f.to_a && !f.to_b)
                    continue;
                if (f.to_a && f.to_b)
                {
                    wire w = f.w;
                    w.a = *f.to_a;
                    w.b = *f.to_b;
                    changed[f.id] = std::move(w);
                    continue;
                }
                const bool end_b = f.to_b.has_value();
                const point p = end_b ? f.w.b : f.w.a;
                const point q = end_b ? *f.to_b : *f.to_a;
                const point far = end_b ? f.w.a : f.w.b;
                const axis along = axis_of(f.w);
                const point d = q - p;
                const point across = along == axis::horizontal ? point{0, d.y()}
                                     : along == axis::vertical ? point{d.x(), 0}
                                                               : point{};

                wire w = f.w;
                point &end = end_b ? w.b : w.a;
                if (direct || along == axis::none || across == point{})
                {
                    end = q;
                    changed[f.id] = std::move(w);
                    continue;
                }
                if (try_slide(f, end_b, q, far, across))
                    continue;
                // A jog: along the wire, then across to the pin. When the wire
                // or the jog would run over another moved pin (the part's own
                // other pin, after a move along its axis), it would join the
                // two: the jog goes a grid square further along the wire (or
                // short of it) instead, across, and back to the pin, round the
                // part.
                end = q - across;
                const auto crosses_a_pin = [&](point from, point to) {
                    const wire leg{from, to, {}};
                    for (const auto &[at, going] : leaving)
                        if (going.pin && going.to != q && on_wire(leg, going.to))
                            return true;
                    return false;
                };
                if (crosses_a_pin(far, end) || crosses_a_pin(end, q))
                {
                    const int square = units_per_grid;
                    const point step = along == axis::horizontal ? point{p.x() > far.x() ? square : -square, 0}
                                                                 : point{0, p.y() > far.y() ? square : -square};
                    bool routed = false;
                    for (const point offset : {step, point{} - step})
                    {
                        const point corner = end + offset;
                        // Not back past the wire's other end.
                        if (dot(corner - far, p - far) <= 0 || crosses_a_pin(far, corner) ||
                            crosses_a_pin(corner, q + offset) || crosses_a_pin(q + offset, q))
                            continue;
                        end = corner;
                        plan.added.push_back(wire{corner, q + offset, {}});
                        plan.added.push_back(wire{q + offset, q, {}});
                        routed = true;
                        break;
                    }
                    if (routed)
                    {
                        changed[f.id] = std::move(w);
                        continue;
                    }
                }
                plan.added.push_back(wire{end, q, {}});
                changed[f.id] = std::move(w);
            }

            // ---- wires from held points to where things went

            for (const point p : leaving_order)
            {
                const departure &d = leaving.find(p)->second;
                if ((d.pin || d.wire_end) && held(p, d.to))
                    add_route(plan.added, p, d.to, horizontal_at(p));
            }
            for (const auto &[p, q] : held_inside)
                add_route(plan.added, p, q, horizontal_at(p));

            // ---- the plan, in the sheet's order

            for (const auto &e : before.wires)
                if (const auto it = changed.find(e.id); it != changed.end() && it->second != e.record)
                    plan.wires.emplace_back(e.id, it->second);
            for (const auto &e : before.labels)
            {
                if (e.record.kind == label_kind::note || is_moving(item_kind::label, e.id))
                    continue;
                const auto to = target_of(e.record.position);
                if (!to || *to == e.record.position || held(e.record.position, *to))
                    continue;
                label l = e.record;
                l.position = *to;
                plan.labels.emplace_back(e.id, std::move(l));
            }
            return plan;
        }
    } // namespace

} // namespace volt::schematic
