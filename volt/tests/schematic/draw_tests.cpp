// Drawing a sheet: junction dots exactly where wires join (T, three and
// four ends, a pin inside a wire) and nowhere else (crossings, corners,
// straight joins, a wire on a pin); the demo ladder (wires carrying the
// nodes they are given, every part its strokes and reference, nothing
// outside the sheet's bounds, no NaN); a selection recolouring exactly its
// items; marks on free pins only; labels, values, pin numbers; and the
// overlays the tools draw.

#include "../math/check.hpp"

#include <schematic/demo.hpp>
#include <schematic/draw.hpp>
#include <schematic/stroke_font.hpp>
#include <schematic/symbol.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using namespace volt;
using namespace volt::schematic;

namespace
{
    bool near(float a, float b, float tolerance = 1e-4f)
    {
        return std::fabs(a - b) <= tolerance;
    }

    bool same(const segment &x, const segment &y)
    {
        return x.a == y.a && x.b == y.b && x.width == y.width && x.color == y.color && x.node == y.node;
    }

    // A sheet point in world units, computed as the drawing computes it, so
    // that the two compare equal.
    math::vec2f world(point p)
    {
        constexpr float scale = 1.0f / 100.0f;
        return math::vec2f{static_cast<float>(p.x()) * scale, static_cast<float>(p.y()) * scale};
    }

    struct drawn
    {
        std::vector<segment> segments;
        std::vector<item_ref> owners;
    };

    drawn draw(const sheet &s, draw_options options = {})
    {
        drawn d;
        options.owners = &d.owners;
        append_sheet(d.segments, s, options);
        return d;
    }

    // The junction dots drawn: segments of no length as wide as a dot.
    std::vector<math::vec2f> dots(const drawn &d, const draw_style &style = {})
    {
        std::vector<math::vec2f> found;
        for (std::size_t i = 0; i < d.segments.size(); ++i)
        {
            const segment &s = d.segments[i];
            if (s.a == s.b && s.width == style.junction_diameter && d.owners[i].id == 0)
                found.push_back(s.a);
        }
        return found;
    }

    sheet of_wires(std::initializer_list<wire> wires)
    {
        sheet s;
        item_id id = 1;
        for (const wire &w : wires)
            s.wires.insert(id++, w);
        return s;
    }

