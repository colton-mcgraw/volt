// The undo stack: steps, transactions (nested, cancelled, empty, guarded),
// merging inside a transaction and across steps, the saved marker, limits,
// inspection, and notification.

#include "../math/check.hpp"

#include <history/stack.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace volt;

namespace
{
    // An edit of text: at `at`, `removed` became `inserted`. Merges typing
    // (an insertion right after this one's); costs its text.
    struct text_change
    {
        std::size_t at = 0;
        std::string removed;
        std::string inserted;

        void apply(std::string &doc) const { doc.replace(at, removed.size(), inserted); }
        void revert(std::string &doc) const { doc.replace(at, inserted.size(), removed); }

        bool merge(const text_change &later)
        {
            if (!later.removed.empty() || later.at != at + inserted.size())
                return false;
            inserted += later.inserted;
            return true;
        }

        std::size_t cost() const { return removed.size() + inserted.size(); }
    };

    text_change typed(std::size_t at, std::string text)
    {
        return {at, {}, std::move(text)};
    }

    // The same, without merging or a cost.
    struct plain_change
    {
        text_change edit;

        void apply(std::string &doc) const { edit.apply(doc); }
        void revert(std::string &doc) const { edit.revert(doc); }
    };

    plain_change plain(std::size_t at, std::string removed, std::string inserted)
    {
        return {{at, std::move(removed), std::move(inserted)}};
    }

    // A value in one slot of a list. Changes to one slot merge; changes to
    // different slots commute, except that slot 9 commutes with nothing.
    struct slot_change
    {
        std::size_t slot = 0;
        int before = 0;
        int after = 0;

        void apply(std::vector<int> &doc) const { doc[slot] = after; }
        void revert(std::vector<int> &doc) const { doc[slot] = before; }

        bool merge(const slot_change &later)
        {
            if (later.slot != slot)
                return false;
            after = later.after;
            return true;
        }

        bool commutes_with(const slot_change &other) const
        {
            return slot != other.slot && slot != 9 && other.slot != 9;
        }

        bool empty() const { return before == after; }
    };

    // An amount added to a number: merges by adding, empty at zero.
    struct add_change
    {
        int amount = 0;

        void apply(int &doc) const { doc += amount; }
        void revert(int &doc) const { doc -= amount; }

        bool merge(const add_change &later)
        {
            amount += later.amount;
            return true;
        }

        bool empty() const { return amount == 0; }
    };

    using add_stack = history::stack<int, add_change>;

    // The whole document, before and after (as history::value_change, which
    // these tests leave to the record tests).
    template <typename T>
    struct whole_change
    {
        T before;
        T after;

        void apply(T &doc) const { doc = after; }
        void revert(T &doc) const { doc = before; }

        bool merge(const whole_change &later)
        {
            after = later.after;
            return true;
        }

        bool empty() const { return before == after; }
    };

    // A change that cannot be copied.
    struct move_only_change
    {
        std::unique_ptr<int> amount;

        void apply(int &doc) const { doc += *amount; }
        void revert(int &doc) const { doc -= *amount; }
    };

    static_assert(history::emptiable<add_change>);
    static_assert(!history::emptiable<text_change>);
    static_assert(std::is_copy_constructible_v<add_stack> && std::is_copy_assignable_v<add_stack>);
    static_assert(!std::is_copy_constructible_v<history::stack<int, move_only_change>>);
    static_assert(!std::is_copy_assignable_v<history::stack<int, move_only_change>>);
    static_assert(std::is_move_constructible_v<history::stack<int, move_only_change>>);

    // The same, merging but not knowing what commutes.
    struct slot_change_nc
    {
        slot_change edit;

        void apply(std::vector<int> &doc) const { edit.apply(doc); }
        void revert(std::vector<int> &doc) const { edit.revert(doc); }
        bool merge(const slot_change_nc &later) { return edit.merge(later.edit); }
    };

    using text_stack = history::stack<std::string, text_change>;
    using plain_stack = history::stack<std::string, plain_change>;

    static_assert(history::change_of<text_change, std::string>);
    static_assert(history::change_of<plain_change, std::string>);
    static_assert(!history::change_of<int, std::string>);
    static_assert(!history::change_of<text_change, std::vector<int>>);
    static_assert(history::mergeable<text_change>);
    static_assert(!history::mergeable<plain_change>);
    static_assert(history::costed<text_change>);
    static_assert(!history::costed<plain_change>);
    static_assert(history::commutable<slot_change>);
    static_assert(!history::commutable<slot_change_nc>);

