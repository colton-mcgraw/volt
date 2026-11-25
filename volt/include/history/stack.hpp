#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// -------------------------------------------------------------------------
// undo stack
// -------------------------------------------------------------------------
//
// Undo and redo for a document held in memory, whatever the document is.
// The stack does not know the document: it keeps a list of the changes
// made to it, and undoes one by reverting it. A change is any value that
// can be applied to the document and reverted from it:
//
//   struct rename
//   {
//       std::string before, after;
//       void apply(part &p) const { p.name = after; }
//       void revert(part &p) const { p.name = before; }
//   };
//
// What one Undo undoes is a step: a label for the menu ("Move parts") and
// the changes made under it, in order. Undo reverts them newest first; redo
// applies them again in order. History is linear: an edit made after an
// undo discards the steps that were undone (there is no undo tree).
//
//   history::stack<part, rename> undo;
//   undo.perform(p, "Rename", rename{p.name, "R1"});   // a step of one change
//
//   auto t = undo.transact(p, "Edit part");          // a step of several
//   t.apply(rename{p.name, "R2"});
//   t.apply(rename{"R2", "R3"});
//   t.commit();                                       // no commit: undone
//
//   undo.undo(p);                                     // p.name is "R1" again
//
// Transactions
//
//   begin(label) opens a step; apply() makes a change and records it,
//   record() records one the caller has already made; commit() closes the
//   step and cancel() reverts everything it made. Transactions nest: an
//   inner begin/commit joins the outermost one (whose label is the step's),
//   so a function that makes a step of its own can be called as part of a
//   bigger one. A commit that leaves no changes adds no step. cancel()
//   closes every level at once. transact() wraps all of this in a guard
//   that cancels on destruction unless committed, which is the safe way to
//   write it.
//
// Merging
//
//   Changes may know how to absorb a later change to the same thing
//   (`bool merge(const Change &later)`): then a drag of 200 mouse moves is
//   one change from where the part was to where it is. Inside a
//   transaction every new change is offered to the newest change first.
//   When the change type also knows which changes are independent
//   (`bool commutes_with(const Change &other) const`: applying the two in
//   either order gives the same document), the new change is offered to
//   older changes too, past ones it commutes with, so a drag of several
//   parts at once, which alternates between them, still merges per part.
//
//   Successive steps merge too, when they are one action to the user
//   (typing, nudging with the arrow keys): commit(merge_key) with a
//   non-empty key equal to the newest step's key puts the new changes into
//   that step instead of making one. Keys are plain text the editor makes
//   up, naming what is being edited: "type:part.name", "nudge:R1,R2". The
//   newest step stops taking merges when it is undone, when it is the saved
//   state, when there is something to redo, and when seal() is called
//   (focus moved, the user paused).
//
//   A change may know when it changes nothing (`bool empty() const`):
//   typing "x" and deleting it again merges into such a change. The stack
//   keeps none: a change that is or becomes empty goes, a transaction left
//   with none adds no step (and discards nothing to redo), and a newest
//   step that a merge empties goes too, so that Undo never does nothing.
//   The step below it is then the newest, sealed (it was finished when the
//   one that went was begun), and the saved marker, if it was there, says
//   unmodified again.
//
// The saved marker
//
//   mark_saved() records that the document as it is now is what is on
//   disk; modified() says whether it differs. Undoing back to the saved
//   state makes it unmodified again. Once that state cannot be reached any
//   more (saved, undo, then a new edit discards it; or the limit drops the
//   steps before it), the document is modified until the next mark_saved.
//
//   When saving changes the document (the library gives a part its id and
//   leaves out blank rows), every state in the history must change the
//   same way, or undo would bring back the unsaved form: rewrite() hands
//   the caller each step's changes to adjust in place, then drops what
//   that left empty. It is for exactly that, a saved form that differs
//   from the edited one; an edit is a step, never a rewrite.
//
// Copies
//
//   A stack of copyable changes copies: the steps, the position, the saved
//   marker, the seals and the limits; not the on_change callback, which
//   belongs to whoever watches the original (a copy's owner sets its own).
//   A stack with a transaction open is not copied (a precondition).
//
// Limits
//
//   The oldest steps are dropped beyond a number of steps (1000 unless set;
//   0 for none) and, optionally, a total cost in approximate bytes (what
//   `std::size_t cost() const` returns, or sizeof the change when it has
//   none). The step being built and the newest step done are never dropped,
//   whatever its cost.
//
// Notification
//
//   revision() changes whenever anything a menu or a history list shows may
//   have: can_undo, can_redo, the labels, modified(), the steps. on_change()
//   sets a callback called at the same moments, after the stack is
//   consistent again. The callback may use the stack, edits included; a
//   change it makes does not call it recursively but once more after it
//   returns, until a call makes no change.
//
// Errors
//
//   Nothing here fails. apply() and revert() are assumed not to fail, and
//   must not use the stack (that is checked in debug builds). If one throws
//   anyway, the stack stays usable, but the document may no longer be what
//   the history says it is. Requests that do not make sense now (undo with
//   nothing to undo, or while a transaction is open) are refused and say
//   so by returning false.
//
// Interface thread only; nothing locks.

