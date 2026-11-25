// Symbols: every built-in one and a range of generated ones are sane (pins
// on the grid in all eight orientations, unique pin numbers, directions
// into the body, bounds around everything); the shapes the conventions ask
// for; lookups that never fail and keep their references; and placing a
// part (orientation, pins on the sheet, bounds).

#include "../math/check.hpp"

#include <schematic/symbol.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace volt;
using namespace volt::schematic;

namespace
{
    bool on_grid(point p)
    {
        return p.x() % units_per_grid == 0 && p.y() % units_per_grid == 0;
    }

    bool within(point p, point min, point max)
    {
        return p.x() >= min.x() && p.x() <= max.x() && p.y() >= min.y() && p.y() <= max.y();
    }

    bool unit_axis(point d)
    {
        return (d.x() == 0 && (d.y() == 1 || d.y() == -1)) || (d.y() == 0 && (d.x() == 1 || d.x() == -1));
    }

    // Everything every symbol must be.
    bool sane(const symbol &s)
    {
        bool ok = !s.pins.empty() && !s.strokes.empty() && !s.prefix.empty();
        std::set<std::string> numbers;
        for (const pin &p : s.pins)
        {
            ok = ok && !p.number.empty() && numbers.insert(p.number).second;
            ok = ok && unit_axis(p.direction) && within(p.position, s.min, s.max);
            // A step from the pin's end goes into the drawing.
            ok = ok && within(p.position + p.direction, s.min, s.max);
            ok = ok && s.find_pin(p.number) == &p;
            for (std::uint8_t r = 0; r < 4; ++r)
                for (const bool m : {false, true})
                    ok = ok && on_grid(orient(p.position, r, m));
        }
        for (const std::vector<point> &stroke : s.strokes)
        {
            ok = ok && stroke.size() >= 2;
            for (const point p : stroke)
                ok = ok && within(p, s.min, s.max);
        }
        ok = ok && s.min.x() <= s.max.x() && s.min.y() <= s.max.y();
        if (!ok)
            std::fprintf(stderr, "symbol %s is not sane\n", s.name.c_str());
        return ok;
    }

    void builtins()
    {
        const auto names = builtin_symbol_names();
        CHECK(names.size() >= 17);
        for (const std::string_view name : names)
        {
            const symbol &s = find_symbol(name);
            CHECK(s.name == name);
            CHECK(is_builtin_symbol(name));
            CHECK(sane(s));
        }
        for (const char *name : {"R", "C", "C_Polarized", "L", "D", "D_Zener", "D_Schottky", "LED", "Q_NPN", "Q_PNP",
                                 "Q_NMOS", "Q_PMOS", "OpAmp", "V_DC", "I_DC", "GND", "PWR"})
            CHECK(is_builtin_symbol(name));
        CHECK(!is_builtin_symbol("Conn_01x04") && !is_builtin_symbol("MCU") && !is_builtin_symbol(""));
    }

