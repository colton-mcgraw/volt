#pragma once

#include <history/stack.hpp>

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// -------------------------------------------------------------------------
// ready-made changes
// -------------------------------------------------------------------------
//
// Change types for the undo stack (<history/stack.hpp>), for the two ways a
// document is usually built.
//
// A small document edited as a whole (a part in the part editor) is a value,
// and a change to it is the value before and after: value_change<T>.
//
//   history::stack<library::part, history::value_change<library::part>> undo;
//   if (auto c = history::make_value_change(before, edited))
//       undo.perform(p, "Edit part", std::move(*c), "type:name");
//
// A big document (a schematic: parts, wires, labels) is made of tables of
// records, each record kept under an id, and an edit changes a few records
// of it: table<Id, Record>, whose edits (insert, replace, erase, move) make
// the edit and return the record_change that describes it, for the stack
// to record:
//
//   history::table<std::string, wire> wires;
//   history::stack<history::table<std::string, wire>, history::record_change<std::string, wire>> undo;
//   if (auto c = wires.insert("w1", wire{...}))
//       undo.record(std::move(*c));
//
// A table keeps its records in order, and undo puts them back in the same
// order: an erase undone puts the record back where it was. That matters
// because Volt writes documents as deterministic JSON, where one changed
// field must be one changed line in a diff; a record that came back at the
// end of the list would be a diff of every record after it.
//
// A document of several tables (and other members) has a change type that
// is one of the changes to its members. member_change<&doc::member> makes a
// member's change a change of the document, and variant_change makes one
// change type of several:
//
//   struct sheet
//   {
//       history::table<std::string, symbol> symbols;
//       history::table<std::string, wire> wires;
//       std::string title;
//   };
//
//   using symbols_change = history::member_change<&sheet::symbols>;
//   using wires_change = history::member_change<&sheet::wires>;
//   using title_change = history::member_change<&sheet::title, history::value_change<std::string>>;
//   using sheet_change = history::variant_change<sheet, symbols_change, wires_change, title_change>;
//
//   history::stack<sheet, sheet_change> undo;
//   if (auto c = s.wires.erase("w1"))
//       undo.record(sheet_change{wires_change{std::move(*c)}});
//
// They pass merging on: changes to one member merge as that member's
// changes do, and changes to different members commute (which is what lets
// a drag of symbols and wires together merge per record).

namespace volt::history
{

    // ---- value_change ----------------------------------------------------------

    // The document as a whole, before and after. Applying assigns `after`,
    // reverting assigns `before`. Merging keeps the first `before` and takes
    // the later `after`, so a run of edits is one change, as big as two
    // copies of the document.
    template <typename T>
    struct value_change
    {
        T before;
        T after;

        void apply(T &doc) const { doc = after; }
        void revert(T &doc) const { doc = before; }

        bool merge(const value_change &later)
        {
            after = later.after;
            return true;
        }

        // Before and after the same: typed and deleted again. Only for a T
        // that compares.
        bool empty() const
            requires std::equality_comparable<T>
        {
            return before == after;
        }

        bool operator==(const value_change &) const = default;
    };

    // The change from one value to the other; nullopt when they are equal,
    // since there is nothing to undo.
    template <std::equality_comparable T>
    std::optional<value_change<T>> make_value_change(T before, T after)
    {
        if (before == after)
            return std::nullopt;
        return value_change<T>{std::move(before), std::move(after)};
    }

    // ---- tables of records -------------------------------------------------------

    template <typename Id>
    concept table_id = std::equality_comparable<Id> && std::copy_constructible<Id> && requires(const Id &id) {
        { std::hash<Id>{}(id) } -> std::convertible_to<std::size_t>;
    };

    template <typename Record>
    concept table_record = std::equality_comparable<Record> && std::copy_constructible<Record> &&
                           std::is_copy_assignable_v<Record>;

    template <table_id Id, table_record Record>
    class table;

    // What happened to one record of a table: it was `before`, at position
    // `from`, and is `after`, at position `to`. A missing value is a record
    // that was not there (an insert) or is not any more (an erase); its
    // position is then 0 and means nothing.
    //
    //   insert    before none, after the record at `to`
    //   erase     before the record at `from`, after none
    //   replace   both, from == to (the record stays where it is)
    //   move      both, from != to (and the value may change too)
    //
    // apply and revert are exact inverses: revert puts an erased record back
    // at `from`, so the table's order comes back with its contents.
    //
    // Successive changes to one id merge into one from the first state to
    // the last: insert then replace is an insert of the new value; replace
    // then erase is an erase of the original, where it was; replace then
    // replace, move then move, erase then insert all compose the same way.
    // Insert then erase leaves a change with neither value, which applies
    // and reverts as nothing: empty(), so the stack drops it, as it does a
    // replace or move that comes back to where it started.
    //
    // A replace commutes with any change to another record, since it uses
    // and changes no position; an insert, erase or move commutes with a
    // replace or nothing change of another record only.
    template <table_id Id, table_record Record>
    struct record_change
    {
        Id id;
        std::optional<Record> before;
        std::optional<Record> after;
        std::size_t from = 0;
        std::size_t to = 0;