    void junction_rules()
    {
        // A T: one wire ends inside another.
        {
            const sheet s = of_wires({{{0, 0}, {400, 0}, {}}, {{200, 0}, {200, -300}, {}}});
            CHECK(junctions(s) == std::vector<point>{{200, 0}});
            const auto d = dots(draw(s));
            CHECK(d.size() == 1 && d[0] == math::vec2f{2.0f, 0.0f});
        }
        // Three wire ends at a point; four.
        CHECK(junctions(of_wires({{{0, 0}, {100, 0}, {}}, {{0, 0}, {0, 100}, {}}, {{0, 0}, {-100, 0}, {}}})) ==
              std::vector<point>{{0, 0}});
        CHECK(junctions(of_wires({{{0, 0}, {100, 0}, {}},
                                  {{0, 0}, {0, 100}, {}},
                                  {{-100, 0}, {0, 0}, {}},
                                  {{0, -100}, {0, 0}, {}}})) == std::vector<point>{{0, 0}});
        // Two T's on one wire, from either side, and a diagonal one.
        CHECK(junctions(of_wires({{{0, 0}, {1000, 0}, {}},
                                  {{300, 0}, {300, 200}, {}},
                                  {{700, -200}, {700, 0}, {}},
                                  {{500, 500}, {500, 0}, {}}})) ==
              (std::vector<point>{{300, 0}, {500, 0}, {700, 0}}));
        CHECK(junctions(of_wires({{{0, 0}, {400, 400}, {}}, {{200, 200}, {200, 600}, {}}})) ==
              std::vector<point>{{200, 200}});

        // Nothing where wires only cross, at a corner, or join in a line.
        CHECK(junctions(of_wires({{{0, -100}, {0, 300}, {}}, {{-100, 100}, {200, 100}, {}}})).empty());
        CHECK(junctions(of_wires({{{0, 0}, {0, 300}, {}}, {{0, 0}, {300, 0}, {}}})).empty());
        CHECK(junctions(of_wires({{{0, 0}, {300, 0}, {}}, {{300, 0}, {600, 0}, {}}})).empty());
        CHECK(dots(draw(of_wires({{{0, -100}, {0, 300}, {}}, {{-100, 100}, {200, 100}, {}}}))).empty());
        CHECK(junctions(sheet{}).empty());

        // Pins count as ends: a wire on a pin is a plain connection; a pin
        // inside a wire, or a pin where two wires meet, is a junction.
        {
            sheet s;
            s.parts.insert(1, part{"R", {0, 0}, 0, false, "R1", "1k", "", {}, {}}); // pins at (-100, 0), (100, 0)
            s.wires.insert(2, wire{{100, 0}, {400, 0}, {}});
            CHECK(junctions(s).empty());
            s.wires.insert(3, wire{{-100, -300}, {-100, 300}, {}}); // through pin 1
            CHECK(junctions(s) == std::vector<point>{{-100, 0}});
            s.wires.insert(4, wire{{100, 0}, {100, 300}, {}}); // a second wire at pin 2
            CHECK(junctions(s) == (std::vector<point>{{-100, 0}, {100, 0}}));
        }
        // Labels are not ends: a net label at a corner adds no dot.
        {
            sheet s = of_wires({{{0, 0}, {0, 300}, {}}, {{0, 0}, {300, 0}, {}}});
            s.labels.insert(9, label{label_kind::net, {0, 0}, 0, "A", {}});
            CHECK(junctions(s).empty());
        }
        // With pins connecting at wire ends only, a pin inside a wire is no
        // T; a pin with a wire end there inside another wire still is; T's
        // of wires are as before.
        {
            const connectivity ends_only{.pins_inside_wires = false};
            sheet s;
            s.parts.insert(1, part{"R", {0, 0}, 0, false, "R1", "1k", "", {}, {}}); // pins at (-100, 0), (100, 0)
            s.wires.insert(2, wire{{-100, -300}, {-100, 300}, {}});                 // through pin 1
            s.wires.insert(3, wire{{100, -300}, {100, 300}, {}});                   // through pin 2...
            s.wires.insert(4, wire{{100, 0}, {400, 0}, {}});                        // ...where a wire ends
            s.wires.insert(5, wire{{0, 1000}, {400, 1000}, {}});
            s.wires.insert(6, wire{{200, 1000}, {200, 1300}, {}});
            CHECK(junctions(s) == (std::vector<point>{{-100, 0}, {100, 0}, {200, 1000}}));
            CHECK(junctions(s, ends_only) == (std::vector<point>{{100, 0}, {200, 1000}}));
            draw_options options;
            options.rules = ends_only;
            CHECK(dots(draw(s, options)).size() == 2 && dots(draw(s)).size() == 3);
        }
    }

