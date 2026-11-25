#pragma once

#include <app/ui/input.hpp>
#include <library/part.hpp>
#include <schematic/document.hpp>
#include <schematic/draw.hpp>
#include <schematic/hit.hpp>
#include <schematic/scene.hpp>
#include <schematic/sheet.hpp>

#include <math/vector.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// schematic tools
// -------------------------------------------------------------------------
//
// What the pointer and the keys do to a schematic document
// (<schematic/document.hpp>) in the schematic editor, for each of its tools:
// the selection, dragging, rubber-band selection, drawing wires, placing
// parts, labels, power ports and notes, and the editing commands (delete,
// rotate, mirror, copy, cut, paste, duplicate, nudge, undo). A state machine
// with no widget, no window and no GPU: the editor's viewport hands it
// pointer events already turned into sheet coordinates, draws what
// append_overlay() adds on top of the sheet, and shows hint() in the status
// bar. Panning and zooming stay with pan_zoom_controller (<ui/controllers.hpp>).
//
// Driving it. The adapter (the editor's viewport_controller) converts each
// pointer event to sheet units, y up, as hit tests take them
// (<schematic/hit.hpp>), and gives it a tolerance: a few pixels in sheet
// units (pixels / zoom * 100). The tolerance is both how near the pointer
// must be to pick something and how far it must move with a button held
// before a press becomes a drag (the slop). Then:
//
//   pointer_down(e)   every button press, e.clicks the press's click count
//                     (2 for the second press of a double-click)
//   pointer_move(e)   every move, button held or not
//   pointer_up(e)     every release
//   pointer_leave()   the pointer left the viewport
//   key_down(k, mods) every key press while the viewport has the focus,
//                     before the editor's own bindings
//
// Each returns an input_result: `consumed` when the tools used the event, so
// that the adapter must not pass it on; `changed` when anything drawn may
// have changed (the document, the selection, the overlay, the hint). A
// middle-button event is never consumed, and neither is a right-button one
// unless a gesture is open (a drag, which it cancels, or a wire being drawn,
// which it ends), so panning and the context menu keep working; pass every
// unconsumed event on to the pan_zoom_controller. Moves are worth passing on
// as well (the pan controller ignores them unless it is panning).
//
// revision() changes whenever the selection, the tool, the overlay or the
// hint may have changed, by an event or by a call; the document's own
// revision() covers the sheet. Redraw when either changes.
//
// The document. set_document() names the document edited, not owned; it
// changes when the user switches sheets, and must be called (with the next
// one, or nullptr) before the document it named dies. Switching cancels any
// gesture, discards a wire being drawn, and clears the selection. With no
// document every call does nothing and every event is unconsumed.
//
// Units and the grid. Everything is in sheet units (<schematic/sheet.hpp>).
// Points placed (a part, a label, a wire's corners, a paste) snap to the
// grid, `grid()` units (100 by default; the editor sets it from its grid
// spacing: 25, 50, 100, 200). A drag snaps its displacement rather than the
// position, so items off the grid keep their offset from it. Holding Ctrl
// turns snapping off while it is held, during a drag or while placing
// (points then round to whole units); set_snap(false) turns it off until
// turned on again. Pins, wire ends and points of wires win over the grid
// where the tools connect something (a wire's ends, a dragged wire end, a
// net label, a power port): the nearest such point within the tolerance
// (nearest_connection()), Ctrl or not.
//
// Connectivity. set_connectivity() gives the project's rules
// (<schematic/sheet.hpp>; the application's schematic_document holds them):
// which pins count as attached to a wire when wires follow a move, and
// which wires a dropped part cuts. Until it is called, the defaults.
//
// The selection is this object's, not the document's, and not undoable: a
// list of items in the order they were selected, each once. Items the
// document no longer has (erased, undone away) leave it by themselves: every
// read of it first drops them, so it never holds dead items when read.
//
// ---- the select tool -----------------------------------------------------
//
//   click              selects the topmost item under the pointer (labels,
//                      then wires, then parts: hit.hpp's priority), or
//                      clears the selection on empty space
//   click again        on the same spot (within the tolerance), as a new
//                      click, not a double-click: selects the next item
//                      under the pointer, round and round (hit_test_all)
//   Shift/Ctrl+click   adds the topmost item, or removes it if it is
//                      selected; empty space leaves the selection alone
//   double-click       asks for the item to be edited: edit_requested(item),
//                      with the item under the pointer selected (the
//                      selected one of those under the pointer, if any)
//   drag an item       moves the selection; the item pressed becomes the
//                      selection first when it was not selected (with
//                      Shift it is added instead; Ctrl at the press does
//                      not add, since Ctrl during a drag is for moving off
//                      the grid). The wires attached to what moves follow
//                      it (document::follow_wires(), the rules in
//                      <schematic/follow.hpp>): a wire attached to a moved
//                      pin stretches along itself, slides sideways when
//                      what holds its far end can stretch, or gets a jog at
//                      the pin, so orthogonal wires stay orthogonal, and a
//                      wire attached at both ends moves whole. Each pointer
//                      move works this out afresh from the sheet as it was
//                      when the drag began, so wandering about adds no
//                      jogs. Alt held at the press (or as the press becomes
//                      a drag) detaches: the selection moves alone and the
//                      wires stay where they are (detached()). The whole
//                      drag is one step ("Move R1", "Move 3 items") held in
//                      a document transaction from the moment the press
//                      becomes a drag: releasing commits it, after cutting
//                      the wires the moved parts were dropped on and tidying
//                      the wires the move touched (document::finish_move():
//                      wires of no length, duplicates, collinear runs
//                      joined; the rest of the sheet is left alone), in the
//                      same step; Escape or a right press cancels it and the
//                      sheet is exactly what it was, with no step made and
//                      nothing to redo lost; a drag that ends where it
//                      began makes no step (and tidies nothing).
//   drag a wire's end  moves that end, and every other wire end at exactly
//                      the same point with it, so a corner or a junction
//                      of wire ends stays joined ("Move wire end", one
//                      step, the wires dragged and those at the old and new
//                      point tidied: an end dragged onto the wire's other
//                      end removes the wire). The end snaps to a pin, a
//                      wire end, a point inside a wire or a net label within
//                      the tolerance (the wires dragged excluded), marked in
//                      the overlay (drag_connection()), else to the grid
//                      itself, not by displacement. A press is on a wire's
//                      end when the topmost item under it is a wire and a
//                      wire end is within the tolerance; except that when
//                      that wire is part of a selection of several items,
//                      the selection moves instead.
//   drag empty space   a rubber band: dragged left to right it selects what
//                      is wholly inside it, right to left what it touches
//                      (crossing); with Shift or Ctrl it adds to the
//                      selection, otherwise it replaces it
//
// ---- the wire tool ---------------------------------------------------------
//
// A click starts a wire at the point under the pointer: a pin, wire end,
// net label or point of a wire within the tolerance, else the grid point.
// The wire then runs from the last fixed point to the pointer as two
// orthogonal segments with one corner (one segment when they line up):
// horizontal first or vertical first, as the pointer first left the last
// point by more than the tolerance (mostly sideways: horizontal first), until
// it comes back to it; toggle_wire_corner() (Space or '/') flips it. A click
// fixes the two segments and continues from the point clicked. A click on a
// connection point (a pin, a wire end, a point inside a wire, a net label),
// a double-click, or Enter finishes the wire. Escape or a right click ends
// it too, keeping what was fixed and dropping the part following the pointer
// (a second Escape goes back to the select tool). Backspace removes the
// last fixed point; removing the starting point abandons the wire.
//
// The document sees nothing until the wire ends: then the whole wire,
// however many clicks it took, is one step, "Draw wire" (with its collinear
// pieces, and pieces of the wires it continues, joined by tidying the new
// wires, document::tidy_wires(wires), in the same step). A wire with nothing
// fixed makes no step. Segments of no length are never added. A wire that
// starts or ends inside another wire connects to it there (a T,
// <schematic/sheet.hpp>); nothing is split. A double-click with no wire
// being drawn does nothing (it is the end of the one before).
//
// ---- placing ----------------------------------------------------------------
//
// The part tool places copies of a template part (begin_place_part(), or
// begin_place_symbol() from a symbol name, or part_template() of a library
// part): its ghost follows the pointer, snapped to the grid; a click places
// it ("Place R1"), numbered by the document (the template's reference is
// kept unnumbered, "R?"), and the tool stays armed with the same template
// for the next one. A part dropped on a wire cuts it: the spans of wire
// between its pins go, in the same step (document::cut_wires_at_parts()),
// so a resistor placed on a wire is in series with it, not shorted. The
// net label, power port and text tools place labels
// (label_kind net, power and note) the same way: net labels and power ports
// snap to a connection point within the tolerance, notes to the grid. A
// power symbol ("GND", "PWR", any symbol whose prefix is "#PWR") is never
// placed as a part: begin_place_part() and begin_place_symbol() of one arm
// the power tool instead.
//
// What a label says is set by set_label_text(); until then a net label is
// "NET1" (the first "NETn" no label has), a power port "GND", a note
// "Text". After a net label is placed whose text ends in digits, the number
// goes up past every label's text ("NET1", "NET2", ...; "D0" then "D1");
// power ports and notes keep their text. The template and the labels can be
// turned and mirrored before placing: rotate_selection() and
// mirror_selection() act on what is being placed while the tool is armed.
// Every placement selects the new item and calls placed(item), for the
// properties dock to show it and focus its text. A double-click does not
// place twice: a press with a click count above 1 is ignored by the
// placing tools.
//
// ---- commands ----------------------------------------------------------------
//
// Callable whatever the tool, from menus or keys:
//
//   delete_selection()   "Delete R1", "Delete 3 items"
//   rotate_selection()   a quarter turn about rotation_pivot() of the
//   mirror_selection()   selection ("Rotate", "Mirror"); while a drag is
//                        moving the selection, in the drag's step; while
//                        something is being placed or pasted, the ghost
//                        instead (no step)
//   copy()               the selection to this object's clipboard (a
//                        fragment, positions relative to the selection's
//                        rotation pivot)
//   cut()                copy, then delete: one step, "Cut R1", "Cut 3 items"
//   paste()              a ghost of the clipboard follows the pointer; a
//                        click drops it ("Paste 3 items", parts renumbered,
//                        the wires they land on cut as placing does, the
//                        wires pasted with them kept) and selects what was
//                        pasted; Escape cancels. The tool stays what it was
//                        underneath.
//   duplicate()          the same with the selection itself, the clipboard
//                        left alone: a ghost of a copy to drop where wanted
//   nudge(dx, dy)        moves the selection by whole grid steps (the arrow
//                        keys), the attached wires following as in a drag
//                        (document::move_with_wires()), or not with
//                        `detached` (Alt and the arrows); successive nudges
//                        are one step, until any other call or press ends
//                        it (document::seal())
//   select_all(), clear_selection()
//   cancel()             Escape, one level per press: a drag or a rubber
//                        band is cancelled; else a wire being drawn ends
//                        (keeping what was fixed); else a paste or duplicate
//                        is abandoned; else the tool goes back to select;
//                        else the selection is cleared
//   undo(), redo()       cancel any gesture (a wire being drawn is
//                        discarded), then the document's undo or redo
//
// The clipboard is in memory; clipboard_text() and set_clipboard_text() turn
// it into the .voltsch text of a sheet holding it (<schematic/io.hpp>) and
// back, for the system clipboard.
//
// key_down() maps the keys that belong to the canvas, without Ctrl, Alt or
// the Super key (menu shortcuts are the editor's): Escape cancel(); Enter
// ends a wire; Backspace removes a wire's last point, or deletes the
// selection when no wire is being drawn; Delete deletes; R rotates counter-
// clockwise, Shift+R clockwise; X mirrors left for right, Y top for bottom;
// Space and '/' flip a wire's corner; the arrows nudge. Alt does not stop
// them in two cases: the arrows with Alt nudge detached, and while a drag
// is open (which Alt may have begun, detached) the keys act as without it.
// Tool shortcuts (W, L, P, A, T) are the editor's: it calls set_tool().
//
// Turning during a drag. R, X and Y while the selection is being dragged
// turn it about its rotation pivot, in the drag's step; the attached wires
// follow the turned pins by the same rules as a move (each by where its own
// pin went), worked out from the sheet the drag began with.
//
// Robustness. Every call tolerates a null document, an empty sheet, events
// out of order (a release with no press, a move with no pointer), and a
// document changed between events (an undo from the menu): a press's items
// are looked up again when it turns into a click or a drag. While a drag is
// open the document holds an open transaction, so its own undo is refused;
// this object's undo() cancels the drag first. If someone else cancels that
// transaction, the drag ends quietly at the next event.
//
// Interface thread only.

