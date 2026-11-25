#include <app/ui/windows/schematic_tools.hpp>

#include <schematic/io.hpp>
#include <schematic/symbol.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <unordered_set>
#include <utility>

namespace volt::ui
{

    using schematic::item_kind;
    using schematic::item_ref;
    using schematic::label_kind;
    using schematic::point;

    namespace
    {
        point round_point(math::vec2f p) noexcept
        {
            return {static_cast<int>(std::lround(p.x())), static_cast<int>(std::lround(p.y()))};
        }

        float distance(math::vec2f a, math::vec2f b) noexcept
        {
            const float dx = a.x() - b.x();
            const float dy = a.y() - b.y();
            return std::sqrt(dx * dx + dy * dy);
        }

        std::uint8_t quarter(int turns) noexcept
        {
            return static_cast<std::uint8_t>(((turns % 4) + 4) % 4);
        }

        bool same_connection(const std::optional<schematic::connection_point> &a,
                             const std::optional<schematic::connection_point> &b) noexcept
        {
            if (a.has_value() != b.has_value())
                return false;
            return !a || (a->at == b->at && a->kind == b->kind && a->item == b->item && a->end_b == b->end_b);
        }

        bool is_power_symbol(const schematic::symbol &s) noexcept
        {
            return s.prefix == "#PWR";
        }

        bool is_digit(char c) noexcept
        {
            return c >= '0' && c <= '9';
        }

        // The text with its trailing number one more, as wide as it was at
        // least: "NET9" to "NET10", "D07" to "D08". Unchanged without one.
        std::string increment(std::string_view text)
        {
            std::size_t digits = 0;
            while (digits < text.size() && is_digit(text[text.size() - 1 - digits]))
                ++digits;
            if (digits == 0 || digits > 9)
                return std::string(text);
            const std::string_view stem = text.substr(0, text.size() - digits);
            std::uint64_t n = 0;
            for (const char c : text.substr(text.size() - digits))
                n = n * 10 + static_cast<std::uint64_t>(c - '0');
            std::string number = std::to_string(n + 1);
            if (number.size() < digits)
                number.insert(0, digits - number.size(), '0');
            return std::string(stem) + number;
        }

        // The text, or with its trailing number raised until no label of
        // the sheet says it.
        std::string next_free(const schematic::sheet *s, std::string text)
        {
            if (s == nullptr || text.empty() || !is_digit(text.back()))
                return text;
            std::unordered_set<std::string> used;
            for (const auto &e : s->labels)
                used.insert(e.record.text);
            for (int guard = 0; used.contains(text) && guard < 100000; ++guard)
                text = increment(text);
            return text;
        }

        // A sheet of the fragment's items, ids from 1, and back.
        schematic::sheet sheet_of(const schematic::fragment &f)
        {
            schematic::sheet s;
            schematic::item_id id = 1;
            for (const schematic::part &p : f.parts)
                (void)s.parts.insert(id++, p);
            for (const schematic::wire &w : f.wires)
                (void)s.wires.insert(id++, w);
            for (const schematic::label &l : f.labels)
                (void)s.labels.insert(id++, l);
            return s;
        }

        schematic::fragment fragment_of(const schematic::sheet &s)
        {
            schematic::fragment f;
            for (const auto &e : s.parts)
                f.parts.push_back(e.record);
            for (const auto &e : s.wires)
                f.wires.push_back(e.record);
            for (const auto &e : s.labels)
                f.labels.push_back(e.record);
            return f;
        }

        std::vector<item_ref> all_items(const schematic::sheet &s)
        {
            std::vector<item_ref> items;
            items.reserve(s.parts.size() + s.wires.size() + s.labels.size());
            for (const auto &e : s.parts)
                items.push_back({item_kind::part, e.id});
            for (const auto &e : s.wires)
                items.push_back({item_kind::wire, e.id});
            for (const auto &e : s.labels)
                items.push_back({item_kind::label, e.id});
            return items;
        }

        // The fragment turned or mirrored about its origin, by the
        // document's own arithmetic.
        schematic::fragment turned(const schematic::fragment &f, bool clockwise)
        {
            schematic::document d(sheet_of(f));
            const std::vector<item_ref> items = all_items(d.sheet());
            (void)d.rotate(items, {}, clockwise);
            return fragment_of(d.sheet());
        }

        schematic::fragment mirrored(const schematic::fragment &f, bool vertical_axis)
        {
            schematic::document d(sheet_of(f));
            const std::vector<item_ref> items = all_items(d.sheet());
            (void)d.mirror(items, {}, vertical_axis);
            return fragment_of(d.sheet());
        }

        schematic::fragment shifted(schematic::fragment f, point offset)
        {
            for (schematic::part &p : f.parts)
                p.position += offset;
            for (schematic::wire &w : f.wires)
            {
                w.a += offset;
                w.b += offset;
            }
            for (schematic::label &l : f.labels)
                l.position += offset;
            return f;
        }

        std::string_view kind_title(label_kind kind) noexcept
        {
            switch (kind)
            {
            case label_kind::net:
                return "Net label";
            case label_kind::power:
                return "Power port";
            case label_kind::note:
                return "Text";
            }
            return "Label";
        }
    } // namespace

    schematic_tools::schematic_tools()
    {
        m_label_text = {std::string(), std::string("GND"), std::string("Text")};
        m_label_text_set = {false, true, true};
    }

    // ---- bookkeeping ------------------------------------------------------------------------

    const schematic::sheet *schematic_tools::sheet() const noexcept
    {
        return m_doc != nullptr ? &m_doc->sheet() : nullptr;
    }

    void schematic_tools::prune() const
    {
        if (m_selection.empty())
            return;
        if (m_doc == nullptr)
        {
            m_selection.clear();
            touch();
            return;
        }
        const std::size_t removed =
            std::erase_if(m_selection, [this](const item_ref &item) { return !m_doc->contains(item); });
        if (removed > 0)
            touch();
    }

    std::uint64_t schematic_tools::stamp() const
    {
        prune();
        return m_revision + (m_doc != nullptr ? m_doc->revision() : 0);
    }

    schematic_tools::input_result schematic_tools::result(std::uint64_t before, bool consumed) const
    {
        return {consumed, stamp() != before};
    }

    void schematic_tools::interrupt()
    {
        if (m_doc != nullptr)
            m_doc->seal();
        m_nudge_key.clear();
    }

    std::uint64_t schematic_tools::revision() const
    {
        prune();
        return m_revision;
    }

    // ---- the document -----------------------------------------------------------------------