    void demo()
    {
        const sheet s = demo_ladder();
        std::unordered_map<item_id, std::int32_t> nodes;
        for (const auto &e : s.wires)
            nodes[e.id] = static_cast<std::int32_t>(e.id % 7);
        draw_options options;
        options.wire_nodes = &nodes;
        const drawn d = draw(s, options);
        CHECK(d.owners.size() == d.segments.size());

        // Wires carry their nodes, and every wire is drawn exactly once.
        std::unordered_map<item_id, int> wire_segments;
        for (std::size_t i = 0; i < d.segments.size(); ++i)
        {
            if (d.owners[i].kind != item_kind::wire || d.owners[i].id == 0)
                continue;
            const segment &seg = d.segments[i];
            const wire &w = *s.wires.find(d.owners[i].id);
            ++wire_segments[d.owners[i].id];
            CHECK(seg.node == nodes.at(d.owners[i].id) && seg.a == world(w.a) && seg.b == world(w.b));
            CHECK(seg.width == draw_style{}.wire_width);
        }
        CHECK(wire_segments.size() == s.wires.size());
        for (const auto &[id, count] : wire_segments)
            CHECK(count == 1);

        // Junction dots: at the three inner nodes of each rail, carrying
        // the node of a wire through them.
        CHECK(junctions(s) ==
              (std::vector<point>{{-400, -200}, {-400, 200}, {0, -200}, {0, 200}, {400, -200}, {400, 200}}));
        std::size_t dot_count = 0;
        for (std::size_t i = 0; i < d.segments.size(); ++i)
        {
            const segment &seg = d.segments[i];
            if (d.owners[i].id != 0)
                continue;
            ++dot_count;
            CHECK(seg.a == seg.b && seg.width == draw_style{}.junction_diameter && seg.node >= 0);
            bool from_a_wire = false;
            const point at{static_cast<int>(std::lround(seg.a.x() * 100.0f)),
                           static_cast<int>(std::lround(seg.a.y() * 100.0f))};
            for (const auto &e : s.wires)
                from_a_wire = from_a_wire || (on_wire(e.record, at) && nodes.at(e.id) == seg.node);
            CHECK(from_a_wire);
        }
        CHECK(dot_count == 6);

        // Every part: its strokes, then its reference among its segments,
        // exactly as the font draws it at its anchor. Symbols and text have
        // no node. All pins are connected: no marks.
        const draw_style style;
        for (const auto &e : s.parts)
        {
            const symbol &sym = find_symbol(e.record.symbol);
            std::vector<segment> own;
            for (std::size_t i = 0; i < d.segments.size(); ++i)
                if (d.owners[i] == item_ref{item_kind::part, e.id})
                    own.push_back(d.segments[i]);
            std::size_t stroke_segments = 0;
            for (const auto &stroke : sym.strokes)
                stroke_segments += stroke.size() - 1;
            CHECK(own.size() > stroke_segments);
            bool strokes = own.size() >= stroke_segments;
            std::size_t k = 0;
            for (const auto &stroke : sym.strokes)
                for (std::size_t j = 1; strokes && j < stroke.size(); ++j, ++k)
                    strokes = own[k].a == world(transform(e.record, stroke[j - 1])) &&
                              own[k].b == world(transform(e.record, stroke[j])) && own[k].color == style.symbol;
            CHECK(strokes);

            std::vector<segment> reference;
            append_text(reference, e.record.reference, world(transform(e.record, sym.reference_anchor)),
                        style.reference_height, horizontal_align::center, vertical_align::middle, e.record.rotation,
                        style.text_stroke, style.text);
            CHECK(!reference.empty());
            bool found = true;
            for (const segment &r : reference)
            {
                bool any = false;
                for (const segment &o : own)
                    any = any || same(o, r);
                found = found && any;
            }
            CHECK(found);
            for (const segment &o : own)
                CHECK(o.node == -1 && !(o.color == style.unconnected));
        }

        // The label: the ground glyph, no text.
        {
            const label &gnd = s.labels.entries().front().record;
            std::size_t glyph_segments = 0;
            for (const auto &stroke : power_symbol(gnd.text).strokes)
                glyph_segments += stroke.size() - 1;
            std::size_t own = 0;
            for (std::size_t i = 0; i < d.segments.size(); ++i)
                own += d.owners[i].kind == item_kind::label ? 1 : 0;
            CHECK(own == glyph_segments);
        }

        // Nothing NaN, nothing outside the sheet's bounds.
        point lo, hi;
        CHECK(sheet_bounds(s, lo, hi));
        const math::vec2f wlo = world(lo), whi = world(hi);
        bool inside = true;
        for (const segment &seg : d.segments)
        {
            const float r = 0.5f * seg.width;
            for (const math::vec2f p : {seg.a, seg.b})
                inside = inside && std::isfinite(p.x()) && std::isfinite(p.y()) && p.x() - r >= wlo.x() - 1e-4f &&
                         p.x() + r <= whi.x() + 1e-4f && p.y() - r >= wlo.y() - 1e-4f && p.y() + r <= whi.y() + 1e-4f;
            inside = inside && std::isfinite(seg.width) && seg.width > 0.0f;
        }
        CHECK(inside);
        // And the bounds are tight: the rails and the source are near them.
        CHECK(lo.x() >= -1000 && lo.x() <= -880 && hi.x() >= 900 && hi.x() <= 1000 && lo.y() >= -400 &&
              lo.y() <= -300 && hi.y() >= 300 && hi.y() <= 400);

        // Without a map, nothing has a node.
        const drawn plain = draw(s);
        CHECK(plain.segments.size() == d.segments.size());
        for (const segment &seg : plain.segments)
            CHECK(seg.node == -1);

        // Empty: nothing drawn, no bounds.
        point untouched{7, 7};
        CHECK(draw(sheet{}).segments.empty());
        CHECK(!sheet_bounds(sheet{}, untouched, untouched) && untouched == point{7, 7});
    }