namespace volt::history
{

    // ---- what a change is ------------------------------------------------------

    // A change to a Document: applying it makes the change, reverting it
    // takes it back, so that revert after apply leaves the document as it
    // was. revert is only called on a document the change was applied to (or
    // that is in the same state again).
    template <typename C, typename Document>
    concept change_of = std::movable<C> && requires(const C &c, Document &doc) {
        c.apply(doc);
        c.revert(doc);
    };

    // A change that can absorb a later one: after a true return, applying it
    // does what applying it and then `later` did, and reverting it undoes
    // both. On false, it must be left as it was.
    template <typename C>
    concept mergeable = requires(C &c, const C &later) {
        { c.merge(later) } -> std::same_as<bool>;
    };

    // A change that knows when another is independent of it: true only when
    // the two, applied in either order, give the same document, and each
    // one's apply and revert work the same before or after the other.
    template <typename C>
    concept commutable = requires(const C &c, const C &other) {
        { c.commutes_with(other) } -> std::same_as<bool>;
    };

    // A change that knows its size, in approximate bytes.
    template <typename C>
    concept costed = requires(const C &c) {
        { c.cost() } -> std::convertible_to<std::size_t>;
    };

    // A change that knows when it changes nothing: true only when applying
    // and reverting it both leave the document as it is.
    template <typename C>
    concept emptiable = requires(const C &c) {
        { c.empty() } -> std::same_as<bool>;
    };

    // Whether the change is known to change nothing: its empty(), or false
    // when it has none.
    template <typename C>
    bool is_empty([[maybe_unused]] const C &c)
    {
        if constexpr (emptiable<C>)
            return c.empty();
        else
            return false;
    }

    // What a change costs: its cost(), or its own size when it has none.
    template <typename C>
    std::size_t cost_of(const C &c)
    {
        if constexpr (costed<C>)
            return static_cast<std::size_t>(c.cost());
        else
            return sizeof(C);
    }

    struct stack_limits
    {
        std::size_t max_steps = 1000; // 0: no limit
        std::size_t max_cost = 0;     // in cost_of units; 0: no limit
        // How many changes back a new change is offered for merging, when
        // the change type is commutable (otherwise only the newest).
        std::size_t merge_window = 256;
    };

    template <typename Document, change_of<Document> Change>
    class stack
    {
    public:
        // What one Undo undoes.
        struct step
        {
            std::string label;
            std::vector<Change> changes; // in the order they were made
            std::string merge_key;       // empty: never merged into
            bool sealed = false;         // takes no more merges
            std::size_t cost = 0;        // of its changes
        };