    void schematic_tools::set_document(schematic::document *doc)
    {
        if (doc == m_doc)
            return;
        abort_gesture();
        m_doc = doc;
        m_selection.clear();
        m_hover_item.reset();
        m_hover_connection.reset();
        m_last_click.reset();
        m_last_hits.clear();
        m_nudge_key.clear();
        m_left_down = false;
        m_right_consumed = false;
        touch();
    }

    // ---- settings ---------------------------------------------------------------------------

    void schematic_tools::set_grid(int step)
    {
        step = std::max(step, 1);
        if (step == m_grid)
            return;
        m_grid = step;
        touch();
    }

    void schematic_tools::set_snap(bool on)
    {
        if (on == m_snap)
            return;
        m_snap = on;
        touch();
    }

    void schematic_tools::set_connectivity(const schematic::connectivity &rules)
    {
        if (rules == m_rules)
            return;
        m_rules = rules;
        touch();
    }

    void schematic_tools::set_style(const schematic::draw_style &style)
    {
        m_style = style;
        touch();
    }

    point schematic_tools::snap_point(math::vec2f at, modifiers mods) const noexcept
    {
        const point p = round_point(at);
        if (!m_snap || has(mods, modifiers::control))
            return p;
        return schematic::snap_to_grid(p, m_grid);
    }

    point schematic_tools::snap_delta(math::vec2f delta, modifiers mods) const noexcept
    {
        return snap_point(delta, mods);
    }

    point schematic_tools::connect_point(math::vec2f at, float tolerance, modifiers mods,
                                         std::optional<schematic::connection_point> *connection) const
    {
        std::optional<schematic::connection_point> found;
        if (const schematic::sheet *s = sheet())
            found = schematic::nearest_connection(*s, at, std::max(tolerance, 0.0f));
        const point p = found ? found->at : snap_point(at, mods);
        if (connection != nullptr)
            *connection = std::move(found);
        return p;
    }

    // ---- the tool ---------------------------------------------------------------------------

    void schematic_tools::set_tool(tool t)
    {
        if (t == m_tool)
            return;
        interrupt();
        abort_gesture();
        end_paste();
        m_tool = t;
        m_hover_item.reset();
        m_hover_connection.reset();
        if (m_has_pointer)
            m_ghost_at = snap_point(m_pointer, m_mods);
        refresh_hover();
        touch();
    }

    void schematic_tools::begin_place_part(schematic::part p)
    {
        const schematic::symbol &sym = schematic::find_symbol(p.symbol);
        if (is_power_symbol(sym))
        {
            begin_place_power(!p.value.empty() ? p.value : sym.name == "GND" ? std::string("GND") : std::string("+5V"));
            return;
        }
        p.position = {};
        std::string prefix = schematic::reference_prefix(p.reference);
        if (prefix.empty())
            prefix = sym.prefix;
        p.reference = prefix.empty() ? std::string() : prefix + "?";
        m_part = std::move(p);
        set_tool(tool::part);
        interrupt();
        abort_gesture();
        end_paste();
        touch();
    }

    void schematic_tools::begin_place_part(const library::part &p)
    {
        begin_place_part(part_template(p));
    }

    void schematic_tools::begin_place_symbol(std::string_view symbol_name)
    {
        const schematic::symbol &sym = schematic::find_symbol(symbol_name);
        schematic::part p;
        p.symbol = std::string(symbol_name);
        p.device = sym.device;
        begin_place_part(std::move(p));
    }

    void schematic_tools::begin_place_power(std::string text)
    {
        set_label_text(label_kind::power, std::move(text));
        set_tool(tool::power);
        interrupt();
        abort_gesture();
        end_paste();
        touch();
    }

    schematic::part schematic_tools::part_template(const library::part &p)
    {
        schematic::part t;
        t.symbol = p.symbol;
        t.value = p.value;
        t.device = p.device;
        t.library_part = p.id;
        std::string prefix = p.prefix;
        if (prefix.empty())
            prefix = schematic::find_symbol(p.symbol).prefix;
        t.reference = prefix.empty() ? std::string() : prefix + "?";
        return t;
    }

    std::string schematic_tools::label_text(label_kind kind) const
    {
        const std::size_t k = kind_index(kind);
        if (k >= m_label_text.size())
            return {};
        if (!m_label_text_set[k])
            return next_free(sheet(), "NET1");
        return m_label_text[k];
    }

    std::uint8_t schematic_tools::label_rotation(label_kind kind) const noexcept
    {
        const std::size_t k = kind_index(kind);
        return k < m_label_rotation.size() ? m_label_rotation[k] : std::uint8_t{0};
    }

    void schematic_tools::set_label_text(std::string text)
    {
        set_label_text(placing_label().value_or(label_kind::net), std::move(text));
    }

    void schematic_tools::set_label_text(label_kind kind, std::string text)
    {
        const std::size_t k = kind_index(kind);
        if (k >= m_label_text.size())
            return;
        m_label_text[k] = std::move(text);
        m_label_text_set[k] = true;
        touch();
    }

    void schematic_tools::label_renamed(label_kind kind, std::string_view text)
    {
        const std::size_t k = kind_index(kind);
        if (k >= m_label_text.size())
            return;
        const std::string before = m_label_text[k];
        advance_label_text(kind, text);
        if (m_label_text[k] != before)
            touch();
    }

    std::optional<label_kind> schematic_tools::placing_label() const noexcept
    {
        switch (m_tool)
        {
        case tool::label:
            return label_kind::net;
        case tool::power:
            return label_kind::power;
        case tool::text:
            return label_kind::note;
        case tool::select:
        case tool::wire:
        case tool::part:
            break;
        }
        return std::nullopt;
    }

    void schematic_tools::advance_label_text(label_kind kind, std::string_view placed_text)
    {
        if (kind != label_kind::net || placed_text.empty() || !is_digit(placed_text.back()))
            return;
        const std::size_t k = kind_index(kind);
        m_label_text[k] = next_free(sheet(), increment(placed_text));
        m_label_text_set[k] = true;
    }

    bool schematic_tools::placing() const noexcept
    {
        if (m_paste)
            return true;
        if (m_tool == tool::part)
            return m_part.has_value();
        return placing_label().has_value();
    }

    // ---- gestures ---------------------------------------------------------------------------

