#pragma once

#include <app/ui/windows/schematic_tools.hpp>
#include <schematic/netlist.hpp>
#include <schematic/sheet.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// selection properties
// -------------------------------------------------------------------------
//
// The schematic editor's Properties page for what the editing tools have
// selected (<app/ui/windows/schematic_tools.hpp>), above the sheet's own
// rows:
//
//   nothing        nothing here (the sheet's rows follow)
//   one part       Reference and Value, typed into; Symbol, Device and the
//                  library part, read only: a placeholder (<app/ui/windows/
//                  part_match.hpp>) says "Placeholder — no part assigned"
//                  with Assign Part…, an assigned part has Show in Library
//                  and Change Part… (both through hooks::assign); X and Y
//                  in grid units; Rotation; Mirrored; then each pin's net
//   one wire       its two ends, its length and its net
//   one label      Text, typed into; Kind; X and Y; Rotation; its net
//   several        how many of each kind
//   any of them    Rotate, Mirror and Delete, as the Edit menu does them
//
// Typing is undoable as the part editor's typing is: every keystroke is an
// edit of the document (set_reference(), set_value(), set_label_text()),
// under a merge key of the item and the field (property_merge_key()), so
// one run of typing is one step; the field losing the focus, or Enter,
// seals it. X and Y move the item by the difference when they are
// committed (Enter, the focus leaving, the arrows), the wires attached to
// it following as they do a drag (document::move_with_wires(), by the
// tools' connectivity rules); Rotation and Mirrored replace the record, one
// step each.
//
// The keyboard and the canvas. focus_text() is how the editor hands the
// keyboard to the page after placing a net label or a note, or on a
// double-click: typing goes into the field, and Enter or Escape, in that
// field or in another of the page's text fields reached from it with Tab,
// keeps what was typed and gives the keyboard back (hooks::return_focus:
// the canvas, its tool still armed). Escape in any of the page's text
// fields does that, and is used up there: it never also cancels the tool or
// clears the selection. A field clicked into keeps Enter for itself. Plain
// keys typed into a field are the field's (the text field claims them), so
// none of the tools' keys or the window's one-key shortcuts fire. While a
// net label focused that way is renamed, hooks::renamed hears each name,
// for the next label to count on from it.
//
// The page is made again only when the selection changes (or the document,
// or a part's symbol), in update(), which the editor calls between frames;
// otherwise update() puts the item's values into the fields that differ,
// leaving alone the one being typed in (whose text is the item's already)
// and a number field with the focus. So the field being typed in is never
// made again and keeps its caret. Nothing here is made again from inside
// a field's or a button's own handler.
//
// Interface thread. The object outlives its widgets (it belongs to the
// editor, the widgets to the window's tree): detach() forgets them.

namespace volt::ui
{

    class box;
    class widget;

    // "type:value:12": what typing into `field` of item `id` merges under.
    std::string property_merge_key(std::string_view field, schematic::item_id id);

    // Sheet units in grid units, as the fields and the status bar show
    // them: 250 is "2.5", -300 is "-3", 25 is "0.25".
    std::string grid_units(int sheet_units);

    // "2 parts, 3 wires, 1 label": the kinds among the items (those not on
    // the sheet left out), in that order.
    std::string selection_summary(const schematic::sheet &s, std::span<const schematic::item_ref> items);

    // "90°": a rotation in quarter turns, as the Rotation choice lists it.
    std::string rotation_text(std::uint8_t quarter_turns);

    // A part's pin and the net it is on, as the page lists them.
    struct pin_net
    {
        std::string number; // "1"
        std::string name;   // "+", empty for a plain passive's
        std::string net;    // "Net-(R1-1)", "GND"; empty when the pin touches nothing
    };

    // The part's pins, in its symbol's order, with their nets from `nets`
    // (the netlist of the sheet the part is on).
    std::vector<pin_net> pin_nets(const schematic::part &p, schematic::item_id id, const schematic::netlist &nets);

    // "1: Net-(R1-1)", "2 (-): not connected".
    std::string pin_net_text(const pin_net &pin);

    // The power ports everyone uses, as the Place menu, the power pick and
    // the Tool section offer them: GND, +3V3, +5V, +12V, -12V, VCC.
    std::span<const std::string_view> standard_power_ports() noexcept;

    // What the power pick lists: the standard ports, then the other power
    // ports' names on the sheet (each once, sorted; empty names left out).
    std::vector<std::string> power_port_names(const schematic::sheet &s);

    // How many placed parts are placeholders, and which.
    std::vector<schematic::item_ref> unassigned_parts(const schematic::sheet &s);

