#include <schematic/draw.hpp>

#include <schematic/stroke_font.hpp>
#include <schematic/symbol.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string_view>
#include <unordered_set>

namespace volt::schematic
{

    namespace
    {
        constexpr float world_per_unit = 1.0f / static_cast<float>(units_per_grid);

        math::vec2f world(point p) noexcept
        {
            return math::vec2f{static_cast<float>(p.x()) * world_per_unit, static_cast<float>(p.y()) * world_per_unit};
        }

        math::vec2f world(math::vec2f sheet_units) noexcept
        {
            return sheet_units * world_per_unit;
        }

        math::vec2f to_float(point p) noexcept
        {
            return math::vec2f{static_cast<float>(p.x()), static_cast<float>(p.y())};
        }

        // ---- connections -------------------------------------------------------------

        // The sheet's wires by the cells of a coarse grid their bounds
        // cover, so that finding the wires through a point looks at a few.
        class wire_grid
        {
        public:
            explicit wire_grid(const sheet &s) : m_sheet(&s)
            {
                const auto wires = s.wires.entries();
                for (std::size_t i = 0; i < wires.size(); ++i)
                {
                    const wire &w = wires[i].record;
                    const int x0 = cell_of(std::min(w.a.x(), w.b.x()));
                    const int x1 = cell_of(std::max(w.a.x(), w.b.x()));
                    const int y0 = cell_of(std::min(w.a.y(), w.b.y()));
                    const int y1 = cell_of(std::max(w.a.y(), w.b.y()));
                    for (int x = x0; x <= x1; ++x)
                        for (int y = y0; y <= y1; ++y)
                            m_cells[key(x, y)].push_back(static_cast<std::uint32_t>(i));
                }
            }

            // Calls f(id, wire) for every wire whose cell holds p: all the
            // wires p can be on, and some more.
            template <typename F>
            void visit(point p, F &&f) const
            {
                const auto found = m_cells.find(key(cell_of(p.x()), cell_of(p.y())));
                if (found == m_cells.end())
                    return;
                const auto wires = m_sheet->wires.entries();
                for (const std::uint32_t i : found->second)
                    f(wires[i].id, wires[i].record);
            }

        private:
            static constexpr int cell_size = 1000;

            static int cell_of(int v) noexcept
            {
                return v >= 0 ? v / cell_size : -((-(v + 1)) / cell_size) - 1;
            }

            static std::uint64_t key(int x, int y) noexcept
            {
                return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
                       static_cast<std::uint32_t>(y);
            }

            const sheet *m_sheet;
            std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> m_cells;
        };

        // What meets at each point where something ends: wire ends, pins,
        // and labels that name a net.
        class connections
        {
        public:
            struct counts
            {
                int wire_ends = 0;
                int pins = 0;
                int labels = 0;
            };

            connections(const sheet &s, const connectivity &rules) : m_wires(s), m_rules(rules)
            {
                m_points.reserve(2 * s.wires.size() + 3 * s.parts.size() + s.labels.size());
                for (const auto &e : s.wires)
                {
                    ++m_points[e.record.a].wire_ends;
                    ++m_points[e.record.b].wire_ends;
                }
                for (const auto &e : s.parts)
                    for (const point p : pin_positions(e.record))
                        ++m_points[p].pins;
                for (const auto &e : s.labels)
                    if (e.record.kind != label_kind::note)
                        ++m_points[e.record.position].labels;
            }

            const std::unordered_map<point, counts> &points() const noexcept { return m_points; }

            bool inside_any_wire(point p) const
            {
                bool inside = false;
                m_wires.visit(p, [&](item_id, const wire &w) { inside = inside || inside_wire(w, p); });
                return inside;
            }

            // Whether a dot belongs at p (see the top of draw.hpp). A wire
            // that p is inside joins what ends there, as two more meeting;
            // pins only when the rules let pins connect inside wires.
            bool junction(point p, const counts &c) const
            {
                int meeting = c.wire_ends + c.pins;
                if (meeting == 0)
                    return false;
                const bool joins_inside = c.wire_ends > 0 || (c.pins > 0 && m_rules.pins_inside_wires);
                if (meeting < 3 && joins_inside && inside_any_wire(p))
                    meeting += 2;
                return meeting >= 3;
            }