    void schematic_tools::abort_gesture()
    {
        switch (m_gesture)
        {
        case gesture::moving:
        case gesture::wire_end:
            if (m_doc != nullptr && m_doc->in_transaction())
                m_doc->cancel();
            break;
        case gesture::wiring:
            m_wire.clear();
            m_target_connection.reset();
            break;
        case gesture::none:
        case gesture::pressed:
        case gesture::band:
            break;
        }
        if (m_gesture != gesture::none)
            touch();
        m_gesture = gesture::none;
        clear_drag();
    }

    void schematic_tools::clear_drag()
    {
        m_ends.clear();
        m_end_connection.reset();
        m_drag_start = {};
        m_drag_items.clear();
        m_turns.clear();
        m_detached = false;
    }

    void schematic_tools::end_paste()
    {
        if (!m_paste)
            return;
        m_paste.reset();
        touch();
    }

    // ---- input ------------------------------------------------------------------------------

    schematic_tools::input_result schematic_tools::pointer_down(const pointer_input &e)
    {
        if (m_doc == nullptr)
            return {};
        const std::uint64_t before = stamp();
        m_has_pointer = true;
        m_pointer = e.at;
        m_tolerance = std::max(e.tolerance, 0.0f);
        m_mods = e.mods;

        bool consumed = false;
        switch (e.button)
        {
        case mouse_button::left:
            interrupt();
            m_left_down = true;
            consumed = true;
            left_down(e);
            break;
        case mouse_button::right:
            if (m_gesture == gesture::wiring)
            {
                (void)finish_wire();
                consumed = true;
            }
            else if (m_gesture != gesture::none)
            {
                abort_gesture();
                consumed = true;
            }
            m_right_consumed = consumed;
            break;
        case mouse_button::middle:
        case mouse_button::back:
        case mouse_button::forward:
            break;
        }
        return result(before, consumed);
    }

    void schematic_tools::left_down(const pointer_input &e)
    {
        // A press while one is still held (a release missed): the first
        // ends as if released, a held press or band is dropped.
        switch (m_gesture)
        {
        case gesture::pressed:
        case gesture::band:
            m_gesture = gesture::none;
            touch();
            break;
        case gesture::moving:
        case gesture::wire_end:
            end_drag();
            break;
        case gesture::wiring:
            wire_click(e);
            return;
        case gesture::none:
            break;
        }

        if (m_paste)
        {
            if (e.clicks <= 1)
                drop_paste(e);
            return;
        }
        switch (m_tool)
        {
        case tool::select:
            select_press(e);
            break;
        case tool::wire:
            if (e.clicks <= 1)
                wire_click(e);
            break;
        case tool::part:
        case tool::label:
        case tool::power:
        case tool::text:
            if (e.clicks <= 1)
                place(e);
            break;
        }
    }

    schematic_tools::input_result schematic_tools::pointer_move(const pointer_input &e)
    {
        if (m_doc == nullptr)
            return {};
        const std::uint64_t before = stamp();
        const bool had_pointer = m_has_pointer;
        m_has_pointer = true;
        m_pointer = e.at;
        m_tolerance = std::max(e.tolerance, 0.0f);
        m_mods = e.mods;

        bool consumed = false;
        switch (m_gesture)
        {
        case gesture::pressed:
            consumed = true;
            if (distance(e.at, m_press_at) > m_tolerance)
                start_drag(e);
            break;
        case gesture::moving:
        case gesture::wire_end:
            consumed = true;
            drag(e);
            break;
        case gesture::band:
            consumed = true;
            touch();
            break;
        case gesture::wiring:
            update_wire_target(e);
            break;
        case gesture::none:
            if (!had_pointer && placing())
                touch();
            update_hover(e);
            break;
        }
        return result(before, consumed);
    }

    schematic_tools::input_result schematic_tools::pointer_up(const pointer_input &e)
    {
        if (m_doc == nullptr)
            return {};
        const std::uint64_t before = stamp();
        m_pointer = e.at;
        m_tolerance = std::max(e.tolerance, 0.0f);
        m_mods = e.mods;

        if (e.button == mouse_button::right)
        {
            const bool consumed = m_right_consumed;
            m_right_consumed = false;
            return result(before, consumed);
        }
        if (e.button != mouse_button::left)
            return result(before, false);

        const bool consumed = m_left_down;
        m_left_down = false;
        switch (m_gesture)
        {
        case gesture::pressed:
            m_gesture = gesture::none;
            touch();
            click(e);
            break;
        case gesture::moving:
        case gesture::wire_end:
            drag(e);
            end_drag();
            break;
        case gesture::band:
            end_band(e);
            break;
        case gesture::wiring:
        case gesture::none:
            break;
        }
        return result(before, consumed);
    }

    schematic_tools::input_result schematic_tools::pointer_leave()
    {
        const std::uint64_t before = stamp();
        if (m_has_pointer && placing())
            touch();
        m_has_pointer = false;
        if (m_gesture == gesture::none)
        {
            if (m_hover_item || m_hover_connection)
                touch();
            m_hover_item.reset();
            m_hover_connection.reset();
        }
        return result(before, false);
    }

    schematic_tools::input_result schematic_tools::key_down(key k, modifiers mods)
    {
        const std::uint64_t before = stamp();
        // Alt is the detaching key: it may be held through a drag, and with
        // the arrows it nudges detached.
        const bool arrow = k == key::left || k == key::right || k == key::up || k == key::down;
        const bool alt = has(mods, modifiers::alt);
        if (has(mods, modifiers::control) || has(mods, modifiers::super) || (alt && !arrow && !dragging()))
            return result(before, false);

        bool consumed = false;
        switch (k)
        {
        case key::escape:
            consumed = cancel();
            break;
        case key::enter:
        case key::kp_enter:
            consumed = finish_wire();
            break;
        case key::backspace:
            consumed = drawing_wire() ? remove_wire_point() : delete_selection();
            break;
        case key::del:
            consumed = delete_selection();
            break;
        case key::r:
            consumed = rotate_selection(has(mods, modifiers::shift));
            break;
        case key::x:
            consumed = mirror_selection(true);
            break;
        case key::y:
            consumed = mirror_selection(false);
            break;
        case key::space:
        case key::slash:
            consumed = toggle_wire_corner();
            break;
        case key::left:
            consumed = nudge(-1, 0, alt);
            break;
        case key::right:
            consumed = nudge(1, 0, alt);
            break;
        case key::up:
            consumed = nudge(0, 1, alt);
            break;
        case key::down:
            consumed = nudge(0, -1, alt);
            break;
        default:
            break;
        }
        return result(before, consumed);
    }

    // ---- the select tool --------------------------------------------------------------------

