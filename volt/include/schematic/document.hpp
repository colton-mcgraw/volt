#pragma once

#include <schematic/sheet.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

// -------------------------------------------------------------------------
// schematic document
// -------------------------------------------------------------------------
//
// A sheet being edited: the sheet (<schematic/sheet.hpp>) and its undo
// history (history::stack<sheet, sheet_change>, <history/stack.hpp>). The
// sheet is read-only from outside; every change goes through the operations
// below, each of which is one step of the history, labelled for the menu
// ("Place R1", "Draw wire", "Move 3 items", "Delete 2 items", "Rotate",
// "Edit value"). Operations that would change nothing (moving by zero,
// setting a value to what it is, erasing what is not there) make no step
// and return false or 0.
//
//   schematic::document doc(schematic::demo_ladder());
//   const item_id r = doc.add_part({.symbol = "R", .position = {300, 0}});  // "Place R9"
//   doc.add_wire({0, 0}, {200, 0});                                      // "Draw wire"
//   doc.undo();                                                          // the wire goes
//
// Ids. The document gives every new item its id, from a counter that only
// grows: max_id() + 1 of the sheet it starts with, then one more for each
// item made. An id is never given twice in a session, not even after the
// item it was given to is undone away; undo and redo bring items back
// under their own ids, since the changes carry them.
//
// Gestures. A drag sends many moves; each is a step, but moves with the
// same non-empty merge key merge into the newest step (the stack's
// "Merging", <history/stack.hpp>): a drag of 200 mouse moves is one step,
// holding one change per record moved, from where it was to where it is,
// and a drag that ends where it began leaves no step at all. Typing into a
// property field works the same way through set_value() and its kin. The
// key is the caller's: "drag:7" for a drag that started at press 7, or
// "type:value:12" for typing into the value of item 12. A key stops merging
// when the step is undone or saved, and at seal() (the mouse was released,
// the field lost the focus).
//
// Transactions. A tool that makes one step of several operations (draw a
// wire and the part it ends on; delete and tidy) opens a transaction with
// begin(label), makes them, and commit()s or cancel()s. Every operation
// called while one is open joins it, labels and merge keys ignored; undo
// and redo are refused until it closes. Nested begin/commit pairs join the
// outermost.
//
// Wires around what moves. move(), rotate() and mirror() move the items
// alone: what they were attached to stays where it is. The editor wants the
// wires to come along and parts dropped on wires to cut them, and has
// operations for that: follow_wires() (the rules are in
// <schematic/follow.hpp>), cut_wires_at_parts(), finish_move() for the end
// of a drag, and move_with_wires() for a nudge. Tidying can be kept to the
// wires an edit touched (tidy_wires(wires), tidy_wires_at(points)), so
// that finishing one thing does not rearrange the rest of the sheet.
//
// Notification. on_change() is called after every operation that changed
// the sheet or what the history shows (once per operation, not per record),
// including those inside a transaction, so a view can redraw while a drag
// is in progress. revision() changes at the same moments.
//
// Interface thread only; nothing locks.

namespace volt::schematic
{

    // Items taken out of a sheet, as the clipboard holds them: in the
    // sheet's order, ids left behind.
    struct fragment
    {
        std::vector<part> parts;
        std::vector<wire> wires;
        std::vector<label> labels;

        bool empty() const noexcept { return parts.empty() && wires.empty() && labels.empty(); }
        bool operator==(const fragment &) const = default;
    };

    // A reference without its number: "R" of "R12", "R?" and "R"; "U" of
    // "U3"; "" of "12". Trailing '?' and digits are dropped.
    std::string reference_prefix(std::string_view reference);

    // A reference not given a number yet: empty, or ending in '?'.
    bool is_unnumbered_reference(std::string_view reference) noexcept;

    class document
    {
    public:
        // The history limits a document uses unless told otherwise: the
        // stack's, with a merge window large enough that a drag of a few
        // thousand items still merges per record.
        static constexpr history::stack_limits default_limits{.max_steps = 1000, .max_cost = 0, .merge_window = 4096};