    void single_steps()
    {
        std::string doc;
        text_stack s;
        CHECK(!s.can_undo() && !s.can_redo());
        CHECK(s.undo_label().empty() && s.redo_label().empty());
        CHECK(!s.undo(doc) && !s.redo(doc));

        s.perform(doc, "Type a", typed(0, "a"));
        s.perform(doc, "Type b", typed(1, "b"));
        s.perform(doc, "Replace", text_change{0, "ab", "xyz"});
        CHECK(doc == "xyz");
        CHECK(s.steps().size() == 3 && s.position() == 3);
        CHECK(s.can_undo() && !s.can_redo());
        CHECK(s.undo_label() == "Replace");

        CHECK(s.undo(doc));
        CHECK(doc == "ab");
        CHECK(s.undo_label() == "Type b" && s.redo_label() == "Replace");
        CHECK(s.undo(doc) && s.undo(doc));
        CHECK(doc.empty());
        CHECK(!s.can_undo() && s.can_redo());
        CHECK(!s.undo(doc));

        CHECK(s.redo(doc));
        CHECK(doc == "a");
        CHECK(s.redo(doc) && s.redo(doc));
        CHECK(doc == "xyz");
        CHECK(!s.redo(doc));

        // An edit after undo discards what was undone.
        CHECK(s.undo(doc) && s.undo(doc));
        CHECK(doc == "a");
        s.perform(doc, "Type c", typed(1, "c"));
        CHECK(doc == "ac");
        CHECK(s.steps().size() == 2 && s.position() == 2);
        CHECK(!s.can_redo());
        CHECK(s.steps()[0].label == "Type a" && s.steps()[1].label == "Type c");
        CHECK(s.undo(doc) && s.undo(doc) && doc.empty());
        CHECK(s.redo(doc) && s.redo(doc) && doc == "ac");
    }

    void transactions()
    {
        std::string doc = "start";
        plain_stack s;

        // Three changes, each depending on the one before: undo must revert
        // them newest first to come back.
        s.begin("Edit");
        CHECK(s.in_transaction());
        s.apply(doc, plain(0, "start", "abc"));
        s.apply(doc, plain(1, "b", "XY"));
        s.apply(doc, plain(0, "aX", ""));
        s.commit();
        CHECK(!s.in_transaction());
        CHECK(doc == "Yc");
        CHECK(s.steps().size() == 1 && s.steps()[0].changes.size() == 3);
        CHECK(s.undo_label() == "Edit");
        CHECK(s.undo(doc) && doc == "start");
        CHECK(s.redo(doc) && doc == "Yc");

        // record(): the caller made the change already.
        doc += "!";
        s.begin("Shout");
        s.record(plain(2, "", "!"));
        s.commit();
        CHECK(s.steps().size() == 2);
        CHECK(s.undo(doc) && doc == "Yc");
        CHECK(s.redo(doc) && doc == "Yc!");

        // Outside a transaction, apply and record make unlabelled steps.
        s.apply(doc, plain(0, "Y", "y"));
        CHECK(doc == "yc!" && s.steps().size() == 3 && s.steps()[2].label.empty());
        doc = "yc?";
        s.record(plain(2, "!", "?"));
        CHECK(s.steps().size() == 4 && !s.in_transaction());
        CHECK(s.undo(doc) && doc == "yc!");
        CHECK(s.undo(doc) && doc == "Yc!");
        CHECK(s.redo(doc) && s.redo(doc) && doc == "yc?");
    }

    void nesting()
    {
        std::string doc;
        plain_stack s;

        s.begin("Outer");
        s.apply(doc, plain(0, "", "a"));
        s.begin("Inner");
        s.apply(doc, plain(1, "", "b"));
        s.commit("ignored");
        CHECK(s.in_transaction());
        s.perform(doc, "Joined", plain(2, "", "c")); // joins too
        CHECK(s.in_transaction());
        s.commit();
        CHECK(!s.in_transaction());
        CHECK(doc == "abc");
        CHECK(s.steps().size() == 1);
        CHECK(s.steps()[0].label == "Outer");
        CHECK(s.steps()[0].changes.size() == 3);
        CHECK(s.steps()[0].merge_key.empty());
        CHECK(s.undo(doc) && doc.empty());
    }

    void empty_transactions()
    {
        std::string doc;
        plain_stack s;
        s.perform(doc, "a", plain(0, "", "a"));
        s.perform(doc, "b", plain(1, "", "b"));
        CHECK(s.undo(doc));

        s.begin("Nothing");
        s.begin("Still nothing");
        s.commit();
        s.commit();
        CHECK(s.steps().size() == 2 && s.position() == 1);
        CHECK(s.can_redo()); // the empty commit discarded nothing
        CHECK(s.redo(doc) && doc == "ab");
    }

    void cancelling()
    {
        std::string doc;
        plain_stack s;
        s.perform(doc, "a", plain(0, "", "a"));
        s.perform(doc, "b", plain(1, "", "b"));
        CHECK(s.undo(doc) && doc == "a");

        s.begin("Doomed");
        s.apply(doc, plain(1, "", "x"));
        s.begin("Inner");
        s.apply(doc, plain(0, "ax", "q"));
        s.record(plain(1, "", "r"));
        doc.insert(1, "r");
        CHECK(doc == "qr");
        s.cancel(doc); // closes both levels
        CHECK(!s.in_transaction());
        CHECK(doc == "a");
        CHECK(s.steps().size() == 2 && s.position() == 1);
        CHECK(s.can_redo() && s.redo_label() == "b"); // nothing discarded

        // Nothing open: nothing happens.
        const auto revision = s.revision();
        s.cancel(doc);
        CHECK(doc == "a" && s.revision() == revision);
    }