namespace volt::ui
{

    class schematic_tools
    {
    public:
        // The editing tools, as the editor's tool strip orders them
        // (schematic_editor::tool).
        enum class tool : std::uint8_t
        {
            select,
            wire,
            label, // net labels
            power, // power ports
            part,
            text, // notes
        };

        // The pointer shape the tools would like.
        enum class pointer_shape : std::uint8_t
        {
            arrow,
            crosshair, // drawing, placing, a rubber band
            move,      // over an item the select tool would drag, or dragging
        };

        // A pointer event in sheet units.
        struct pointer_input
        {
            math::vec2f at{};       // the pointer, sheet units, y up
            float tolerance = 0.0f; // pick distance and drag slop, sheet units
            mouse_button button = mouse_button::left;
            modifiers mods = modifiers::none;
            int clicks = 1; // presses only: 2 for a double-click's second press
        };

        struct input_result
        {
            bool consumed = false; // the tools used it: do not pass it on
            bool changed = false;  // something drawn may have changed: redraw
        };

        schematic_tools();

        // ---- the document ---------------------------------------------------------

        // Not owned (see the top of this file).
        void set_document(schematic::document *doc);
        schematic::document *document() const noexcept { return m_doc; }

        // ---- settings ---------------------------------------------------------------

        // The snap step in sheet units; values below 1 are taken as 1.
        void set_grid(int step);
        int grid() const noexcept { return m_grid; }