        // A document of the sheet, with no history and unmodified.
        explicit document(schematic::sheet start = {}, history::stack_limits limits = default_limits);

        // A copy has the same sheet, history and id counter, and no
        // callback. Precondition: no transaction is open in `other`.
        document(const document &other);
        document &operator=(const document &other);
        document(document &&) noexcept = default;
        document &operator=(document &&) noexcept = default;
        ~document() = default;

        // ---- reading -------------------------------------------------------------

        const schematic::sheet &sheet() const noexcept { return m_sheet; }

        const part *find_part(item_id id) const { return m_sheet.parts.find(id); }
        const wire *find_wire(item_id id) const { return m_sheet.wires.find(id); }
        const label *find_label(item_id id) const { return m_sheet.labels.find(id); }
        bool contains(item_ref item) const noexcept { return schematic::contains(m_sheet, item); }

        // The id the next new item will get.
        item_id next_id() const noexcept { return m_next_id; }

        // The smallest reference of that prefix no part has: "R1", then
        // "R2", ... filling gaps ("R1", "R3" in use gives "R2").
        std::string next_reference(std::string_view prefix) const;

        // The reference add_part() would give the part now: its own when
        // numbered, else the next of its prefix (or of its symbol's). For
        // labelling a transaction that places it ("Place R4").
        std::string reference_for(const part &p) const;

        // The items, copied for the clipboard (unknown ones ignored), in the
        // sheet's order.
        fragment copy(std::span<const item_ref> items) const;

        // ---- adding --------------------------------------------------------------
        //
        // Each returns the new item's id (or ids), 0 (or none) when nothing
        // was added.

        // Places a part. An unnumbered reference ("" or "R?") is numbered:
        // next_reference() of its prefix, or of the symbol's prefix when it
        // has none. An empty device is the symbol's. "Place R1".
        item_id add_part(part p);

        // Draws a wire; a wire of no length is refused (0). "Draw wire".
        item_id add_wire(point a, point b);
        item_id add_wire(wire w);

        // Several wires as one step (a path of several bends): those of no
        // length are left out, and get 0 in the result, which has one id per
        // segment. "Draw wire" ("Draw 3 wires").
        std::vector<item_id> add_wires(std::span<const std::pair<point, point>> segments);

        // A path through the points: a wire from each to the next.
        std::vector<item_id> add_path(std::span<const point> points);

        // Places a label. "Add label" ("Add GND").
        item_id add_label(label l);

        // Adds copies of the items, moved by `offset`, as one step: the
        // paste and duplicate commands. Parts are numbered afresh, as
        // add_part() numbers an unnumbered one, whatever reference they had.
        // Returns the new items, parts first, then wires, then labels, each
        // in the order given. "Paste 3 items".
        std::vector<item_ref> paste(std::span<const part> parts, std::span<const wire> wires,
                                    std::span<const label> labels, point offset = {});
        std::vector<item_ref> paste(const fragment &items, point offset = {});

        // ---- changing --------------------------------------------------------------

        // Removes the items (unknown ones ignored); how many went. "Delete
        // R1" ("Delete 3 items").
        std::size_t erase(std::span<const item_ref> items);

        // Moves the items by delta: parts and labels by their position,
        // wires by both ends. Successive moves with the same non-empty key
        // merge into one step (see the top of this file). False when nothing
        // moved. "Move R1" ("Move 3 items").
        bool move(std::span<const item_ref> items, point delta, std::string_view merge_key = {});

        // Moves one end of a wire (end_b: `b`, else `a`) to a point, for
        // dragging a wire's end; merges as move() does. The wire may have no
        // length on the way; tidy_wires() removes such a wire. "Move wire end".
        bool move_wire_end(item_id wire_id, bool end_b, point to, std::string_view merge_key = {});

        // Turns the items a quarter turn about a point: their positions turn
        // about it, and parts and labels turn too, so that what is drawn is
        // the same drawing turned. Four turns are where they started. "Rotate".
        bool rotate(std::span<const item_ref> items, point about, bool clockwise = false);