            // Whether anything but the pin itself is attached at p.
            bool pin_connected(point p) const
            {
                const auto found = m_points.find(p);
                if (found != m_points.end())
                {
                    const counts &c = found->second;
                    if (c.wire_ends > 0 || c.pins > 1 || c.labels > 0)
                        return true;
                }
                return m_rules.pins_inside_wires && inside_any_wire(p);
            }

            // The node of a wire through p, from the map; -1 for none.
            std::int32_t node_at(point p, const std::unordered_map<item_id, std::int32_t> *nodes) const
            {
                if (nodes == nullptr)
                    return -1;
                std::int32_t node = -1;
                m_wires.visit(p, [&](item_id id, const wire &w) {
                    if (node >= 0 || !on_wire(w, p))
                        return;
                    if (const auto found = nodes->find(id); found != nodes->end())
                        node = found->second;
                });
                return node;
            }

        private:
            wire_grid m_wires;
            connectivity m_rules;
            std::unordered_map<point, counts> m_points;
        };

        // The points that get a junction dot, sorted (by x, then y), so the
        // drawing comes out the same every time.
        std::vector<point> junction_points(const connections &links)
        {
            std::vector<point> found;
            for (const auto &[at, met] : links.points())
                if (links.junction(at, met))
                    found.push_back(at);
            std::ranges::sort(found, [](point a, point b) { return a.x() != b.x() ? a.x() < b.x() : a.y() < b.y(); });
            return found;
        }

        // ---- text ----------------------------------------------------------------------

        // A piece of text as it is placed: anchor in world units.
        struct placed_text
        {
            std::string_view text;
            math::vec2f anchor{};
            float height = 0.0f;
            horizontal_align horizontal = horizontal_align::center;
            vertical_align vertical = vertical_align::middle;
            int quarter_turns = 0;
            bool pin_text = false; // a pin's number or name, not the part's own
        };

        // Where the lead of a pin meets the body, in symbol coordinates: the
        // far end of the stroke the pin starts (the symbol builder draws a
        // lead from each pin), or a grid square in when there is none.
        point lead_end(const symbol &sym, const pin &p)
        {
            for (const std::vector<point> &stroke : sym.strokes)
                if (stroke.size() == 2 && stroke.front() == p.position)
                    return stroke.back();
            return p.position + p.direction * units_per_grid;
        }

        // Whether a symbol's pins are numbered on the sheet: boxes,
        // connectors, and anything with more than three pins. Two-terminal
        // parts and transistors are not, as is the custom.
        bool shows_pin_numbers(const symbol &sym)
        {
            return sym.name_inside || sym.prefix == "J" || sym.pins.size() > 3;
        }

        // Where a box's name goes, in symbol coordinates: the centre of its
        // bounds, moved up or down by half grid squares until it is clear of
        // the rows its side pins' names are on.
        point name_anchor(const symbol &sym)
        {
            const point center{(sym.min.x() + sym.max.x()) / 2, (sym.min.y() + sym.max.y()) / 2};
            const auto clear = [&](int y) {
                for (const pin &p : sym.pins)
                    if (p.direction.x() != 0 && std::abs(p.position.y() - y) < 60)
                        return false;
                return true;
            };
            for (int step = 0; step <= 8; ++step)
            {
                const int offset = (step + 1) / 2 * 50 * (step % 2 == 1 ? 1 : -1);
                if (clear(center.y() + offset))
                    return point{center.x(), center.y() + offset};
            }
            return center;
        }

        // The width (in x, symbol coordinates) of the body: the strokes that
        // are not a pin's lead.
        int body_width(const symbol &sym)
        {
            const auto is_lead = [&](const std::vector<point> &stroke) {
                if (stroke.size() != 2)
                    return false;
                for (const pin &p : sym.pins)
                    if (stroke.front() == p.position)
                        return true;
                return false;
            };
            bool first = true;
            int low = 0, high = 0;
            for (const std::vector<point> &stroke : sym.strokes)
            {
                if (is_lead(stroke))
                    continue;
                for (const point q : stroke)
                {
                    low = first ? q.x() : std::min(low, q.x());
                    high = first ? q.x() : std::max(high, q.x());
                    first = false;
                }
            }
            return first ? sym.max.x() - sym.min.x() : high - low;
        }

