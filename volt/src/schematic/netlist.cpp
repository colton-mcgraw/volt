#include <schematic/netlist.hpp>

#include <schematic/symbol.hpp>

#include <math/electrical/engineering.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <initializer_list>
#include <limits>
#include <map>
#include <numeric>
#include <set>

namespace volt::schematic
{

    namespace
    {
        // Union-find, by size, with path halving.
        class disjoint_sets
        {
        public:
            explicit disjoint_sets(std::size_t count) : m_parent(count), m_size(count, 1)
            {
                std::iota(m_parent.begin(), m_parent.end(), std::uint32_t{0});
            }

            std::uint32_t find(std::uint32_t i)
            {
                while (m_parent[i] != i)
                {
                    m_parent[i] = m_parent[m_parent[i]];
                    i = m_parent[i];
                }
                return i;
            }

            void join(std::uint32_t a, std::uint32_t b)
            {
                a = find(a);
                b = find(b);
                if (a == b)
                    return;
                if (m_size[a] < m_size[b])
                    std::swap(a, b);
                m_parent[b] = a;
                m_size[a] += m_size[b];
            }

        private:
            std::vector<std::uint32_t> m_parent;
            std::vector<std::uint32_t> m_size;
        };

        std::uint64_t point_key(point p) noexcept
        {
            return static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.x())) << 32 |
                   static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.y()));
        }

        // An item and a place within it, for ordering nets by their smallest
        // member: (id, 0) for a wire or label, (id, pin index) for a pin.
        std::uint64_t member_key(item_id id, std::uint32_t sub) noexcept
        {
            return static_cast<std::uint64_t>(id) << 32 | sub;
        }

        constexpr std::uint64_t no_key = std::numeric_limits<std::uint64_t>::max();
        constexpr std::uint32_t no_net = std::numeric_limits<std::uint32_t>::max();

        // The direction of a wire, reduced by the gcd and pointing right (or
        // up); false for a wire of no length.
        bool direction_of(const wire &w, std::int64_t &dx, std::int64_t &dy) noexcept
        {
            dx = static_cast<std::int64_t>(w.b.x()) - w.a.x();
            dy = static_cast<std::int64_t>(w.b.y()) - w.a.y();
            if (dx == 0 && dy == 0)
                return false;
            const std::int64_t g = std::gcd(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
            dx /= g;
            dy /= g;
            if (dx < 0 || (dx == 0 && dy < 0))
            {
                dx = -dx;
                dy = -dy;
            }
            return true;
        }

        std::int64_t offset_of(std::int64_t dx, std::int64_t dy, point p) noexcept
        {
            return dx * p.y() - dy * p.x();
        }

        std::int64_t along(std::int64_t dx, std::int64_t dy, point p) noexcept
        {
            return dx * p.x() + dy * p.y();
        }

        // What a part is called in a message: its reference, or its
        // symbol and a question mark when it has none.
        std::string part_name(const part &p)
        {
            if (!p.reference.empty())
                return p.reference;
            return p.symbol + "?";
        }

        // "A", "A and B", "A, B and C"; past `most`, "A, B, C and 4 more".
        std::string join_names(const std::vector<std::string> &names, std::size_t most = 6)
        {
            std::string text;
            const std::size_t shown = names.size() > most ? most - 1 : names.size();
            for (std::size_t i = 0; i < shown; ++i)
            {
                if (i > 0)
                    text += i + 1 == shown && shown == names.size() ? " and " : ", ";
                text += names[i];
            }
            if (shown < names.size())
                text += std::format(" and {} more", names.size() - shown);
            return text;
        }

        std::string plural(std::size_t count, std::string_view one, std::string_view many)
        {
            return std::format("{} {}", count, count == 1 ? one : many);
        }

        std::string volts(double value)
        {
            math::electrical::engineering_format format;
            format.unit = "V";
            return math::electrical::format_engineering(value, format);
        }

        // The part's value as a number, in engineering notation, with one
        // of these units after it or none.
        std::optional<double> parse_value(std::string_view text, std::initializer_list<std::string_view> units)
        {
            const auto parsed = math::electrical::parse_engineering(text);
            if (!parsed || !std::isfinite(parsed->value))
                return std::nullopt;
            if (!parsed->unit.empty() && std::ranges::find(units, parsed->unit) == units.end())
                return std::nullopt;
            return parsed->value;
        }
    } // namespace

    std::string_view to_string(severity level) noexcept
    {
        switch (level)
        {
        case severity::info:
            return "info";
        case severity::warning:
            return "warning";
        case severity::error:
            return "error";
        }
        return "info";
    }

    // ---- lookups ---------------------------------------------------------------------

    void netlist::runs_at(point p, std::vector<std::uint32_t> &out) const
    {
        for (const direction &d : m_directions)
        {
            const auto line = d.lines.find(offset_of(d.dx, d.dy, p));
            if (line == d.lines.end())
                continue;
            const std::int64_t t = along(d.dx, d.dy, p);
            // The last run that starts at or before t; the runs of a line
            // do not overlap.
            const auto after =
                std::ranges::upper_bound(line->second, t, std::less<>{}, [](const run &r) { return r.from; });
            if (after == line->second.begin())
                continue;
            const run &r = *(after - 1);
            if (t <= r.to)
                out.push_back(r.net);
        }
    }

    std::optional<std::size_t> netlist::net_of_wire(item_id wire) const
    {
        const auto it = m_wire_net.find(wire);
        return it == m_wire_net.end() ? std::nullopt : std::optional<std::size_t>(it->second);
    }

    std::optional<std::size_t> netlist::net_of_pin(item_id part, std::string_view number) const
    {
        const auto it = m_pin_net.find(part);
        if (it == m_pin_net.end())
            return std::nullopt;
        for (const auto &[pin_number, index] : it->second)
            if (pin_number == number)
                return index;
        return std::nullopt;
    }

    std::optional<std::size_t> netlist::net_of_label(item_id label) const
    {
        const auto it = m_label_net.find(label);
        return it == m_label_net.end() ? std::nullopt : std::optional<std::size_t>(it->second);
    }

    std::optional<std::size_t> netlist::net_at(point p) const
    {
        if (const auto it = m_point_net.find(point_key(p)); it != m_point_net.end())
            return it->second;
        std::vector<std::uint32_t> hits;
        runs_at(p, hits);
        if (hits.empty())
            return std::nullopt;
        return hits.front();
    }

    std::optional<std::size_t> netlist::find(std::string_view name) const
    {
        const auto it = m_name_net.find(std::string(name));
        return it == m_name_net.end() ? std::nullopt : std::optional<std::size_t>(it->second);
    }

    // ---- building the netlist ----------------------------------------------------------

    netlist build_netlist(const sheet &s, const connectivity &rules)
    {
        netlist result;

        // The elements joined: wires, then pins, then net labels and power
        // ports, then one per distinct name.
        struct pin_entry
        {
            item_id part = 0;
            std::uint32_t index = 0; // in the symbol
            const pin *symbol_pin = nullptr;
            point position{};
        };
        std::vector<pin_entry> pins;
        for (const auto &e : s.parts)
        {
            const std::vector<placed_pin> placed = placed_pins(e.record);
            for (std::size_t i = 0; i < placed.size(); ++i)
                pins.push_back({e.id, static_cast<std::uint32_t>(i), placed[i].pin, placed[i].position});
        }

        std::vector<const history::table<item_id, label>::entry *> labels;
        for (const auto &e : s.labels)
            if (e.record.kind != label_kind::note)
                labels.push_back(&e);

        std::map<std::string, std::uint32_t, std::less<>> name_index;
        for (const auto *e : labels)
            if (!e->record.text.empty())
                name_index.try_emplace(e->record.text, static_cast<std::uint32_t>(name_index.size()));

        const auto wire_count = static_cast<std::uint32_t>(s.wires.size());
        const auto first_pin = wire_count;
        const auto first_label = first_pin + static_cast<std::uint32_t>(pins.size());
        const auto first_name = first_label + static_cast<std::uint32_t>(labels.size());
        const auto total = first_name + static_cast<std::uint32_t>(name_index.size());
        disjoint_sets sets(total);

        // ---- points: wire ends, pins and labels at one point are joined

        struct point_entry
        {
            std::uint32_t element = 0;
            bool solid = false; // a wire end or a pin is there
        };
        std::unordered_map<std::uint64_t, point_entry> points;
        points.reserve(2 * s.wires.size() + pins.size() + labels.size());

        // Returns whether a wire end or pin was there already.
        const auto at_point = [&](point p, std::uint32_t element, bool solid) {
            const auto [it, added] = points.try_emplace(point_key(p), point_entry{element, solid});
            if (added)
                return false;
            sets.join(it->second.element, element);
            const bool touched = it->second.solid;
            it->second.solid = touched || solid;
            return touched;
        };

        for (std::uint32_t i = 0; i < wire_count; ++i)
        {
            at_point(s.wires[i].record.a, i, true);
            at_point(s.wires[i].record.b, i, true);
        }
        for (std::uint32_t k = 0; k < pins.size(); ++k)
            at_point(pins[k].position, first_pin + k, true);
        std::vector<char> touched(labels.size(), 0);
        for (std::uint32_t k = 0; k < labels.size(); ++k)
            touched[k] = at_point(labels[k]->record.position, first_label + k, false) ? 1 : 0;

        // ---- lines: the wires on each, merged into runs where they overlap

        for (std::uint32_t i = 0; i < wire_count; ++i)
        {
            const wire &w = s.wires[i].record;
            std::int64_t dx = 0;
            std::int64_t dy = 0;
            if (!direction_of(w, dx, dy))
                continue;
            auto d = std::ranges::find_if(result.m_directions,
                                          [&](const netlist::direction &each) { return each.dx == dx && each.dy == dy; });
            if (d == result.m_directions.end())
            {
                result.m_directions.push_back({dx, dy, {}});
                d = result.m_directions.end() - 1;
            }
            const std::int64_t ta = along(dx, dy, w.a);
            const std::int64_t tb = along(dx, dy, w.b);
            // `net` holds the wire's element until the nets are known.
            d->lines[offset_of(dx, dy, w.a)].push_back({std::min(ta, tb), std::max(ta, tb), i});
        }
        for (netlist::direction &d : result.m_directions)
        {
            for (auto &[offset, runs] : d.lines)
            {
                std::ranges::sort(runs, [](const netlist::run &x, const netlist::run &y) {
                    return x.from != y.from ? x.from < y.from : x.net < y.net;
                });
                // Wires that overlap, or meet end to end, are connected:
                // merge them into one run of the first one's element.
                std::size_t kept = 0;
                for (std::size_t i = 1; i < runs.size(); ++i)
                {
                    netlist::run &current = runs[kept];
                    if (runs[i].from <= current.to)
                    {
                        sets.join(current.net, runs[i].net);
                        current.to = std::max(current.to, runs[i].to);
                    }
                    else
                        runs[++kept] = runs[i];
                }
                runs.resize(kept + 1);
            }
        }

        // ---- points inside wires: T junctions, pins and labels on a wire

        std::vector<std::uint32_t> hits;
        const auto join_inside = [&](point p, std::uint32_t element) {
            hits.clear();
            result.runs_at(p, hits);
            for (const std::uint32_t wire_element : hits)
                sets.join(wire_element, element);
            return !hits.empty();
        };
        for (std::uint32_t i = 0; i < wire_count; ++i)
        {
            join_inside(s.wires[i].record.a, i);
            join_inside(s.wires[i].record.b, i);
        }
        if (rules.pins_inside_wires)
            for (std::uint32_t k = 0; k < pins.size(); ++k)
                join_inside(pins[k].position, first_pin + k);
        for (std::uint32_t k = 0; k < labels.size(); ++k)
            if (join_inside(labels[k]->record.position, first_label + k))
                touched[k] = 1;

        // ---- names: labels of one text are one net

        for (std::uint32_t k = 0; k < labels.size(); ++k)
            if (!labels[k]->record.text.empty())
                sets.join(first_label + k, first_name + name_index.find(labels[k]->record.text)->second);

        // ---- the nets, ordered by their smallest member

        const auto key_of = [&](std::uint32_t element) {
            if (element < first_pin)
                return member_key(s.wires[element].id, 0);
            if (element < first_label)
                return member_key(pins[element - first_pin].part, pins[element - first_pin].index);
            return member_key(labels[element - first_label]->id, 0);
        };
        std::vector<std::uint64_t> smallest(total, no_key);
        for (std::uint32_t e = 0; e < first_name; ++e)
        {
            const std::uint32_t root = sets.find(e);
            smallest[root] = std::min(smallest[root], key_of(e));
        }
        std::vector<std::pair<std::uint64_t, std::uint32_t>> roots;
        for (std::uint32_t r = 0; r < total; ++r)
            if (smallest[r] != no_key)
                roots.emplace_back(smallest[r], r);
        std::ranges::sort(roots);

        std::vector<std::uint32_t> net_of_root(total, no_net);
        result.nets.resize(roots.size());
        for (std::size_t n = 0; n < roots.size(); ++n)
            net_of_root[roots[n].second] = static_cast<std::uint32_t>(n);
        const auto net_of = [&](std::uint32_t element) { return net_of_root[sets.find(element)]; };

        // Members, then sorted.
        std::vector<std::vector<const pin_entry *>> net_pins(result.nets.size());
        std::vector<std::vector<std::uint32_t>> net_labels(result.nets.size()); // label elements' k
        for (std::uint32_t i = 0; i < wire_count; ++i)
        {
            const std::uint32_t n = net_of(i);
            result.nets[n].wires.push_back(s.wires[i].id);
            result.m_wire_net.emplace(s.wires[i].id, n);
        }
        for (std::uint32_t k = 0; k < pins.size(); ++k)
        {
            const std::uint32_t n = net_of(first_pin + k);
            net_pins[n].push_back(&pins[k]);
            result.m_pin_net[pins[k].part].emplace_back(pins[k].symbol_pin->number, n);
        }
        for (std::uint32_t k = 0; k < labels.size(); ++k)
        {
            const std::uint32_t n = net_of(first_label + k);
            net_labels[n].push_back(k);
            result.m_label_net.emplace(labels[k]->id, n);
        }
        for (const auto &[key, entry] : points)
            result.m_point_net.emplace(key, net_of(entry.element));
        for (netlist::direction &d : result.m_directions)
            for (auto &[offset, runs] : d.lines)
                for (netlist::run &r : runs)
                    r.net = net_of(r.net);

        for (std::size_t n = 0; n < result.nets.size(); ++n)
        {
            net &each = result.nets[n];
            std::ranges::sort(each.wires);
            std::ranges::sort(net_pins[n], [](const pin_entry *a, const pin_entry *b) {
                return a->part != b->part ? a->part < b->part : a->index < b->index;
            });
            for (const pin_entry *p : net_pins[n])
                each.pins.push_back({p->part, p->symbol_pin->number});
            std::ranges::sort(net_labels[n],
                              [&](std::uint32_t a, std::uint32_t b) { return labels[a]->id < labels[b]->id; });
            for (const std::uint32_t k : net_labels[n])
                each.labels.push_back(labels[k]->id);
        }

        // ---- names, and the problems of names

        std::set<std::string, std::less<>> taken;
        for (const auto &[text, index] : name_index)
            taken.insert(text);

        std::vector<char> global(result.nets.size(), 0); // named by a power port
        std::vector<std::vector<std::string>> names_of(result.nets.size());
        for (std::size_t n = 0; n < result.nets.size(); ++n)
        {
            net &each = result.nets[n];
            const std::string *power = nullptr;
            const std::string *local = nullptr;
            std::set<std::string> distinct;
            for (const std::uint32_t k : net_labels[n])
            {
                const label &l = labels[k]->record;
                if (l.text.empty())
                    continue;
                distinct.insert(l.text);
                const std::string *&best = l.kind == label_kind::power ? power : local;
                if (best == nullptr || l.text < *best)
                    best = &l.text;
            }
            const std::string *chosen = power != nullptr ? power : local;
            if (chosen != nullptr)
            {
                each.name = *chosen;
                each.named = true;
            }
            global[n] = power != nullptr ? 1 : 0;
            each.ground = std::ranges::any_of(distinct, [](const std::string &t) { return is_ground_name(t); });
            names_of[n].assign(distinct.begin(), distinct.end());
        }

        for (std::size_t n = 0; n < result.nets.size(); ++n)
        {
            net &each = result.nets[n];
            if (each.named)
                continue;
            std::string base;
            if (!net_pins[n].empty())
            {
                const pin_entry &first = *net_pins[n].front();
                base = std::format("Net-({}-{})", part_name(*s.parts.find(first.part)), first.symbol_pin->number);
            }
            else
                base = std::format("N{}", roots[n].first >> 32);
            std::string name = base;
            for (int suffix = 2; taken.contains(name); ++suffix)
                name = std::format("{}_{}", base, suffix);
            taken.insert(name);
            each.name = std::move(name);
        }
        for (std::size_t n = 0; n < result.nets.size(); ++n)
            result.m_name_net.emplace(result.nets[n].name, static_cast<std::uint32_t>(n));

        // ---- problems: per net, then per label

        const auto describe_pin = [&](const pin_ref &p) {
            return std::format("{} pin {}", part_name(*s.parts.find(p.part)), p.number);
        };
        for (std::size_t n = 0; n < result.nets.size(); ++n)
        {
            const net &each = result.nets[n];
            if (names_of[n].size() > 1)
            {
                problem p{problem_kind::conflicting_names, severity::warning, {}, {}};
                for (const item_id id : each.labels)
                    p.items.push_back({item_kind::label, id});
                p.message = std::format("One net has the names {}; it is called {}.", join_names(names_of[n], 8),
                                        each.name);
                result.problems.push_back(std::move(p));
            }
            if (each.pins.size() != 1)
                continue;
            if (each.wires.empty() && each.labels.empty())
            {
                result.problems.push_back({problem_kind::unconnected_pin,
                                           severity::warning,
                                           {{item_kind::part, each.pins.front().part}},
                                           std::format("{} is not connected.", describe_pin(each.pins.front()))});
            }
            else if (global[n] == 0)
            {
                problem p{problem_kind::single_pin_net, severity::warning, {{item_kind::part, each.pins.front().part}}, {}};
                for (const item_id id : each.wires)
                    p.items.push_back({item_kind::wire, id});
                for (const item_id id : each.labels)
                    p.items.push_back({item_kind::label, id});
                p.message = std::format("{} is the only pin on net {}.", describe_pin(each.pins.front()), each.name);
                result.problems.push_back(std::move(p));
            }
        }

        std::vector<std::uint32_t> by_id(labels.size());
        std::iota(by_id.begin(), by_id.end(), std::uint32_t{0});
        std::ranges::sort(by_id, [&](std::uint32_t a, std::uint32_t b) { return labels[a]->id < labels[b]->id; });
        for (const std::uint32_t k : by_id)
        {
            if (touched[k] != 0)
                continue;
            const label &l = labels[k]->record;
            const std::string what = l.kind == label_kind::power ? "Power port" : "Net label";
            result.problems.push_back({problem_kind::label_unconnected,
                                       severity::warning,
                                       {{item_kind::label, labels[k]->id}},
                                       l.text.empty() ? std::format("A {} with no text touches no wire or pin.",
                                                                    l.kind == label_kind::power ? "power port"
                                                                                                : "net label")
                                                      : std::format("{} {} touches no wire or pin.", what, l.text)});
        }

        return result;
    }

    // ---- the circuit ---------------------------------------------------------------------

    circuit build_circuit(const sheet &s, const netlist &nets)
    {
        circuit c;
        const std::size_t net_count = nets.nets.size();
        const auto has_node = [&](std::size_t n) { return !nets.nets[n].pins.empty() || !nets.nets[n].wires.empty(); };

        // ---- the devices, in part order

        enum class kind
        {
            resistor,
            voltage_source,
            current_source,
            inductor,
        };
        struct device
        {
            kind type = kind::resistor;
            item_id id = 0;
            std::string name;
            std::size_t a = 0; // the nets of pins 1 and 2
            std::size_t b = 0;
            double value = 0.0;
        };
        std::vector<device> devices;
        std::vector<std::pair<std::string, std::vector<item_id>>> left_out; // per device kind, first seen first
        std::vector<item_id> inductors;

        for (const auto &e : s.parts)
        {
            const part &p = e.record;
            const std::string &device_name = p.device.empty() ? find_symbol(p.symbol).device : p.device;
            kind type{};
            if (device_name == "resistor")
                type = kind::resistor;
            else if (device_name == "voltage_source")
                type = kind::voltage_source;
            else if (device_name == "current_source")
                type = kind::current_source;
            else if (device_name == "inductor")
                type = kind::inductor;
            else
            {
                if (device_name != "capacitor") // open at DC
                {
                    auto it = std::ranges::find(left_out, device_name, &std::pair<std::string, std::vector<item_id>>::first);
                    if (it == left_out.end())
                    {
                        left_out.emplace_back(device_name, std::vector<item_id>{});
                        it = left_out.end() - 1;
                    }
                    it->second.push_back(e.id);
                }
                continue;
            }

            device d{type, e.id, part_name(p), 0, 0, 0.0};
            const auto a = nets.net_of_pin(e.id, "1");
            const auto b = nets.net_of_pin(e.id, "2");
            if (!a || !b)
            {
                c.problems.push_back({problem_kind::missing_pin,
                                      severity::error,
                                      {{item_kind::part, e.id}},
                                      std::format("{} ({}) has no pin {}, so it is left out.", d.name, device_name,
                                                  a ? "2" : "1")});
                continue;
            }
            d.a = *a;
            d.b = *b;

            if (type == kind::inductor)
            {
                inductors.push_back(e.id);
                devices.push_back(std::move(d));
                continue;
            }

            // Ohms as the Greek capital omega, the ohm sign, the RKM R
            // ("100R") or spelled out.
            const std::optional<double> value =
                type == kind::resistor
                    ? parse_value(p.value, {"\xCE\xA9", "\xE2\x84\xA6", "R", "Ohm", "ohm", "Ohms", "ohms", "OHM"})
                : type == kind::voltage_source ? parse_value(p.value, {"V"})
                                               : parse_value(p.value, {"A"});
            std::string wrong;
            if (p.value.empty())
                wrong = std::format("{} has no value, so it is left out.", d.name);
            else if (!value)
                wrong = std::format("{}'s value \"{}\" is not a {}, so it is left out.", d.name, p.value,
                                    type == kind::resistor         ? "resistance"
                                    : type == kind::voltage_source ? "voltage"
                                                                   : "current");
            else if (type == kind::resistor && !(*value > 0.0))
                wrong = std::format("{}'s resistance must be more than zero, so it is left out.", d.name);
            if (!wrong.empty())
            {
                c.problems.push_back({problem_kind::bad_value, severity::error, {{item_kind::part, e.id}}, wrong});
                continue;
            }
            d.value = *value;
            devices.push_back(std::move(d));
        }

        // ---- nodes: the ground nets are one, and inductors short their two

        disjoint_sets groups(net_count);
        std::optional<std::uint32_t> ground;
        for (std::size_t n = 0; n < net_count; ++n)
        {
            if (!nets.nets[n].ground || !has_node(n))
                continue;
            if (ground)
                groups.join(*ground, static_cast<std::uint32_t>(n));
            else
                ground = static_cast<std::uint32_t>(n);
        }
        for (const device &d : devices)
            if (d.type == kind::inductor)
                groups.join(static_cast<std::uint32_t>(d.a), static_cast<std::uint32_t>(d.b));

        std::vector<std::int32_t> node_of_group(net_count, -1);
        std::int32_t node_count = 0;
        if (ground)
            node_of_group[groups.find(*ground)] = node_count++;
        c.node_of_net.assign(net_count, -1);
        for (std::size_t n = 0; n < net_count; ++n)
        {
            if (!has_node(n))
                continue;
            std::int32_t &node = node_of_group[groups.find(static_cast<std::uint32_t>(n))];
            if (node < 0)
                node = node_count++;
            c.node_of_net[n] = node;
        }
        const auto nodes = static_cast<std::size_t>(node_count);
        c.network.nodes = static_cast<std::uint32_t>(node_count);
        for (std::size_t n = 0; n < net_count; ++n)
            for (const item_id w : nets.nets[n].wires)
                c.wire_nodes.emplace(w, c.node_of_net[n]);

        const auto node_of = [&](std::size_t n) { return static_cast<std::uint32_t>(c.node_of_net[n]); };
        const auto net_name_of_node = [&](std::uint32_t node) -> const std::string & {
            for (std::size_t n = 0; n < net_count; ++n)
                if (c.node_of_net[n] == static_cast<std::int32_t>(node))
                    return nets.nets[n].name;
            static const std::string none;
            return none;
        };

        // ---- branches and currents; which nodes the circuit touches

        std::vector<char> simulated(nodes, 0);
        disjoint_sets circuits(nodes); // what is connected to what by any device
        std::size_t resistors = 0;
        std::size_t sources = 0;
        for (const device &d : devices)
        {
            const std::uint32_t a = node_of(d.a);
            const std::uint32_t b = node_of(d.b);
            if (d.type == kind::resistor)
            {
                ++resistors;
                simulated[a] = simulated[b] = 1;
                circuits.join(a, b);
                if (a != b)
                    c.network.branches.push_back({a, b, static_cast<float>(1.0 / d.value)});
            }
            else if (d.type == kind::current_source)
            {
                ++sources;
                simulated[a] = simulated[b] = 1;
                circuits.join(a, b);
                if (a != b)
                {
                    // Out of pin 1, into the circuit; back in at pin 2.
                    c.network.sources.push_back({a, static_cast<float>(d.value)});
                    c.network.sources.push_back({b, static_cast<float>(-d.value)});
                }
            }
            else if (d.type == kind::voltage_source)
                circuits.join(a, b);
        }

        // ---- holds: ground at 0, then through the voltage sources

        std::vector<std::optional<double>> held(nodes);
        std::vector<item_id> held_by(nodes, 0); // the source that set it; 0 for ground or a reference
        std::vector<char> anchored(nodes, 0);   // per circuit (its root): something in it is held
        const auto hold = [&](std::uint32_t node, double potential, item_id by) {
            held[node] = potential;
            held_by[node] = by;
            anchored[circuits.find(node)] = 1;
        };
        if (ground)
            hold(0, 0.0, 0);

        std::vector<const device *> pending;
        for (const device &d : devices)
        {
            if (d.type != kind::voltage_source)
                continue;
            if (node_of(d.a) == node_of(d.b))
            {
                if (d.value != 0.0)
                    c.problems.push_back(
                        {problem_kind::shorted_source,
                         severity::error,
                         {{item_kind::part, d.id}},
                         std::format("{} is shorted: both its terminals are on net {}, so it is left out.", d.name,
                                     net_name_of_node(node_of(d.a)))});
                continue;
            }
            pending.push_back(&d);
        }

        const auto agree = [](double x, double y) {
            return std::abs(x - y) <= 1e-6 * std::max({1.0, std::abs(x), std::abs(y)});
        };
        const auto name_of_part = [&](item_id id) { return part_name(*s.parts.find(id)); };
        std::vector<const device *> placed_sources;
        for (;;)
        {
            bool progress = false;
            for (auto it = pending.begin(); it != pending.end();)
            {
                const device &d = **it;
                const std::uint32_t plus = node_of(d.a);
                const std::uint32_t minus = node_of(d.b);
                if (!held[plus] && !held[minus])
                {
                    ++it;
                    continue;
                }
                progress = true;
                if (held[plus] && held[minus])
                {
                    if (agree(*held[plus] - *held[minus], d.value))
                        placed_sources.push_back(&d);
                    else
                    {
                        // Whichever was held by a source, the other one
                        // is named; ground needs no name.
                        problem p{problem_kind::conflicting_sources, severity::error, {{item_kind::part, d.id}}, {}};
                        for (const std::uint32_t node : {plus, minus})
                            if (held_by[node] != 0 && held_by[node] != d.id &&
                                std::ranges::find(p.items, item_ref{item_kind::part, held_by[node]}) == p.items.end())
                                p.items.push_back({item_kind::part, held_by[node]});
                        std::vector<std::string> others;
                        for (std::size_t i = 1; i < p.items.size(); ++i)
                            others.push_back(name_of_part(p.items[i].id));
                        p.message = std::format(
                            "{} would put {} across nets {} and {}, which are at {} already{}, so it is left out.",
                            d.name, volts(d.value), net_name_of_node(plus), net_name_of_node(minus),
                            volts(*held[plus] - *held[minus]),
                            others.empty() ? std::string() : std::format(" (set by {})", join_names(others)));
                        c.problems.push_back(std::move(p));
                    }
                }
                else if (held[minus])
                {
                    hold(plus, *held[minus] + d.value, d.id);
                    placed_sources.push_back(&d);
                }
                else
                {
                    hold(minus, *held[plus] - d.value, d.id);
                    placed_sources.push_back(&d);
                }
                it = pending.erase(it);
            }
            if (progress)
                continue;
            if (ground)
                break;
            // No ground: the first source of a circuit nothing holds yet
            // sets its "-" as 0 V.
            const auto unheld = std::ranges::find_if(
                pending, [&](const device *each) { return !anchored[circuits.find(node_of(each->b))]; });
            if (unheld == pending.end())
                break;
            const device &d = **unheld;
            hold(node_of(d.b), 0.0, 0);
            c.problems.push_back({problem_kind::reference_chosen,
                                  severity::warning,
                                  {{item_kind::part, d.id}},
                                  std::format("There is no ground, so net {} ({}'s - terminal) is taken as 0 V.",
                                              net_name_of_node(node_of(d.b)), d.name)});
        }
        for (const device *d : pending)
        {
            c.problems.push_back(
                {problem_kind::floating_source,
                 severity::error,
                 {{item_kind::part, d->id}},
                 std::format("{} has neither terminal on ground, which the simulation cannot handle yet, so it is "
                             "left out.",
                             d->name)});
        }
        for (const device *d : placed_sources)
        {
            ++sources;
            simulated[node_of(d->a)] = simulated[node_of(d->b)] = 1;
        }

        bool any_held = false;
        for (std::uint32_t node = 0; node < nodes; ++node)
        {
            if (!held[node])
                continue;
            const auto potential = static_cast<float>(*held[node]);
            c.network.holds.push_back({node, potential});
            c.low = any_held ? std::min(c.low, potential) : potential;
            c.high = any_held ? std::max(c.high, potential) : potential;
            any_held = true;
        }

        // ---- what is left out, said once per kind

        if (!inductors.empty())
        {
            std::vector<std::string> names;
            problem p{problem_kind::inductor_short, severity::info, {}, {}};
            for (const item_id id : inductors)
            {
                names.push_back(name_of_part(id));
                p.items.push_back({item_kind::part, id});
            }
            p.message = std::format("{} {} a short at DC: the nets at either end are one node.", join_names(names),
                                    inductors.size() == 1 ? "is" : "are");
            c.problems.push_back(std::move(p));
        }
        for (const auto &[device_name, ids] : left_out)
        {
            std::vector<std::string> names;
            problem p{problem_kind::not_simulated, severity::info, {}, {}};
            for (const item_id id : ids)
            {
                names.push_back(name_of_part(id));
                p.items.push_back({item_kind::part, id});
            }
            p.message = device_name.empty()
                            ? std::format("{} {} no simulation model, so {} left out.", join_names(names),
                                          ids.size() == 1 ? "has" : "have", ids.size() == 1 ? "it is" : "they are")
                            : std::format("{} ({}) {} not simulated yet, so {} left out.", join_names(names),
                                          device_name, ids.size() == 1 ? "is" : "are",
                                          ids.size() == 1 ? "it is" : "they are");
            c.problems.push_back(std::move(p));
        }

        // ---- nodes with no path to a held one

        const bool any_device = std::ranges::any_of(simulated, [](char x) { return x != 0; });
        if (any_device && !any_held)
        {
            c.problems.push_back({problem_kind::nothing_held, severity::warning, {},
                                  "Nothing sets a potential: the sheet has no ground and no voltage source."});
        }
        else if (any_device)
        {
            std::vector<std::vector<std::uint32_t>> adjacent(nodes);
            for (const simulation::network::branch &b : c.network.branches)
            {
                adjacent[b.a].push_back(b.b);
                adjacent[b.b].push_back(b.a);
            }
            std::vector<char> reached(nodes, 0);
            std::vector<std::uint32_t> queue;
            for (std::uint32_t node = 0; node < nodes; ++node)
                if (held[node])
                {
                    reached[node] = 1;
                    queue.push_back(node);
                }
            while (!queue.empty())
            {
                const std::uint32_t node = queue.back();
                queue.pop_back();
                for (const std::uint32_t next : adjacent[node])
                    if (reached[next] == 0)
                    {
                        reached[next] = 1;
                        queue.push_back(next);
                    }
            }

            std::vector<char> reported(nodes, 0);
            for (std::size_t n = 0; n < net_count; ++n)
            {
                if (c.node_of_net[n] < 0)
                    continue;
                const auto node = static_cast<std::uint32_t>(c.node_of_net[n]);
                if (reached[node] != 0 || simulated[node] == 0 || reported[node] != 0)
                    continue;
                reported[node] = 1;
                problem p{problem_kind::floating_net, severity::warning, {}, {}};
                for (std::size_t m = n; m < net_count; ++m)
                {
                    if (c.node_of_net[m] != c.node_of_net[n])
                        continue;
                    for (const item_id w : nets.nets[m].wires)
                        p.items.push_back({item_kind::wire, w});
                    for (const pin_ref &pin : nets.nets[m].pins)
                        if (std::ranges::find(p.items, item_ref{item_kind::part, pin.part}) == p.items.end())
                            p.items.push_back({item_kind::part, pin.part});
                }
                p.message = std::format("Net {} has no path to ground or a voltage source, so its potential is "
                                        "undefined.",
                                        nets.nets[n].name);
                c.problems.push_back(std::move(p));
            }
        }

        // ---- the status bar's line

        if (nodes == 0)
            c.summary = "Nothing to simulate";
        else
        {
            c.summary = plural(nodes, "node", "nodes");
            if (resistors > 0)
                c.summary += ", " + plural(resistors, "resistor", "resistors");
            if (!inductors.empty())
                c.summary += ", " + plural(inductors.size(), "inductor", "inductors");
            if (sources > 0)
                c.summary += ", " + plural(sources, "source", "sources");
        }
        return c;
    }

} // namespace volt::schematic