    void schematic_tools::select_press(const pointer_input &e)
    {
        if (e.clicks >= 2)
        {
            // A double-click: edit what the first click selected.
            const std::vector<item_ref> hits = schematic::hit_test_all(*sheet(), e.at, m_tolerance, m_style);
            if (hits.empty())
                return;
            item_ref pick = hits.front();
            for (const item_ref &hit : hits)
            {
                if (is_selected(hit))
                {
                    pick = hit;
                    break;
                }
            }
            if (!is_selected(pick))
                set_selection(std::span<const item_ref>(&pick, 1));
            if (edit_requested)
                edit_requested(pick);
            return;
        }
        m_gesture = gesture::pressed;
        m_press_at = e.at;
        m_press_mods = e.mods;
        touch();
    }

    void schematic_tools::click(const pointer_input &e)
    {
        const std::vector<item_ref> hits = schematic::hit_test_all(*sheet(), m_press_at, m_tolerance, m_style);
        const bool toggle = has(m_press_mods, modifiers::shift) || has(m_press_mods, modifiers::control);
        if (hits.empty())
        {
            if (!toggle)
                set_selection({});
            m_last_click.reset();
            m_last_hits.clear();
            return;
        }
        if (toggle)
        {
            if (is_selected(hits.front()))
                deselect(hits.front());
            else
                add_to_selection(std::span<const item_ref>(hits.data(), 1));
            m_last_click.reset();
            m_last_hits.clear();
            return;
        }

        // Clicking again on the same spot goes to the next item under it.
        std::size_t index = 0;
        const std::span<const item_ref> now = selection();
        if (m_last_click && distance(*m_last_click, m_press_at) <= std::max(m_tolerance, 0.5f) &&
            m_last_hits == hits && m_cycle < hits.size() && now.size() == 1 && now.front() == hits[m_cycle])
            index = (m_cycle + 1) % hits.size();
        set_selection(std::span<const item_ref>(&hits[index], 1));
        m_cycle = index;
        m_last_hits = hits;
        m_last_click = m_press_at;
        (void)e;
    }

    void schematic_tools::start_drag(const pointer_input &e)
    {
        const schematic::sheet &s = *sheet();
        const std::vector<item_ref> hits = schematic::hit_test_all(s, m_press_at, m_tolerance, m_style);
        m_last_click.reset();
        if (hits.empty())
        {
            m_gesture = gesture::band;
            touch();
            return;
        }
        // Shift adds what is pressed; Ctrl, which a drag holds to move off
        // the grid, does not.
        const bool add = has(m_press_mods, modifiers::shift);

        // A wire's end, unless that wire moves with others.
        if (hits.front().kind == item_kind::wire)
        {
            if (const auto end = schematic::hit_wire_end(s, m_press_at, m_tolerance))
            {
                const item_ref wire_item{item_kind::wire, end->wire};
                const bool with_others = selection().size() > 1 && is_selected(wire_item);
                const schematic::wire *w = s.wires.find(end->wire);
                if (!with_others && w != nullptr)
                {
                    if (!is_selected(wire_item))
                    {
                        if (add)
                            add_to_selection(std::span<const item_ref>(&wire_item, 1));
                        else
                            set_selection(std::span<const item_ref>(&wire_item, 1));
                    }
                    m_end_from = end->end_b ? w->b : w->a;
                    m_end_at = m_end_from;
                    m_ends = schematic::wire_ends_at(s, m_end_from);
                    m_end_connection.reset();
                    m_doc->begin("Move wire end");
                    m_gesture = gesture::wire_end;
                    touch();
                    drag(e);
                    return;
                }
            }
        }

        // The selected item under the pointer, or the topmost one.
        std::optional<item_ref> pick;
        for (const item_ref &hit : hits)
        {
            if (is_selected(hit))
            {
                pick = hit;
                break;
            }
        }
        if (!pick)
        {
            if (add)
                add_to_selection(std::span<const item_ref>(hits.data(), 1));
            else
                set_selection(std::span<const item_ref>(hits.data(), 1));
        }
        m_applied = {};
        m_turns.clear();
        m_detached = has(m_press_mods, modifiers::alt) || has(e.mods, modifiers::alt);
        m_drag_start = s;
        m_drag_items.assign(selection().begin(), selection().end());
        m_drag_label = "Move " + describe(m_drag_items);
        m_doc->begin(m_drag_label);
        m_gesture = gesture::moving;
        touch();
        drag(e);
    }

    void schematic_tools::drag(const pointer_input &e)
    {
        if (m_doc == nullptr || !m_doc->in_transaction())
        {
            // Someone closed the drag's transaction.
            m_gesture = gesture::none;
            clear_drag();
            touch();
            return;
        }
        if (m_gesture == gesture::moving)
        {
            const point delta = snap_delta(e.at - m_press_at, e.mods);
            if (delta == m_applied)
                return;
            redo_drag(delta);
            touch();
        }
        else if (m_gesture == gesture::wire_end)
        {
            // A connection near the pointer wins over the grid; the wires
            // dragged offer none.
            std::vector<item_ref> dragged;
            for (const schematic::wire_end_hit &end : m_ends)
                dragged.push_back({item_kind::wire, end.wire});
            auto connection = schematic::nearest_connection(*sheet(), e.at, m_tolerance, dragged);
            const point to = connection ? connection->at : snap_point(e.at, e.mods);
            if (!same_connection(connection, m_end_connection))
                touch();
            m_end_connection = std::move(connection);
            if (to == m_end_at)
                return;
            for (const schematic::wire_end_hit &end : m_ends)
                (void)m_doc->move_wire_end(end.wire, end.end_b, to);
            m_end_at = to;
            touch();
        }
    }

    void schematic_tools::redo_drag(point delta)
    {
        // Undone back to the sheet the drag began with, and done again:
        // the wires follow from there, not from where the last pointer
        // position left them.
        m_doc->cancel();
        m_doc->begin(m_drag_label);
        for (const drag_turn &t : m_turns)
        {
            if (t.mirror)
                (void)m_doc->mirror(m_drag_items, t.about, t.vertical_axis);
            else
                (void)m_doc->rotate(m_drag_items, t.about, t.clockwise);
        }
        (void)m_doc->move(m_drag_items, delta);
        if (!m_detached)
            (void)m_doc->follow_wires(m_drag_start, m_drag_items, m_rules);
        m_applied = delta;
    }

