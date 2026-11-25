// Ready-made changes: value_change, the table of records and its
// record_change (edits, exact inverses, merging, commuting), a document of
// several members through member_change and variant_change, and a long
// random run of table edits through an undo stack.

#include "../math/check.hpp"

#include <history/record.hpp>
#include <history/stack.hpp>

#include <cstddef>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace volt;

namespace
{
    struct part
    {
        std::string value;
        int x = 0;
        int y = 0;

        bool operator==(const part &) const = default;
    };

    using parts = history::table<std::string, part>;
    using parts_change = parts::change;
    using parts_stack = history::stack<parts, parts_change>;

    static_assert(history::change_of<parts_change, parts>);
    static_assert(history::mergeable<parts_change>);
    static_assert(history::commutable<parts_change>);
    static_assert(history::change_of<history::value_change<part>, part>);
    static_assert(history::mergeable<history::value_change<part>>);
    static_assert(history::emptiable<history::value_change<part>>);
    static_assert(history::emptiable<parts_change>);

    // A value that does not compare: a value_change of it still works, but
    // cannot say when it is empty.
    struct opaque
    {
        int n = 0;
    };

    static_assert(history::change_of<history::value_change<opaque>, opaque>);
    static_assert(history::mergeable<history::value_change<opaque>>);
    static_assert(!history::emptiable<history::value_change<opaque>>);

    // The ids in order.
    std::string order(const parts &t)
    {
        std::string ids;
        for (const auto &e : t)
            ids += e.id;
        return ids;
    }

    parts make_parts(std::string_view ids)
    {
        parts t;
        for (const char c : ids)
            t.insert(std::string(1, c), part{std::string(1, c), 0, 0});
        return t;
    }

    // Applying the change to `before` gives `after`, reverting it from
    // `after` gives `before`.
    bool inverse(const parts &before, const parts &after, const parts_change &c)
    {
        parts forward = before;
        c.apply(forward);
        parts back = after;
        c.revert(back);
        return forward == after && back == before;
    }

    // The index agrees with the order.
    bool indexed(const parts &t)
    {
        for (std::size_t i = 0; i < t.size(); ++i)
        {
            if (t.position_of(t[i].id) != i || t.find(t[i].id) != &t[i].record)
                return false;
        }
        return true;
    }

    void value_changes()
    {
        std::string doc = "a";
        history::value_change<std::string> c{"a", "b"};
        c.apply(doc);
        CHECK(doc == "b");
        c.revert(doc);
        CHECK(doc == "a");

        CHECK(c.merge({"b", "c"}));
        CHECK(c.before == "a" && c.after == "c");

        CHECK(!history::make_value_change<std::string>("x", "x"));
        const auto made = history::make_value_change<std::string>("x", "y");
        CHECK(made && made->before == "x" && made->after == "y");

        // A part edited as a whole, typing merged into one step.
        part p{"10k", 0, 0};
        history::stack<part, history::value_change<part>> undo;
        for (const char *value : {"1", "10", "100", "100k"})
        {
            part edited = p;
            edited.value = value;
            if (auto change = history::make_value_change(p, edited))
                undo.perform(p, "Edit value", std::move(*change), "type:value");
        }
        part moved = p;
        moved.x = 5;
        undo.perform(p, "Move", *history::make_value_change(p, moved));
        CHECK(undo.steps().size() == 2);
        CHECK(undo.steps()[0].changes.size() == 1);
        CHECK(undo.undo(p) && p == part{"100k", 0, 0});
        CHECK(undo.undo(p) && p == part{"10k", 0, 0});
        CHECK(undo.redo(p) && undo.redo(p) && p == part{"100k", 5, 0});

        // Typed and deleted again: the step goes, and Undo is the one before.
        CHECK(history::value_change<std::string>{"a", "a"}.empty());
        CHECK(!history::value_change<std::string>{"a", "b"}.empty());
        for (const char *value : {"100k ", "100k"})
        {
            part edited = p;
            edited.value = value;
            undo.perform(p, "Edit value", *history::make_value_change(p, edited), "type:value");
        }
        CHECK(undo.steps().size() == 2 && undo.undo_label() == "Move");

        // Without ==, every change stays.
        opaque o;
        history::stack<opaque, history::value_change<opaque>> blind;
        blind.perform(o, "Set", {opaque{0}, opaque{1}}, "set");
        blind.perform(o, "Set", {opaque{1}, opaque{0}}, "set");
        CHECK(blind.steps().size() == 1 && o.n == 0);
        CHECK(blind.undo(o) && o.n == 0);
    }

