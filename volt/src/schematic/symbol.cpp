#include <schematic/symbol.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <numbers>
#include <string>

namespace volt::schematic
{

    namespace
    {
        // Builds a symbol: strokes and pins, then the bounds of both.
        class builder
        {
        public:
            builder(std::string_view name, std::string_view prefix, std::string_view device)
            {
                m_symbol.name = std::string(name);
                m_symbol.prefix = std::string(prefix);
                m_symbol.device = std::string(device);
            }

            // An open polyline; repeated points are dropped.
            void line(std::initializer_list<point> points) { add(std::vector<point>(points)); }

            void add(std::vector<point> points)
            {
                points.erase(std::unique(points.begin(), points.end()), points.end());
                if (points.size() >= 2)
                    m_symbol.strokes.push_back(std::move(points));
            }

            // A closed rectangle.
            void box(point min, point max)
            {
                line({min, point{max.x(), min.y()}, max, point{min.x(), max.y()}, min});
            }

            // An arc about `center`, counter-clockwise from `from` to `to`
            // degrees, as `segments` straight pieces, appended to `points`.
            static void arc(std::vector<point> &points, point center, int radius, double from, double to,
                            int segments)
            {
                for (int i = 0; i <= segments; ++i)
                {
                    const double degrees = from + (to - from) * static_cast<double>(i) / static_cast<double>(segments);
                    const double radians = degrees * std::numbers::pi / 180.0;
                    points.push_back(point{
                        center.x() + static_cast<int>(std::lround(static_cast<double>(radius) * std::cos(radians))),
                        center.y() + static_cast<int>(std::lround(static_cast<double>(radius) * std::sin(radians)))});
                }
            }

            void circle(point center, int radius, int segments = 24)
            {
                std::vector<point> points;
                arc(points, center, radius, 0.0, 360.0, segments);
                add(std::move(points));
            }

            // A pin, and the lead from it to the body: `length` units along
            // its direction (none for 0).
            void pin(std::string_view name, std::string_view number, point position, point direction, int length = 0)
            {
                m_symbol.pins.push_back({std::string(name), std::string(number), position, direction});
                if (length > 0)
                    line({position, position + direction * length});
            }

            void anchors(point reference, point value)
            {
                m_symbol.reference_anchor = reference;
                m_symbol.value_anchor = value;
            }

            void name_inside() { m_symbol.name_inside = true; }

            symbol finish()
            {
                bool first = true;
                const auto take = [&](point p) {
                    if (first)
                    {
                        m_symbol.min = m_symbol.max = p;
                        first = false;
                        return;
                    }
                    m_symbol.min = point{std::min(m_symbol.min.x(), p.x()), std::min(m_symbol.min.y(), p.y())};
                    m_symbol.max = point{std::max(m_symbol.max.x(), p.x()), std::max(m_symbol.max.y(), p.y())};
                };
                for (const std::vector<point> &stroke : m_symbol.strokes)
                    for (const point p : stroke)
                        take(p);
                for (const schematic::pin &p : m_symbol.pins)
                    take(p.position);
                return std::move(m_symbol);
            }

        private:
            symbol m_symbol;
        };

        constexpr point left{-1, 0};
        constexpr point right{1, 0};
        constexpr point up{0, 1};
        constexpr point down{0, -1};

        // ---- two-terminal parts, horizontal, 200 between pins ---------------------

        // Pins at -100 and +100 on the x axis, numbered 1 and 2, with
        // leads `lead` long from each.
        void two_pins(builder &b, int lead, std::string_view name1 = {}, std::string_view name2 = {})
        {
            b.pin(name1, "1", {-100, 0}, right, lead);
            b.pin(name2, "2", {100, 0}, left, lead);
            b.anchors({0, 75}, {0, -75});
        }

        symbol resistor()
        {
            builder b("R", "R", "resistor");
            two_pins(b, 40);
            b.line({{-60, 0}, {-50, 25}, {-30, -25}, {-10, 25}, {10, -25}, {30, 25}, {50, -25}, {60, 0}});
            return b.finish();
        }