    void schematic_tools::end_drag()
    {
        const gesture was = m_gesture;
        m_gesture = gesture::none;
        touch();
        if (m_doc == nullptr || !m_doc->in_transaction())
        {
            clear_drag();
            return;
        }
        if (was == gesture::moving)
        {
            if (m_applied != point{} || !m_turns.empty())
                (void)m_doc->finish_move(m_drag_start, m_drag_items, m_rules);
        }
        else if (m_end_at != m_end_from)
        {
            std::vector<schematic::item_id> wires;
            for (const schematic::wire_end_hit &end : m_ends)
                wires.push_back(end.wire);
            (void)m_doc->tidy_wires(wires);
            const point points[] = {m_end_from, m_end_at};
            (void)m_doc->tidy_wires_at(points);
        }
        m_doc->commit();
        m_doc->seal();
        clear_drag();
    }

    void schematic_tools::end_band(const pointer_input &e)
    {
        m_gesture = gesture::none;
        touch();
        const point a = round_point(m_press_at);
        const point b = round_point(e.at);
        const bool crossing = b.x() < a.x();
        const std::vector<item_ref> items = schematic::items_in_rect(*sheet(), a, b, crossing, m_style);
        if (has(m_press_mods, modifiers::shift) || has(m_press_mods, modifiers::control))
            add_to_selection(items);
        else
            set_selection(items);
    }

    void schematic_tools::update_hover(const pointer_input &e)
    {
        std::optional<item_ref> item;
        std::optional<schematic::connection_point> connection;
        point ghost = m_ghost_at;
        const schematic::sheet &s = *sheet();
        if (m_paste)
            ghost = snap_point(e.at, e.mods);
        else
        {
            switch (m_tool)
            {
            case tool::select:
                item = schematic::hit_test(s, e.at, m_tolerance, m_style);
                break;
            case tool::wire:
                connection = schematic::nearest_connection(s, e.at, m_tolerance);
                break;
            case tool::label:
            case tool::power:
                ghost = connect_point(e.at, m_tolerance, e.mods, &connection);
                break;
            case tool::part:
            case tool::text:
                ghost = snap_point(e.at, e.mods);
                break;
            }
        }
        if (item != m_hover_item || !same_connection(connection, m_hover_connection) || ghost != m_ghost_at)
            touch();
        m_hover_item = item;
        m_hover_connection = std::move(connection);
        m_ghost_at = ghost;
    }

    void schematic_tools::refresh_hover()
    {
        if (m_doc == nullptr || !m_has_pointer || m_gesture != gesture::none)
            return;
        update_hover({.at = m_pointer, .tolerance = m_tolerance, .button = mouse_button::left, .mods = m_mods});
    }

    // ---- the wire tool ----------------------------------------------------------------------

    std::vector<point> schematic_tools::route(point from, point to) const
    {
        if (from == to)
            return {};
        if (from.x() == to.x() || from.y() == to.y())
            return {to};
        const bool horizontal =
            m_direction_set ? m_horizontal_first : std::abs(to.x() - from.x()) >= std::abs(to.y() - from.y());
        const point corner = horizontal ? point{to.x(), from.y()} : point{from.x(), to.y()};
        return {corner, to};
    }

    std::vector<point> schematic_tools::wire_preview() const
    {
        std::vector<point> path = m_wire;
        if (!path.empty())
            for (const point p : route(path.back(), m_wire_target))
                path.push_back(p);
        return path;
    }

    void schematic_tools::update_wire_target(const pointer_input &e)
    {
        if (m_wire.empty())
            return;
        const point from = m_wire.back();
        const float dx = e.at.x() - static_cast<float>(from.x());
        const float dy = e.at.y() - static_cast<float>(from.y());
        const float away = std::max(std::abs(dx), std::abs(dy));
        const bool was_horizontal = m_horizontal_first;
        const bool was_set = m_direction_set;
        if (!m_direction_set)
        {
            if (away > m_tolerance)
            {
                m_horizontal_first = std::abs(dx) >= std::abs(dy);
                m_direction_set = true;
            }
        }
        else if (!m_corner_forced && away <= m_tolerance)
            m_direction_set = false;

        std::optional<schematic::connection_point> connection;
        const point target = connect_point(e.at, m_tolerance, e.mods, &connection);
        if (target != m_wire_target || !same_connection(connection, m_target_connection) ||
            was_horizontal != m_horizontal_first || was_set != m_direction_set)
            touch();
        m_wire_target = target;
        m_target_connection = std::move(connection);
    }

    void schematic_tools::wire_click(const pointer_input &e)
    {
        std::optional<schematic::connection_point> connection;
        const point at = connect_point(e.at, m_tolerance, e.mods, &connection);
        if (m_gesture != gesture::wiring)
        {
            if (e.clicks > 1)
                return;
            m_wire.assign(1, at);
            m_wire_target = at;
            m_target_connection = std::move(connection);
            m_direction_set = false;
            m_corner_forced = false;
            m_gesture = gesture::wiring;
            touch();
            return;
        }

        bool added = false;
        for (const point p : route(m_wire.back(), at))
        {
            if (p != m_wire.back())
            {
                m_wire.push_back(p);
                added = true;
            }
        }
        m_direction_set = false;
        m_corner_forced = false;
        m_wire_target = at;
        const bool on_connection = connection.has_value();
        m_target_connection = std::move(connection);
        touch();
        if (e.clicks > 1 || (added && on_connection))
            (void)finish_wire();
    }

    bool schematic_tools::toggle_wire_corner()
    {
        if (m_gesture != gesture::wiring || m_wire.empty())
            return false;
        if (!m_direction_set)
        {
            const point from = m_wire.back();
            const bool automatic =
                std::abs(m_wire_target.x() - from.x()) >= std::abs(m_wire_target.y() - from.y());
            m_horizontal_first = !automatic;
        }
        else
            m_horizontal_first = !m_horizontal_first;
        m_direction_set = true;
        m_corner_forced = true;
        touch();
        return true;
    }

    bool schematic_tools::remove_wire_point()
    {
        if (m_gesture != gesture::wiring || m_wire.empty())
            return false;
        m_wire.pop_back();
        m_direction_set = false;
        m_corner_forced = false;
        if (m_wire.empty())
        {
            m_gesture = gesture::none;
            m_target_connection.reset();
        }
        touch();
        return true;
    }

    bool schematic_tools::finish_wire()
    {
        if (m_gesture != gesture::wiring)
            return false;
        const std::vector<point> path = std::move(m_wire);
        m_wire.clear();
        m_target_connection.reset();
        m_gesture = gesture::none;
        touch();
        if (m_doc != nullptr && path.size() >= 2)
        {
            m_doc->begin("Draw wire");
            const std::vector<schematic::item_id> made = m_doc->add_path(path);
            (void)m_doc->tidy_wires(made);
            m_doc->commit();
            m_doc->seal();
        }
        refresh_hover();
        return true;
    }