        // Snapping to the grid at all (Ctrl suspends it while held).
        void set_snap(bool on);
        bool snap() const noexcept { return m_snap; }

        // The project's connectivity rules (see the top of this file).
        void set_connectivity(const schematic::connectivity &rules);
        const schematic::connectivity &connectivity() const noexcept { return m_rules; }

        // The style hit tests measure labels with (its text height); the
        // editor's drawing style.
        void set_style(const schematic::draw_style &style);

        // The point a placement at `at` would use: snapped to the grid,
        // unless snapping is off or Ctrl is held (then rounded).
        schematic::point snap_point(math::vec2f at, modifiers mods = modifiers::none) const noexcept;

        // ---- the tool ----------------------------------------------------------------

        tool active_tool() const noexcept { return m_tool; }

        // Cancels a gesture (a wire being drawn is discarded) and a paste;
        // nothing when the tool is already active.
        void set_tool(tool t);

        // Arms the part tool with a template (its position ignored, its
        // reference made unnumbered: "R5" is placed as "R?"). A power
        // symbol arms the power tool instead, with the template's value as
        // the text, or "GND" or "+5V" by the symbol.
        void begin_place_part(schematic::part p);
        void begin_place_part(const library::part &p);

        // The same from a symbol's name: its prefix and device
        // (find_symbol(), which knows every name). "GND" and "PWR" arm the
        // power tool, with "GND" or "+5V".
        void begin_place_symbol(std::string_view symbol_name);