        void apply(table<Id, Record> &t) const { change(t, before, after, to); }
        void revert(table<Id, Record> &t) const { change(t, after, before, from); }

        // The record keeps its place: a replace, or a change of nothing.
        bool keeps_positions() const noexcept { return before.has_value() == after.has_value() && from == to; }

        // Changes nothing: inserted and erased again, or the same record in
        // the same place.
        bool empty() const
        {
            return keeps_positions() && (!before || *before == *after);
        }

        bool merge(const record_change &later)
        {
            if (later.id != id || after.has_value() != later.before.has_value())
                return false;
            // A replace uses no position, so after commuting past moves of
            // other records its recorded one may be out of date: it leaves
            // the record where this change put it.
            const bool later_in_place = later.before && later.after && later.from == later.to;
            if (after && !later_in_place && to != later.from)
                return false;
            after = later.after;
            if (!after)
                to = 0;
            else if (!later_in_place)
                to = later.to;
            if (!before && !after)
                from = 0;
            return true;
        }

        bool commutes_with(const record_change &other) const
        {
            return id != other.id && (keeps_positions() || other.keeps_positions());
        }

        bool operator==(const record_change &) const = default;

    private:
        // Takes the record from `now` to `then` (at `at`).
        void change(table<Id, Record> &t, const std::optional<Record> &now, const std::optional<Record> &then,
                    std::size_t at) const
        {
            if (!now)
            {
                if (then)
                    t.put(at, id, *then);
                return;
            }
            const std::optional<std::size_t> found = t.position_of(id);
            assert(found && "a record change applied to a table without its record");
            if (!found)
                return;
            if (!then)
            {
                t.take(*found);
                return;
            }
            std::size_t i = *found;
            if (from != to)
            {
                t.shift(i, at);
                i = at;
            }
            t.m_entries[i].record = *then;
        }
    };

    // Records under ids, in an order of their own. Finding a record by id
    // takes constant time; inserting or erasing one in the middle takes time
    // in proportion to the records after it (appending does not). Records
    // change only through the edits below, each of which returns the change
    // it made for an undo stack, or nullopt when it made none.
    template <table_id Id, table_record Record>
    class table
    {
    public:
        struct entry
        {
            Id id;
            Record record;

            bool operator==(const entry &) const = default;
        };

        using change = record_change<Id, Record>;

        std::size_t size() const noexcept { return m_entries.size(); }
        bool empty() const noexcept { return m_entries.empty(); }

        // The records in order.
        std::span<const entry> entries() const noexcept { return m_entries; }
        auto begin() const noexcept { return m_entries.begin(); }
        auto end() const noexcept { return m_entries.end(); }
        const entry &operator[](std::size_t position) const { return m_entries[position]; }

        bool contains(const Id &id) const { return m_index.contains(id); }

        // The record, or null.
        const Record *find(const Id &id) const
        {
            const auto i = m_index.find(id);
            return i == m_index.end() ? nullptr : &m_entries[i->second].record;
        }

        std::optional<std::size_t> position_of(const Id &id) const
        {
            const auto i = m_index.find(id);
            return i == m_index.end() ? std::nullopt : std::optional<std::size_t>(i->second);
        }

        // ---- edits -----------------------------------------------------------

        // Adds a record at the end, or at a position (0 to size()). Nothing
        // when the id is there already or the position is past the end.
        std::optional<change> insert(Id id, Record record) { return insert(std::move(id), std::move(record), size()); }

        std::optional<change> insert(Id id, Record record, std::size_t position)
        {
            if (position > size() || contains(id))
                return std::nullopt;
            change c{id, std::nullopt, record, 0, position};
            put(position, std::move(id), std::move(record));
            return c;
        }

        // Puts a new value in the record's place. Nothing when there is no
        // such record or the value is the same.
        std::optional<change> replace(const Id &id, Record record)
        {
            const auto i = position_of(id);
            if (!i)
                return std::nullopt;
            Record &current = m_entries[*i].record;
            if (current == record)
                return std::nullopt;
            change c{id, std::move(current), record, *i, *i};
            current = std::move(record);
            return c;
        }

        // Removes the record. Nothing when there is none.
        std::optional<change> erase(const Id &id)
        {
            const auto i = position_of(id);
            if (!i)
                return std::nullopt;
            change c{id, m_entries[*i].record, std::nullopt, *i, 0};
            take(*i);
            return c;
        }