    void selection()
    {
        const sheet s = demo_ladder();
        std::unordered_map<item_id, std::int32_t> nodes;
        for (const auto &e : s.wires)
            nodes[e.id] = 1;
        const std::vector<item_ref> chosen{{item_kind::part, 3},
                                           {item_kind::wire, s.wires.entries()[2].id},
                                           {item_kind::label, s.labels.entries().front().id},
                                           {item_kind::part, 999}}; // not on the sheet: ignored
        draw_options options;
        options.wire_nodes = &nodes;
        const drawn before = draw(s, options);
        options.selection = chosen;
        const drawn after = draw(s, options);

        CHECK(before.segments.size() == after.segments.size() && before.owners == after.owners);
        const draw_style style;
        std::size_t changed = 0;
        bool exact = before.segments.size() == after.segments.size();
        for (std::size_t i = 0; exact && i < before.segments.size(); ++i)
        {
            const item_ref owner = before.owners[i];
            const bool is_chosen = owner.id != 0 && std::find(chosen.begin(), chosen.end(), owner) != chosen.end();
            const segment &x = before.segments[i];
            const segment &y = after.segments[i];
            if (!is_chosen)
            {
                exact = same(x, y);
                continue;
            }
            ++changed;
            exact = y.color == style.selection && near(y.width, x.width + style.selection_widening) && y.a == x.a &&
                    y.b == x.b && !(x.color == style.selection);
            if (owner.kind == item_kind::wire)
                exact = exact && x.node == 1 && y.node == -1;
        }
        CHECK(exact);
        std::size_t owned = 0;
        for (const item_ref owner : before.owners)
            owned += std::find(chosen.begin(), chosen.end(), owner) != chosen.end() ? 1 : 0;
        CHECK(changed == owned && changed > 3);
    }

    void free_pins()
    {
        const draw_style style;
        const auto marks_near = [&](const drawn &d, math::vec2f at) {
            std::size_t count = 0;
            for (const segment &seg : d.segments)
                if (seg.color == style.unconnected && std::fabs(seg.a.x() - at.x()) <= style.unconnected_size &&
                    std::fabs(seg.a.y() - at.y()) <= style.unconnected_size)
                    ++count;
            return count;
        };

        sheet s;
        s.parts.insert(1, part{"R", {0, 0}, 0, false, "R1", "1k", "", {}, {}});
        s.wires.insert(2, wire{{100, 0}, {300, 0}, {}});
        {
            const drawn d = draw(s);
            CHECK(marks_near(d, {-1.0f, 0.0f}) == 4); // pin 1: an open square
            CHECK(marks_near(d, {1.0f, 0.0f}) == 0);  // pin 2: a wire's end
        }
        // A wire passing through the pin connects it, and so does a net
        // label on it; a note does not.
        {
            sheet t = s;
            t.labels.insert(3, label{label_kind::note, {-100, 0}, 0, "here", {}});
            CHECK(marks_near(draw(t), {-1.0f, 0.0f}) == 4);
            t.labels.insert(4, label{label_kind::net, {-100, 0}, 0, "IN", {}});
            CHECK(marks_near(draw(t), {-1.0f, 0.0f}) == 0);
        }
        {
            sheet t = s;
            t.wires.insert(3, wire{{-100, -200}, {-100, 200}, {}});
            CHECK(marks_near(draw(t), {-1.0f, 0.0f}) == 0);
            // Not when pins connect at wire ends only: the wire goes by.
            draw_options options;
            options.rules.pins_inside_wires = false;
            const drawn d = draw(t, options);
            CHECK(marks_near(d, {-1.0f, 0.0f}) == 4 && marks_near(d, {1.0f, 0.0f}) == 0);
        }
        // Another part's pin on it.
        {
            sheet t = s;
            t.parts.insert(3, part{"C", {-200, 0}, 0, false, "C1", "1n", "", {}, {}}); // pins at -300, -100
            const drawn d = draw(t);
            CHECK(marks_near(d, {-1.0f, 0.0f}) == 0 && marks_near(d, {-3.0f, 0.0f}) == 4);
        }
        // The marks belong to the part, and turn the selection colour with
        // it.
        {
            const std::vector<item_ref> chosen{{item_kind::part, 1}};
            draw_options options;
            options.selection = chosen;
            const drawn d = draw(s, options);
            CHECK(marks_near(d, {-1.0f, 0.0f}) == 0);
        }
    }