        // Arms the power tool with a port's name ("GND", "+5V", "+3V3").
        void begin_place_power(std::string text);

        // The template placing a library part gives: its symbol, value,
        // device and id (as library_part), and the reference "<prefix>?".
        static schematic::part part_template(const library::part &p);

        // The part tool's template, if armed.
        const std::optional<schematic::part> &pending_part() const noexcept { return m_part; }

        // What the next label of that kind will say (see the top of this
        // file), and its quarter turns.
        std::string label_text(schematic::label_kind kind) const;
        std::uint8_t label_rotation(schematic::label_kind kind) const noexcept;

        // Sets what the next label of that kind says; the first form, the
        // kind of the active tool (net labels when it is not a label tool).
        void set_label_text(std::string text);
        void set_label_text(schematic::label_kind kind, std::string text);

        // A label just placed was renamed `text` (typed into its
        // properties): the next one of its kind counts on from that name as
        // it would from a placed one ("D0" then "D1"). Nothing for a name
        // that does not end in digits, or for other kinds than net labels.
        void label_renamed(schematic::label_kind kind, std::string_view text);

        // ---- input -------------------------------------------------------------------

        input_result pointer_down(const pointer_input &e);
        input_result pointer_move(const pointer_input &e);
        input_result pointer_up(const pointer_input &e);
        input_result pointer_leave();
        input_result key_down(key k, modifiers mods = modifiers::none);