        // Calls f(placed_text) for every piece of text of a placed part: its
        // reference (unless hidden, "#..."), its value (unless empty), a
        // box's name, and its pins' numbers and names as the style shows.
        template <typename F>
        void each_part_text(const part &p, const symbol &sym, const draw_style &style, F &&f)
        {
            const int turns = p.rotation & 3;
            if (!p.reference.empty() && p.reference.front() != '#')
                f(placed_text{p.reference, world(transform(p, sym.reference_anchor)), style.reference_height,
                              horizontal_align::center, vertical_align::middle, turns, false});
            if (!p.value.empty())
                f(placed_text{p.value, world(transform(p, sym.value_anchor)), style.reference_height,
                              horizontal_align::center, vertical_align::middle, turns, false});
            if (sym.name_inside && !sym.name.empty())
            {
                // As high as a reference, or less, to take up no more than
                // three quarters of the body's width.
                const float room = 0.75f * static_cast<float>(body_width(sym)) * world_per_unit;
                const float wide = text_width(sym.name, 1.0f);
                const float height = wide > 0.0f ? std::min(style.reference_height, room / wide) : 0.0f;
                f(placed_text{sym.name, world(transform(p, name_anchor(sym))), height, horizontal_align::center,
                              vertical_align::middle, turns, false});
            }

            const bool numbers = style.show_pin_numbers && shows_pin_numbers(sym);
            const bool names = style.show_pin_names && sym.name_inside;
            if (!numbers && !names)
                return;
            for (const pin &each : sym.pins)
            {
                const point at = transform(p, each.position);
                const point into = transform_direction(p, each.direction);
                const point end = transform(p, lead_end(sym, each));
                const bool across = into.x() != 0; // a pin on the left or right side
                const int pin_turns = across ? 0 : 1;
                if (numbers && !each.number.empty())
                {
                    // Over the middle of the lead (to its left, upright).
                    const math::vec2f middle = (to_float(at) + to_float(end)) * 0.5f;
                    const math::vec2f side = across ? math::vec2f{0.0f, 15.0f} : math::vec2f{-15.0f, 0.0f};
                    f(placed_text{each.number, world(middle + side), style.pin_number_height,
                                  horizontal_align::center, vertical_align::bottom, pin_turns, true});
                }
                if (names && !each.name.empty())
                {
                    // Inside the body, reading away from the lead.
                    const math::vec2f start = to_float(end) + to_float(into) * 25.0f;
                    const bool forwards = across ? into.x() > 0 : into.y() > 0;
                    f(placed_text{each.name, world(start), style.pin_name_height,
                                  forwards ? horizontal_align::left : horizontal_align::right, vertical_align::middle,
                                  pin_turns, true});
                }
            }
        }

        // How a net label or a note sits: text reading from its point in
        // the direction of its rotation, on the upper (left) side.
        struct label_layout
        {
            math::vec2f along{};  // the rotation's direction, a unit
            math::vec2f anchor{}; // world units
            horizontal_align horizontal = horizontal_align::left;
            int quarter_turns = 0;
        };

        label_layout layout_label(const label &l, float gap_units, float lift_units)
        {
            static constexpr math::vec2f directions[] = {{1.0f, 0.0f}, {0.0f, 1.0f}, {-1.0f, 0.0f}, {0.0f, -1.0f}};
            const int turns = l.rotation & 3;
            label_layout layout;
            layout.along = directions[turns];
            const math::vec2f up = (turns & 1) != 0 ? math::vec2f{-1.0f, 0.0f} : math::vec2f{0.0f, 1.0f};
            layout.anchor = world(to_float(l.position) + layout.along * gap_units + up * lift_units);
            layout.horizontal = turns >= 2 ? horizontal_align::right : horizontal_align::left;
            layout.quarter_turns = turns & 1;
            return layout;
        }

        // Net labels: the gap between the point and the text, and how far
        // the text sits off the wire, in sheet units.
        constexpr float label_gap = 10.0f;
        constexpr float label_lift = 16.0f; // descenders just clear the wire

        // ---- drawing items ------------------------------------------------------------------

        // How an item is drawn besides its style: a colour for all of it
        // (selected, a ghost), and extra width.
        struct look
        {
            const math::vec4f *color = nullptr;
            float widen = 0.0f;

            const math::vec4f &pick(const math::vec4f &own) const noexcept { return color != nullptr ? *color : own; }
        };