        class transaction;

        stack() = default;
        explicit stack(stack_limits limits) : m_limits(limits) {}

        // A copy has the same history and no callback (see the top of this
        // file). Precondition: no transaction is open in `other`.
        stack(const stack &other)
            requires std::copyable<Change>
            : m_steps(other.m_steps), m_position(other.m_position), m_saved(other.m_saved),
              m_total_cost(other.m_total_cost), m_limits(other.m_limits), m_revision(other.m_revision)
        {
            assert(!other.in_transaction() && "a stack copied with a transaction open");
        }

        // Takes the history of `other`; keeps its own callback, and calls
        // it. Precondition: no transaction is open in either.
        stack &operator=(const stack &other)
            requires std::copyable<Change>
        {
            assert(!in_transaction() && !other.in_transaction() && "a stack copied with a transaction open");
            if (this == &other)
                return *this;
            m_steps = other.m_steps;
            m_position = other.m_position;
            m_saved = other.m_saved;
            m_total_cost = other.m_total_cost;
            m_limits = other.m_limits;
            m_open.clear();
            m_open_label.clear();
            m_depth = 0;
            changed();
            return *this;
        }

        // A stack moves, callback and all (but not while a transaction guard
        // refers to it).
        stack(stack &&) = default;
        stack &operator=(stack &&) = default;

        // ---- transactions ------------------------------------------------------

        // Opens a step, or a nested level of the one open (the label is then
        // not used).
        void begin(std::string label)
        {
            if (m_depth++ > 0)
                return;
            m_open_label = std::move(label);
            ++m_serial;
            changed();
        }

        // Closes a level. Closing the outermost adds the step: merged into
        // the newest step when merge_key says so (see the top of this file,
        // which also says when that step goes instead), nothing when no
        // changes were made or all came to nothing (empty()). The key of a
        // nested commit is not used. Precondition: a transaction is open.
        void commit(std::string merge_key = {})
        {
            assert(m_depth > 0 && "commit without begin");
            if (m_depth == 0)
                return;
            if (--m_depth > 0)
                return;
            finish(std::move(merge_key));
        }

        // Reverts every change of the open transaction, newest first, and
        // closes all its levels. Nothing when none is open.
        void cancel(Document &doc)
        {
            if (m_depth == 0)
                return;
            {
                const busy running(*this);
                for (auto i = m_open.size(); i-- > 0;)
                    m_open[i].revert(doc);
            }
            m_depth = 0;
            m_open.clear();
            m_open_label.clear();
            changed();
        }

        bool in_transaction() const noexcept { return m_depth > 0; }

        // Applies the change to the document and records it in the open
        // transaction. Outside one, it is a step of its own, unlabelled
        // (perform() gives it a label).
        void apply(Document &doc, Change change)
        {
            if (m_depth == 0)
            {
                perform(doc, {}, std::move(change));
                return;
            }
            {
                const busy running(*this);
                change.apply(doc);
            }
            add(std::move(change));
        }

        // Records a change the caller has already made to the document, as
        // apply() does otherwise.
        void record(Change change)
        {
            if (m_depth == 0)
            {
                record({}, std::move(change));
                return;
            }
            add(std::move(change));
        }

        // A step of one change the caller has already made: begin, record,
        // commit, as perform() is for apply().
        void record(std::string label, Change change, std::string merge_key = {})
        {
            const hold quiet(*this);
            begin(std::move(label));
            add(std::move(change));
            commit(std::move(merge_key));
        }

        // A step of one change: begin, apply, commit. Inside a transaction
        // it joins it, like any nested one.
        void perform(Document &doc, std::string label, Change change, std::string merge_key = {})
        {
            const hold quiet(*this);
            begin(std::move(label));
            apply(doc, std::move(change));
            commit(std::move(merge_key));
        }