        // ---- the wire being drawn --------------------------------------------------------

        bool drawing_wire() const noexcept { return m_gesture == gesture::wiring; }

        // Its fixed points, the start first; empty when none is drawn.
        std::span<const schematic::point> wire_points() const noexcept { return m_wire; }

        // The whole path the overlay shows: the fixed points, then the
        // corner and the point under the pointer.
        std::vector<schematic::point> wire_preview() const;

        // Flips the corner of the part following the pointer. False when no
        // wire is being drawn.
        bool toggle_wire_corner();

        // Removes the last fixed point (the start too, abandoning the wire).
        bool remove_wire_point();

        // Ends the wire, keeping what was fixed: one step, or none when
        // nothing was. False when none was being drawn.
        bool finish_wire();

        // ---- the selection ------------------------------------------------------------------

        std::span<const schematic::item_ref> selection() const;
        bool is_selected(schematic::item_ref item) const;

        // Replaces the selection (items not on the sheet and repeats left
        // out), adds to it, or takes one item out of it.
        void select(std::span<const schematic::item_ref> items);
        void select(schematic::item_ref item) { select(std::span<const schematic::item_ref>(&item, 1)); }
        void add_to_selection(std::span<const schematic::item_ref> items);
        void deselect(schematic::item_ref item);
        void select_all();
        void clear_selection();

        // ---- commands (see the top of this file) ------------------------------------------------

        bool delete_selection();
        bool rotate_selection(bool clockwise = false);
        bool mirror_selection(bool vertical_axis = true);
        bool copy();
        bool cut();
        bool paste();
        bool duplicate();
        bool nudge(int dx, int dy);
        // With `detached`, the wires attached to the selection stay.
        bool nudge(int dx, int dy, bool detached);
        bool cancel();

        bool can_paste() const noexcept { return !m_clipboard.empty(); }
        bool pasting() const noexcept { return m_paste.has_value(); }
        const schematic::fragment &clipboard() const noexcept { return m_clipboard; }

        // The clipboard as .voltsch text (empty for an empty clipboard), and
        // back: false, the clipboard unchanged, for text that is not a
        // sheet or holds nothing.
        std::string clipboard_text() const;
        bool set_clipboard_text(std::string_view text);

        bool undo();
        bool redo();
        // Whether undo() and redo() would do something: a gesture open here
        // does not stop them (they cancel it), a transaction someone else
        // holds does.
        bool can_undo() const noexcept;
        bool can_redo() const noexcept;
        // "Move R1", for "Undo Move R1"; empty for none.
        std::string_view undo_label() const noexcept;
        std::string_view redo_label() const noexcept;