    void table_basics()
    {
        parts t;
        CHECK(t.empty() && t.size() == 0);

        const auto a = t.insert("a", {"A", 1, 1});
        CHECK(a && a->id == "a" && !a->before && a->after == part{"A", 1, 1} && a->to == 0);
        CHECK(t.insert("c", {"C", 3, 3}));
        CHECK(t.insert("b", {"B", 2, 2}, 1));
        CHECK(order(t) == "abc");
        CHECK(t.size() == 3 && indexed(t));

        // Refused: an id that is there, a position past the end.
        CHECK(!t.insert("a", {"again", 0, 0}));
        CHECK(!t.insert("d", {"D", 0, 0}, 4));
        CHECK(t.insert("d", {"D", 0, 0}, 3));
        CHECK(order(t) == "abcd");

        CHECK(t.contains("b") && !t.contains("z"));
        CHECK(t.find("c") && t.find("c")->value == "C");
        CHECK(t.find("z") == nullptr);
        CHECK(t.position_of("d") == 3u && !t.position_of("z"));
        CHECK(t.entries().size() == 4 && t[1].id == "b");

        // Replace: missing, or the same value, is nothing.
        CHECK(!t.replace("z", {"Z", 0, 0}));
        CHECK(!t.replace("b", {"B", 2, 2}));
        const auto r = t.replace("b", {"B2", 2, 2});
        CHECK(r && r->before == part{"B", 2, 2} && r->after == part{"B2", 2, 2} && r->from == 1 && r->to == 1);
        CHECK(t.find("b")->value == "B2" && order(t) == "abcd");

        // Erase.
        CHECK(!t.erase("z"));
        const auto e = t.erase("b");
        CHECK(e && e->before == part{"B2", 2, 2} && !e->after && e->from == 1);
        CHECK(order(t) == "acd" && indexed(t));

        // Move: to a position counted with the record put back.
        CHECK(!t.move("z", 0));
        CHECK(!t.move("a", 0)); // already there
        CHECK(!t.move("a", 3)); // past the end
        const auto m = t.move("a", 2);
        CHECK(m && m->from == 0 && m->to == 2 && m->before == m->after);
        CHECK(order(t) == "cda" && indexed(t));
        CHECK(t.move("a", 0) && order(t) == "acd" && indexed(t));
        CHECK(t.move("d", 1) && order(t) == "adc" && indexed(t));

        // Equal means the same records in the same order.
        CHECK(make_parts("abc") == make_parts("abc"));
        CHECK(!(make_parts("abc") == make_parts("acb")));
    }

    void changes_are_inverses()
    {
        const parts start = make_parts("abcde");

        auto check_edit = [&](auto edit) {
            parts t = start;
            const auto c = edit(t);
            CHECK(c.has_value());
            if (c)
            {
                CHECK(inverse(start, t, *c));
                parts back = t;
                c->revert(back);
                CHECK(indexed(back) && order(back) == "abcde");
            }
        };

        for (std::size_t at = 0; at <= 5; ++at)
            check_edit([at](parts &t) { return t.insert("x", {"X", 9, 9}, at); });
        for (const char *id : {"a", "c", "e"})
        {
            check_edit([id](parts &t) { return t.replace(id, {"new", 1, 2}); });
            check_edit([id](parts &t) { return t.erase(id); });
        }
        for (std::size_t to = 0; to < 5; ++to)
        {
            if (to != 2)
                check_edit([to](parts &t) { return t.move("c", to); });
        }
    }