    class selection_properties
    {
    public:
        struct hooks
        {
            // The nets of the sheet the tools edit; null for none.
            std::function<const schematic::netlist *()> netlist;
            // Shows a project-library part (by its id there) in the
            // Library Browser; unset, or can_reveal() false, greys the
            // button out.
            std::function<void(const std::string &id)> reveal;
            std::function<bool()> can_reveal;
            // After the page changed the document or the selection, for
            // the editor to bring the view up to date.
            std::function<void()> edited;
            // Typing done (see the top of this file): the keyboard goes back
            // to the canvas.
            std::function<void()> return_focus;
            // A net label the page was handed (focus_text()) renamed.
            std::function<void(schematic::label_kind kind, const std::string &text)> renamed;
            // Assign Part… and Change Part… of the selected part; unset
            // hides both.
            std::function<void()> assign;
            // A project-library part's name, by its id there, for the
            // Library part row; empty (or unset) shows the id.
            std::function<std::string(const std::string &id)> part_name;
        };

        selection_properties(schematic_tools &tools, hooks h);
        ~selection_properties();

        selection_properties(const selection_properties &) = delete;
        selection_properties &operator=(const selection_properties &) = delete;

        // The page's widget, empty until update(); the caller puts it in its
        // tree.
        std::unique_ptr<widget> build();

        // The widgets went (the window closed).
        void detach();

        // Made again for another selection, or brought up to date.
        void update();

        // Focuses the item's main text field (a part's Value, a label's
        // Text) with all of it selected, after making the page for the
        // selection now if need be. False when the item is not what the
        // page shows or has no such field.
        bool focus_text(schematic::item_ref item);

        // What the page was last made for.
        std::span<const schematic::item_ref> shown() const noexcept { return m_items; }

    private:
        struct fields;

        void rebuild();
        void refresh();
        void build_part(schematic::item_id id);
        void build_wire(schematic::item_id id);
        void build_label(schematic::item_id id);
        void build_several();
        void build_actions();
        void done();
        // Enter or Escape in one of the text fields: `escape`, or the page
        // was handed the keyboard.
        void typing_done(bool escape);

        schematic_tools &m_tools;
        hooks m_hooks;
        box *m_holder = nullptr;
        std::unique_ptr<fields> m_fields;

        // What the page shows: the document, the items, a part's symbol.
        const schematic::document *m_document = nullptr;
        std::vector<schematic::item_ref> m_items;
        std::string m_symbol;
        bool m_made = false;
        bool m_building = false;
        // focus_text() handed the keyboard over; until a field is clicked
        // into, the page is made again, or the keyboard is given back.
        bool m_handed = false;
    };

    // ---------------------------------------------------------------------
    // tool options
    // ---------------------------------------------------------------------
    //
    // The Properties page's Tool section, above the selection's: what the
    // armed placing tool will place next, changeable before placing it.
    //
    //   Power Port   Name, typed into (each keystroke arms it, an empty name
    //                is not taken), and Preset, power_port_names() of the
    //                sheet: choosing one arms it
    //   Net Label    Next text (what the next label says; numbers count up
    //   Text         as labels are placed)
    //   Part         what is armed: Symbol, Value (typed into: the next
    //                parts placed get it), the library part or "Placeholder";
    //                with nothing armed, Choose Part… (hooks::choose_part)
    //   Select, Wire nothing (the hint line under the canvas has their keys)
    //
    // Enter or Escape in its field gives the keyboard back to the canvas
    // (hooks::return_focus), keeping what was typed. Made again when the
    // tool, or the part armed, changes, in update(); otherwise the fields
    // follow the tools, but not one being typed in.

    class tool_options
    {
    public:
        struct hooks
        {
            // After the section changed the tools, for the editor to bring
            // the view up to date.
            std::function<void()> edited;
            std::function<void()> return_focus;
            std::function<void()> choose_part;
        };

        tool_options(schematic_tools &tools, hooks h);
        ~tool_options();

        tool_options(const tool_options &) = delete;
        tool_options &operator=(const tool_options &) = delete;

        // The section's widget, empty until update().
        std::unique_ptr<widget> build();
        void detach();
        void update();

        // Focuses the section's field (a power port's name, a label's next
        // text, a part's value), all of it selected, after making the
        // section now if need be. False when the armed tool has none.
        bool focus_field();

    private:
        struct fields;

        void rebuild();
        void refresh();

        schematic_tools &m_tools;
        hooks m_hooks;
        box *m_holder = nullptr;
        std::unique_ptr<fields> m_fields;

        // What the section was made for: the tool, the part armed (its
        // symbol and library part), the document (for the presets).
        std::optional<schematic_tools::tool> m_tool;
        std::string m_part_key;
        const schematic::document *m_document = nullptr;
        std::uint64_t m_presets_revision = 0;
        bool m_building = false;
    };

} // namespace volt::ui