        // ---- what is going on -------------------------------------------------------------------

        // A press held, a drag, a rubber band or a wire being drawn.
        bool in_gesture() const noexcept { return m_gesture != gesture::none; }
        // A drag moving the selection or a wire's end.
        bool dragging() const noexcept { return m_gesture == gesture::moving || m_gesture == gesture::wire_end; }
        bool banding() const noexcept { return m_gesture == gesture::band; }

        // The selection is being dragged alone, its wires left where they
        // are (Alt was held as the drag began).
        bool detached() const noexcept { return m_gesture == gesture::moving && m_detached; }

        // Where a dragged wire end snaps to a connection (a pin, a wire end,
        // a point inside a wire), if it does.
        const std::optional<schematic::connection_point> &drag_connection() const noexcept
        {
            return m_end_connection;
        }

        // Something follows the pointer to be placed: a part, a label, a
        // paste.
        bool placing() const noexcept;

        // Everything transient, in sheet-drawing segments (world units, as
        // <schematic/draw.hpp> makes them), appended after the sheet: the
        // ghost of what is being placed or pasted, the wire being drawn, a
        // marker where a wire, a dragged wire end or a label would connect,
        // the rubber band.
        void append_overlay(std::vector<schematic::segment> &out, const schematic::draw_style &style) const;

        // One line for the status bar: what the tool does now.
        std::string hint() const;

        pointer_shape cursor() const;

        // The item under the pointer (select tool), and the connection point
        // a wire or label would attach to (wire and label tools).
        std::optional<schematic::item_ref> hover() const;
        const std::optional<schematic::connection_point> &hover_connection() const noexcept
        {
            return m_hover_connection;
        }

        // Changes whenever what the tools show may have (see the top).
        std::uint64_t revision() const;

        // ---- notifications ------------------------------------------------------------------------

        // A double-click on an item: focus its properties.
        std::function<void(schematic::item_ref)> edit_requested;

        // A part or label was placed (and is now the selection): focus its
        // text.
        std::function<void(schematic::item_ref)> placed;

    private:
        enum class gesture : std::uint8_t
        {
            none,
            pressed,  // the left button is down, not moved past the slop
            moving,   // dragging the selection (a transaction is open)
            wire_end, // dragging wire ends (a transaction is open)
            band,     // a rubber band
            wiring,   // a wire being drawn (nothing in the document yet)
        };

        // Drops dead items from the selection.
        void prune() const;
        void touch() const noexcept { ++m_revision; }
        // Grows whenever revision() or the document's does.
        std::uint64_t stamp() const;
        input_result result(std::uint64_t before, bool consumed) const;
        // The selection replaced, without ending a nudge.
        void set_selection(std::span<const schematic::item_ref> items);
        void left_down(const pointer_input &e);
        // Another action: the nudging step ends.
        void interrupt();
        const schematic::sheet *sheet() const noexcept;

        // Cancels a drag or band, discards a wire being drawn.
        void abort_gesture();
        // Forgets what a drag kept.
        void clear_drag();
        // After parts were placed or pasted (in the open transaction): the
        // wires they landed on cut (those given kept), and the wires given
        // and those at the parts' pins tidied.
        void settle_parts(std::span<const schematic::item_id> parts, std::span<const schematic::item_id> wires);
        void end_paste();

        // The point a wire or label snaps to, and the connection if any.
        schematic::point connect_point(math::vec2f at, float tolerance, modifiers mods,
                                       std::optional<schematic::connection_point> *connection) const;
        schematic::point snap_delta(math::vec2f delta, modifiers mods) const noexcept;
        std::vector<schematic::point> route(schematic::point from, schematic::point to) const;