    void refused_while_open()
    {
        std::string doc;
        text_stack s;
        s.perform(doc, "a", typed(0, "a"));
        s.perform(doc, "b", typed(1, "b"));
        CHECK(s.undo(doc));

        s.begin("Open");
        CHECK(!s.can_undo() && !s.can_redo());
        CHECK(s.undo_label().empty() && s.redo_label().empty());
        CHECK(!s.undo(doc) && !s.redo(doc));
        CHECK(!s.mark_saved());
        CHECK(doc == "a");
        s.commit();
        CHECK(s.can_undo() && s.can_redo());
        CHECK(s.undo_label() == "a" && s.redo_label() == "b");
    }

    void merging_in_a_transaction()
    {
        // Typing 200 characters is one change.
        {
            std::string doc;
            text_stack s;
            s.begin("Type");
            for (std::size_t i = 0; i < 200; ++i)
                s.apply(doc, typed(i, std::string(1, static_cast<char>('a' + i % 26))));
            s.commit();
            CHECK(doc.size() == 200);
            CHECK(s.steps().size() == 1 && s.steps()[0].changes.size() == 1);
            CHECK(s.steps()[0].changes[0].inserted == doc);
            CHECK(s.undo(doc) && doc.empty());
            CHECK(s.redo(doc) && doc.size() == 200);
        }
        // Without merge, every change stays.
        {
            std::string doc;
            plain_stack s;
            s.begin("Type");
            for (std::size_t i = 0; i < 50; ++i)
                s.apply(doc, plain(i, "", "x"));
            s.commit();
            CHECK(s.steps()[0].changes.size() == 50);
        }
        // A change that cannot merge with the newest is not offered older
        // ones when the type does not say what commutes.
        {
            std::string doc;
            text_stack s;
            s.begin("Type");
            s.apply(doc, typed(0, "ab"));
            s.apply(doc, text_change{0, "a", "A"});
            s.apply(doc, typed(2, "c")); // would merge with the first
            s.commit();
            CHECK(doc == "Abc");
            CHECK(s.steps()[0].changes.size() == 3);
            CHECK(s.undo(doc) && doc.empty());
        }
    }

    void merging_past_commuting_changes()
    {
        // A drag of three slots at once: each mouse move changes all three.
        {
            std::vector<int> doc(10, 0);
            history::stack<std::vector<int>, slot_change> s;
            s.begin("Drag");
            for (int move = 1; move <= 100; ++move)
            {
                for (std::size_t slot = 0; slot < 3; ++slot)
                    s.apply(doc, slot_change{slot, doc[slot], move * 10 + static_cast<int>(slot)});
            }
            s.commit();
            CHECK(doc[0] == 1000 && doc[1] == 1001 && doc[2] == 1002);
            CHECK(s.steps()[0].changes.size() == 3);
            CHECK(s.undo(doc) && doc == std::vector<int>(10, 0));
            CHECK(s.redo(doc) && doc[0] == 1000 && doc[1] == 1001 && doc[2] == 1002);
        }
        // Without commutes_with, only the newest is offered the change.
        {
            std::vector<int> doc(10, 0);
            history::stack<std::vector<int>, slot_change_nc> s;
            s.begin("Drag");
            for (int move = 1; move <= 100; ++move)
            {
                for (std::size_t slot = 0; slot < 3; ++slot)
                    s.apply(doc, slot_change_nc{{slot, doc[slot], move}});
            }
            s.commit();
            CHECK(s.steps()[0].changes.size() == 300);
            CHECK(s.undo(doc) && doc == std::vector<int>(10, 0));
        }
        // A change that does not commute stops the search.
        {
            std::vector<int> doc(10, 0);
            history::stack<std::vector<int>, slot_change> s;
            s.begin("Barrier");
            s.apply(doc, slot_change{0, 0, 1});
            s.apply(doc, slot_change{9, 0, 1});
            s.apply(doc, slot_change{0, 1, 2});
            s.commit();
            CHECK(s.steps()[0].changes.size() == 3);
            CHECK(s.undo(doc) && doc == std::vector<int>(10, 0));
        }
        // The window bounds how far back it looks.
        for (const std::size_t window : {std::size_t{2}, std::size_t{3}})
        {
            std::vector<int> doc(10, 0);
            history::stack<std::vector<int>, slot_change> s(history::stack_limits{.merge_window = window});
            s.begin("Window");
            s.apply(doc, slot_change{0, 0, 1});
            s.apply(doc, slot_change{1, 0, 1});
            s.apply(doc, slot_change{2, 0, 1});
            s.apply(doc, slot_change{0, 1, 2});
            s.commit();
            CHECK(s.steps()[0].changes.size() == (window == 2 ? 4u : 3u));
            CHECK(doc[0] == 2);
            CHECK(s.undo(doc) && doc == std::vector<int>(10, 0));
        }
    }