        // Moves the record to a position (0 to size() - 1, counted with it
        // removed and put back there). Nothing when there is no such record,
        // the position is past the end, or the record is there already.
        std::optional<change> move(const Id &id, std::size_t position)
        {
            const auto i = position_of(id);
            if (!i || position >= size() || position == *i)
                return std::nullopt;
            const Record &record = m_entries[*i].record;
            change c{id, record, record, *i, position};
            shift(*i, position);
            return c;
        }

        // The same records in the same order.
        bool operator==(const table &other) const { return m_entries == other.m_entries; }

    private:
        friend struct record_change<Id, Record>;

        void put(std::size_t position, Id id, Record record)
        {
            assert(!contains(id) && position <= size());
            if (position > size())
                position = size();
            m_entries.insert(m_entries.begin() + static_cast<std::ptrdiff_t>(position),
                             entry{std::move(id), std::move(record)});
            renumber(position, size());
        }

        void take(std::size_t position)
        {
            m_index.erase(m_entries[position].id);
            m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(position));
            renumber(position, size());
        }

        void shift(std::size_t from, std::size_t to)
        {
            assert(from < size() && to < size());
            if (to >= size())
                to = size() - 1;
            const auto at = m_entries.begin();
            if (from < to)
                std::rotate(at + static_cast<std::ptrdiff_t>(from), at + static_cast<std::ptrdiff_t>(from) + 1,
                            at + static_cast<std::ptrdiff_t>(to) + 1);
            else if (to < from)
                std::rotate(at + static_cast<std::ptrdiff_t>(to), at + static_cast<std::ptrdiff_t>(from),
                            at + static_cast<std::ptrdiff_t>(from) + 1);
            renumber(std::min(from, to), std::max(from, to) + 1);
        }

        void renumber(std::size_t first, std::size_t last)
        {
            for (std::size_t i = first; i < last; ++i)
                m_index.insert_or_assign(m_entries[i].id, i);
        }

        std::vector<entry> m_entries;
        std::unordered_map<Id, std::size_t> m_index;
    };

    // ---- documents of several members -------------------------------------------

    namespace detail
    {
        template <typename>
        struct member_pointer;

        template <typename Document, typename Member>
        struct member_pointer<Member Document::*>
        {
            using document = Document;
            using member = Member;
        };
    } // namespace detail

    // A change to one member of a document, made a change of the document:
    // Member is a pointer to the member (&sheet::wires), Change a change of
    // the member's type, by default its `change` (a table's record_change).
    // Merges and commutes as Change does.
    template <auto Member, typename Change = typename detail::member_pointer<decltype(Member)>::member::change>
    struct member_change
    {
        using document = typename detail::member_pointer<decltype(Member)>::document;

        Change change;

        void apply(document &doc) const { change.apply(doc.*Member); }
        void revert(document &doc) const { change.revert(doc.*Member); }

        bool merge(const member_change &later)
            requires mergeable<Change>
        {
            return change.merge(later.change);
        }

        bool commutes_with(const member_change &other) const
            requires commutable<Change>
        {
            return change.commutes_with(other.change);
        }

        bool empty() const
            requires emptiable<Change>
        {
            return change.empty();
        }

        std::size_t cost() const { return cost_of(change); }

        bool operator==(const member_change &) const = default;
    };

    // One of several kinds of change to a document, held in a std::variant.
    // Each alternative must change a part of the document no other one
    // changes (a member each, typically): changes of different alternatives
    // are taken to commute, and are never merged.
    template <typename Document, change_of<Document>... Changes>
    struct variant_change
    {
        std::variant<Changes...> change;

        void apply(Document &doc) const
        {
            std::visit([&doc](const auto &c) { c.apply(doc); }, change);
        }

        void revert(Document &doc) const
        {
            std::visit([&doc](const auto &c) { c.revert(doc); }, change);
        }

        bool merge(const variant_change &later)
        {
            if (change.index() != later.change.index())
                return false;
            return std::visit(
                [](auto &c, const auto &l) -> bool {
                    using C = std::remove_cvref_t<decltype(c)>;
                    if constexpr (std::is_same_v<C, std::remove_cvref_t<decltype(l)>> && mergeable<C>)
                        return c.merge(l);
                    else
                        return false;
                },
                change, later.change);
        }

        bool commutes_with(const variant_change &other) const
        {
            if (change.index() != other.change.index())
                return true;
            return std::visit(
                [](const auto &c, const auto &o) -> bool {
                    using C = std::remove_cvref_t<decltype(c)>;
                    if constexpr (std::is_same_v<C, std::remove_cvref_t<decltype(o)>> && commutable<C>)
                        return c.commutes_with(o);
                    else
                        return false;
                },
                change, other.change);
        }

        // As the change held says (false when its type cannot say).
        bool empty() const
        {
            return std::visit([](const auto &c) { return is_empty(c); }, change);
        }

        std::size_t cost() const
        {
            return std::visit([](const auto &c) { return cost_of(c); }, change);
        }

        bool operator==(const variant_change &) const = default;
    };

} // namespace volt::history