    // ---- placing ----------------------------------------------------------------------------

    void schematic_tools::place(const pointer_input &e)
    {
        item_ref made{};
        if (m_tool == tool::part)
        {
            if (!m_part)
                return;
            schematic::part p = *m_part;
            p.position = snap_point(e.at, e.mods);
            m_ghost_at = p.position;
            // Placed, and the wires it lands on cut, as one step.
            m_doc->begin("Place " + m_doc->reference_for(p));
            const schematic::item_id id = m_doc->add_part(std::move(p));
            if (id != 0)
                settle_parts(std::span<const schematic::item_id>(&id, 1), {});
            m_doc->commit();
            if (id == 0)
                return;
            made = {item_kind::part, id};
        }
        else if (const auto kind = placing_label())
        {
            schematic::label l;
            l.kind = *kind;
            l.position = *kind == label_kind::note ? snap_point(e.at, e.mods)
                                                   : connect_point(e.at, m_tolerance, e.mods, nullptr);
            l.rotation = label_rotation(*kind);
            l.text = label_text(*kind);
            m_ghost_at = l.position;
            const std::string text = l.text;
            const schematic::item_id id = m_doc->add_label(std::move(l));
            if (id == 0)
                return;
            made = {item_kind::label, id};
            advance_label_text(*kind, text);
        }
        else
            return;
        set_selection(std::span<const item_ref>(&made, 1));
        refresh_hover();
        touch();
        if (placed)
            placed(made);
    }

    void schematic_tools::settle_parts(std::span<const schematic::item_id> parts,
                                       std::span<const schematic::item_id> wires)
    {
        (void)m_doc->cut_wires_at_parts(parts, m_rules, wires);
        std::vector<point> pins;
        for (const schematic::item_id id : parts)
            if (const schematic::part *p = m_doc->find_part(id))
                for (const point at : schematic::pin_positions(*p))
                    pins.push_back(at);
        (void)m_doc->tidy_wires(wires);
        (void)m_doc->tidy_wires_at(pins);
    }

    void schematic_tools::drop_paste(const pointer_input &e)
    {
        const schematic::fragment items = std::move(*m_paste);
        m_paste.reset();
        // Pasted, and the wires the parts land on cut, as one step.
        const std::size_t n = items.parts.size() + items.wires.size() + items.labels.size();
        m_doc->begin(n == 1 ? std::string("Paste 1 item") : std::format("Paste {} items", n));
        const std::vector<item_ref> made = m_doc->paste(items, snap_point(e.at, e.mods));
        std::vector<schematic::item_id> parts;
        std::vector<schematic::item_id> wires;
        for (const item_ref &item : made)
        {
            if (item.kind == item_kind::part)
                parts.push_back(item.id);
            else if (item.kind == item_kind::wire)
                wires.push_back(item.id);
        }
        settle_parts(parts, wires);
        m_doc->commit();
        set_selection(made);
        refresh_hover();
        touch();
    }

    // ---- the selection ----------------------------------------------------------------------

    std::span<const item_ref> schematic_tools::selection() const
    {
        prune();
        return m_selection;
    }

    bool schematic_tools::is_selected(item_ref item) const
    {
        prune();
        return std::ranges::find(m_selection, item) != m_selection.end();
    }

    void schematic_tools::set_selection(std::span<const item_ref> items)
    {
        std::vector<item_ref> next;
        next.reserve(items.size());
        std::unordered_set<item_ref> seen;
        for (const item_ref &item : items)
            if (m_doc != nullptr && m_doc->contains(item) && seen.insert(item).second)
                next.push_back(item);
        if (next != m_selection)
        {
            m_selection = std::move(next);
            touch();
        }
    }

    void schematic_tools::select(std::span<const item_ref> items)
    {
        interrupt();
        set_selection(items);
    }

    void schematic_tools::add_to_selection(std::span<const item_ref> items)
    {
        interrupt();
        prune();
        std::vector<item_ref> next = m_selection;
        next.insert(next.end(), items.begin(), items.end());
        set_selection(next);
    }

    void schematic_tools::deselect(item_ref item)
    {
        interrupt();
        prune();
        if (std::erase(m_selection, item) > 0)
            touch();
    }

    void schematic_tools::select_all()
    {
        interrupt();
        if (const schematic::sheet *s = sheet())
            set_selection(all_items(*s));
    }

    void schematic_tools::clear_selection()
    {
        interrupt();
        set_selection({});
    }

    std::optional<item_ref> schematic_tools::hover() const
    {
        if (m_hover_item && m_doc != nullptr && m_doc->contains(*m_hover_item))
            return m_hover_item;
        return std::nullopt;
    }

    // ---- commands ---------------------------------------------------------------------------

    std::string schematic_tools::describe(std::span<const item_ref> items) const
    {
        std::size_t count = 0;
        std::optional<item_ref> only;
        for (const item_ref &item : items)
        {
            if (m_doc != nullptr && m_doc->contains(item))
            {
                ++count;
                only = item;
            }
        }
        if (count == 1)
        {
            switch (only->kind)
            {
            case item_kind::part:
            {
                const schematic::part &p = *m_doc->find_part(only->id);
                return p.reference.empty() ? p.symbol : p.reference;
            }
            case item_kind::wire:
                return "wire";
            case item_kind::label:
            {
                const schematic::label &l = *m_doc->find_label(only->id);
                return l.text.empty() ? std::string("label") : l.text;
            }
            }
        }
        return count == 1 ? std::string("1 item") : std::format("{} items", count);
    }

    bool schematic_tools::delete_selection()
    {
        if (m_doc == nullptr)
            return false;
        interrupt();
        abort_gesture();
        const std::vector<item_ref> items(selection().begin(), selection().end());
        if (items.empty())
            return false;
        const std::size_t erased = m_doc->erase(items);
        set_selection({});
        touch();
        return erased > 0;
    }