    void merging_steps()
    {
        // Typing with a key: one step, one change.
        {
            std::string doc;
            text_stack s;
            s.perform(doc, "Typing", typed(0, "a"), "type");
            s.perform(doc, "Typing more", typed(1, "b"), "type");
            s.begin("Typing in a transaction");
            s.apply(doc, typed(2, "c"));
            s.commit("type");
            CHECK(doc == "abc");
            CHECK(s.steps().size() == 1);
            CHECK(s.steps()[0].label == "Typing"); // the first label stays
            CHECK(s.steps()[0].changes.size() == 1);
            CHECK(s.total_cost() == 3);
            CHECK(s.undo(doc) && doc.empty());
            CHECK(s.redo(doc) && doc == "abc");
        }
        // Changes that do not merge are appended to the step.
        {
            std::string doc;
            plain_stack s;
            s.perform(doc, "Nudge", plain(0, "", "a"), "nudge");
            s.perform(doc, "Nudge", plain(0, "a", "b"), "nudge");
            s.perform(doc, "Nudge", plain(0, "b", "c"), "nudge");
            CHECK(s.steps().size() == 1 && s.steps()[0].changes.size() == 3);
            CHECK(s.undo(doc) && doc.empty());
        }
        // A different key, or none, makes a new step.
        {
            std::string doc;
            text_stack s;
            s.perform(doc, "a", typed(0, "a"), "one");
            s.perform(doc, "b", typed(1, "b"), "two");
            s.perform(doc, "c", typed(2, "c"));
            s.perform(doc, "d", typed(3, "d"));
            CHECK(s.steps().size() == 4);
        }
        // Not when there is something to redo.
        {
            std::string doc;
            text_stack s;
            s.perform(doc, "a", typed(0, "a"), "type");
            s.perform(doc, "x", text_change{0, "a", "x"}, "replace");
            CHECK(s.undo(doc) && doc == "a");
            s.perform(doc, "b", typed(1, "b"), "type");
            CHECK(s.steps().size() == 2 && s.steps()[1].label == "b");
            CHECK(s.undo(doc) && doc == "a");
            CHECK(s.undo(doc) && doc.empty());
        }
        // Not after seal().
        {
            std::string doc;
            text_stack s;
            s.perform(doc, "a", typed(0, "a"), "type");
            s.seal();
            s.perform(doc, "b", typed(1, "b"), "type");
            s.perform(doc, "c", typed(2, "c"), "type"); // into "b"
            CHECK(s.steps().size() == 2);
            CHECK(s.steps()[0].sealed && !s.steps()[1].sealed);
            CHECK(s.undo(doc) && doc == "a");
        }
        // Not into the saved state.
        {
            std::string doc;
            text_stack s;
            s.perform(doc, "a", typed(0, "a"), "type");
            CHECK(s.mark_saved());
            s.perform(doc, "b", typed(1, "b"), "type");
            CHECK(s.steps().size() == 2);
            CHECK(s.modified());
            CHECK(s.undo(doc) && !s.modified() && doc == "a");
        }
        // Not into a step that was undone and redone.
        {
            std::string doc;
            text_stack s;
            s.perform(doc, "a", typed(0, "a"), "type");
            CHECK(s.undo(doc) && s.redo(doc));
            s.perform(doc, "b", typed(1, "b"), "type");
            CHECK(s.steps().size() == 2);
        }
        // An empty commit with the key changes nothing.
        {
            std::string doc;
            text_stack s;
            s.perform(doc, "a", typed(0, "a"), "type");
            s.begin("nothing");
            s.commit("type");
            s.perform(doc, "b", typed(1, "b"), "type");
            CHECK(s.steps().size() == 1 && doc == "ab");
        }
    }

    void saved_marker()
    {
        std::string doc;
        text_stack s;
        CHECK(!s.modified());

        s.perform(doc, "a", typed(0, "a"));
        CHECK(s.modified());
        CHECK(s.undo(doc) && !s.modified());
        CHECK(s.redo(doc) && s.modified());

        CHECK(s.mark_saved());
        CHECK(!s.modified());
        s.perform(doc, "b", typed(1, "b"));
        CHECK(s.modified());
        CHECK(s.undo(doc) && !s.modified());
        CHECK(s.undo(doc) && s.modified());
        CHECK(s.redo(doc) && !s.modified());

        // An open transaction with changes is a modification; without, not.
        s.begin("t");
        CHECK(!s.modified());
        s.apply(doc, typed(1, "x"));
        CHECK(s.modified());
        s.cancel(doc);
        CHECK(!s.modified() && doc == "a");

        // Saved, undo, a new edit: the saved state is gone for good.
        s.perform(doc, "b", typed(1, "b"));
        CHECK(s.mark_saved());
        CHECK(s.undo(doc) && s.modified());
        s.perform(doc, "c", typed(1, "c"));
        CHECK(doc == "ac" && s.modified());
        CHECK(s.undo(doc) && s.modified());
        CHECK(s.undo(doc) && s.modified());
        CHECK(s.redo(doc) && s.redo(doc) && s.modified());
        // ... until the next save.
        CHECK(s.mark_saved());
        CHECK(!s.modified());
        CHECK(s.undo(doc) && s.modified());
        CHECK(s.redo(doc) && !s.modified());

        // An empty transaction or a cancelled one leaves the saved state
        // reachable, redo included.
        CHECK(s.undo(doc) && s.modified());
        s.begin("nothing");
        s.commit();
        s.begin("cancelled");
        s.apply(doc, typed(0, "z"));
        s.cancel(doc);
        CHECK(s.redo(doc) && !s.modified());
    }