        // Opens a transaction (begin) and returns its guard, which cancels
        // it when destroyed uncommitted. The guard refers to the stack and
        // the document, so neither may move or die before it does.
        [[nodiscard]] transaction transact(Document &doc, std::string label)
        {
            return transaction(*this, doc, std::move(label));
        }

        // ---- merging steps -----------------------------------------------------

        // The newest step done takes no more merges: the next commit makes a
        // step of its own whatever its key.
        void seal() noexcept
        {
            if (m_position > 0)
                m_steps[m_position - 1].sealed = true;
        }

        // ---- undo and redo -----------------------------------------------------

        // Whether undo() and redo() would do something. Both are false while
        // a transaction is open, since undo is refused then: the document
        // has changes no step holds yet.
        bool can_undo() const noexcept { return m_depth == 0 && m_position > 0; }
        bool can_redo() const noexcept { return m_depth == 0 && m_position < m_steps.size(); }

        // The label of the step undo() or redo() would take, for the menu
        // ("Undo Move parts"); empty when there is none. Valid until the
        // stack next changes.
        std::string_view undo_label() const noexcept
        {
            return can_undo() ? std::string_view(m_steps[m_position - 1].label) : std::string_view();
        }

        std::string_view redo_label() const noexcept
        {
            return can_redo() ? std::string_view(m_steps[m_position].label) : std::string_view();
        }

        // Reverts the newest step done, newest change first. False when there
        // is none or a transaction is open. An undone step is sealed: done
        // again, it takes no merges.
        bool undo(Document &doc)
        {
            if (!can_undo())
                return false;
            step &s = m_steps[m_position - 1];
            s.sealed = true;
            {
                const busy running(*this);
                for (auto i = s.changes.size(); i-- > 0;)
                    s.changes[i].revert(doc);
            }
            --m_position;
            changed();
            return true;
        }

        // Applies the oldest step undone again, in order. False when there is
        // none or a transaction is open.
        bool redo(Document &doc)
        {
            if (!can_redo())
                return false;
            const step &s = m_steps[m_position];
            {
                const busy running(*this);
                for (const Change &c : s.changes)
                    c.apply(doc);
            }
            ++m_position;
            changed();
            return true;
        }

        // ---- the saved marker --------------------------------------------------

        // The document as it is now is the saved one. Refused (false) while
        // a transaction is open: its changes are not a step yet, so that
        // state could not be found again. Save between transactions.
        bool mark_saved()
        {
            if (m_depth > 0)
                return false;
            m_saved = m_position;
            changed();
            return true;
        }

        // Whether the document differs from the one saved: it is not at the
        // saved position, the saved state can no longer be reached, or an
        // open transaction has changed it.
        bool modified() const noexcept { return !m_open.empty() || m_saved != m_position; }

        // The position (as position()) of the saved state; nullopt when it
        // can no longer be reached.
        std::optional<std::size_t> saved_position() const noexcept { return m_saved; }