        symbol capacitor(bool polarized)
        {
            builder b(polarized ? "C_Polarized" : "C", "C", "capacitor");
            if (polarized)
                two_pins(b, 85, "+", "-");
            else
                two_pins(b, 85);
            b.line({{-15, -40}, {-15, 40}});
            if (polarized)
            {
                // The negative plate curved, and a plus by the positive one.
                b.line({{25, -40}, {18, -20}, {15, 0}, {18, 20}, {25, 40}});
                b.line({{-50, 35}, {-30, 35}});
                b.line({{-40, 25}, {-40, 45}});
            }
            else
                b.line({{15, -40}, {15, 40}});
            return b.finish();
        }

        symbol inductor()
        {
            builder b("L", "L", "inductor");
            two_pins(b, 40);
            std::vector<point> coil;
            for (int k = 0; k < 4; ++k)
                builder::arc(coil, {-45 + 30 * k, 0}, 15, 180.0, 0.0, 6);
            b.add(std::move(coil));
            return b.finish();
        }

        enum class diode_kind
        {
            plain,
            zener,
            schottky,
            led,
        };

        // The cathode (pin 1, "K") on the left, the anode (pin 2, "A") on
        // the right, as KiCad numbers them: the triangle points left, at
        // the bar.
        symbol diode(diode_kind kind)
        {
            const std::string_view name = kind == diode_kind::zener      ? "D_Zener"
                                          : kind == diode_kind::schottky ? "D_Schottky"
                                          : kind == diode_kind::led      ? "LED"
                                                                         : "D";
            const std::string_view device = kind == diode_kind::zener ? "zener" : kind == diode_kind::led ? "led" : "diode";
            builder b(name, "D", device);
            b.pin("K", "1", {-100, 0}, right, 75);
            b.pin("A", "2", {100, 0}, left, 75);
            b.line({{25, -30}, {25, 30}, {-25, 0}, {25, -30}});
            switch (kind)
            {
            case diode_kind::zener:
                b.line({{-35, -40}, {-25, -30}, {-25, 30}, {-15, 40}});
                break;
            case diode_kind::schottky:
                b.line({{-15, -25}, {-15, -30}, {-25, -30}, {-25, 30}, {-35, 30}, {-35, 25}});
                break;
            default:
                b.line({{-25, -30}, {-25, 30}});
                break;
            }
            if (kind == diode_kind::led)
            {
                // Light going out, up and to the right.
                for (const int x : {-10, 15})
                {
                    b.line({{x, 40}, {x + 25, 65}});
                    b.line({{x + 13, 65}, {x + 25, 65}, {x + 25, 53}});
                }
                b.anchors({0, 100}, {0, -75});
            }
            else
                b.anchors({0, 75}, {0, -75});
            return b.finish();
        }

        // ---- transistors: base or gate on the left, 200 out ------------------------

        symbol bipolar(bool npn)
        {
            builder b(npn ? "Q_NPN" : "Q_PNP", "Q", "bjt");
            b.pin("B", "1", {-200, 0}, right, 225);
            b.line({{25, -75}, {25, 75}});
            // The collector up and the emitter down for NPN; the other way
            // round for PNP, whose emitter goes to the supply.
            const int c = npn ? 1 : -1;
            b.pin("C", "2", {100, 200 * c}, point{0, -c}, 100);
            b.pin("E", "3", {100, -200 * c}, point{0, c}, 100);
            b.line({{25, 25 * c}, {100, 100 * c}});
            b.line({{25, -25 * c}, {100, -100 * c}});
            if (npn) // the arrow out of the emitter
                b.line({{78, -61}, {90, -90}, {61, -78}, {78, -61}});
            else // into the base
                b.line({{70, 53}, {40, 40}, {53, 70}, {70, 53}});
            b.circle({50, 0}, 110);
            b.anchors({250, 50}, {250, -50});
            return b.finish();
        }

