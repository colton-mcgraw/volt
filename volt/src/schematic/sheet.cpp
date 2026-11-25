#include <schematic/sheet.hpp>

#include <algorithm>

namespace volt::schematic
{

    namespace
    {
        int snap(int v, int step) noexcept
        {
            const long long half = step / 2;
            const long long magnitude = (v < 0 ? -static_cast<long long>(v) : static_cast<long long>(v)) + half;
            const long long snapped = magnitude / step * step;
            return static_cast<int>(v < 0 ? -snapped : snapped);
        }
    } // namespace

    point snap_to_grid(point p, int step) noexcept
    {
        if (step <= 0)
            return p;
        return point{snap(p.x(), step), snap(p.y(), step)};
    }

    std::string_view to_string(label_kind kind) noexcept
    {
        switch (kind)
        {
        case label_kind::net:
            return "net";
        case label_kind::power:
            return "power";
        case label_kind::note:
            return "note";
        }
        return "net";
    }

    bool parse_label_kind(std::string_view text, label_kind &kind) noexcept
    {
        for (const label_kind k : {label_kind::net, label_kind::power, label_kind::note})
        {
            if (to_string(k) == text)
            {
                kind = k;
                return true;
            }
        }
        return false;
    }

    bool on_wire(const wire &w, point p) noexcept
    {
        const long long ax = w.a.x(), ay = w.a.y();
        const long long bx = w.b.x(), by = w.b.y();
        const long long px = p.x(), py = p.y();
        const long long cross = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
        if (cross != 0)
            return false;
        return px >= std::min(ax, bx) && px <= std::max(ax, bx) && py >= std::min(ay, by) && py <= std::max(ay, by);
    }

    bool inside_wire(const wire &w, point p) noexcept
    {
        return p != w.a && p != w.b && on_wire(w, p);
    }

    bool pin_on_wire(const wire &w, point p, const connectivity &rules) noexcept
    {
        if (p == w.a || p == w.b)
            return true;
        return rules.pins_inside_wires && on_wire(w, p);
    }

    item_id sheet::max_id() const noexcept
    {
        item_id most = 0;
        for (const auto &e : parts)
            most = std::max(most, e.id);
        for (const auto &e : wires)
            most = std::max(most, e.id);
        for (const auto &e : labels)
            most = std::max(most, e.id);
        return most;
    }

    bool contains(const sheet &s, item_ref item) noexcept
    {
        switch (item.kind)
        {
        case item_kind::part:
            return s.parts.contains(item.id);
        case item_kind::wire:
            return s.wires.contains(item.id);
        case item_kind::label:
            return s.labels.contains(item.id);
        }
        return false;
    }

} // namespace volt::schematic