        // Adjusts the recorded changes in place, for when the document has
        // been changed outside the history in a way every state it records
        // must follow too: saving gave the part an id, or left blank rows
        // out (see the top of this file). Calls fix(index, changes) for each
        // step, oldest first, with its changes in order; the step takes the
        // state at position `index` to the one at `index + 1`. Then changes
        // the fix left empty (when the type says so) go, and steps left
        // with none, the position and the saved marker moving down past
        // them. Labels, keys and seals stay, except that a newest step done
        // that goes leaves the one below it sealed.
        //
        // The stack checks nothing: the fixed changes must still take each
        // state to the next, and the document must be (made) the state at
        // position() afterwards. Refused (false) while a transaction is
        // open.
        template <typename Fix>
            requires std::invocable<Fix &, std::size_t, std::span<Change>>
        bool rewrite(Fix &&fix)
        {
            if (m_depth > 0)
                return false;
            {
                const busy running(*this);
                for (std::size_t i = 0; i < m_steps.size(); ++i)
                    fix(i, std::span<Change>(m_steps[i].changes));
            }
            const std::size_t done = m_position;
            bool newest_went = false;
            std::size_t kept = 0;
            m_total_cost = 0;
            for (std::size_t i = 0; i < m_steps.size(); ++i)
            {
                step &s = m_steps[i];
                std::erase_if(s.changes, [](const Change &c) { return is_empty(c); });
                if (s.changes.empty())
                {
                    // States i and i + 1 are the same: whatever was at i + 1
                    // or above moves down one.
                    if (m_position > kept)
                        --m_position;
                    if (m_saved && *m_saved > kept)
                        --*m_saved;
                    newest_went = newest_went || i + 1 == done;
                    continue;
                }
                s.cost = cost_of_changes(s.changes);
                m_total_cost += s.cost;
                if (kept != i)
                    m_steps[kept] = std::move(s);
                ++kept;
            }
            m_steps.erase(m_steps.begin() + static_cast<std::ptrdiff_t>(kept), m_steps.end());
            if (newest_went && m_position > 0)
                m_steps[m_position - 1].sealed = true;
            changed();
            return true;
        }

        // ---- limits ------------------------------------------------------------

        const stack_limits &limits() const noexcept { return m_limits; }

        // Takes effect at once: steps beyond the new limits are dropped.
        void set_limits(stack_limits limits)
        {
            m_limits = limits;
            if (enforce_limits())
                changed();
        }

        // What all the steps cost together (the open transaction not
        // counted).
        std::size_t total_cost() const noexcept { return m_total_cost; }

        // ---- inspection --------------------------------------------------------

        // Every step, oldest first: those before position() are done (undo
        // takes them back), those from it on are undone (redo makes them).
        std::span<const step> steps() const noexcept { return m_steps; }
        std::size_t position() const noexcept { return m_position; }

        // Forgets every step. The document stays as it is, and so does its
        // saved status: unmodified stays so, modified stays so for good. An
        // open transaction is not touched.
        void clear()
        {
            m_saved = m_saved == m_position ? std::optional<std::size_t>(0) : std::nullopt;
            m_steps.clear();
            m_position = 0;
            m_total_cost = 0;
            changed();
        }

        // ---- notification ------------------------------------------------------

        // Changes (only ever grows) whenever what is described at the top of
        // this file may have changed.
        std::uint64_t revision() const noexcept { return m_revision; }

        // Called after each such change; nullptr for none. Not called now.
        void on_change(std::function<void()> callback) { m_on_change = std::move(callback); }

    private:
        // Notices that apply/revert called back into the stack, which they
        // must not.
        struct busy
        {
            explicit busy(stack &s) noexcept : owner(s)
            {
                assert(!owner.m_running && "a change used the undo stack while it was applied");
                owner.m_running = true;
            }
            ~busy() { owner.m_running = false; }
            busy(const busy &) = delete;
            busy &operator=(const busy &) = delete;
            stack &owner;
        };

        // Holds back the callback until the outermost hold ends, so that an
        // operation made of others (perform) calls it once.
        struct hold
        {
            explicit hold(stack &s) noexcept : owner(s) { ++owner.m_holds; }
            ~hold()
            {
                if (--owner.m_holds == 0)
                    owner.notify();
            }
            hold(const hold &) = delete;
            hold &operator=(const hold &) = delete;
            stack &owner;
        };

        void changed()
        {
            ++m_revision;
            m_pending = true;
            notify();
        }

        void notify()
        {
            if (m_holds > 0 || m_notifying || !m_pending)
                return;
            m_pending = false;
            if (!m_on_change)
                return;
            m_notifying = true;
            struct reset
            {
                bool &flag;
                ~reset() { flag = false; }
            } const done{m_notifying};
            // A change made by the callback sets m_pending again: call it
            // once more, by a copy, since it may replace itself.
            bool again = true;
            while (again && m_on_change)
            {
                const std::function<void()> callback = m_on_change;
                callback();
                again = std::exchange(m_pending, false);
            }
        }