    void undoing_an_erase_keeps_the_order()
    {
        parts t = make_parts("abcde");
        const parts start = t;
        parts_stack undo;
        for (const char *id : {"c", "a", "e"})
        {
            auto c = t.erase(id);
            CHECK(c.has_value());
            if (c)
                undo.record(std::move(*c));
        }
        CHECK(order(t) == "bd");
        CHECK(undo.undo(t) && order(t) == "bde");
        CHECK(undo.undo(t) && order(t) == "abde");
        CHECK(undo.undo(t) && order(t) == "abcde");
        CHECK(t == start && indexed(t));

        // All in one step, undone together.
        auto tx = undo.transact(t, "Delete");
        for (const char *id : {"b", "d", "a"})
            tx.record(*t.erase(id));
        CHECK(tx.commit());
        CHECK(order(t) == "ce");
        CHECK(undo.undo(t) && t == start);
    }

    // `first` then `later`, from `start`, as one merged change: the same
    // result, and reverting it gives `start` back.
    bool merges_exactly(const parts &start, const parts &end, parts_change first, const parts_change &later)
    {
        if (!first.merge(later))
            return false;
        return inverse(start, end, first);
    }

    void merging()
    {
        const parts start = make_parts("abcd");

        // insert then replace: one insert of the new value.
        {
            parts t = start;
            auto first = *t.insert("x", {"X", 0, 0}, 2);
            const auto later = *t.replace("x", {"X2", 1, 1});
            CHECK(merges_exactly(start, t, first, later));
            CHECK(first.merge(later) && !first.before && first.after == part{"X2", 1, 1} && first.to == 2);
        }
        // replace then replace.
        {
            parts t = start;
            auto first = *t.replace("b", {"B1", 0, 0});
            const auto later = *t.replace("b", {"B2", 0, 0});
            CHECK(merges_exactly(start, t, first, later));
            CHECK(first.merge(later) && first.before == part{"b", 0, 0} && first.after == part{"B2", 0, 0});
        }
        // replace then erase: an erase of the original, where it was.
        {
            parts t = start;
            auto first = *t.replace("c", {"C1", 0, 0});
            const auto later = *t.erase("c");
            CHECK(merges_exactly(start, t, first, later));
            CHECK(first.merge(later) && first.before == part{"c", 0, 0} && !first.after && first.from == 2);
        }
        // insert then erase: nothing.
        {
            parts t = start;
            auto first = *t.insert("x", {"X", 0, 0}, 1);
            const auto later = *t.erase("x");
            CHECK(first.merge(later));
            CHECK(!first.before && !first.after && first.keeps_positions() && first.empty());
            parts u = start;
            first.apply(u);
            CHECK(u == start);
            first.revert(u);
            CHECK(u == start);
        }
        // move then move, move then replace, replace then move.
        {
            parts t = start;
            auto first = *t.move("a", 2);
            const auto later = *t.move("a", 3);
            CHECK(merges_exactly(start, t, first, later));
        }
        {
            parts t = start;
            auto first = *t.move("a", 2);
            const auto later = *t.replace("a", {"A", 5, 5});
            CHECK(merges_exactly(start, t, first, later));
        }
        {
            parts t = start;
            auto first = *t.replace("d", {"D", 5, 5});
            const auto later = *t.move("d", 0);
            CHECK(merges_exactly(start, t, first, later));
        }
        // erase then insert of the same id: one change, moved and replaced.
        {
            parts t = start;
            auto first = *t.erase("b");
            const auto later = *t.insert("b", {"B", 7, 7}, 3);
            CHECK(merges_exactly(start, t, first, later));
            CHECK(first.merge(later) && first.from == 1 && first.to == 3);
        }
        // insert, erase, insert again.
        {
            parts t = start;
            auto first = *t.insert("x", {"X", 0, 0}, 0);
            const auto second = *t.erase("x");
            const auto third = *t.insert("x", {"Y", 0, 0}, 4);
            CHECK(first.merge(second));
            CHECK(merges_exactly(start, t, first, third));
        }
        // Refused: different ids, a change that does not follow.
        {
            parts t = start;
            auto first = *t.replace("a", {"A", 0, 0});
            const auto other = *t.replace("b", {"B", 0, 0});
            CHECK(!first.merge(other));
            CHECK(first.after == part{"A", 0, 0}); // untouched

            parts u = start;
            auto erased = *u.erase("a");
            const auto erased_again = parts_change{"a", part{"a", 0, 0}, std::nullopt, 0, 0};
            CHECK(!erased.merge(erased_again)); // nothing there to erase

            parts v = start;
            auto moved = *v.move("a", 2);
            const auto stale = parts_change{"a", part{"a", 0, 0}, part{"a", 0, 0}, 1, 3};
            CHECK(!moved.merge(stale)); // "a" is at 2, not 1
            CHECK(moved.to == 2);
        }
    }