        symbol mosfet(bool n_channel)
        {
            builder b(n_channel ? "Q_NMOS" : "Q_PMOS", "Q", "mosfet");
            b.pin("G", "1", {-200, 0}, right, 190);
            b.line({{-10, -60}, {-10, 60}});
            // The channel, broken in three.
            b.line({{20, 45}, {20, 75}});
            b.line({{20, -15}, {20, 15}});
            b.line({{20, -75}, {20, -45}});
            // The drain up and the source down for N; the other way round
            // for P. The body is tied to the source.
            const int d = n_channel ? 1 : -1;
            b.pin("D", "2", {100, 200 * d}, point{0, -d}, 0);
            b.pin("S", "3", {100, -200 * d}, point{0, d}, 0);
            b.line({{20, 60 * d}, {100, 60 * d}, {100, 200 * d}});
            b.line({{20, -60 * d}, {100, -60 * d}, {100, -200 * d}});
            b.line({{20, 0}, {100, 0}, {100, -60 * d}});
            if (n_channel) // the arrow into the channel
                b.line({{55, 12}, {25, 0}, {55, -12}, {55, 12}});
            else // out of it
                b.line({{45, 12}, {75, 0}, {45, -12}, {45, 12}});
            b.circle({50, 0}, 110);
            b.anchors({250, 50}, {250, -50});
            return b.finish();
        }

        // ---- the rest ------------------------------------------------------------------

        // A single op amp, pins numbered as an eight-pin package numbers
        // them (2 -, 3 +, 4 V-, 6 out, 7 V+).
        symbol opamp()
        {
            builder b("OpAmp", "U", "");
            b.line({{-200, 200}, {200, 0}, {-200, -200}, {-200, 200}});
            b.pin("+", "3", {-300, 100}, right, 100);
            b.pin("-", "2", {-300, -100}, right, 100);
            b.pin("OUT", "6", {300, 0}, left, 100);
            b.pin("V+", "7", {0, 300}, down, 200);
            b.pin("V-", "4", {0, -300}, up, 200);
            b.line({{-175, 100}, {-135, 100}});
            b.line({{-155, 80}, {-155, 120}});
            b.line({{-175, -100}, {-135, -100}});
            b.anchors({200, 200}, {200, -200});
            return b.finish();
        }

        // A source, vertical, 400 between pins: pin 1 at the top.
        symbol source(bool voltage)
        {
            builder b(voltage ? "V_DC" : "I_DC", voltage ? "V" : "I", voltage ? "voltage_source" : "current_source");
            b.pin(voltage ? "+" : "", "1", {0, 200}, down, 120);
            b.pin(voltage ? "-" : "", "2", {0, -200}, up, 120);
            b.circle({0, 0}, 80);
            if (voltage)
            {
                b.line({{0, 25}, {0, 55}});
                b.line({{-15, 40}, {15, 40}});
                b.line({{-15, -40}, {15, -40}});
            }
            else
            {
                // The way the current goes, through the source to pin 1.
                b.line({{0, -45}, {0, 45}});
                b.line({{-15, 25}, {0, 45}, {15, 25}});
            }
            b.anchors({150, 40}, {150, -40});
            return b.finish();
        }

        // Power glyphs: one pin at the origin, where the net is.
        symbol ground()
        {
            builder b("GND", "#PWR", "");
            b.pin("", "1", {0, 0}, down, 50);
            b.line({{0, -50}, {50, -50}, {0, -100}, {-50, -50}, {0, -50}});
            b.anchors({0, -200}, {0, -150});
            return b.finish();
        }

        symbol supply()
        {
            builder b("PWR", "#PWR", "");
            b.pin("", "1", {0, 0}, up, 50);
            b.line({{-30, 20}, {0, 50}, {30, 20}});
            b.anchors({0, 150}, {0, 100});
            return b.finish();
        }