    void conventions()
    {
        // Two-terminal parts: horizontal, 200 between pins, pin 1 on the
        // left.
        for (const char *name : {"R", "C", "C_Polarized", "L", "D", "D_Zener", "D_Schottky", "LED"})
        {
            const symbol &s = find_symbol(name);
            CHECK(s.pins.size() == 2);
            const pin *one = s.find_pin("1");
            const pin *two = s.find_pin("2");
            CHECK(one && two);
            if (one && two)
            {
                CHECK(one->position == point{-100, 0} && two->position == point{100, 0});
                CHECK(one->direction == point{1, 0} && two->direction == point{-1, 0});
            }
        }
        CHECK(find_symbol("R").prefix == "R" && find_symbol("R").device == "resistor");
        CHECK(find_symbol("C").device == "capacitor" && find_symbol("L").device == "inductor");
        CHECK(find_symbol("D").find_pin("1")->name == "K" && find_symbol("D").find_pin("2")->name == "A");
        CHECK(find_symbol("LED").device == "led" && find_symbol("D_Zener").device == "zener");
        CHECK(find_symbol("C_Polarized").find_pin("1")->name == "+");

        // Sources: vertical, 400 long, + at the top.
        for (const char *name : {"V_DC", "I_DC"})
        {
            const symbol &s = find_symbol(name);
            CHECK(s.pins.size() == 2);
            CHECK(s.find_pin("1") && s.find_pin("1")->position == point{0, 200});
            CHECK(s.find_pin("2") && s.find_pin("2")->position == point{0, -200});
        }
        CHECK(find_symbol("V_DC").device == "voltage_source" && find_symbol("V_DC").prefix == "V");
        CHECK(find_symbol("V_DC").find_pin("1")->name == "+" && find_symbol("V_DC").find_pin("2")->name == "-");
        CHECK(find_symbol("I_DC").device == "current_source" && find_symbol("I_DC").prefix == "I");

        // Transistors: three pins, base or gate pin 1.
        for (const char *name : {"Q_NPN", "Q_PNP", "Q_NMOS", "Q_PMOS"})
        {
            const symbol &s = find_symbol(name);
            CHECK(s.pins.size() == 3 && s.prefix == "Q" && s.find_pin("1") &&
                  s.find_pin("1")->position == point{-200, 0});
        }
        CHECK(find_symbol("Q_NPN").find_pin("2")->name == "C" && find_symbol("Q_NMOS").find_pin("3")->name == "S");

        // The op amp's five pins.
        {
            const symbol &s = find_symbol("OpAmp");
            std::set<std::string> names;
            for (const pin &p : s.pins)
                names.insert(p.name);
            CHECK(s.pins.size() == 5 && names == std::set<std::string>{"+", "-", "OUT", "V+", "V-"});
        }

        // Power glyphs: one pin, at the origin.
        for (const char *name : {"GND", "PWR"})
        {
            const symbol &s = find_symbol(name);
            CHECK(s.pins.size() == 1 && s.pins[0].position == point{0, 0} && s.prefix == "#PWR");
        }
        CHECK(find_symbol("GND").max.y() == 0 && find_symbol("PWR").min.y() == 0); // below, above

        // The 555: eight pins, 1 to 8.
        {
            const symbol &s = find_symbol("Timer_555");
            CHECK(s.pins.size() == 8 && s.name_inside);
            for (int n = 1; n <= 8; ++n)
                CHECK(s.find_pin(std::to_string(n)) != nullptr);
            CHECK(s.find_pin("8")->name == "VCC" && s.find_pin("1")->name == "GND");
        }
    }

    void generated()
    {
        // One column: pins 1..N down the left, pin 1 at the top.
        for (int n : {1, 2, 3, 4, 7, 10, 20, 40})
        {
            const std::string name = std::format("Conn_01x{:02}", n);
            const symbol &s = find_symbol(name);
            CHECK(s.name == name && sane(s) && !s.name_inside && s.prefix == "J");
            CHECK(s.pins.size() == static_cast<std::size_t>(n));
            bool down_the_left = true;
            for (int k = 1; k <= n; ++k)
            {
                const pin *p = s.find_pin(std::to_string(k));
                down_the_left = down_the_left && p != nullptr && p->position.x() == -200 &&
                                p->position.y() == s.pins[0].position.y() - 100 * (k - 1);
            }
            CHECK(down_the_left);
        }
        // Two columns: odd on the left, even on the right, across.
        for (int n : {1, 2, 5, 20, 40})
        {
            const symbol &s = find_symbol(std::format("Conn_02x{:02}", n));
            CHECK(sane(s) && s.pins.size() == static_cast<std::size_t>(2 * n));
            const pin *one = s.find_pin("1");
            const pin *two = s.find_pin("2");
            CHECK(one && two && one->position.x() < 0 && two->position.x() > 0 &&
                  one->position.y() == two->position.y());
        }
        // Names that are not connectors after all, and anything else: a box
        // of eight pins with the name inside.
        for (const char *name : {"MCU", "OpAmp_Dual", "Regulator_Linear", "USB_C_Receptacle_USB2.0", "Conn_01x00",
                                 "Conn_01x", "Conn_01x1000", "Conn_03x04", "", "ünïcödé"})
        {
            const symbol &s = find_symbol(name);
            CHECK(s.name == name && sane(s) && s.name_inside && s.pins.size() == 8 && s.prefix == "U");
            for (int n = 1; n <= 8; ++n)
                CHECK(s.find_pin(std::to_string(n)) != nullptr);
            CHECK(!is_builtin_symbol(name));
        }
    }

