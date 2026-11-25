#include <schematic/document.hpp>

#include <schematic/follow.hpp>
#include <schematic/symbol.hpp>

#include <algorithm>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace volt::schematic
{

    namespace
    {
        // The items once each, in the order given.
        std::vector<item_ref> unique_items(std::span<const item_ref> items)
        {
            std::vector<item_ref> unique;
            unique.reserve(items.size());
            std::unordered_set<item_ref> seen;
            for (const item_ref &item : items)
                if (seen.insert(item).second)
                    unique.push_back(item);
            return unique;
        }

        std::string count_of(std::size_t n)
        {
            return n == 1 ? std::string("1 item") : std::format("{} items", n);
        }

        bool is_digit(char c) noexcept
        {
            return c >= '0' && c <= '9';
        }

        // A quarter turn of p about a point.
        point turn(point p, point about, bool clockwise) noexcept
        {
            return orient(p - about, clockwise ? std::uint8_t{3} : std::uint8_t{1}, false) + about;
        }

        // p mirrored about a vertical line through `about` (x changes), or
        // a horizontal one (y changes).
        point reflect(point p, point about, bool vertical_axis) noexcept
        {
            return vertical_axis ? point{2 * about.x() - p.x(), p.y()} : point{p.x(), 2 * about.y() - p.y()};
        }

        std::uint8_t quarter(int turns) noexcept
        {
            return static_cast<std::uint8_t>(((turns % 4) + 4) % 4);
        }

        long long cross(point u, point v) noexcept
        {
            return static_cast<long long>(u.x()) * v.y() - static_cast<long long>(u.y()) * v.x();
        }

        long long dot(point u, point v) noexcept
        {
            return static_cast<long long>(u.x()) * v.x() + static_cast<long long>(u.y()) * v.y();
        }

        // The two ends of a wire in one order, for finding duplicates.
        std::pair<point, point> ends_of(const wire &w) noexcept
        {
            const auto less = [](point p, point q) { return p.x() != q.x() ? p.x() < q.x() : p.y() < q.y(); };
            return less(w.b, w.a) ? std::pair{w.b, w.a} : std::pair{w.a, w.b};
        }

        // Points by the lines through them, so that finding the points on a
        // horizontal or vertical wire is a search along one line.
        class point_index
        {
        public:
            explicit point_index(std::span<const point> points) : m_points(points)
            {
                for (std::size_t i = 0; i < points.size(); ++i)
                {
                    m_rows[points[i].y()].emplace_back(points[i].x(), i);
                    m_columns[points[i].x()].emplace_back(points[i].y(), i);
                }
                for (auto &[y, line] : m_rows)
                    std::ranges::sort(line);
                for (auto &[x, line] : m_columns)
                    std::ranges::sort(line);
            }

            // Calls f(i) for every point i on the wire, its ends included.
            template <typename F>
            void visit(const wire &w, F &&f) const
            {
                const auto along = [&](const auto &lines, int line, int from, int to) {
                    const auto found = lines.find(line);
                    if (found == lines.end())
                        return;
                    const auto &points = found->second;
                    auto it = std::ranges::lower_bound(points, std::pair<int, std::size_t>{std::min(from, to), 0});
                    for (; it != points.end() && it->first <= std::max(from, to); ++it)
                        f(it->second);
                };
                if (w.a.y() == w.b.y())
                    along(m_rows, w.a.y(), w.a.x(), w.b.x());
                else if (w.a.x() == w.b.x())
                    along(m_columns, w.a.x(), w.a.y(), w.b.y());
                else
                    for (std::size_t i = 0; i < m_points.size(); ++i)
                        if (on_wire(w, m_points[i]))
                            f(i);
            }

        private:
            std::span<const point> m_points;
            std::unordered_map<int, std::vector<std::pair<int, std::size_t>>> m_rows;
            std::unordered_map<int, std::vector<std::pair<int, std::size_t>>> m_columns;
        };

        struct ends_hash
        {
            std::size_t operator()(const std::pair<point, point> &e) const noexcept
            {
                const std::size_t h = std::hash<point>{}(e.first);
                return h ^ (std::hash<point>{}(e.second) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
            }
        };
    } // namespace

    std::string reference_prefix(std::string_view reference)
    {
        while (!reference.empty() && reference.back() == '?')
            reference.remove_suffix(1);
        while (!reference.empty() && is_digit(reference.back()))
            reference.remove_suffix(1);
        return std::string(reference);
    }

    bool is_unnumbered_reference(std::string_view reference) noexcept
    {
        return reference.empty() || reference.back() == '?';
    }

    // ---- construction ------------------------------------------------------------------

    document::document(schematic::sheet start, history::stack_limits limits)
        : m_sheet(std::move(start)), m_history(limits), m_next_id(m_sheet.max_id() + 1)
    {
    }

    document::document(const document &other)
        : m_sheet(other.m_sheet), m_history(other.m_history), m_next_id(other.m_next_id),
          m_revision(other.m_revision), m_notified(other.m_revision)
    {
    }

    document &document::operator=(const document &other)
    {
        if (this == &other)
            return *this;
        m_sheet = other.m_sheet;
        m_history = other.m_history;
        m_next_id = other.m_next_id;
        m_revision = std::max(m_revision, other.m_revision) + 1;
        notify();
        return *this;
    }

    // ---- reading -------------------------------------------------------------------------

    std::string document::next_reference(std::string_view prefix) const
    {
        std::vector<bool> used;
        for (const auto &e : m_sheet.parts)
        {
            const std::string_view reference = e.record.reference;
            if (!reference.starts_with(prefix))
                continue;
            const std::string_view digits = reference.substr(prefix.size());
            if (digits.empty() || digits.size() > 6 || !std::ranges::all_of(digits, is_digit))
                continue;
            std::size_t n = 0;
            for (const char c : digits)
                n = n * 10 + static_cast<std::size_t>(c - '0');
            if (n >= used.size())
                used.resize(n + 1, false);
            used[n] = true;
        }
        std::size_t n = 1;
        while (n < used.size() && used[n])
            ++n;
        return std::string(prefix) + std::to_string(n);
    }

    std::string document::reference_for(const part &p) const
    {
        return numbered(p, false).reference;
    }

    fragment document::copy(std::span<const item_ref> items) const
    {
        const std::unordered_set<item_ref> wanted(items.begin(), items.end());
        fragment f;
        for (const auto &e : m_sheet.parts)
            if (wanted.contains({item_kind::part, e.id}))
                f.parts.push_back(e.record);
        for (const auto &e : m_sheet.wires)
            if (wanted.contains({item_kind::wire, e.id}))
                f.wires.push_back(e.record);
        for (const auto &e : m_sheet.labels)
            if (wanted.contains({item_kind::label, e.id}))
                f.labels.push_back(e.record);
        return f;
    }

    // ---- the history ------------------------------------------------------------------------

    void document::open(std::string step_label)
    {
        m_history.begin(std::move(step_label));
    }

    void document::close(std::string_view merge_key)
    {
        m_history.commit(std::string(merge_key));
        notify();
    }

    void document::record(std::optional<history::record_change<item_id, part>> change)
    {
        if (!change)
            return;
        m_history.record(sheet_change{parts_change{std::move(*change)}});
        ++m_revision;
    }

    void document::record(std::optional<history::record_change<item_id, wire>> change)
    {
        if (!change)
            return;
        m_history.record(sheet_change{wires_change{std::move(*change)}});
        ++m_revision;
    }

    void document::record(std::optional<history::record_change<item_id, label>> change)
    {
        if (!change)
            return;
        m_history.record(sheet_change{labels_change{std::move(*change)}});
        ++m_revision;
    }

    void document::notify()
    {
        if (m_notifying)
            return;
        if (!m_on_change)
        {
            m_notified = m_revision;
            return;
        }
        m_notifying = true;
        // A change made by the callback calls it once more, after it
        // returns; by a copy, since it may replace itself.
        while (m_on_change && m_notified != m_revision)
        {
            m_notified = m_revision;
            const std::function<void()> callback = m_on_change;
            callback();
        }
        m_notifying = false;
    }

    void document::on_change(std::function<void()> callback)
    {
        m_on_change = std::move(callback);
        m_notified = m_revision;
    }

    part document::numbered(part p, bool renumber) const
    {
        if (!renumber && !is_unnumbered_reference(p.reference))
            return p;
        std::string prefix = reference_prefix(p.reference);
        if (prefix.empty())
            prefix = find_symbol(p.symbol).prefix;
        if (prefix.empty())
            prefix = "U";
        p.reference = next_reference(prefix);
        return p;
    }

    std::string document::describe(std::span<const item_ref> items) const
    {
        std::size_t count = 0;
        const item_ref *only = nullptr;
        for (const item_ref &item : items)
        {
            if (contains(item))
            {
                ++count;
                only = &item;
            }
        }
        if (count != 1)
            return count_of(count);
        switch (only->kind)
        {
        case item_kind::part:
        {
            const part &p = *find_part(only->id);
            return p.reference.empty() ? p.symbol : p.reference;
        }
        case item_kind::wire:
            return "wire";
        case item_kind::label:
        {
            const label &l = *find_label(only->id);
            return l.text.empty() ? std::string("label") : l.text;
        }
        }
        return count_of(count);
    }

    // ---- adding -------------------------------------------------------------------------------

    item_id document::add_part(part p)
    {
        p = numbered(std::move(p), false);
        if (p.device.empty())
            p.device = find_symbol(p.symbol).device;
        const item_id id = take_id();
        open("Place " + (p.reference.empty() ? p.symbol : p.reference));
        record(m_sheet.parts.insert(id, std::move(p)));
        close({});
        return id;
    }

    item_id document::add_wire(point a, point b)
    {
        return add_wire(wire{a, b, {}});
    }

    item_id document::add_wire(wire w)
    {
        if (w.a == w.b)
            return 0;
        const item_id id = take_id();
        open("Draw wire");
        record(m_sheet.wires.insert(id, std::move(w)));
        close({});
        return id;
    }

    std::vector<item_id> document::add_wires(std::span<const std::pair<point, point>> segments)
    {
        std::vector<item_id> ids(segments.size(), 0);
        const std::size_t count = static_cast<std::size_t>(
            std::ranges::count_if(segments, [](const std::pair<point, point> &s) { return s.first != s.second; }));
        if (count == 0)
            return ids;
        open(count == 1 ? std::string("Draw wire") : std::format("Draw {} wires", count));
        for (std::size_t i = 0; i < segments.size(); ++i)
        {
            if (segments[i].first == segments[i].second)
                continue;
            ids[i] = take_id();
            record(m_sheet.wires.insert(ids[i], wire{segments[i].first, segments[i].second, {}}));
        }
        close({});
        return ids;
    }

    std::vector<item_id> document::add_path(std::span<const point> points)
    {
        std::vector<std::pair<point, point>> segments;
        for (std::size_t i = 1; i < points.size(); ++i)
            segments.emplace_back(points[i - 1], points[i]);
        return add_wires(segments);
    }

    item_id document::add_label(label l)
    {
        const item_id id = take_id();
        open(l.kind == label_kind::power && !l.text.empty() ? "Place " + l.text : std::string("Add label"));
        record(m_sheet.labels.insert(id, std::move(l)));
        close({});
        return id;
    }

    std::vector<item_ref> document::paste(std::span<const part> parts, std::span<const wire> wires,
                                          std::span<const label> labels, point offset)
    {
        std::vector<item_ref> made;
        const std::size_t count = parts.size() + wires.size() + labels.size();
        if (count == 0)
            return made;
        made.reserve(count);
        open("Paste " + count_of(count));
        for (const part &original : parts)
        {
            part p = original;
            p.position += offset;
            // Numbered one at a time, so each sees the ones before it.
            p = numbered(std::move(p), true);
            const item_id id = take_id();
            record(m_sheet.parts.insert(id, std::move(p)));
            made.push_back({item_kind::part, id});
        }
        for (const wire &original : wires)
        {
            wire w = original;
            w.a += offset;
            w.b += offset;
            const item_id id = take_id();
            record(m_sheet.wires.insert(id, std::move(w)));
            made.push_back({item_kind::wire, id});
        }
        for (const label &original : labels)
        {
            label l = original;
            l.position += offset;
            const item_id id = take_id();
            record(m_sheet.labels.insert(id, std::move(l)));
            made.push_back({item_kind::label, id});
        }
        close({});
        return made;
    }

    std::vector<item_ref> document::paste(const fragment &items, point offset)
    {
        return paste(items.parts, items.wires, items.labels, offset);
    }

    // ---- changing -------------------------------------------------------------------------------

    std::size_t document::erase(std::span<const item_ref> items)
    {
        const std::vector<item_ref> unique = unique_items(items);
        open("Delete " + describe(unique));
        std::size_t erased = 0;
        for (const item_ref &item : unique)
        {
            const std::uint64_t before = m_revision;
            switch (item.kind)
            {
            case item_kind::part:
                record(m_sheet.parts.erase(item.id));
                break;
            case item_kind::wire:
                record(m_sheet.wires.erase(item.id));
                break;
            case item_kind::label:
                record(m_sheet.labels.erase(item.id));
                break;
            }
            if (m_revision != before)
                ++erased;
        }
        close({});
        return erased;
    }

    bool document::move(std::span<const item_ref> items, point delta, std::string_view merge_key)
    {
        if (delta == point{})
            return false;
        const std::vector<item_ref> unique = unique_items(items);
        const std::uint64_t before = m_revision;
        open("Move " + describe(unique));
        for (const item_ref &item : unique)
        {
            switch (item.kind)
            {
            case item_kind::part:
                if (const part *found = find_part(item.id))
                {
                    part p = *found;
                    p.position += delta;
                    record(m_sheet.parts.replace(item.id, std::move(p)));
                }
                break;
            case item_kind::wire:
                if (const wire *found = find_wire(item.id))
                {
                    wire w = *found;
                    w.a += delta;
                    w.b += delta;
                    record(m_sheet.wires.replace(item.id, std::move(w)));
                }
                break;
            case item_kind::label:
                if (const label *found = find_label(item.id))
                {
                    label l = *found;
                    l.position += delta;
                    record(m_sheet.labels.replace(item.id, std::move(l)));
                }
                break;
            }
        }
        close(merge_key);
        return m_revision != before;
    }

    bool document::move_wire_end(item_id wire_id, bool end_b, point to, std::string_view merge_key)
    {
        const wire *found = find_wire(wire_id);
        if (found == nullptr)
            return false;
        wire w = *found;
        (end_b ? w.b : w.a) = to;
        const std::uint64_t before = m_revision;
        open("Move wire end");
        record(m_sheet.wires.replace(wire_id, std::move(w)));
        close(merge_key);
        return m_revision != before;
    }

    bool document::rotate(std::span<const item_ref> items, point about, bool clockwise)
    {
        const std::vector<item_ref> unique = unique_items(items);
        const int quarters = clockwise ? 3 : 1;
        const std::uint64_t before = m_revision;
        open("Rotate");
        for (const item_ref &item : unique)
        {
            switch (item.kind)
            {
            case item_kind::part:
                if (const part *found = find_part(item.id))
                {
                    // Turning the drawing turns the symbol a quarter more,
                    // mirrored or not: Q * R^r * M = R^(r+1) * M.
                    part p = *found;
                    p.position = turn(p.position, about, clockwise);
                    p.rotation = quarter(p.rotation + quarters);
                    record(m_sheet.parts.replace(item.id, std::move(p)));
                }
                break;
            case item_kind::wire:
                if (const wire *found = find_wire(item.id))
                {
                    wire w = *found;
                    w.a = turn(w.a, about, clockwise);
                    w.b = turn(w.b, about, clockwise);
                    record(m_sheet.wires.replace(item.id, std::move(w)));
                }
                break;
            case item_kind::label:
                if (const label *found = find_label(item.id))
                {
                    label l = *found;
                    l.position = turn(l.position, about, clockwise);
                    l.rotation = quarter(l.rotation + quarters);
                    record(m_sheet.labels.replace(item.id, std::move(l)));
                }
                break;
            }
        }
        close({});
        return m_revision != before;
    }

    bool document::mirror(std::span<const item_ref> items, point about, bool vertical_axis)
    {
        const std::vector<item_ref> unique = unique_items(items);
        const std::uint64_t before = m_revision;
        open("Mirror");
        for (const item_ref &item : unique)
        {
            switch (item.kind)
            {
            case item_kind::part:
                if (const part *found = find_part(item.id))
                {
                    // With F the flip of x, F * R^r * M^m = R^-r * M^(1-m);
                    // the flip of y is R^2 * F, which gives R^(2-r).
                    part p = *found;
                    p.position = reflect(p.position, about, vertical_axis);
                    p.mirrored = !p.mirrored;
                    p.rotation = quarter(vertical_axis ? -p.rotation : 2 - p.rotation);
                    record(m_sheet.parts.replace(item.id, std::move(p)));
                }
                break;
            case item_kind::wire:
                if (const wire *found = find_wire(item.id))
                {
                    wire w = *found;
                    w.a = reflect(w.a, about, vertical_axis);
                    w.b = reflect(w.b, about, vertical_axis);
                    record(m_sheet.wires.replace(item.id, std::move(w)));
                }
                break;
            case item_kind::label:
                if (const label *found = find_label(item.id))
                {
                    // A direction at angle a goes to 180 - a (left for
                    // right) or to -a (up for down).
                    label l = *found;
                    l.position = reflect(l.position, about, vertical_axis);
                    l.rotation = quarter(vertical_axis ? 2 - l.rotation : -l.rotation);
                    record(m_sheet.labels.replace(item.id, std::move(l)));
                }
                break;
            }
        }
        close({});
        return m_revision != before;
    }

    bool document::set_reference(item_id part_id, std::string_view text, std::string_view merge_key)
    {
        const part *found = find_part(part_id);
        if (found == nullptr || found->reference == text)
            return false;
        part p = *found;
        p.reference = std::string(text);
        open("Edit reference");
        record(m_sheet.parts.replace(part_id, std::move(p)));
        close(merge_key);
        return true;
    }

    bool document::set_value(item_id part_id, std::string_view text, std::string_view merge_key)
    {
        const part *found = find_part(part_id);
        if (found == nullptr || found->value == text)
            return false;
        part p = *found;
        p.value = std::string(text);
        open("Edit value");
        record(m_sheet.parts.replace(part_id, std::move(p)));
        close(merge_key);
        return true;
    }

    bool document::set_label_text(item_id label_id, std::string_view text, std::string_view merge_key)
    {
        const label *found = find_label(label_id);
        if (found == nullptr || found->text == text)
            return false;
        label l = *found;
        l.text = std::string(text);
        open("Edit label");
        record(m_sheet.labels.replace(label_id, std::move(l)));
        close(merge_key);
        return true;
    }

    bool document::replace_part(item_id part_id, part p, std::string_view merge_key)
    {
        const part *found = find_part(part_id);
        if (found == nullptr || *found == p)
            return false;
        open("Edit " + (found->reference.empty() ? found->symbol : found->reference));
        record(m_sheet.parts.replace(part_id, std::move(p)));
        close(merge_key);
        return true;
    }

    bool document::replace_wire(item_id wire_id, wire w, std::string_view merge_key)
    {
        const wire *found = find_wire(wire_id);
        if (found == nullptr || *found == w)
            return false;
        open("Edit wire");
        record(m_sheet.wires.replace(wire_id, std::move(w)));
        close(merge_key);
        return true;
    }

    bool document::replace_label(item_id label_id, label l, std::string_view merge_key)
    {
        const label *found = find_label(label_id);
        if (found == nullptr || *found == l)
            return false;
        open("Edit label");
        record(m_sheet.labels.replace(label_id, std::move(l)));
        close(merge_key);
        return true;
    }

    std::size_t document::tidy_wires()
    {
        return tidy(nullptr);
    }

    std::size_t document::tidy_wires(std::span<const item_id> wires)
    {
        std::unordered_set<item_id> scope;
        for (const item_id id : wires)
            if (m_sheet.wires.contains(id))
                scope.insert(id);
        if (scope.empty())
            return 0;
        return tidy(&scope);
    }

    std::size_t document::tidy_wires_at(std::span<const point> points)
    {
        std::unordered_set<item_id> scope;
        const point_index index(points);
        for (const auto &e : m_sheet.wires)
            index.visit(e.record, [&](std::size_t) { scope.insert(e.id); });
        if (scope.empty())
            return 0;
        return tidy(&scope);
    }

    std::size_t document::tidy(std::unordered_set<item_id> *scope)
    {
        const auto in_scope = [&](item_id id) { return scope == nullptr || scope->contains(id); };
        std::size_t removed = 0;
        open("Tidy wires");
        const auto take = [&](item_id id) {
            record(m_sheet.wires.erase(id));
            if (scope != nullptr)
                scope->erase(id);
            ++removed;
        };

        // Joining two wires can make one that duplicates another, so the
        // three passes go round until none changes anything.
        for (bool changed = true; changed;)
        {
            changed = false;

            // Wires of no length.
            std::vector<item_id> doomed;
            for (const auto &e : m_sheet.wires)
                if (e.record.a == e.record.b && in_scope(e.id))
                    doomed.push_back(e.id);
            // Exact duplicates: the first stays, unless only it is in the
            // scope.
            std::unordered_map<std::pair<point, point>, item_id, ends_hash> seen;
            for (const auto &e : m_sheet.wires)
            {
                if (e.record.a == e.record.b)
                    continue;
                const auto [it, added] = seen.try_emplace(ends_of(e.record), e.id);
                if (added)
                    continue;
                if (in_scope(e.id))
                    doomed.push_back(e.id);
                else if (in_scope(it->second))
                {
                    doomed.push_back(it->second);
                    it->second = e.id;
                }
            }
            for (const item_id id : doomed)
                take(id);
            changed = !doomed.empty();

            // Two wires in a line through a shared end nothing else uses.
            // One join at a time: each changes what is attached where.
            for (bool joined = true; joined;)
            {
                joined = false;
                std::unordered_map<point, std::vector<item_id>> ends;
                for (const auto &e : m_sheet.wires)
                {
                    ends[e.record.a].push_back(e.id);
                    ends[e.record.b].push_back(e.id);
                }
                std::unordered_set<point> attached;
                for (const auto &e : m_sheet.parts)
                    for (const point p : pin_positions(e.record))
                        attached.insert(p);
                for (const auto &e : m_sheet.labels)
                    attached.insert(e.record.position);

                for (const auto &e : m_sheet.wires)
                {
                    if (!in_scope(e.id))
                        continue;
                    for (const point at : {e.record.a, e.record.b})
                    {
                        const auto here = ends.find(at);
                        if (here->second.size() != 2 || attached.contains(at))
                            continue;
                        const item_id other_id = here->second[0] == e.id ? here->second[1] : here->second[0];
                        const wire &other = *find_wire(other_id);
                        const point far_end = e.record.a == at ? e.record.b : e.record.a;
                        const point other_far = other.a == at ? other.b : other.a;
                        if (cross(far_end - at, other_far - at) != 0 || dot(far_end - at, other_far - at) >= 0)
                            continue;
                        // A wire through the point (a T on its inside) is
                        // attached there too.
                        const bool crossed = std::ranges::any_of(
                            m_sheet.wires, [&](const auto &w) { return inside_wire(w.record, at); });
                        if (crossed)
                            continue;

                        // The first of the two in the table stays.
                        const bool first = *m_sheet.wires.position_of(e.id) < *m_sheet.wires.position_of(other_id);
                        const item_id keep = first ? e.id : other_id;
                        const item_id drop = first ? other_id : e.id;
                        wire kept = *find_wire(keep);
                        (kept.a == at ? kept.a : kept.b) = keep == e.id ? other_far : far_end;
                        take(drop);
                        record(m_sheet.wires.replace(keep, std::move(kept)));
                        if (scope != nullptr)
                            scope->insert(keep);
                        joined = true;
                        break;
                    }
                    if (joined)
                        break;
                }
                changed = changed || joined;
            }
        }
        close({});
        return removed;
    }

    // ---- wires that follow, and wires cut ------------------------------------------------------

    std::vector<item_id> document::follow_wires(const schematic::sheet &before, std::span<const item_ref> moved,
                                                const connectivity &rules)
    {
        const follow_plan plan = plan_follow(before, m_sheet, moved, rules);
        std::vector<item_id> touched;
        if (plan.empty())
            return touched;
        open("Move wires");
        for (const auto &[id, w] : plan.wires)
        {
            if (!m_sheet.wires.contains(id))
                continue;
            record(m_sheet.wires.replace(id, w));
            touched.push_back(id);
        }
        for (const wire &w : plan.added)
        {
            if (w.a == w.b)
                continue;
            const item_id id = take_id();
            record(m_sheet.wires.insert(id, w));
            touched.push_back(id);
        }
        for (const auto &[id, l] : plan.labels)
            if (m_sheet.labels.contains(id))
                record(m_sheet.labels.replace(id, l));
        close({});
        return touched;
    }

    std::size_t document::cut_wires_at_parts(std::span<const item_id> parts, const connectivity &rules,
                                             std::span<const item_id> keep)
    {
        // Every pin of the parts, and which part (of those found) it is of.
        std::vector<point> pins;
        std::vector<std::size_t> owner;
        std::unordered_set<item_id> seen;
        for (const item_id id : parts)
        {
            const part *found = find_part(id);
            if (found == nullptr || !seen.insert(id).second)
                continue;
            for (const point p : pin_positions(*found))
            {
                pins.push_back(p);
                owner.push_back(seen.size() - 1);
            }
        }
        const point_index index(pins);

        // The spans to take out of each wire: from the first to the last
        // pin of one part on it, along it. Worked out before cutting any.
        struct span_out
        {
            long long from = 0; // along the wire: dot product with its direction
            long long to = 0;
            point start{};
            point end{};
        };
        std::vector<std::pair<item_id, std::vector<span_out>>> cuts;
        for (const auto &e : m_sheet.wires)
        {
            const wire &w = e.record;
            if (w.a == w.b || std::ranges::find(keep, e.id) != keep.end())
                continue;
            const point direction = w.b - w.a;
            const auto along = [&](point p) { return dot(p - w.a, direction); };
            // Per part: its pins on the wire.
            std::vector<std::pair<std::size_t, std::vector<point>>> on;
            index.visit(w, [&](std::size_t i) {
                if (!pin_on_wire(w, pins[i], rules))
                    return;
                auto it = std::ranges::find(on, owner[i], &std::pair<std::size_t, std::vector<point>>::first);
                if (it == on.end())
                    it = on.insert(on.end(), {owner[i], {}});
                if (std::ranges::find(it->second, pins[i]) == it->second.end())
                    it->second.push_back(pins[i]);
            });
            std::vector<span_out> out;
            for (const auto &[slot, stops] : on)
            {
                if (stops.size() < 2)
                    continue;
                const auto [first, last] = std::ranges::minmax(stops, {}, along);
                out.push_back({along(first), along(last), first, last});
            }
            if (!out.empty())
                cuts.emplace_back(e.id, std::move(out));
        }

        open("Cut wires");
        for (auto &[id, out] : cuts)
        {
            const wire w = *find_wire(id);
            std::ranges::sort(out, {}, &span_out::from);
            // What is left: the stretches before, between and after the
            // spans taken out.
            std::vector<wire> left;
            point from = w.a;
            long long reached = 0;
            for (const span_out &taken : out)
            {
                if (taken.from > reached)
                    left.push_back(wire{from, taken.start, w.extra});
                if (taken.to > reached)
                {
                    from = taken.end;
                    reached = taken.to;
                }
            }
            left.push_back(wire{from, w.b, w.extra});
            std::erase_if(left, [](const wire &piece) { return piece.a == piece.b; });

            if (left.empty())
                record(m_sheet.wires.erase(id));
            else
            {
                record(m_sheet.wires.replace(id, left.front()));
                for (std::size_t i = 1; i < left.size(); ++i)
                    record(m_sheet.wires.insert(take_id(), left[i]));
            }
        }
        close({});
        return cuts.size();
    }

    bool document::finish_move(const schematic::sheet &before, std::span<const item_ref> moved,
                               const connectivity &rules)
    {
        const std::vector<item_ref> unique = unique_items(moved);
        const std::uint64_t revision_before = m_revision;
        open("Move " + describe(unique));

        // The moved parts and their pins as they were, and the points where
        // moved things were and are.
        std::vector<item_id> parts;
        std::vector<point> old_pins;
        std::vector<std::size_t> owner;
        std::vector<point> points;
        for (const item_ref &item : unique)
        {
            switch (item.kind)
            {
            case item_kind::part:
            {
                const part *now = find_part(item.id);
                if (now == nullptr)
                    break;
                parts.push_back(item.id);
                for (const point p : pin_positions(*now))
                    points.push_back(p);
                if (const part *was = before.parts.find(item.id))
                {
                    for (const point p : pin_positions(*was))
                    {
                        points.push_back(p);
                        old_pins.push_back(p);
                        owner.push_back(parts.size() - 1);
                    }
                }
                break;
            }
            case item_kind::wire:
                for (const wire *w : {find_wire(item.id), before.wires.find(item.id)})
                {
                    if (w != nullptr)
                    {
                        points.push_back(w->a);
                        points.push_back(w->b);
                    }
                }
                break;
            case item_kind::label:
                for (const label *l : {find_label(item.id), before.labels.find(item.id)})
                    if (l != nullptr)
                        points.push_back(l->position);
                break;
            }
        }

        // The wires that joined two pins of one moved part before it moved
        // (a jumper, meant): kept, not cut.
        std::vector<item_id> keep;
        const point_index old_index(old_pins);
        for (const auto &e : before.wires)
        {
            std::vector<std::pair<std::size_t, int>> counts; // per part
            bool joins = false;
            old_index.visit(e.record, [&](std::size_t i) {
                if (joins || !pin_on_wire(e.record, old_pins[i], rules))
                    return;
                auto it = std::ranges::find(counts, owner[i], &std::pair<std::size_t, int>::first);
                if (it == counts.end())
                    it = counts.insert(counts.end(), {owner[i], 0});
                joins = ++it->second >= 2;
            });
            if (joins)
                keep.push_back(e.id);
        }
        (void)cut_wires_at_parts(parts, rules, keep);

        // What the move changed or added, and what touches where things
        // were and went.
        std::unordered_set<item_id> scope;
        const point_index index(points);
        for (const auto &e : m_sheet.wires)
        {
            const wire *was = before.wires.find(e.id);
            if (was == nullptr || *was != e.record)
                scope.insert(e.id);
            else
                index.visit(e.record, [&](std::size_t) { scope.insert(e.id); });
        }
        if (!scope.empty())
            (void)tidy(&scope);
        close({});
        return m_revision != revision_before;
    }

    bool document::move_with_wires(std::span<const item_ref> items, point delta, std::string_view merge_key,
                                   const connectivity &rules)
    {
        if (delta == point{})
            return false;
        const std::vector<item_ref> unique = unique_items(items);
        const schematic::sheet before = m_sheet;
        const std::uint64_t revision_before = m_revision;
        open("Move " + describe(unique));
        if (move(unique, delta))
        {
            (void)follow_wires(before, unique, rules);
            (void)finish_move(before, unique, rules);
        }
        close(merge_key);
        return m_revision != revision_before;
    }

    // ---- transactions -----------------------------------------------------------------------------

    void document::begin(std::string step_label)
    {
        m_history.begin(std::move(step_label));
        ++m_revision;
        notify();
    }

    void document::commit(std::string merge_key)
    {
        m_history.commit(std::move(merge_key));
        ++m_revision;
        notify();
    }

    void document::cancel()
    {
        if (!m_history.in_transaction())
            return;
        m_history.cancel(m_sheet);
        ++m_revision;
        notify();
    }

    // ---- undo and redo ------------------------------------------------------------------------------

    bool document::undo()
    {
        if (!m_history.undo(m_sheet))
            return false;
        ++m_revision;
        notify();
        return true;
    }

    bool document::redo()
    {
        if (!m_history.redo(m_sheet))
            return false;
        ++m_revision;
        notify();
        return true;
    }

    bool document::mark_saved()
    {
        if (!m_history.mark_saved())
            return false;
        ++m_revision;
        notify();
        return true;
    }

} // namespace volt::schematic