    std::size_t owned_by(const drawn &d, item_ref item)
    {
        std::size_t n = 0;
        for (const item_ref owner : d.owners)
            n += owner == item ? 1 : 0;
        return n;
    }

    void texts()
    {
        // An empty value is left out; a hidden reference ("#PWR1") too.
        {
            sheet with, without, hidden;
            with.parts.insert(1, part{"R", {0, 0}, 0, false, "R1", "4.7k", "", {}, {}});
            without.parts.insert(1, part{"R", {0, 0}, 0, false, "R1", "", "", {}, {}});
            hidden.parts.insert(1, part{"R", {0, 0}, 0, false, "#R1", "", "", {}, {}});
            std::vector<segment> value;
            append_text(value, "4.7k", {}, 0.5f, horizontal_align::center, vertical_align::middle, 0, 0.06f, {});
            std::vector<segment> reference;
            append_text(reference, "R1", {}, 0.5f, horizontal_align::center, vertical_align::middle, 0, 0.06f, {});
            const std::size_t a = draw(with).segments.size();
            const std::size_t b = draw(without).segments.size();
            const std::size_t c = draw(hidden).segments.size();
            CHECK(a == b + value.size() && b == c + reference.size());
        }

        // Pin numbers and names: a box shows them as the style says; a
        // resistor never does.
        {
            sheet box, resistor;
            box.parts.insert(1, part{"Timer_555", {0, 0}, 1, false, "U1", "NE555", "", {}, {}});
            resistor.parts.insert(1, part{"R", {0, 0}, 0, false, "R1", "1k", "", {}, {}});
            draw_options options;
            const std::size_t both = draw(box, options).segments.size();
            const std::size_t r_both = draw(resistor, options).segments.size();
            options.style.show_pin_numbers = false;
            const std::size_t names_only = draw(box, options).segments.size();
            options.style.show_pin_names = false;
            const std::size_t neither = draw(box, options).segments.size();
            CHECK(both > names_only && names_only > neither);
            CHECK(draw(resistor, options).segments.size() == r_both);

            // Pin text in its own colour, the name inside in the text colour.
            const drawn d = draw(box);
            const draw_style style;
            std::size_t pin_text = 0, text = 0;
            for (const segment &seg : d.segments)
            {
                pin_text += seg.color == style.pin_text ? 1 : 0;
                text += seg.color == style.text ? 1 : 0;
            }
            CHECK(pin_text == both - neither);
            CHECK(text > 0);
        }

        // Labels.
        {
            const draw_style style;
            sheet s;
            s.labels.insert(1, label{label_kind::net, {0, 0}, 0, "CLK", {}});
            s.labels.insert(2, label{label_kind::power, {500, 0}, 0, "+5V", {}});
            s.labels.insert(3, label{label_kind::power, {1000, 0}, 2, "GND", {}});
            s.labels.insert(4, label{label_kind::note, {1500, 0}, 1, "Note", {}});
            const drawn d = draw(s);

            std::vector<segment> clk;
            append_text(clk, "CLK", {}, style.label_height, horizontal_align::left, vertical_align::bottom, 0,
                        style.text_stroke, style.net_label);
            CHECK(owned_by(d, {item_kind::label, 1}) == clk.size() + 2); // the text, and its flag
            std::size_t pwr = 0, gnd = 0;
            for (const auto &stroke : find_symbol("PWR").strokes)
                pwr += stroke.size() - 1;
            for (const auto &stroke : find_symbol("GND").strokes)
                gnd += stroke.size() - 1;
            CHECK(owned_by(d, {item_kind::label, 2}) > pwr);  // the glyph and its text
            CHECK(owned_by(d, {item_kind::label, 3}) == gnd); // ground: the glyph alone
            CHECK(owned_by(d, {item_kind::label, 4}) > 0);

            bool colours = true;
            for (std::size_t i = 0; i < d.segments.size(); ++i)
            {
                const item_id id = d.owners[i].id;
                const math::vec4f &c = d.segments[i].color;
                colours = colours && (id == 1   ? c == style.net_label
                                      : id == 4 ? c == style.note
                                                : c == style.power_label);
            }
            CHECK(colours);

            // A ground port turned twice points up: its glyph is above the
            // point.
            float lowest = 1e30f;
            for (std::size_t i = 0; i < d.segments.size(); ++i)
                if (d.owners[i].id == 3)
                    lowest = std::min({lowest, d.segments[i].a.y(), d.segments[i].b.y()});
            CHECK(near(lowest, 0.0f));

            // The net label's text reads from its point, above it; turned,
            // still from the point.
            for (std::uint8_t r = 0; r < 4; ++r)
            {
                sheet t;
                t.labels.insert(1, label{label_kind::net, {0, 0}, r, "DATA", {}});
                math::vec2f lo, hi;
                label_bounds(t.labels.entries().front().record, style, lo, hi);
                const float length = text_width("DATA", style.label_height) * 100.0f;
                switch (r)
                {
                case 0:
                    CHECK(lo.x() >= -1.0f && hi.x() >= length && lo.y() >= -1.0f && hi.y() > 40.0f);
                    break;
                case 1:
                    CHECK(lo.y() >= -1.0f && hi.y() >= length && hi.x() <= 1.0f && lo.x() < -40.0f);
                    break;
                case 2:
                    CHECK(hi.x() <= 1.0f && lo.x() <= -length && lo.y() >= -1.0f && hi.y() > 40.0f);
                    break;
                default:
                    CHECK(hi.y() <= 1.0f && lo.y() <= -length && hi.x() <= 1.0f && lo.x() < -40.0f);
                    break;
                }
                // Everything drawn is inside the label's box (widths aside).
                const drawn dl = draw(t);
                bool inside = !dl.segments.empty();
                for (const segment &seg : dl.segments)
                    for (const math::vec2f p : {seg.a, seg.b})
                        inside = inside && p.x() * 100.0f >= lo.x() - 0.5f && p.x() * 100.0f <= hi.x() + 0.5f &&
                                 p.y() * 100.0f >= lo.y() - 12.0f && p.y() * 100.0f <= hi.y() + 12.0f;
                CHECK(inside);
            }
        }
    }