    void empty_record_changes()
    {
        parts t = make_parts("abcd");
        const parts start = t;

        // Which changes are empty.
        {
            parts u = start;
            CHECK(!u.insert("x", {"X", 0, 0}, 1)->empty());
            CHECK(!u.erase("c")->empty());
        }
        {
            parts u = start;
            auto replace = *u.replace("a", {"A", 0, 0});
            CHECK(!replace.empty());
            CHECK(replace.merge(*u.replace("a", {"a", 0, 0})) && replace.empty());
        }
        {
            parts u = start;
            auto move = *u.move("b", 3);
            CHECK(!move.empty());
            auto changed = move;
            CHECK(move.merge(*u.move("b", 1)) && move.empty());
            const auto back_but_changed = parts_change{"b", part{"b", 0, 0}, part{"B", 0, 0}, 3, 1};
            CHECK(changed.merge(back_but_changed) && !changed.empty());
        }

        // Through a stack: a transaction that comes to nothing is no step.
        parts_stack undo;
        {
            auto tx = undo.transact(t, "Place and delete");
            tx.record(*t.insert("x", {"X", 0, 0}, 2));
            tx.record(*t.replace("a", {"A", 1, 1}));
            tx.record(*t.erase("x"));
            tx.record(*t.replace("a", {"a", 0, 0}));
            CHECK(tx.commit());
        }
        CHECK(t == start && undo.steps().empty() && !undo.modified());

        // A record nudged and nudged back: the merged step goes.
        undo.record("Nudge", *t.replace("b", {"b", 1, 0}), "nudge:b");
        undo.record("Nudge", *t.replace("b", {"b", 1, 1}), "nudge:b");
        CHECK(undo.steps().size() == 1);
        undo.record("Nudge", *t.replace("b", {"b", 0, 0}), "nudge:b");
        CHECK(undo.steps().empty() && !undo.modified() && t == start);
    }

    void commuting()
    {
        parts t = make_parts("abcd");
        const auto replace_a = *t.replace("a", {"A", 0, 0});
        const auto replace_b = *t.replace("b", {"B", 0, 0});
        const auto move_c = *t.move("c", 0);
        const auto erase_d = *t.erase("d");
        const auto insert_e = *t.insert("e", {"E", 0, 0}, 1);
        const auto replace_a2 = *t.replace("a", {"A2", 0, 0});

        CHECK(replace_a.commutes_with(replace_b));
        CHECK(replace_a.commutes_with(move_c) && move_c.commutes_with(replace_a));
        CHECK(replace_b.commutes_with(erase_d) && insert_e.commutes_with(replace_b));
        CHECK(!move_c.commutes_with(erase_d));
        CHECK(!erase_d.commutes_with(insert_e));
        CHECK(!replace_a.commutes_with(replace_a2)); // the same record

        // A replace merged past a move of another record keeps its place.
        parts u = make_parts("abcd");
        const parts start = u;
        parts_stack undo;
        undo.begin("Drag");
        undo.record(*u.insert("x", {"X", 0, 0}, 2));
        undo.record(*u.move("a", 3)); // shifts "x" from 2 to 1
        undo.record(*u.replace("x", {"X", 1, 1}));
        undo.commit();
        CHECK(undo.steps()[0].changes.size() == 2);
        const parts end = u;
        CHECK(undo.undo(u) && u == start && indexed(u));
        CHECK(undo.redo(u) && u == end && indexed(u));
    }