        // Mirrors the items about a line through a point: a vertical line
        // (left for right) when vertical_axis, else a horizontal one. Parts
        // are mirrored and turned so that what is drawn is the mirror image;
        // labels, which do not mirror, have their text run the mirrored way.
        // Twice is where they started. "Mirror".
        bool mirror(std::span<const item_ref> items, point about, bool vertical_axis = true);

        // Property edits, for typing into a field: successive edits with the
        // same key merge, and typing that ends at the starting text leaves
        // no step. False when the item is missing or the text is the same.
        bool set_reference(item_id part_id, std::string_view text, std::string_view merge_key = {}); // "Edit reference"
        bool set_value(item_id part_id, std::string_view text, std::string_view merge_key = {});     // "Edit value"
        bool set_label_text(item_id label_id, std::string_view text, std::string_view merge_key = {}); // "Edit label"

        // Replaces a record whole (a properties dialog's OK). "Edit R1",
        // "Edit wire", "Edit label".
        bool replace_part(item_id part_id, part p, std::string_view merge_key = {});
        bool replace_wire(item_id wire_id, wire w, std::string_view merge_key = {});
        bool replace_label(item_id label_id, label l, std::string_view merge_key = {});

        // Tidies the wires: removes those of no length and exact duplicates
        // (the same two ends, either way round; the first in the table
        // stays), and joins two wires that continue each other in a straight
        // line through a shared end where nothing else is attached: no third
        // wire's end or inside, no pin, no label. The wire first in the
        // table stays, with its far end moved; the other goes. Returns how
        // many wires went. "Tidy wires".
        //
        // A pin at the shared end stops a join whatever the connectivity
        // rules: under the default ones the joined wire would still reach it
        // (inside), but not under rules that connect pins at wire ends only,
        // so the sheet would mean something else under those.
        std::size_t tidy_wires();

        // The same for some wires only: the wires given (unknown ids
        // ignored), or those touching one of the points (an end there, or
        // the point inside them). Only those are removed; a join may keep
        // a wire not in the scope, the one first in the table, which then
        // counts as in it. A duplicate of a wire out of the scope is removed
        // whichever comes first. For tidying after an edit without
        // rearranging the rest of the sheet. "Tidy wires".
        std::size_t tidy_wires(std::span<const item_id> wires);
        std::size_t tidy_wires_at(std::span<const point> points);

        // ---- wires that follow, and wires cut ----------------------------------------
        //
        // What dragging and dropping parts does to the wires around them
        // (the editor's tools, <app/ui/windows/schematic_tools.hpp>). Each
        // is an operation as any other: a step alone, joining an open
        // transaction.

        // Makes the wires attached to the moved items follow them, by
        // plan_follow(before, sheet(), moved, rules) (<schematic/follow.hpp>):
        // `before` is the sheet as it was before the items moved (by
        // move(), rotate(), mirror()), nothing else changed since. Returns
        // the wires changed or added, in that order. "Move wires".
        std::vector<item_id> follow_wires(const schematic::sheet &before, std::span<const item_ref> moved,
                                          const connectivity &rules = {});

        // Cuts the wires the parts were dropped on: every wire on which two
        // or more pins of one of the parts lie (where they connect to it
        // under the rules: anywhere on it, or at its ends only) loses the
        // spans between those pins, so that what is left ends at the pins (a
        // wire through both pins of a resistor becomes two, one ending at
        // each pin; a wire from pin to pin goes; along a connector, every
        // span between consecutive pins goes). A wire touching one pin is
        // left alone, and so are the wires in `keep` (a wire pasted with the
        // part, a wire that joined two of its pins before a move). Returns
        // how many wires were cut. "Cut wires".
        std::size_t cut_wires_at_parts(std::span<const item_id> parts, const connectivity &rules = {},
                                       std::span<const item_id> keep = {});