    void clearing()
    {
        std::string doc;
        text_stack s;
        s.perform(doc, "a", typed(0, "a"));
        CHECK(s.mark_saved());
        s.clear();
        CHECK(s.steps().empty() && s.position() == 0 && s.total_cost() == 0);
        CHECK(!s.can_undo() && !s.modified());
        s.perform(doc, "b", typed(1, "b"));
        CHECK(s.modified());
        CHECK(s.undo(doc) && !s.modified() && doc == "a");

        // Modified when cleared: modified for good.
        s.perform(doc, "c", typed(1, "c"));
        s.clear();
        CHECK(s.modified() && !s.can_undo());
        s.perform(doc, "d", typed(2, "d"));
        CHECK(s.undo(doc) && s.modified());

        // An open transaction survives a clear.
        s.begin("open");
        s.apply(doc, typed(0, "q"));
        s.clear();
        CHECK(s.in_transaction());
        s.cancel(doc);
        CHECK(doc == "ac");
    }

    void step_limit()
    {
        std::string doc;
        text_stack s(history::stack_limits{.max_steps = 3});
        for (char c = 'a'; c <= 'e'; ++c)
            s.perform(doc, std::string(1, c), typed(doc.size(), std::string(1, c)));
        CHECK(doc == "abcde");
        CHECK(s.steps().size() == 3 && s.position() == 3);
        CHECK(s.steps()[0].label == "c");
        CHECK(s.total_cost() == 3);
        CHECK(s.undo(doc) && s.undo(doc) && s.undo(doc));
        CHECK(doc == "ab");
        CHECK(!s.can_undo());
        CHECK(s.redo(doc) && s.redo(doc) && s.redo(doc) && doc == "abcde");

        // 0: no limit.
        text_stack unlimited(history::stack_limits{.max_steps = 0});
        std::string other;
        for (std::size_t i = 0; i < 2000; ++i)
            unlimited.perform(other, "x", typed(i, "x"));
        CHECK(unlimited.steps().size() == 2000);

        // The default.
        CHECK(text_stack().limits().max_steps == 1000);
    }

    void saved_marker_and_limits()
    {
        // The saved state dropped off the bottom: modified for good.
        {
            std::string doc;
            text_stack s(history::stack_limits{.max_steps = 2});
            s.perform(doc, "a", typed(0, "a"));
            CHECK(s.mark_saved());
            s.perform(doc, "b", typed(1, "b"));
            CHECK(s.undo(doc) && !s.modified());
            CHECK(s.redo(doc));
            s.perform(doc, "c", typed(2, "c")); // drops "a"; "saved" is position 0 now
            CHECK(s.undo(doc) && s.undo(doc) && doc == "a" && !s.modified());
            CHECK(s.redo(doc) && s.redo(doc));
            s.perform(doc, "d", typed(3, "d")); // drops "b": the saved state goes
            CHECK(s.undo(doc) && s.undo(doc) && doc == "ab");
            CHECK(s.modified());
        }
        // The saved state above what is dropped moves down with it.
        {
            std::string doc;
            text_stack s(history::stack_limits{.max_steps = 0});
            for (char c = 'a'; c <= 'e'; ++c)
                s.perform(doc, std::string(1, c), typed(doc.size(), std::string(1, c)));
            CHECK(s.undo(doc));
            CHECK(s.mark_saved()); // at "abcd", position 4
            CHECK(s.redo(doc) && s.modified());
            s.set_limits({.max_steps = 2});
            CHECK(s.steps().size() == 2 && s.position() == 2);
            CHECK(s.undo(doc) && !s.modified() && doc == "abcd");
            CHECK(s.undo(doc) && s.modified() && !s.can_undo());
        }
    }

    void cost_limit()
    {
        std::string doc;
        text_stack s(history::stack_limits{.max_steps = 0, .max_cost = 10});
        s.perform(doc, "4", typed(0, "aaaa"));
        s.perform(doc, "4", typed(4, "bbbb"));
        CHECK(s.steps().size() == 2 && s.total_cost() == 8);
        s.perform(doc, "3", typed(8, "ccc")); // 11: drops the first
        CHECK(s.steps().size() == 2 && s.total_cost() == 7);
        CHECK(s.steps()[0].changes[0].inserted == "bbbb");

        // A step bigger than the limit by itself stays: it is the newest.
        s.perform(doc, "20", typed(11, std::string(20, 'd')));
        CHECK(s.steps().size() == 1 && s.total_cost() == 20);
        CHECK(s.undo(doc) && doc == "aaaabbbbccc");
        CHECK(!s.can_undo());

        // Discarded redo steps leave the total.
        s.perform(doc, "1", typed(11, "e"));
        CHECK(s.steps().size() == 1 && s.total_cost() == 1);

        // Changes without cost() count their size.
        plain_stack p;
        std::string other;
        p.perform(other, "x", plain(0, "", "x"));
        CHECK(p.total_cost() == sizeof(plain_change));
    }

    void lowering_limits_keeps_the_newest_done()
    {
        std::string doc;
        text_stack s(history::stack_limits{.max_steps = 0});
        for (char c = 'a'; c <= 'e'; ++c)
            s.perform(doc, std::string(1, c), typed(doc.size(), std::string(1, c)));
        CHECK(s.undo(doc) && s.undo(doc) && s.undo(doc)); // position 2, three to redo
        const auto revision = s.revision();
        s.set_limits({.max_steps = 1});
        CHECK(s.revision() != revision);
        // Only the step below the position could go; redo steps stay.
        CHECK(s.position() == 1 && s.steps().size() == 4);
        CHECK(s.undo(doc) && doc == "a" && !s.can_undo());
        CHECK(s.redo(doc) && s.redo(doc) && s.redo(doc) && s.redo(doc) && doc == "abcde");

        // Raising them changes nothing.
        const auto later = s.revision();
        s.set_limits({.max_steps = 100});
        CHECK(s.revision() == later);
    }