        // Records a change already made in the open transaction.
        void add(Change change)
        {
            const bool had_none = m_open.empty();
            absorb(m_open, std::move(change));
            if (had_none != m_open.empty())
                changed(); // modified() may have changed
        }

        // Puts a change made after those of `into` among them: merged into
        // one, or at the end. One that is empty, or a merge that leaves one
        // empty, leaves nothing.
        void absorb(std::vector<Change> &into, Change change) const
        {
            if (is_empty(change))
                return;
            if (const std::optional<std::size_t> i = merge_into(into, change))
            {
                if (is_empty(into[*i]))
                    into.erase(into.begin() + static_cast<std::ptrdiff_t>(*i));
                return;
            }
            into.push_back(std::move(change));
        }

        // Offers the change to those of `into`, newest first; the one that
        // took it, if any. Older ones are offered it only past changes it
        // commutes with, since merging moves it back before them.
        std::optional<std::size_t> merge_into([[maybe_unused]] std::vector<Change> &into,
                                              [[maybe_unused]] const Change &change) const
        {
            if constexpr (mergeable<Change>)
            {
                std::size_t looked = 0;
                for (auto i = into.size(); i-- > 0;)
                {
                    if (into[i].merge(change))
                        return i;
                    if constexpr (commutable<Change>)
                    {
                        if (++looked >= m_limits.merge_window || !change.commutes_with(into[i]))
                            return std::nullopt;
                    }
                    else
                    {
                        return std::nullopt;
                    }
                }
            }
            return std::nullopt;
        }

        bool merges_into_top(const std::string &key) const
        {
            if (key.empty() || m_position == 0 || m_position != m_steps.size() || m_saved == m_position)
                return false;
            const step &top = m_steps.back();
            return !top.sealed && top.merge_key == key;
        }

        static std::size_t cost_of_changes(const std::vector<Change> &changes)
        {
            std::size_t total = 0;
            for (const Change &c : changes)
                total += cost_of(c);
            return total;
        }

        // The outermost commit.
        void finish(std::string merge_key)
        {
            std::vector<Change> changes = std::exchange(m_open, {});
            std::string label = std::exchange(m_open_label, {});
            if (changes.empty())
            {
                changed(); // can_undo and can_redo are back
                return;
            }

            if (merges_into_top(merge_key))
            {
                step &top = m_steps.back();
                for (Change &c : changes)
                    absorb(top.changes, std::move(c));
                m_total_cost -= top.cost;
                if (top.changes.empty())
                {
                    // Undone by what was merged into it: it goes, and the
                    // state below it is the newest (and the saved one, if
                    // that was where the marker was).
                    m_steps.pop_back();
                    --m_position;
                    seal();
                }
                else
                {
                    top.cost = cost_of_changes(top.changes);
                    m_total_cost += top.cost;
                }
            }
            else
            {
                // A new edit: what was undone cannot be redone, and the saved
                // state, if it was among it, cannot be reached again.
                if (m_saved && *m_saved > m_position)
                    m_saved.reset();
                for (auto i = m_position; i < m_steps.size(); ++i)
                    m_total_cost -= m_steps[i].cost;
                m_steps.erase(m_steps.begin() + static_cast<std::ptrdiff_t>(m_position), m_steps.end());

                const std::size_t cost = cost_of_changes(changes);
                m_steps.push_back(step{std::move(label), std::move(changes), std::move(merge_key), false, cost});
                m_total_cost += cost;
                ++m_position;
                enforce_limits();
            }
            changed();
        }

        bool over_limits(std::size_t steps) const noexcept
        {
            return (m_limits.max_steps > 0 && steps > m_limits.max_steps) ||
                   (m_limits.max_cost > 0 && m_total_cost > m_limits.max_cost);
        }