        symbol timer_555()
        {
            builder b("Timer_555", "U", "");
            b.box({-200, -300}, {200, 300});
            b.pin("R", "4", {-300, 200}, right, 100);
            b.pin("TR", "2", {-300, 0}, right, 100);
            b.pin("THR", "6", {-300, -200}, right, 100);
            b.pin("Q", "3", {300, 200}, left, 100);
            b.pin("DIS", "7", {300, 0}, left, 100);
            b.pin("CV", "5", {300, -200}, left, 100);
            b.pin("VCC", "8", {0, 400}, down, 100);
            b.pin("GND", "1", {0, -400}, up, 100);
            b.anchors({250, 350}, {250, -350});
            b.name_inside();
            return b.finish();
        }

        // ---- generated -------------------------------------------------------------------

        // NN of "Conn_01xNN" / "Conn_02xNN": 1 to 100, or 0 for a name of
        // another form.
        int connector_rows(std::string_view name, std::string_view prefix)
        {
            if (!name.starts_with(prefix))
                return 0;
            const std::string_view digits = name.substr(prefix.size());
            if (digits.empty() || digits.size() > 3)
                return 0;
            int rows = 0;
            for (const char c : digits)
            {
                if (c < '0' || c > '9')
                    return 0;
                rows = rows * 10 + (c - '0');
            }
            return rows >= 1 && rows <= 100 ? rows : 0;
        }

        // A connector of one or two columns, pins on a grid of 100, pin 1
        // at the top left; two columns are numbered across (odd on the
        // left, even on the right), as headers are.
        symbol connector(std::string_view name, int columns, int rows)
        {
            builder b(name, "J", "");
            const int top = 100 * ((rows - 1) / 2);
            const int bottom = top - 100 * (rows - 1);
            const int half_width = columns == 1 ? 50 : 100;
            b.box({columns == 1 ? -50 : -100, bottom - 50}, {half_width, top + 50});
            for (int row = 0; row < rows; ++row)
            {
                const int y = top - 100 * row;
                if (columns == 1)
                {
                    const std::string number = std::to_string(row + 1);
                    b.pin("Pin_" + number, number, {-200, y}, right, 150);
                }
                else
                {
                    const std::string odd = std::to_string(2 * row + 1);
                    const std::string even = std::to_string(2 * row + 2);
                    b.pin("Pin_" + odd, odd, {-200, y}, right, 100);
                    b.pin("Pin_" + even, even, {200, y}, left, 100);
                }
            }
            b.anchors({0, top + 100}, {0, bottom - 100});
            return b.finish();
        }

        // Any other name: a box, four pins down the left (1 to 4) and four
        // up the right (5 to 8), as an eight-pin package goes round.
        symbol unknown(std::string_view name)
        {
            builder b(name, "U", "");
            b.box({-200, -400}, {200, 400});
            for (int k = 0; k < 4; ++k)
            {
                b.pin("", std::to_string(k + 1), {-300, 300 - 200 * k}, right, 100);
            }
            for (int k = 0; k < 4; ++k)
            {
                b.pin("", std::to_string(k + 5), {300, -300 + 200 * k}, left, 100);
            }
            b.anchors({0, 500}, {0, -500});
            b.name_inside();
            return b.finish();
        }

        constexpr std::array<std::string_view, 18> builtin_names = {
            "R",      "C",      "C_Polarized", "L",    "D",    "D_Zener", "D_Schottky", "LED", "Q_NPN",
            "Q_PNP",  "Q_NMOS", "Q_PMOS",      "OpAmp", "V_DC", "I_DC",   "GND",        "PWR", "Timer_555",
        };

        symbol make_symbol(std::string_view name)
        {
            if (name == "R")
                return resistor();
            if (name == "C")
                return capacitor(false);
            if (name == "C_Polarized")
                return capacitor(true);
            if (name == "L")
                return inductor();
            if (name == "D")
                return diode(diode_kind::plain);
            if (name == "D_Zener")
                return diode(diode_kind::zener);
            if (name == "D_Schottky")
                return diode(diode_kind::schottky);
            if (name == "LED")
                return diode(diode_kind::led);
            if (name == "Q_NPN")
                return bipolar(true);
            if (name == "Q_PNP")
                return bipolar(false);
            if (name == "Q_NMOS")
                return mosfet(true);
            if (name == "Q_PMOS")
                return mosfet(false);
            if (name == "OpAmp")
                return opamp();
            if (name == "V_DC")
                return source(true);
            if (name == "I_DC")
                return source(false);
            if (name == "GND")
                return ground();
            if (name == "PWR")
                return supply();
            if (name == "Timer_555")
                return timer_555();
            if (const int rows = connector_rows(name, "Conn_01x"))
                return connector(name, 1, rows);
            if (const int rows = connector_rows(name, "Conn_02x"))
                return connector(name, 2, rows);
            return unknown(name);
        }