        void polyline(std::vector<segment> &out, std::span<const math::vec2f> points, float width,
                      const math::vec4f &color)
        {
            for (std::size_t i = 1; i < points.size(); ++i)
                out.push_back({points[i - 1], points[i], width, color, -1});
        }

        void square(std::vector<segment> &out, math::vec2f center, float side, float width, const math::vec4f &color)
        {
            const float h = 0.5f * side;
            const math::vec2f corners[] = {center + math::vec2f{-h, -h}, center + math::vec2f{h, -h},
                                           center + math::vec2f{h, h}, center + math::vec2f{-h, h},
                                           center + math::vec2f{-h, -h}};
            polyline(out, corners, width, color);
        }

        // `free_pins`: the sheet's connections, to mark the pin ends nothing
        // is attached to; null for none marked.
        void draw_part(std::vector<segment> &out, const part &p, const draw_style &style, const look &how,
                       const connections *free_pins)
        {
            const symbol &sym = find_symbol(p.symbol);
            const math::vec4f &ink = how.pick(style.symbol);
            const float width = style.symbol_width + how.widen;
            for (const std::vector<point> &stroke : sym.strokes)
            {
                math::vec2f from = world(transform(p, stroke.front()));
                for (std::size_t i = 1; i < stroke.size(); ++i)
                {
                    const math::vec2f to = world(transform(p, stroke[i]));
                    out.push_back({from, to, width, ink, -1});
                    from = to;
                }
            }

            each_part_text(p, sym, style, [&](const placed_text &t) {
                append_text(out, t.text, t.anchor, t.height, t.horizontal, t.vertical, t.quarter_turns,
                            (t.pin_text ? style.pin_text_stroke : style.text_stroke) + how.widen,
                            how.pick(t.pin_text ? style.pin_text : style.text));
            });

            if (free_pins != nullptr)
            {
                for (const pin &each : sym.pins)
                {
                    const point at = transform(p, each.position);
                    if (!free_pins->pin_connected(at))
                        square(out, world(at), style.unconnected_size, 0.25f * style.symbol_width + how.widen,
                               how.pick(style.unconnected));
                }
            }
        }

        void draw_wire(std::vector<segment> &out, const wire &w, const draw_style &style, const look &how,
                       std::int32_t node)
        {
            out.push_back({world(w.a), world(w.b), style.wire_width + how.widen, how.pick(style.wire), node});
        }

        void draw_label(std::vector<segment> &out, const label &l, const draw_style &style, const look &how)
        {
            const float stroke = style.text_stroke + how.widen;
            switch (l.kind)
            {
            case label_kind::net:
            {
                const math::vec4f &ink = how.pick(style.net_label);
                const label_layout layout = layout_label(l, label_gap, label_lift);
                append_text(out, l.text, layout.anchor, style.label_height, layout.horizontal, vertical_align::bottom,
                            layout.quarter_turns, stroke, ink);
                // The flag: a line under the text from the point, and a
                // tick up at the point.
                const float length = text_width(l.text, style.label_height) + 2.0f * label_gap * world_per_unit;
                const math::vec2f at = world(l.position);
                const math::vec2f up = (l.rotation & 1) != 0 ? math::vec2f{-1.0f, 0.0f} : math::vec2f{0.0f, 1.0f};
                out.push_back({at, at + layout.along * length, stroke, ink, -1});
                out.push_back({at, at + up * (0.35f * style.label_height), stroke, ink, -1});
                break;
            }
            case label_kind::power:
            {
                const math::vec4f &ink = how.pick(style.power_label);
                const symbol &glyph = power_symbol(l.text);
                const float width = style.symbol_width + how.widen;
                for (const std::vector<point> &s : glyph.strokes)
                {
                    math::vec2f from = world(orient(s.front(), l.rotation, false) + l.position);
                    for (std::size_t i = 1; i < s.size(); ++i)
                    {
                        const math::vec2f to = world(orient(s[i], l.rotation, false) + l.position);
                        out.push_back({from, to, width, ink, -1});
                        from = to;
                    }
                }
                if (!is_ground_name(l.text))
                    append_text(out, l.text, world(orient(glyph.value_anchor, l.rotation, false) + l.position),
                                style.label_height, horizontal_align::center, vertical_align::middle, l.rotation & 3,
                                stroke, ink);
                break;
            }
            case label_kind::note:
            {
                const label_layout layout = layout_label(l, 0.0f, 0.0f);
                append_text(out, l.text, layout.anchor, style.label_height, layout.horizontal, vertical_align::bottom,
                            layout.quarter_turns, stroke, how.pick(style.note));
                break;
            }
            }
        }