    void guards()
    {
        std::string doc;
        plain_stack s;

        // Committed.
        {
            auto t = s.transact(doc, "Edit");
            CHECK(t.open() && s.in_transaction());
            CHECK(t.apply(plain(0, "", "a")));
            doc += "b";
            CHECK(t.record(plain(1, "", "b")));
            CHECK(t.commit());
            CHECK(!t.open());
            CHECK(!t.commit()); // once
            CHECK(!t.apply(plain(0, "", "z")) && doc == "ab");
        }
        CHECK(!s.in_transaction() && s.steps().size() == 1 && doc == "ab");

        // Not committed: cancelled.
        {
            auto t = s.transact(doc, "Doomed");
            t.apply(plain(2, "", "c"));
            CHECK(doc == "abc");
        }
        CHECK(!s.in_transaction() && s.steps().size() == 1 && doc == "ab");

        // Nested, both committed: one step.
        {
            auto outer = s.transact(doc, "Outer");
            outer.apply(plain(2, "", "c"));
            {
                auto inner = s.transact(doc, "Inner");
                inner.apply(plain(3, "", "d"));
                // Levels close innermost first.
                CHECK(!outer.commit());
                CHECK(inner.commit());
            }
            CHECK(outer.commit());
        }
        CHECK(s.steps().size() == 2 && s.steps()[1].label == "Outer" && doc == "abcd");

        // The inner one not committed cancels everything; the outer one
        // finds its transaction gone.
        {
            auto outer = s.transact(doc, "Outer");
            outer.apply(plain(4, "", "e"));
            {
                auto inner = s.transact(doc, "Inner");
                inner.apply(plain(5, "", "f"));
            }
            CHECK(!s.in_transaction() && doc == "abcd");
            CHECK(!outer.open());
            CHECK(!outer.apply(plain(0, "", "z")));
            CHECK(!outer.commit());

            // Nor does it reach into a transaction opened since.
            s.begin("Someone else's");
            CHECK(!outer.open() && !outer.cancel());
            s.apply(doc, plain(4, "", "g"));
        }
        CHECK(s.in_transaction() && doc == "abcdg"); // the outer guard left it alone
        s.commit();
        CHECK(s.steps().size() == 3);

        // An explicit cancel.
        {
            auto t = s.transact(doc, "Cancelled");
            t.apply(plain(0, "", "!"));
            CHECK(t.cancel());
            CHECK(!t.cancel() && !t.open() && doc == "abcdg");
        }
        CHECK(s.steps().size() == 3);

        // With a merge key.
        text_stack typing;
        std::string text;
        for (std::size_t i = 0; i < 3; ++i)
        {
            auto t = typing.transact(text, "Typing");
            t.apply(typed(i, "x"));
            t.commit("type");
        }
        CHECK(typing.steps().size() == 1 && text == "xxx");
    }

    void notification()
    {
        std::string doc;
        text_stack s;
        int calls = 0;
        s.on_change([&] { ++calls; });

        auto revision = s.revision();
        s.perform(doc, "a", typed(0, "a"));
        CHECK(calls == 1); // once for the whole perform
        CHECK(s.revision() > revision);

        calls = 0;
        s.begin("t"); // can_undo turns false
        CHECK(calls == 1);
        s.apply(doc, typed(1, "b")); // modified may change
        CHECK(calls == 2);
        s.apply(doc, typed(2, "c")); // nothing visible changes
        CHECK(calls == 2);
        s.commit();
        CHECK(calls == 3);

        calls = 0;
        CHECK(s.undo(doc));
        CHECK(s.redo(doc));
        CHECK(s.mark_saved());
        CHECK(calls == 3);

        // Refused and invisible operations: no call, same revision.
        calls = 0;
        revision = s.revision();
        CHECK(!s.redo(doc));
        s.seal();
        s.cancel(doc);
        CHECK(calls == 0 && s.revision() == revision);

        s.on_change(nullptr);
        s.perform(doc, "d", typed(3, "d"));
        CHECK(calls == 0 && s.revision() > revision);
    }

    void reentrant_notification()
    {
        std::string doc;
        text_stack s;
        int calls = 0;
        int depth = 0;
        int deepest = 0;
        bool undo_once = false;
        std::string seen;
        s.on_change([&] {
            ++calls;
            ++depth;
            deepest = std::max(deepest, depth);
            seen = std::string(s.undo_label());
            // A callback may edit: it is not called recursively, but again.
            if (undo_once && s.can_undo())
            {
                undo_once = false;
                CHECK(s.undo(doc));
            }
            --depth;
        });

        s.perform(doc, "a", typed(0, "a"));
        CHECK(calls == 1 && seen == "a");

        calls = 0;
        undo_once = true;
        s.perform(doc, "b", typed(1, "b"));
        CHECK(calls == 2);
        CHECK(deepest == 1);
        CHECK(doc == "a" && seen == "a" && s.can_redo());

        // A callback that replaces itself.
        int replaced = 0;
        s.on_change([&] { s.on_change([&] { ++replaced; }); });
        s.perform(doc, "c", typed(1, "c"));
        CHECK(replaced == 0);
        s.perform(doc, "d", typed(2, "d"));
        CHECK(replaced == 1);
        CHECK(doc == "acd");
    }