        // Drops the oldest steps beyond the limits, never the newest done.
        // True if any went.
        bool enforce_limits()
        {
            std::size_t dropped = 0;
            while (m_position - dropped > 1 && over_limits(m_steps.size() - dropped))
                m_total_cost -= m_steps[dropped++].cost;
            if (dropped == 0)
                return false;
            m_steps.erase(m_steps.begin(), m_steps.begin() + static_cast<std::ptrdiff_t>(dropped));
            m_position -= dropped;
            if (m_saved)
            {
                if (*m_saved < dropped)
                    m_saved.reset(); // the saved state was before the oldest step left
                else
                    *m_saved -= dropped;
            }
            return true;
        }

        std::vector<step> m_steps;
        std::size_t m_position = 0;                 // steps done
        std::optional<std::size_t> m_saved = 0;     // the saved position; none: unreachable
        std::size_t m_total_cost = 0;
        stack_limits m_limits;

        std::vector<Change> m_open; // the open transaction's changes
        std::string m_open_label;
        std::size_t m_depth = 0;     // levels open
        std::uint64_t m_serial = 0;  // counts outermost begins, for the guards

        std::function<void()> m_on_change;
        std::uint64_t m_revision = 0;
        int m_holds = 0;
        bool m_pending = false;
        bool m_notifying = false;
        bool m_running = false;
    };

    // ---- the transaction guard ------------------------------------------------
    //
    // An open transaction that cancels itself unless committed, so that an
    // early return or an error path cannot leave half an edit in the
    // document or a transaction open forever. Made by stack::transact; it
    // neither copies nor moves, and holds the stack and the document by
    // reference.
    //
    // Guards nest like begin/commit. Each remembers which transaction and
    // level it opened: if an inner one cancels (which cancels the whole
    // transaction), the outer ones find their transaction gone, and their
    // apply, record and commit return false and do nothing, so a later
    // edit does not slip into some other transaction.

    template <typename Document, change_of<Document> Change>
    class [[nodiscard]] stack<Document, Change>::transaction
    {
    public:
        transaction(const transaction &) = delete;
        transaction &operator=(const transaction &) = delete;
        transaction(transaction &&) = delete;
        transaction &operator=(transaction &&) = delete;

        ~transaction() { cancel(); }

        // Whether this guard's level is still open (not committed or
        // cancelled, by it or another).
        bool open() const noexcept
        {
            return !m_done && m_stack.m_serial == m_serial && m_stack.m_depth >= m_level;
        }

        // stack::apply and stack::record, into this transaction. False, and
        // nothing done, when it is no longer open.
        bool apply(Change change)
        {
            if (!open())
                return false;
            m_stack.apply(m_document, std::move(change));
            return true;
        }

        bool record(Change change)
        {
            if (!open())
                return false;
            m_stack.record(std::move(change));
            return true;
        }

        // Closes this level (stack::commit). False when it is no longer open
        // or a guard opened inside it still is: levels close innermost first.
        bool commit(std::string merge_key = {})
        {
            if (!open() || m_stack.m_depth != m_level)
                return false;
            m_done = true;
            m_stack.commit(std::move(merge_key));
            return true;
        }

        // Cancels the whole transaction now (stack::cancel). False when it
        // was no longer open.
        bool cancel()
        {
            if (!open())
                return false;
            m_done = true;
            m_stack.cancel(m_document);
            return true;
        }

    private:
        friend class stack;

        transaction(stack &s, Document &doc, std::string label) : m_stack(s), m_document(doc)
        {
            m_stack.begin(std::move(label));
            m_serial = m_stack.m_serial;
            m_level = m_stack.m_depth;
        }

        stack &m_stack;
        Document &m_document;
        std::uint64_t m_serial = 0;
        std::size_t m_level = 0;
        bool m_done = false;
    };

} // namespace volt::history