        void select_press(const pointer_input &e);
        void click(const pointer_input &e);
        void start_drag(const pointer_input &e);
        void drag(const pointer_input &e);
        // The drag of the selection done again from the sheet it began
        // with: its turns, then the displacement, then the wires following.
        void redo_drag(schematic::point delta);
        void end_drag();
        void end_band(const pointer_input &e);
        void wire_click(const pointer_input &e);
        void place(const pointer_input &e);
        void drop_paste(const pointer_input &e);
        void update_hover(const pointer_input &e);
        // update_hover() at the pointer as last seen, when idle.
        void refresh_hover();
        void update_wire_target(const pointer_input &e);

        // The selection as a fragment about its pivot, for copy and
        // duplicate.
        std::optional<schematic::fragment> selection_fragment() const;
        std::string describe(std::span<const schematic::item_ref> items) const;
        std::size_t kind_index(schematic::label_kind kind) const noexcept { return static_cast<std::size_t>(kind); }
        std::optional<schematic::label_kind> placing_label() const noexcept;
        // After a label of that text was placed: the next number.
        void advance_label_text(schematic::label_kind kind, std::string_view placed_text);

        schematic::document *m_doc = nullptr;
        int m_grid = schematic::units_per_grid;
        bool m_snap = true;
        schematic::draw_style m_style;
        tool m_tool = tool::select;

        // The pointer, as last seen.
        bool m_has_pointer = false;
        math::vec2f m_pointer{};
        float m_tolerance = 0.0f;
        modifiers m_mods = modifiers::none;

        gesture m_gesture = gesture::none;
        math::vec2f m_press_at{};
        modifiers m_press_mods = modifiers::none;
        bool m_left_down = false;      // the left press was consumed, its release is ours
        bool m_right_consumed = false; // the right press was consumed, its release is ours

        // A drag of the selection: the sheet it began with, the items
        // dragged, the step's label, the turns made on the way (each about
        // its pivot less the displacement at the time, so that they apply
        // before the displacement), the displacement applied now, and
        // whether the wires stay. A drag of wire ends: the ends dragged,
        // where they were and are, and the connection they snap to.
        struct drag_turn
        {
            bool mirror = false;
            bool clockwise = false;     // a rotation's way
            bool vertical_axis = false; // a mirror's axis
            schematic::point about{};
        };
        schematic::sheet m_drag_start;
        std::vector<schematic::item_ref> m_drag_items;
        std::string m_drag_label;
        std::vector<drag_turn> m_turns;
        schematic::point m_applied{};
        bool m_detached = false;
        std::vector<schematic::wire_end_hit> m_ends;
        schematic::point m_end_from{};
        schematic::point m_end_at{};
        std::optional<schematic::connection_point> m_end_connection;
        schematic::connectivity m_rules;

        // The wire being drawn: fixed points, the point under the pointer,
        // the connection there, the corner's way (decided by the pointer, or
        // forced by toggle_wire_corner()).
        std::vector<schematic::point> m_wire;
        schematic::point m_wire_target{};
        std::optional<schematic::connection_point> m_target_connection;
        bool m_horizontal_first = true;
        bool m_direction_set = false;
        bool m_corner_forced = false;

        // The last click, for clicking through what is under the pointer.
        std::optional<math::vec2f> m_last_click;
        std::vector<schematic::item_ref> m_last_hits;
        std::size_t m_cycle = 0;

        // What is placed: the part template, labels' text and turns, a
        // paste's fragment (about its pivot); where the ghost is.
        std::optional<schematic::part> m_part;
        std::array<std::string, 3> m_label_text;
        std::array<bool, 3> m_label_text_set{};
        std::array<std::uint8_t, 3> m_label_rotation{};
        std::optional<schematic::fragment> m_paste;
        schematic::point m_ghost_at{};

        schematic::fragment m_clipboard;

        std::string m_nudge_key;
        std::uint64_t m_serial = 0;

        std::optional<schematic::item_ref> m_hover_item;
        std::optional<schematic::connection_point> m_hover_connection;

        mutable std::vector<schematic::item_ref> m_selection;
        mutable std::uint64_t m_revision = 0;
    };

} // namespace volt::ui