        // Every symbol asked for, made once. A map's nodes do not move, so
        // the references handed out stay valid.
        struct symbol_cache
        {
            std::mutex mutex;
            std::map<std::string, symbol, std::less<>> symbols;
        };

        symbol_cache &cache()
        {
            static symbol_cache instance;
            return instance;
        }
    } // namespace

    const pin *symbol::find_pin(std::string_view number) const noexcept
    {
        for (const pin &p : pins)
            if (p.number == number)
                return &p;
        return nullptr;
    }

    const symbol &find_symbol(std::string_view name)
    {
        symbol_cache &c = cache();
        const std::lock_guard lock(c.mutex);
        if (const auto it = c.symbols.find(name); it != c.symbols.end())
            return it->second;
        return c.symbols.emplace(std::string(name), make_symbol(name)).first->second;
    }

    bool is_builtin_symbol(std::string_view name) noexcept
    {
        return std::ranges::find(builtin_names, name) != builtin_names.end();
    }

    std::span<const std::string_view> builtin_symbol_names() noexcept
    {
        return builtin_names;
    }

    bool is_ground_name(std::string_view text) noexcept
    {
        std::string upper;
        for (const char c : text)
            upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        // "GND" with something before or after it (AGND, GND_ISO), not
        // anywhere inside a name (PGND_SENSE is a signal).
        return upper == "0" || upper == "VSS" || upper == "EARTH" || upper == "CHASSIS" ||
               upper.starts_with("GND") || upper.ends_with("GND");
    }

    const symbol &power_symbol(std::string_view text)
    {
        return find_symbol(is_ground_name(text) ? "GND" : "PWR");
    }

    // ---- placing -----------------------------------------------------------------------

    point orient(point local, std::uint8_t rotation, bool mirrored) noexcept
    {
        int x = mirrored ? -local.x() : local.x();
        int y = local.y();
        for (int turn = 0; turn < (rotation & 3); ++turn)
        {
            const int turned = -y;
            y = x;
            x = turned;
        }
        return point{x, y};
    }

    point transform(const part &p, point local) noexcept
    {
        return orient(local, p.rotation, p.mirrored) + p.position;
    }

    point transform_direction(const part &p, point local) noexcept
    {
        return orient(local, p.rotation, p.mirrored);
    }

    std::vector<placed_pin> placed_pins(const part &p)
    {
        const symbol &s = find_symbol(p.symbol);
        std::vector<placed_pin> pins;
        pins.reserve(s.pins.size());
        for (const pin &each : s.pins)
            pins.push_back({&each, transform(p, each.position), transform_direction(p, each.direction)});
        return pins;
    }

    std::vector<point> pin_positions(const part &p)
    {
        const symbol &s = find_symbol(p.symbol);
        std::vector<point> positions;
        positions.reserve(s.pins.size());
        for (const pin &each : s.pins)
            positions.push_back(transform(p, each.position));
        return positions;
    }

    void part_bounds(const part &p, point &min, point &max)
    {
        const symbol &s = find_symbol(p.symbol);
        const point corners[] = {s.min, point{s.max.x(), s.min.y()}, s.max, point{s.min.x(), s.max.y()}};
        min = max = transform(p, corners[0]);
        for (const point corner : corners)
        {
            const point q = transform(p, corner);
            min = point{std::min(min.x(), q.x()), std::min(min.y(), q.y())};
            max = point{std::max(max.x(), q.x()), std::max(max.y(), q.y())};
        }
    }

} // namespace volt::schematic