    // ---- a document of several members -----------------------------------------

    struct wire
    {
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;

        bool operator==(const wire &) const = default;
    };

    struct sheet
    {
        history::table<std::string, part> symbols;
        history::table<std::string, wire> wires;
        std::string title;

        bool operator==(const sheet &) const = default;
    };

    using symbols_change = history::member_change<&sheet::symbols>;
    using wires_change = history::member_change<&sheet::wires>;
    using title_change = history::member_change<&sheet::title, history::value_change<std::string>>;
    using sheet_change = history::variant_change<sheet, symbols_change, wires_change, title_change>;

    static_assert(history::change_of<sheet_change, sheet>);
    static_assert(history::mergeable<sheet_change>);
    static_assert(history::commutable<sheet_change>);
    static_assert(history::commutable<symbols_change>);
    static_assert(!history::commutable<title_change>);
    static_assert(history::emptiable<symbols_change> && history::emptiable<title_change>);
    static_assert(history::emptiable<sheet_change>);

    void several_members()
    {
        sheet s;
        history::stack<sheet, sheet_change> undo;

        {
            auto t = undo.transact(s, "Draw");
            t.record(sheet_change{symbols_change{*s.symbols.insert("R1", {"10k", 0, 0})}});
            t.record(sheet_change{symbols_change{*s.symbols.insert("R2", {"1k", 10, 0})}});
            t.record(sheet_change{wires_change{*s.wires.insert("w1", {0, 0, 10, 0})}});
            t.apply(sheet_change{title_change{{"", "Blinky"}}});
            CHECK(t.commit());
        }
        const sheet drawn = s;
        CHECK(s.title == "Blinky" && s.symbols.size() == 2 && s.wires.size() == 1);

        // Drag R1 and the wire together: every move changes both, and they
        // still merge to one change each.
        {
            auto t = undo.transact(s, "Drag");
            for (int step = 1; step <= 50; ++step)
            {
                t.record(sheet_change{symbols_change{*s.symbols.replace("R1", {"10k", step, step})}});
                t.record(sheet_change{wires_change{*s.wires.replace("w1", {step, step, 10, 0})}});
            }
            CHECK(t.commit());
        }
        CHECK(undo.steps().size() == 2 && undo.steps()[1].changes.size() == 2);
        const sheet dragged = s;

        // Retitle, delete R2.
        undo.perform(s, "Retitle", sheet_change{title_change{{s.title, "Blinky 2"}}});
        {
            auto t = undo.transact(s, "Delete");
            t.record(sheet_change{symbols_change{*s.symbols.erase("R2")}});
            CHECK(t.commit());
        }
        const sheet last = s;

        CHECK(undo.undo(s) && undo.undo(s) && s == dragged);
        CHECK(undo.undo(s) && s == drawn);
        CHECK(undo.undo(s) && s == sheet{});
        CHECK(undo.redo(s) && s == drawn);
        CHECK(undo.redo(s) && undo.redo(s) && undo.redo(s) && s == last);
        CHECK(s.symbols.find("R1") && s.symbols.find("R1")->x == 50);

        // Changed and changed back, through the variant: nothing.
        const std::string title = s.title;
        {
            auto t = undo.transact(s, "Retitle and back");
            t.apply(sheet_change{title_change{{title, "x"}}});
            t.apply(sheet_change{title_change{{"x", title}}});
            CHECK(t.commit());
        }
        CHECK(undo.steps().size() == 4 && undo.position() == 4 && s == last);
    }

    // ---- a long random run ---------------------------------------------------------

    void random_edits()
    {
        std::mt19937 rng(20261122u);
        auto pick = [&rng](std::size_t n) { return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng); };
        auto chance = [&rng](int percent) { return std::uniform_int_distribution<int>(0, 99)(rng) < percent; };