        // ---- bounds ------------------------------------------------------------------------

        struct box
        {
            math::vec2f min{};
            math::vec2f max{};
            bool empty = true;

            void take(math::vec2f p)
            {
                if (empty)
                {
                    min = max = p;
                    empty = false;
                    return;
                }
                min = math::vec2f{std::min(min.x(), p.x()), std::min(min.y(), p.y())};
                max = math::vec2f{std::max(max.x(), p.x()), std::max(max.y(), p.y())};
            }
        };

        // Text bounds in sheet units.
        void take_text(box &b, std::string_view text, math::vec2f anchor, float height, horizontal_align h,
                       vertical_align v, int turns)
        {
            math::vec2f lo, hi;
            text_bounds(text, anchor, height, h, v, turns, lo, hi);
            b.take(lo * static_cast<float>(units_per_grid));
            b.take(hi * static_cast<float>(units_per_grid));
        }

        std::unordered_set<item_ref> selection_set(std::span<const item_ref> items)
        {
            return std::unordered_set<item_ref>(items.begin(), items.end());
        }
    } // namespace

    void append_sheet(std::vector<segment> &out, const sheet &s, const draw_options &options)
    {
        const draw_style &style = options.style;
        const connections links(s, options.rules);
        const std::unordered_set<item_ref> selected = selection_set(options.selection);
        const look plain{};
        const look highlighted{&style.selection, style.selection_widening};

        std::vector<item_ref> *owners = options.owners;
        if (owners != nullptr)
            owners->resize(out.size(), item_ref{item_kind::wire, 0});
        const auto own = [&](item_ref item) {
            if (owners != nullptr)
                owners->resize(out.size(), item);
        };

        for (const auto &e : s.parts)
        {
            const item_ref item{item_kind::part, e.id};
            draw_part(out, e.record, style, selected.contains(item) ? highlighted : plain, &links);
            own(item);
        }

        const auto *nodes = options.wire_nodes;
        for (const auto &e : s.wires)
        {
            const item_ref item{item_kind::wire, e.id};
            const bool is_selected = selected.contains(item);
            std::int32_t node = -1;
            if (nodes != nullptr && !is_selected)
                if (const auto found = nodes->find(e.id); found != nodes->end())
                    node = found->second;
            draw_wire(out, e.record, style, is_selected ? highlighted : plain, node);
            own(item);
        }

        for (const point at : junction_points(links))
        {
            const math::vec2f w = world(at);
            out.push_back({w, w, style.junction_diameter, style.wire, links.node_at(at, nodes)});
        }
        own(item_ref{item_kind::wire, 0});

        for (const auto &e : s.labels)
        {
            const item_ref item{item_kind::label, e.id};
            draw_label(out, e.record, style, selected.contains(item) ? highlighted : plain);
            own(item);
        }
    }

    std::vector<point> junctions(const sheet &s, const connectivity &rules)
    {
        return junction_points(connections(s, rules));
    }

    // ---- overlays ----------------------------------------------------------------------

    void append_fragment(std::vector<segment> &out, std::span<const part> parts, std::span<const wire> wires,
                         std::span<const label> labels, point offset, const draw_style &style,
                         const math::vec4f &color)
    {
        const look ghost{&color, 0.0f};
        for (const part &each : parts)
        {
            part moved = each;
            moved.position = moved.position + offset;
            draw_part(out, moved, style, ghost, nullptr);
        }
        for (const wire &each : wires)
            draw_wire(out, wire{each.a + offset, each.b + offset, {}}, style, ghost, -1);
        for (const label &each : labels)
        {
            label moved = each;
            moved.position = moved.position + offset;
            draw_label(out, moved, style, ghost);
        }
    }

    void append_fragment(std::vector<segment> &out, const fragment &items, point offset, const draw_style &style,
                         const math::vec4f &color)
    {
        append_fragment(out, items.parts, items.wires, items.labels, offset, style, color);
    }