    bool schematic_tools::rotate_selection(bool clockwise)
    {
        if (m_doc == nullptr)
            return false;
        if (m_gesture == gesture::moving && m_doc->in_transaction())
        {
            // The items being dragged turn, in the drag's step: the drag is
            // done again from its start with the turn in it.
            const auto pivot = schematic::rotation_pivot(*sheet(), m_drag_items, m_style);
            if (!pivot)
                return false;
            m_turns.push_back(
                {.mirror = false, .clockwise = clockwise, .vertical_axis = false, .about = *pivot - m_applied});
            redo_drag(m_applied);
            touch();
            return true;
        }
        if (in_gesture())
            return false;
        interrupt();
        if (m_paste)
        {
            m_paste = turned(*m_paste, clockwise);
            touch();
            return true;
        }
        if (m_tool == tool::part && m_part)
        {
            m_part->rotation = quarter(m_part->rotation + (clockwise ? 3 : 1));
            touch();
            return true;
        }
        if (const auto kind = placing_label())
        {
            std::uint8_t &r = m_label_rotation[kind_index(*kind)];
            r = quarter(r + (clockwise ? 3 : 1));
            touch();
            return true;
        }
        const std::vector<item_ref> items(selection().begin(), selection().end());
        const auto pivot = schematic::rotation_pivot(*sheet(), items, m_style);
        if (!pivot)
            return false;
        return m_doc->rotate(items, *pivot, clockwise);
    }

    bool schematic_tools::mirror_selection(bool vertical_axis)
    {
        if (m_doc == nullptr)
            return false;
        if (m_gesture == gesture::moving && m_doc->in_transaction())
        {
            const auto pivot = schematic::rotation_pivot(*sheet(), m_drag_items, m_style);
            if (!pivot)
                return false;
            m_turns.push_back(
                {.mirror = true, .clockwise = false, .vertical_axis = vertical_axis, .about = *pivot - m_applied});
            redo_drag(m_applied);
            touch();
            return true;
        }
        if (in_gesture())
            return false;
        interrupt();
        if (m_paste)
        {
            m_paste = mirrored(*m_paste, vertical_axis);
            touch();
            return true;
        }
        if (m_tool == tool::part && m_part)
        {
            // As document::mirror() turns a part (see there).
            m_part->mirrored = !m_part->mirrored;
            m_part->rotation = quarter(vertical_axis ? -m_part->rotation : 2 - m_part->rotation);
            touch();
            return true;
        }
        if (const auto kind = placing_label())
        {
            std::uint8_t &r = m_label_rotation[kind_index(*kind)];
            r = quarter(vertical_axis ? 2 - r : -r);
            touch();
            return true;
        }
        const std::vector<item_ref> items(selection().begin(), selection().end());
        const auto pivot = schematic::rotation_pivot(*sheet(), items, m_style);
        if (!pivot)
            return false;
        return m_doc->mirror(items, *pivot, vertical_axis);
    }

    std::optional<schematic::fragment> schematic_tools::selection_fragment() const
    {
        if (m_doc == nullptr)
            return std::nullopt;
        const std::span<const item_ref> items = selection();
        const auto pivot = schematic::rotation_pivot(*sheet(), items, m_style);
        if (items.empty() || !pivot)
            return std::nullopt;
        schematic::fragment f = m_doc->copy(items);
        if (f.empty())
            return std::nullopt;
        return shifted(std::move(f), point{} - *pivot);
    }

    bool schematic_tools::copy()
    {
        interrupt();
        auto f = selection_fragment();
        if (!f)
            return false;
        m_clipboard = std::move(*f);
        touch();
        return true;
    }

    bool schematic_tools::cut()
    {
        if (m_doc == nullptr)
            return false;
        interrupt();
        abort_gesture();
        auto f = selection_fragment();
        if (!f)
            return false;
        m_clipboard = std::move(*f);
        const std::vector<item_ref> items(selection().begin(), selection().end());
        m_doc->begin("Cut " + describe(items));
        (void)m_doc->erase(items);
        m_doc->commit();
        set_selection({});
        touch();
        return true;
    }

    bool schematic_tools::paste()
    {
        if (m_doc == nullptr || m_clipboard.empty())
            return false;
        interrupt();
        abort_gesture();
        m_paste = m_clipboard;
        if (m_has_pointer)
            m_ghost_at = snap_point(m_pointer, m_mods);
        refresh_hover();
        touch();
        return true;
    }

    bool schematic_tools::duplicate()
    {
        if (m_doc == nullptr)
            return false;
        interrupt();
        auto f = selection_fragment();
        if (!f)
            return false;
        abort_gesture();
        m_paste = std::move(*f);
        if (m_has_pointer)
            m_ghost_at = snap_point(m_pointer, m_mods);
        refresh_hover();
        touch();
        return true;
    }

    bool schematic_tools::nudge(int dx, int dy)
    {
        return nudge(dx, dy, false);
    }

    bool schematic_tools::nudge(int dx, int dy, bool detached)
    {
        if (m_doc == nullptr || in_gesture() || (dx == 0 && dy == 0))
            return false;
        const std::vector<item_ref> items(selection().begin(), selection().end());
        if (items.empty())
            return false;
        if (m_nudge_key.empty())
            m_nudge_key = "nudge:" + std::to_string(++m_serial);
        const point delta{dx * m_grid, dy * m_grid};
        bool moved = false;
        if (detached)
        {
            // The items alone; what they are dropped on cut, what they
            // touch tidied.
            const schematic::sheet before = m_doc->sheet();
            m_doc->begin("Move " + describe(items));
            moved = m_doc->move(items, delta);
            if (moved)
                (void)m_doc->finish_move(before, items, m_rules);
            m_doc->commit(m_nudge_key);
        }
        else
            moved = m_doc->move_with_wires(items, delta, m_nudge_key, m_rules);
        touch();
        return moved;
    }

    bool schematic_tools::cancel()
    {
        interrupt();
        switch (m_gesture)
        {
        case gesture::pressed:
        case gesture::moving:
        case gesture::wire_end:
        case gesture::band:
            abort_gesture();
            return true;
        case gesture::wiring:
            return finish_wire();
        case gesture::none:
            break;
        }
        if (m_paste)
        {
            end_paste();
            return true;
        }
        if (m_tool != tool::select)
        {
            set_tool(tool::select);
            return true;
        }
        if (!selection().empty())
        {
            set_selection({});
            return true;
        }
        return false;
    }

    std::string schematic_tools::clipboard_text() const
    {
        if (m_clipboard.empty())
            return {};
        return schematic::write_sheet(sheet_of(m_clipboard));
    }

    bool schematic_tools::set_clipboard_text(std::string_view text)
    {
        auto loaded = schematic::parse_sheet(text);
        if (!loaded)
            return false;
        schematic::fragment f = fragment_of(loaded->sheet);
        if (f.empty())
            return false;
        m_clipboard = std::move(f);
        touch();
        return true;
    }

    // ---- undo and redo ----------------------------------------------------------------------