    void overlays()
    {
        const draw_style style;
        const math::vec4f ghost{0.1f, 0.2f, 0.3f, 0.4f};

        // A ghost: everything in the colour given, moved, no node, no marks.
        fragment items;
        items.parts.push_back(part{"R", {0, 0}, 0, false, "R?", "1k", "", {}, {}});
        items.wires.push_back(wire{{0, 0}, {100, 0}, {}});
        items.labels.push_back(label{label_kind::net, {0, 0}, 0, "A", {}});
        std::vector<segment> out;
        append_fragment(out, items, point{200, 300}, style, ghost);
        CHECK(!out.empty());
        bool ghostly = true;
        for (const segment &seg : out)
            ghostly = ghostly && seg.color == ghost && seg.node == -1;
        CHECK(ghostly);
        bool moved_wire = false;
        for (const segment &seg : out)
            moved_wire = moved_wire || (seg.a == math::vec2f{2.0f, 3.0f} && seg.b == math::vec2f{3.0f, 3.0f} &&
                                        seg.width == style.wire_width);
        CHECK(moved_wire);
        // The same as the parts, wires and labels given apart.
        std::vector<segment> apart;
        append_fragment(apart, items.parts, items.wires, items.labels, point{200, 300}, style, ghost);
        CHECK(apart.size() == out.size());
        // Drawn with marks on the sheet, without as a ghost: fewer here.
        sheet alone;
        alone.parts.insert(1, items.parts[0]);
        std::vector<segment> part_ghost;
        append_fragment(part_ghost, items.parts, {}, {}, point{}, style, ghost);
        CHECK(draw(alone).segments.size() == part_ghost.size() + 8); // two open squares

        // The wire being drawn.
        const point path[] = {{0, 0}, {300, 0}, {300, 200}};
        std::vector<segment> preview;
        append_wire_preview(preview, path, style);
        CHECK(preview.size() == 2 && preview[1].a == math::vec2f{3.0f, 0.0f} &&
              preview[1].b == math::vec2f{3.0f, 2.0f} && preview[0].width == style.wire_width &&
              preview[0].color == style.ghost);
        preview.clear();
        append_wire_preview(preview, std::span<const point>(path, 1), style);
        CHECK(preview.empty());

        // The rubber band: four sides, whichever corners.
        std::vector<segment> band, flipped;
        append_rect(band, {100, 200}, {-300, -400}, style);
        append_rect(flipped, {-300, -400}, {100, 200}, style);
        CHECK(band.size() == 4 && flipped.size() == 4);
        bool equal = true, closed = true;
        for (std::size_t i = 0; i < 4; ++i)
        {
            equal = equal && same(band[i], flipped[i]);
            closed = closed && band[i].b == band[(i + 1) % 4].a && band[i].width == style.rubber_band_width &&
                     band[i].color == style.rubber_band;
        }
        CHECK(equal && closed);
        CHECK(band[0].a == math::vec2f{-3.0f, -4.0f} && band[1].b == math::vec2f{1.0f, 2.0f});

        // The marker: a ring about the point.
        std::vector<segment> marker;
        append_marker(marker, {500, -100}, style);
        CHECK(marker.size() >= 8);
        bool ring = true;
        for (const segment &seg : marker)
        {
            const math::vec2f d = seg.a - math::vec2f{5.0f, -1.0f};
            ring = ring && near(std::sqrt(d.x() * d.x() + d.y() * d.y()), style.marker_radius, 1e-4f) &&
                   seg.color == style.marker;
        }
        CHECK(ring);
    }