    void empty_changes()
    {
        // Empty from the start: not recorded.
        {
            int doc = 0;
            add_stack s;
            s.perform(doc, "Nothing", add_change{0});
            CHECK(s.steps().empty() && !s.modified());
        }
        // A transaction that comes to nothing adds no step and discards
        // nothing to redo; modified() follows it there and back.
        {
            int doc = 0;
            add_stack s;
            s.perform(doc, "a", add_change{1});
            s.perform(doc, "b", add_change{2});
            CHECK(s.undo(doc) && doc == 1);
            CHECK(s.mark_saved());
            int calls = 0;
            s.on_change([&] { ++calls; });
            s.begin("Typed and deleted");
            CHECK(calls == 1);
            s.apply(doc, add_change{5});
            CHECK(s.modified() && calls == 2);
            s.apply(doc, add_change{-5});
            CHECK(!s.modified() && calls == 3);
            s.commit();
            CHECK(calls == 4);
            CHECK(doc == 1 && s.steps().size() == 2 && s.position() == 1);
            CHECK(s.can_redo() && s.redo_label() == "b");
        }
        // A newest step that a merge empties goes, back to the saved state.
        {
            int doc = 0;
            add_stack s;
            s.perform(doc, "Set", add_change{10});
            CHECK(s.mark_saved());
            s.perform(doc, "Type", add_change{1}, "type");
            s.perform(doc, "Type", add_change{2}, "type");
            CHECK(s.steps().size() == 2 && s.modified());
            const auto revision = s.revision();
            s.perform(doc, "Delete", add_change{-3}, "type");
            CHECK(doc == 10);
            CHECK(s.steps().size() == 1 && s.position() == 1);
            CHECK(s.revision() != revision);
            CHECK(!s.modified());
            CHECK(s.undo_label() == "Set");
            CHECK(s.undo(doc) && doc == 0 && s.modified());
        }
        // ... leaving the step below it sealed: it was finished when the one
        // that went was begun.
        {
            int doc = 0;
            add_stack s;
            s.perform(doc, "Set", add_change{10}, "set");
            s.perform(doc, "Type", add_change{1}, "type");
            s.perform(doc, "Type", add_change{-1}, "type");
            CHECK(s.steps().size() == 1 && s.steps()[0].sealed);
            s.perform(doc, "Set", add_change{1}, "set");
            CHECK(s.steps().size() == 2);
            CHECK(s.undo(doc) && s.undo(doc) && doc == 0);
        }
        // In a transaction, a change that comes back to where it started
        // goes, even from behind others.
        {
            std::vector<int> doc(10, 0);
            history::stack<std::vector<int>, slot_change> s;
            s.begin("Drag");
            for (int move = 1; move <= 10; ++move)
            {
                s.apply(doc, slot_change{0, doc[0], move == 10 ? 0 : move});
                s.apply(doc, slot_change{1, doc[1], move});
            }
            s.commit();
            CHECK(s.steps().size() == 1 && s.steps()[0].changes.size() == 1);
            CHECK(s.steps()[0].changes[0].slot == 1);
            CHECK(s.undo(doc) && doc == std::vector<int>(10, 0));
        }
    }

    void recording_steps()
    {
        int doc = 0;
        add_stack s;
        doc += 2;
        s.record("Add", add_change{2}, "add");
        doc += 3;
        s.record("Add", add_change{3}, "add");
        CHECK(s.steps().size() == 1 && s.steps()[0].changes[0].amount == 5);
        doc += 1;
        s.record("Other", add_change{1});
        CHECK(s.steps().size() == 2 && s.undo_label() == "Other");

        // Inside a transaction it joins it.
        s.begin("Outer");
        doc += 4;
        s.record("Inner", add_change{4}, "x");
        CHECK(s.in_transaction());
        s.commit();
        CHECK(s.steps().size() == 3 && s.steps()[2].label == "Outer");
        CHECK(s.undo(doc) && s.undo(doc) && s.undo(doc) && doc == 0);
    }

    void copying()
    {
        int doc = 0;
        add_stack s(history::stack_limits{.max_steps = 7});
        int calls = 0;
        s.on_change([&] { ++calls; });
        s.perform(doc, "a", add_change{1}, "k");
        s.seal();
        s.perform(doc, "b", add_change{2});
        CHECK(s.mark_saved());
        s.perform(doc, "c", add_change{4});
        CHECK(s.undo(doc) && doc == 3);

        calls = 0;
        add_stack copy = s;
        CHECK(calls == 0);
        CHECK(copy.steps().size() == 3 && copy.position() == 2);
        CHECK(copy.saved_position() == 2u && !copy.modified());
        CHECK(copy.steps()[0].sealed && copy.steps()[0].merge_key == "k" && !copy.steps()[1].sealed);
        CHECK(copy.steps()[2].label == "c" && copy.steps()[2].sealed); // undone
        CHECK(copy.limits().max_steps == 7 && copy.total_cost() == s.total_cost());
        CHECK(copy.undo_label() == "b" && copy.redo_label() == "c");

        // Independent of the original, and without its callback.
        int other = doc;
        CHECK(copy.undo(other) && copy.undo(other) && other == 0);
        CHECK(calls == 0);
        CHECK(s.position() == 2 && s.redo(doc) && doc == 7);
        CHECK(calls == 1);

        // Assignment keeps the target's own callback, and calls it.
        int copy_calls = 0;
        copy.on_change([&] { ++copy_calls; });
        copy = s;
        CHECK(copy_calls == 1);
        CHECK(copy.position() == 3 && copy.steps().size() == 3 && copy.modified());
        CHECK(copy.undo(doc) && doc == 3);
        CHECK(copy_calls == 2 && calls == 1);
    }