    void append_wire_preview(std::vector<segment> &out, std::span<const point> path, const draw_style &style)
    {
        for (std::size_t i = 1; i < path.size(); ++i)
            out.push_back({world(path[i - 1]), world(path[i]), style.wire_width, style.ghost, -1});
    }

    void append_rect(std::vector<segment> &out, point a, point b, const draw_style &style)
    {
        const math::vec2f lo = world(point{std::min(a.x(), b.x()), std::min(a.y(), b.y())});
        const math::vec2f hi = world(point{std::max(a.x(), b.x()), std::max(a.y(), b.y())});
        const math::vec2f corners[] = {lo, math::vec2f{hi.x(), lo.y()}, hi, math::vec2f{lo.x(), hi.y()}, lo};
        polyline(out, corners, style.rubber_band_width, style.rubber_band);
    }

    void append_marker(std::vector<segment> &out, point at, const draw_style &style)
    {
        constexpr int sides = 16;
        const math::vec2f center = world(at);
        math::vec2f from = center + math::vec2f{style.marker_radius, 0.0f};
        for (int i = 1; i <= sides; ++i)
        {
            const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(sides);
            const math::vec2f to = center + math::vec2f{std::cos(angle), std::sin(angle)} * style.marker_radius;
            out.push_back({from, to, style.marker_width, style.marker, -1});
            from = to;
        }
    }

    // ---- extents ------------------------------------------------------------------------

    void label_bounds(const label &l, const draw_style &style, math::vec2f &min, math::vec2f &max)
    {
        box b;
        switch (l.kind)
        {
        case label_kind::net:
        {
            const label_layout layout = layout_label(l, label_gap, label_lift);
            take_text(b, l.text, layout.anchor, style.label_height, layout.horizontal, vertical_align::bottom,
                      layout.quarter_turns);
            const float length =
                text_width(l.text, style.label_height) * static_cast<float>(units_per_grid) + 2.0f * label_gap;
            b.take(to_float(l.position));
            b.take(to_float(l.position) + layout.along * length);
            break;
        }
        case label_kind::power:
        {
            const symbol &glyph = power_symbol(l.text);
            b.take(to_float(orient(glyph.min, l.rotation, false) + l.position));
            b.take(to_float(orient(glyph.max, l.rotation, false) + l.position));
            if (!is_ground_name(l.text))
                take_text(b, l.text, world(orient(glyph.value_anchor, l.rotation, false) + l.position),
                          style.label_height, horizontal_align::center, vertical_align::middle, l.rotation & 3);
            break;
        }
        case label_kind::note:
        {
            const label_layout layout = layout_label(l, 0.0f, 0.0f);
            take_text(b, l.text, layout.anchor, style.label_height, layout.horizontal, vertical_align::bottom,
                      layout.quarter_turns);
            b.take(to_float(l.position));
            break;
        }
        }
        min = b.min;
        max = b.max;
    }

    bool sheet_bounds(const sheet &s, point &min, point &max, const draw_style &style)
    {
        box b;
        for (const auto &e : s.parts)
        {
            point lo, hi;
            part_bounds(e.record, lo, hi);
            b.take(to_float(lo));
            b.take(to_float(hi));
            each_part_text(e.record, find_symbol(e.record.symbol), style, [&](const placed_text &t) {
                take_text(b, t.text, t.anchor, t.height, t.horizontal, t.vertical, t.quarter_turns);
            });
        }
        for (const auto &e : s.wires)
        {
            b.take(to_float(e.record.a));
            b.take(to_float(e.record.b));
        }
        for (const auto &e : s.labels)
        {
            math::vec2f lo, hi;
            label_bounds(e.record, style, lo, hi);
            b.take(lo);
            b.take(hi);
        }
        if (b.empty)
            return false;

        // The lines' own width: the widest of them is a junction dot.
        const float reach =
            0.5f * std::max({style.junction_diameter, style.wire_width + style.selection_widening,
                             style.symbol_width + style.selection_widening, style.unconnected_size}) *
            static_cast<float>(units_per_grid);
        min = point{static_cast<int>(std::floor(b.min.x() - reach)), static_cast<int>(std::floor(b.min.y() - reach))};
        max = point{static_cast<int>(std::ceil(b.max.x() + reach)), static_cast<int>(std::ceil(b.max.y() + reach))};
        return true;
    }

} // namespace volt::schematic
