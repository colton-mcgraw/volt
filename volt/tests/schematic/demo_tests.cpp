// The demo sheet: the ladder's parts, on the grid; every wire end on a pin
// or another wire, every pin on a wire; the nets the circuit should have;
// nothing for tidy_wires() to do; and a round trip through the file.

#include "../math/check.hpp"

#include <schematic/demo.hpp>
#include <schematic/document.hpp>
#include <schematic/io.hpp>
#include <schematic/symbol.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace volt;
using namespace volt::schematic;

namespace
{
    bool on_grid(point p)
    {
        return p.x() % units_per_grid == 0 && p.y() % units_per_grid == 0;
    }

    // Points joined by wires: a union-find over every point a wire, pin or
    // label is at.
    class nets
    {
    public:
        explicit nets(const sheet &s)
        {
            for (const auto &w : s.wires)
                join(w.record.a, w.record.b);
            // A wire's end on another wire's inside is joined to it.
            for (const auto &w : s.wires)
                for (const auto &other : s.wires)
                    for (const point end : {w.record.a, w.record.b})
                        if (inside_wire(other.record, end))
                            join(end, other.record.a);
        }

        std::size_t of(point p) { return find(index(p)); }

    private:
        std::size_t index(point p)
        {
            const auto [it, added] = m_index.try_emplace(std::pair{p.x(), p.y()}, m_parent.size());
            if (added)
                m_parent.push_back(m_parent.size());
            return it->second;
        }

        std::size_t find(std::size_t i)
        {
            while (m_parent[i] != i)
                i = m_parent[i] = m_parent[m_parent[i]];
            return i;
        }

        void join(point a, point b) { m_parent[find(index(a))] = find(index(b)); }

        std::map<std::pair<int, int>, std::size_t> m_index;
        std::vector<std::size_t> m_parent;
    };

    void contents()
    {
        const sheet s = demo_ladder();
        CHECK(s.parts.size() == 9 && s.wires.size() == 20 && s.labels.size() == 1);

        // Ids from 1, in table order.
        std::vector<item_id> ids;
        for (const auto &e : s.parts)
            ids.push_back(e.id);
        for (const auto &e : s.wires)
            ids.push_back(e.id);
        for (const auto &e : s.labels)
            ids.push_back(e.id);
        std::vector<item_id> expected(ids.size());
        std::iota(expected.begin(), expected.end(), item_id{1});
        CHECK(ids == expected && s.max_id() == 30);

        const part &v1 = s.parts[0].record;
        CHECK(v1.symbol == "V_DC" && v1.reference == "V1" && v1.value == "5" && v1.device == "voltage_source");
        std::set<std::string> references;
        int series = 0;
        int shunt = 0;
        for (const auto &e : s.parts)
        {
            references.insert(e.record.reference);
            CHECK(on_grid(e.record.position));
            if (e.record.symbol == "R")
            {
                CHECK(e.record.device == "resistor");
                series += e.record.value == "100" ? 1 : 0;
                shunt += e.record.value == "220" ? 1 : 0;
            }
        }
        CHECK(series == 4 && shunt == 4 && references.size() == 9);
        CHECK(s.labels[0].record.kind == label_kind::power && s.labels[0].record.text == "GND");

        // Within the layout: x from -800 to 800, y from -200 to 200.
        for (const auto &w : s.wires)
            for (const point p : {w.record.a, w.record.b})
                CHECK(on_grid(p) && p.x() >= -800 && p.x() <= 800 && p.y() >= -200 && p.y() <= 200);
    }

    void connected()
    {
        const sheet s = demo_ladder();
        std::vector<point> pins;
        for (const auto &e : s.parts)
            for (const point p : pin_positions(e.record))
                pins.push_back(p);
        CHECK(pins.size() == 18);

        // Every wire end touches a pin, another wire's end, or another wire.
        for (const auto &w : s.wires)
        {
            for (const point end : {w.record.a, w.record.b})
            {
                const bool on_pin = std::ranges::find(pins, end) != pins.end();
                const bool on_other = std::ranges::any_of(
                    s.wires, [&](const auto &other) { return other.id != w.id && on_wire(other.record, end); });
                CHECK(on_pin || on_other);
            }
        }
        // Every pin is on a wire's end.
        for (const point p : pins)
            CHECK(std::ranges::any_of(s.wires, [&](const auto &w) { return w.record.a == p || w.record.b == p; }));
        // The label is on a wire.
        CHECK(std::ranges::any_of(s.wires, [&](const auto &w) { return on_wire(w.record, s.labels[0].record.position); }));

        // The nets: the top node of each stage, and ground.
        nets n(s);
        std::set<std::size_t> distinct;
        for (const point p : pins)
            distinct.insert(n.of(p));
        CHECK(distinct.size() == 6);

        const auto pin_of = [&](std::string_view reference, std::string_view number) {
            for (const auto &e : s.parts)
                if (e.record.reference == reference)
                    for (const placed_pin &p : placed_pins(e.record))
                        if (p.pin->number == number)
                            return p.position;
            return point{};
        };
        const std::size_t ground = n.of(pin_of("V1", "2"));
        CHECK(n.of(s.labels[0].record.position) == ground);
        CHECK(n.of(pin_of("V1", "1")) == n.of(pin_of("R1", "1")));
        for (int stage = 0; stage < 4; ++stage)
        {
            const std::string series = "R" + std::to_string(2 * stage + 1);
            const std::string shunt = "R" + std::to_string(2 * stage + 2);
            CHECK(n.of(pin_of(series, "1")) != n.of(pin_of(series, "2")));
            CHECK(n.of(pin_of(series, "2")) == n.of(pin_of(shunt, "2")));
            CHECK(n.of(pin_of(shunt, "1")) == ground);
            if (stage < 3)
                CHECK(n.of(pin_of(series, "2")) == n.of(pin_of("R" + std::to_string(2 * stage + 3), "1")));
        }

        // Nothing to tidy.
        document d(s);
        CHECK(d.tidy_wires() == 0 && d.sheet() == s && !d.can_undo());
    }

    void round_trip()
    {
        const sheet s = demo_ladder();
        const std::string text = write_sheet(s);
        const auto loaded = parse_sheet(text);
        CHECK(loaded && loaded->report.clean() && loaded->sheet == s);
        CHECK(loaded && write_sheet(loaded->sheet) == text);
        CHECK(demo_ladder() == s); // the same every time
    }

} // namespace

int main()
{
    contents();
    connected();
    round_trip();
    return volt::tests::result("schematic demo");
}