        parts t;
        for (int i = 0; i < 40; ++i)
            t.insert("p" + std::to_string(i), part{"v" + std::to_string(i), i, -i});
        const parts initial = t;

        parts_stack undo(history::stack_limits{.max_steps = 0});
        // The table at each position of the stack: states[k] is what it
        // must be with k steps done.
        std::vector<parts> states{t};
        int next_id = 1000;
        int edits = 0;
        int cancelled = 0;
        bool consistent = true;

        // One random edit; nullopt when it did not apply.
        auto random_edit = [&](const std::vector<std::string> &hot) -> std::optional<parts_change> {
            const std::string id = !hot.empty() && chance(60) ? hot[pick(hot.size())]
                                   : t.empty()                ? std::string("none")
                                                              : t[pick(t.size())].id;
            switch (pick(5))
            {
            case 0:
            {
                const int n = next_id++;
                return t.insert("n" + std::to_string(n), part{"new", n, 0}, pick(t.size() + 1));
            }
            case 1:
                return t.erase(id);
            case 2:
                return t.empty() ? std::nullopt : t.move(id, pick(t.size()));
            default:
                return t.replace(id, part{"r" + std::to_string(pick(1000)), static_cast<int>(pick(50)), 0});
            }
        };

        for (int round = 0; round < 1500; ++round)
        {
            // Now and then, go back a few steps or forward again.
            if (chance(15))
            {
                const std::size_t n = pick(4);
                for (std::size_t i = 0; i < n; ++i)
                {
                    if (chance(60) ? !undo.undo(t) : !undo.redo(t))
                        break;
                }
                consistent = consistent && t == states[undo.position()];
            }
            if (chance(10))
                undo.seal();

            // A transaction of a few edits, often to the same records.
            std::vector<std::string> hot;
            for (std::size_t i = 0, n = pick(3); i < n && !t.empty(); ++i)
                hot.push_back(t[pick(t.size())].id);
            const parts before = t;
            const std::size_t steps_before = undo.steps().size();
            const std::size_t position_before = undo.position();

            undo.begin("Edit");
            for (std::size_t i = 0, n = 1 + pick(6); i < n; ++i)
            {
                if (auto c = random_edit(hot))
                {
                    undo.record(std::move(*c));
                    ++edits;
                }
            }
            if (chance(5))
            {
                undo.cancel(t);
                ++cancelled;
                consistent = consistent && t == before && undo.steps().size() == steps_before &&
                             undo.position() == position_before;
                continue;
            }
            const char *keys[] = {"", "", "nudge", "type"};
            undo.commit(keys[pick(4)]);

            // A new step, a merge into the newest, or nothing at all.
            if (undo.position() == position_before && t == before)
                continue;
            states.resize(undo.position());
            states.push_back(t);
            consistent = consistent && undo.position() + 1 == states.size() && indexed(t);
        }
        CHECK(consistent);
        CHECK(edits > 3000);
        CHECK(cancelled > 0);

        // Merging must have happened, in steps and across them.
        std::size_t changes = 0;
        for (const auto &s : undo.steps())
            changes += s.changes.size();
        CHECK(changes < static_cast<std::size_t>(edits));
        CHECK(undo.steps().size() < 1500);

        // Redo whatever was undone last, then all the way back and forward.
        while (undo.redo(t))
            consistent = consistent && t == states[undo.position()];
        const parts final_table = t;
        while (undo.undo(t))
            consistent = consistent && t == states[undo.position()] && indexed(t);
        CHECK(t == initial);
        while (undo.redo(t))
            consistent = consistent && t == states[undo.position()];
        CHECK(t == final_table);
        CHECK(consistent);
    }

} // namespace

int main()
{
    value_changes();
    table_basics();
    changes_are_inverses();
    undoing_an_erase_keeps_the_order();
    merging();
    empty_record_changes();
    commuting();
    several_members();
    random_edits();
    return volt::tests::result("history record");
}