    bool schematic_tools::undo()
    {
        if (m_doc == nullptr)
            return false;
        interrupt();
        abort_gesture();
        const bool done = m_doc->undo();
        touch();
        prune();
        refresh_hover();
        return done;
    }

    bool schematic_tools::redo()
    {
        if (m_doc == nullptr)
            return false;
        interrupt();
        abort_gesture();
        const bool done = m_doc->redo();
        touch();
        prune();
        refresh_hover();
        return done;
    }

    bool schematic_tools::can_undo() const noexcept
    {
        if (m_doc == nullptr)
            return false;
        if (dragging() && m_doc->in_transaction())
            return m_doc->history().position() > 0;
        return m_doc->can_undo();
    }

    bool schematic_tools::can_redo() const noexcept
    {
        if (m_doc == nullptr)
            return false;
        if (dragging() && m_doc->in_transaction())
            return m_doc->history().position() < m_doc->history().steps().size();
        return m_doc->can_redo();
    }

    std::string_view schematic_tools::undo_label() const noexcept
    {
        if (m_doc == nullptr)
            return {};
        if (dragging() && m_doc->in_transaction())
        {
            const std::size_t position = m_doc->history().position();
            return position > 0 ? std::string_view(m_doc->history().steps()[position - 1].label) : std::string_view();
        }
        return m_doc->undo_label();
    }

    std::string_view schematic_tools::redo_label() const noexcept
    {
        if (m_doc == nullptr)
            return {};
        if (dragging() && m_doc->in_transaction())
        {
            const auto steps = m_doc->history().steps();
            const std::size_t position = m_doc->history().position();
            return position < steps.size() ? std::string_view(steps[position].label) : std::string_view();
        }
        return m_doc->redo_label();
    }

    // ---- what is shown ----------------------------------------------------------------------

    void schematic_tools::append_overlay(std::vector<schematic::segment> &out, const schematic::draw_style &style) const
    {
        if (m_doc == nullptr)
            return;
        switch (m_gesture)
        {
        case gesture::band:
            schematic::append_rect(out, round_point(m_press_at), round_point(m_pointer), style);
            return;
        case gesture::wiring:
        {
            const std::vector<point> path = wire_preview();
            schematic::append_wire_preview(out, path, style);
            if (m_target_connection)
                schematic::append_marker(out, m_target_connection->at, style);
            return;
        }
        case gesture::wire_end:
            if (m_end_connection)
                schematic::append_marker(out, m_end_connection->at, style);
            return;
        case gesture::pressed:
        case gesture::moving:
            return;
        case gesture::none:
            break;
        }

        if (m_paste)
        {
            if (m_has_pointer)
                schematic::append_fragment(out, *m_paste, m_ghost_at, style, style.ghost);
            return;
        }
        switch (m_tool)
        {
        case tool::select:
            break;
        case tool::wire:
            if (m_has_pointer && m_hover_connection)
                schematic::append_marker(out, m_hover_connection->at, style);
            break;
        case tool::part:
            if (m_has_pointer && m_part)
                schematic::append_fragment(out, std::span<const schematic::part>(&*m_part, 1), {}, {}, m_ghost_at,
                                           style, style.ghost);
            break;
        case tool::label:
        case tool::power:
        case tool::text:
            if (m_has_pointer)
            {
                const label_kind kind = *placing_label();
                schematic::label l;
                l.kind = kind;
                l.rotation = label_rotation(kind);
                l.text = label_text(kind);
                schematic::append_fragment(out, {}, {}, std::span<const schematic::label>(&l, 1), m_ghost_at, style,
                                           style.ghost);
                if (m_hover_connection)
                    schematic::append_marker(out, m_ghost_at, style);
            }
            break;
        }
    }

    std::string schematic_tools::hint() const
    {
        if (m_doc == nullptr)
            return "No sheet open";
        switch (m_gesture)
        {
        case gesture::pressed:
            return "Drag to move, or release to select";
        case gesture::moving:
            return std::format("Moving {}{}: release to drop, R to rotate, X/Y to mirror, Ctrl to move off the "
                               "grid, Esc to cancel",
                               describe(selection()), m_detached ? " alone, leaving its wires" : "");
        case gesture::wire_end:
            return "Moving a wire end: it snaps to pins and wires; release to drop, Ctrl to move off the grid, "
                   "Esc to cancel";
        case gesture::band:
            return round_point(m_pointer).x() < round_point(m_press_at).x()
                       ? "Release to select everything the rectangle touches"
                       : "Release to select everything inside the rectangle";
        case gesture::wiring:
            return "Wire: click to add a corner, double-click or click a pin to finish, Space to flip the corner, "
                   "Backspace to undo a corner, Esc to stop";
        case gesture::none:
            break;
        }
        if (m_paste)
        {
            const std::size_t n = m_paste->parts.size() + m_paste->wires.size() + m_paste->labels.size();
            return std::format("Paste {}: click to drop, R to rotate, X/Y to mirror, Esc to cancel",
                               n == 1 ? std::string("1 item") : std::format("{} items", n));
        }
        switch (m_tool)
        {
        case tool::select:
            return "Select: click to select, drag to move (the wires follow; Alt to leave them), drag on empty "
                   "space to select an area (left to right: inside, right to left: touching), double-click to edit";
        case tool::wire:
            return "Wire: click to start a wire (it snaps to pins and wire ends), Esc for Select";
        case tool::part:
            if (!m_part)
                return "Part: choose a part in the Parts panel";
            return std::format("Place {}: click to place (on a wire, it cuts the wire), R to rotate, X/Y to "
                               "mirror, Ctrl to place off the grid, Esc to stop",
                               m_part->value.empty() ? m_part->symbol : m_part->value);
        case tool::label:
        case tool::power:
        case tool::text:
        {
            const label_kind kind = *placing_label();
            return std::format("{} \"{}\": click to place{}, R to rotate, Esc to stop", kind_title(kind),
                               label_text(kind), kind == label_kind::note ? "" : " on a wire or pin");
        }
        }
        return {};
    }

    schematic_tools::pointer_shape schematic_tools::cursor() const
    {
        switch (m_gesture)
        {
        case gesture::moving:
        case gesture::wire_end:
        case gesture::pressed:
            return pointer_shape::move;
        case gesture::band:
        case gesture::wiring:
            return pointer_shape::crosshair;
        case gesture::none:
            break;
        }
        if (m_paste || m_tool != tool::select)
            return pointer_shape::crosshair;
        return hover() ? pointer_shape::move : pointer_shape::arrow;
    }

} // namespace volt::ui