    void lookups()
    {
        // The same symbol every time, at the same address.
        CHECK(&find_symbol("R") == &find_symbol("R"));
        const symbol *box = &find_symbol("Some_New_Thing");
        for (int i = 0; i < 100; ++i)
            find_symbol("Filler_" + std::to_string(i));
        CHECK(&find_symbol("Some_New_Thing") == box && box->name == "Some_New_Thing");

        // From several threads at once.
        std::vector<std::thread> threads;
        std::vector<const symbol *> seen(8, nullptr);
        for (std::size_t t = 0; t < seen.size(); ++t)
            threads.emplace_back([t, &seen] {
                for (int i = 0; i < 200; ++i)
                    find_symbol("Threaded_" + std::to_string(i));
                seen[t] = &find_symbol("Threaded_7");
            });
        for (std::thread &t : threads)
            t.join();
        CHECK(std::ranges::all_of(seen, [&](const symbol *s) { return s == seen[0] && s != nullptr; }));

        CHECK(is_ground_name("GND") && is_ground_name("agnd") && is_ground_name("GNDA") && is_ground_name("0") &&
              is_ground_name("VSS"));
        CHECK(!is_ground_name("+5V") && !is_ground_name("VCC") && !is_ground_name("3V3"));
        CHECK(&power_symbol("DGND") == &find_symbol("GND") && &power_symbol("+3V3") == &find_symbol("PWR"));
    }

    void placing()
    {
        // Four turns and two mirrors are nothing.
        const point p{300, -100};
        CHECK(orient(p, 0, false) == p);
        CHECK(orient(p, 1, false) == point{100, 300});
        CHECK(orient(p, 2, false) == point{-300, 100});
        CHECK(orient(p, 3, false) == point{-100, -300});
        CHECK(orient(p, 4, false) == p); // taken modulo 4
        CHECK(orient(p, 0, true) == point{-300, -100});
        CHECK(orient(p, 1, true) == point{100, -300}); // mirrored first, then turned

        // A part's pins on the sheet.
        const part r{"R", {500, 300}, 1, true, "R1", "1k", "resistor", {}, {}};
        CHECK(transform(r, {100, 0}) == point{500, 200});
        CHECK(transform(r, {-100, 0}) == point{500, 400});
        CHECK(transform_direction(r, {1, 0}) == point{0, -1});
        const std::vector<point> pins = pin_positions(r);
        CHECK(pins.size() == 2 && pins[0] == point{500, 400} && pins[1] == point{500, 200});
        const std::vector<placed_pin> placed = placed_pins(r);
        CHECK(placed.size() == 2 && placed[0].pin->number == "1" && placed[0].position == pins[0] &&
              placed[0].direction == point{0, -1});

        // On the grid in every orientation of every symbol, and inside the
        // part's bounds.
        for (const std::string_view name : builtin_symbol_names())
        {
            for (std::uint8_t rotation = 0; rotation < 4; ++rotation)
            {
                for (const bool mirrored : {false, true})
                {
                    const part q{std::string(name), {-700, 1200}, rotation, mirrored, "X1", "", "", {}, {}};
                    point min, max;
                    part_bounds(q, min, max);
                    bool ok = true;
                    for (const point pin_at : pin_positions(q))
                        ok = ok && on_grid(pin_at) && within(pin_at, min, max);
                    for (const auto &stroke : find_symbol(name).strokes)
                        for (const point local : stroke)
                            ok = ok && within(transform(q, local), min, max);
                    CHECK(ok);
                }
            }
        }
    }

} // namespace

int main()
{
    builtins();
    conventions();
    generated();
    lookups();
    placing();
    return volt::tests::result("schematic symbol");
}