    void bounds()
    {
        sheet s;
        s.wires.insert(1, wire{{-500, 100}, {700, 100}, {}});
        point lo, hi;
        CHECK(sheet_bounds(s, lo, hi));
        // The wire, and half a junction dot around it.
        CHECK(lo.x() <= -518 && lo.x() >= -519 && lo.y() <= 82 && lo.y() >= 81);
        CHECK(hi.x() >= 718 && hi.x() <= 719 && hi.y() >= 118 && hi.y() <= 119);

        // Text counts: a long value reaches past the body.
        s.parts.insert(2, part{"R", {0, 1000}, 0, false, "R1", "a very long value indeed", "", {}, {}});
        CHECK(sheet_bounds(s, lo, hi));
        const float value = text_width("a very long value indeed", draw_style{}.reference_height) * 100.0f;
        CHECK(hi.x() >= static_cast<int>(value / 2.0f) && hi.y() >= 1100);

        // A large sheet draws in one piece, its dots computed.
        sheet big;
        item_id id = 1;
        for (int row = 0; row < 40; ++row)
        {
            big.wires.insert(id++, wire{{0, row * 200}, {8000, row * 200}, {}});
            for (int column = 0; column < 40; ++column)
                big.wires.insert(id++, wire{{column * 200, row * 200}, {column * 200, row * 200 + 100}, {}});
        }
        // Each stub's lower end is a T on its row except where the row ends
        // (x = 0 is the row's end: a corner).
        CHECK(junctions(big).size() == 40 * 39);
    }

} // namespace

int main()
{
    junction_rules();
    demo();
    selection();
    free_pins();
    texts();
    overlays();
    bounds();
    return volt::tests::result("schematic draw");
}