    std::string upper(std::string text)
    {
        for (char &c : text)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return text;
    }

    void rewriting()
    {
        using change = whole_change<std::string>;
        std::string doc;
        history::stack<std::string, change> s;
        auto set = [&](const char *label, std::string to) { s.perform(doc, label, change{doc, std::move(to)}); };
        set("s0", "x");
        set("s1", "X");
        set("s2", "Xy");
        CHECK(s.mark_saved());
        set("s3", "XY");
        set("s4", "XYz");
        CHECK(s.undo(doc) && doc == "XY");
        CHECK(s.position() == 4 && s.saved_position() == 3u);

        // Saving writes everything in capitals: every state follows, and the
        // steps that were only a change of case go.
        std::vector<std::size_t> seen;
        CHECK(s.rewrite([&](std::size_t index, std::span<change> changes) {
            seen.push_back(index);
            for (change &c : changes)
            {
                c.before = upper(c.before);
                c.after = upper(c.after);
            }
        }));
        doc = upper(doc);
        CHECK((seen == std::vector<std::size_t>{0, 1, 2, 3, 4}));
        CHECK(s.steps().size() == 3);
        CHECK(s.steps()[0].label == "s0" && s.steps()[1].label == "s2" && s.steps()[2].label == "s4");
        CHECK(s.position() == 2 && s.saved_position() == 2u && !s.modified());
        CHECK(s.steps()[1].sealed); // the newest done, since "s3" went
        CHECK(s.total_cost() == 3 * sizeof(change));
        CHECK(s.undo(doc) && doc == "X" && s.modified());
        CHECK(s.undo(doc) && doc.empty());
        CHECK(s.redo(doc) && s.redo(doc) && doc == "XY" && !s.modified());
        CHECK(s.redo(doc) && doc == "XYZ" && !s.can_redo());

        // Refused while a transaction is open.
        s.begin("open");
        CHECK(!s.rewrite([](std::size_t, std::span<change>) {}));
        s.commit();
    }

    struct part_doc
    {
        std::string id;
        std::string name;

        bool operator==(const part_doc &) const = default;
    };

    void rewriting_on_save()
    {
        // As the part editor saves: the library gives the part an id and
        // trims its name. The state saved becomes what the library made of
        // it, every other state gets the id (which nothing undoes), and a
        // step the trim made into nothing goes.
        using change = whole_change<part_doc>;
        part_doc doc;
        history::stack<part_doc, change> s;
        auto edit = [&](std::string name) {
            part_doc next = doc;
            next.name = std::move(name);
            s.perform(doc, "Edit name", change{doc, std::move(next)}, "type:name");
        };
        edit("R");
        edit("R1");
        s.seal();
        edit("R1 ");
        CHECK(s.steps().size() == 2);

        const part_doc saved{"p-1", "R1"};
        const std::size_t at = s.position();
        CHECK(s.rewrite([&](std::size_t index, std::span<change> changes) {
            auto adjust = [&](part_doc &state, std::size_t position) {
                if (position == at)
                    state = saved;
                else
                    state.id = saved.id;
            };
            for (change &c : changes)
            {
                adjust(c.before, index);
                adjust(c.after, index + 1);
            }
        }));
        CHECK(s.mark_saved());
        doc = saved;
        CHECK(s.steps().size() == 1 && s.position() == 1 && !s.modified());
        CHECK(s.undo(doc) && doc == part_doc{"p-1", ""});
        CHECK(s.redo(doc) && doc == saved && !s.modified());
        edit("R12"); // typing on after a save is a step of its own
        CHECK(s.steps().size() == 2);
    }

    void moving_a_stack()
    {
        std::string doc;
        text_stack s;
        s.perform(doc, "a", typed(0, "a"));
        text_stack moved = std::move(s);
        CHECK(moved.steps().size() == 1);
        CHECK(moved.undo(doc) && doc.empty());
    }

} // namespace

int main()
{
    single_steps();
    transactions();
    nesting();
    empty_transactions();
    cancelling();
    refused_while_open();
    merging_in_a_transaction();
    merging_past_commuting_changes();
    merging_steps();
    saved_marker();
    clearing();
    step_limit();
    saved_marker_and_limits();
    cost_limit();
    lowering_limits_keeps_the_newest_done();
    guards();
    notification();
    reentrant_notification();
    empty_changes();
    recording_steps();
    copying();
    rewriting();
    rewriting_on_save();
    moving_a_stack();
    return volt::tests::result("history stack");
}