        // Ends a move of the items made since `before` (by the operations
        // above, wires followed or not): cuts the wires the moved parts
        // were dropped on, keeping those that already joined two of a moved
        // part's pins before, then tidies the wires the move changed, added
        // or touches (at the old and new places of the moved pins, labels
        // and wire ends), and no others. False when it changed nothing.
        // "Move R1" ("Move 3 items").
        bool finish_move(const schematic::sheet &before, std::span<const item_ref> moved,
                         const connectivity &rules = {});

        // move(), follow_wires() and finish_move() as one step: what a nudge
        // does. Merges as move() does. "Move R1" ("Move 3 items").
        bool move_with_wires(std::span<const item_ref> items, point delta, std::string_view merge_key = {},
                             const connectivity &rules = {});

        // ---- transactions ------------------------------------------------------------

        // Opens a step: what the operations do until commit() is that one
        // step, labelled `step_label`. Nested calls join the outermost.
        void begin(std::string step_label);

        // Closes a level; the outermost adds the step, merged into the
        // newest step when the key says so. Precondition: one is open.
        void commit(std::string merge_key = {});

        // Undoes everything the open transaction did and closes it, every
        // level. Nothing when none is open.
        void cancel();

        bool in_transaction() const noexcept { return m_history.in_transaction(); }

        // ---- undo and redo ------------------------------------------------------------

        bool can_undo() const noexcept { return m_history.can_undo(); }
        bool can_redo() const noexcept { return m_history.can_redo(); }

        // The labels for the menu ("Undo Move 3 items"); empty for none.
        // Valid until the next change.
        std::string_view undo_label() const noexcept { return m_history.undo_label(); }
        std::string_view redo_label() const noexcept { return m_history.redo_label(); }

        // False when there is nothing to undo or redo, or a transaction is
        // open.
        bool undo();
        bool redo();

        // The newest step takes no more merges: the gesture ended.
        void seal() noexcept { m_history.seal(); }

        // ---- saving -------------------------------------------------------------------

        // Whether the sheet differs from the one last saved (or the one the
        // document started with): undoing back to it is unmodified again.
        bool modified() const noexcept { return m_history.modified(); }

        // The sheet as it is now is what is on disk. Refused (false) while a
        // transaction is open.
        bool mark_saved();

        // ---- notification --------------------------------------------------------------

        // Changes (only ever grows) whenever the sheet or what the history
        // shows (can_undo, labels, modified) may have changed; not for an
        // operation that changed nothing.
        std::uint64_t revision() const noexcept { return m_revision; }

        // Called after each operation that changed revision(); nullptr for
        // none. Not called now. The callback may use the document, edits
        // included: it is then called again once it returns, not inside
        // itself.
        void on_change(std::function<void()> callback);

        // The history, for inspection (a history panel, tests).
        const sheet_history &history() const noexcept { return m_history; }

    private:
        // Opens a level of the history for an operation, and closes it.
        void open(std::string step_label);
        void close(std::string_view merge_key);
        void notify();

        // Records a table edit made to m_sheet, if one was made.
        void record(std::optional<history::record_change<item_id, part>> change);
        void record(std::optional<history::record_change<item_id, wire>> change);
        void record(std::optional<history::record_change<item_id, label>> change);

        item_id take_id() noexcept { return m_next_id++; }
        // The part numbered as add_part() and paste() number it.
        part numbered(part p, bool renumber) const;
        // What the menu calls one item, or a count of them.
        std::string describe(std::span<const item_ref> items) const;
        // tidy_wires() over the wires in `scope`, or all of them when null.
        std::size_t tidy(std::unordered_set<item_id> *scope);

        schematic::sheet m_sheet;
        sheet_history m_history;
        item_id m_next_id = 1;
        // revision(): counts the records changed, undos, redos, saves and
        // transactions opened and closed. The history's own revision is not
        // used: it also changes for an operation that changed nothing (its
        // begin and commit), and not for every change inside a transaction.
        std::uint64_t m_revision = 0;

        std::function<void()> m_on_change;
        std::uint64_t m_notified = 0;
        bool m_notifying = false;
    };

} // namespace volt::schematic
