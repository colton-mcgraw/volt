#pragma once

#include <history/record.hpp>
#include <history/stack.hpp>
#include <json/json.hpp>
#include <math/vector.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// -------------------------------------------------------------------------
// sheet
// -------------------------------------------------------------------------
//
// One schematic sheet as plain data: the parts placed on it, the wires
// between them, and the labels that name nets or say something. No GPU, no
// widgets: whatever draws a sheet, lists its nets or edits it builds on
// these records.
//
// Coordinates are integers in sheet units, y up. A hundred units are one
// grid square, which is one world unit as the schematic renderer draws it
// (<schematic/scene.hpp>). Everything electrical happens at exact
// integer equality: a wire is connected to a pin when the pin's point is
// on it (an end, or a point inside it, unless the project's connectivity
// says pins connect at wire ends only: `connectivity` below), to another
// wire when one's end is on the other (an end, or a point inside it: a T
// junction). Wires that only cross, interior to interior, are not
// connected. Symbols keep their pins on the grid
// (<schematic/symbol.hpp>), so parts placed on the grid have their pins on
// it in every orientation.
//
// The sheet is three tables of records (history::table, <history/record.hpp>),
// each record under an id. Ids are unique across the whole sheet, not per
// table: a part, a wire and a label never share one, so an id alone names
// an item and a selection can hold all three kinds. 0 is never an id. A
// table keeps its records in order, the order the file is written in, and
// undo puts them back in that order.
//
// The sheet changes only through its tables' edits, each of which returns
// the record_change it made; sheet_change is one of those, for the undo
// stack (history::stack<sheet, sheet_change>). <schematic/document.hpp> is
// the editor's way to change a sheet: it allocates ids and records every
// edit as a step of the history. Changes to different records commute, so
// a drag of many items at once merges into one change per record.

namespace volt::schematic
{

    // A point on the sheet, in sheet units.
    using point = math::vec2i;

    // Sheet units in a grid square (and in one world unit of the renderer).
    inline constexpr int units_per_grid = 100;

    // The grid point nearest to p (halves away from zero), for a grid of
    // `step` units (step > 0).
    point snap_to_grid(point p, int step = units_per_grid) noexcept;

    // The id of a part, wire or label: unique across the sheet, never 0.
    using item_id = std::uint32_t;

    // A placed symbol. Its drawing is its symbol's (find_symbol(symbol)),
    // mirrored, then turned, then moved to `position`: see
    // schematic::transform in <schematic/symbol.hpp>.
    struct part
    {
        std::string symbol;     // the symbol's name: "R", "Conn_01x04"
        point position{};       // where the symbol's origin is
        std::uint8_t rotation = 0; // quarter turns counter-clockwise, 0 to 3
        bool mirrored = false;  // mirrored in x (left for right) before turning

        std::string reference;  // "R1"
        std::string value;      // "10k"
        // What the part is to a simulation: "resistor", "capacitor",
        // "voltage_source", ...; empty when no simulation models it.
        std::string device;
        // The part's id in the project library (<library/project.hpp>);
        // empty for a generic part placed from a symbol alone.
        std::string library_part;

        // Fields of the file this version does not know, in file order,
        // written back on save.
        json::object extra;

        bool operator==(const part &) const = default;
    };

    // One straight segment of wire, at any angle. A path of several bends is
    // several wires that share their ends.
    struct wire
    {
        point a{};
        point b{};

        json::object extra;

        bool operator==(const wire &) const = default;
    };

    enum class label_kind : std::uint8_t
    {
        // Names the net of the wire or pin at its position. Net labels of
        // the same text on one sheet are the same net.
        net,
        // A power port ("GND", "+5V"): names its net as a net label does,
        // and is global by name, the same net on every sheet. Drawn as a
        // power glyph (power_symbol() in <schematic/symbol.hpp>).
        power,
        // Free text, with no electrical meaning.
        note,
    };

    struct label
    {
        label_kind kind = label_kind::net;
        // Where it attaches: the point it names the net of.
        point position{};
        // Quarter turns counter-clockwise, 0 to 3: the direction the text
        // runs from `position` (0: to the right), or the way a power glyph
        // points (0: as drawn, up for a supply, down for ground).
        std::uint8_t rotation = 0;
        std::string text;

        json::object extra;

        bool operator==(const label &) const = default;
    };

    // The file's name for a kind ("net", "power", "note"), and back; false
    // for a name it does not know.
    std::string_view to_string(label_kind kind) noexcept;
    bool parse_label_kind(std::string_view text, label_kind &kind) noexcept;

    // Whether p is on the wire: one of its ends, or a point inside it (with
    // exact integer arithmetic, so only points of the segment itself, which
    // for a diagonal wire may be few).
    bool on_wire(const wire &w, point p) noexcept;

    // Whether p is on the wire but neither of its ends.
    bool inside_wire(const wire &w, point p) noexcept;

    // The rules of connection a project may choose (the others are fixed:
    // see the top of this file). They belong to the project, not to the
    // user, so that a sheet means the same wherever it is opened: the
    // project file holds them (<library/project.hpp>). Everything that works
    // out what is connected takes them, with these defaults: the netlist,
    // junction dots and free-pin marks, cutting wires at parts, and wires
    // following moved parts.
    struct connectivity
    {
        // A pin lying strictly inside a wire connects to it. When false, a
        // pin connects to a wire only at one of the wire's ends (and to a pin
        // at the same point), and a wire drawn past a pin goes by it.
        bool pins_inside_wires = true;

        bool operator==(const connectivity &) const = default;
    };

    // Whether a pin at p is connected to the wire under the rules: at one of
    // its ends, or inside it when the rules allow that.
    bool pin_on_wire(const wire &w, point p, const connectivity &rules = {}) noexcept;

    // ---- the sheet ---------------------------------------------------------

    struct sheet
    {
        history::table<item_id, part> parts;
        history::table<item_id, wire> wires;
        history::table<item_id, label> labels;

        // Top-level fields of the file this version does not know, written
        // back on save. Not edited: no change of the history touches it.
        json::object extra;

        bool operator==(const sheet &) const = default;

        // The largest id any table holds; 0 for an empty sheet.
        item_id max_id() const noexcept;
    };

    // A change of one table's record, as a change of the sheet; and one of
    // those three, the change type of the sheet's undo stack.
    using parts_change = history::member_change<&sheet::parts>;
    using wires_change = history::member_change<&sheet::wires>;
    using labels_change = history::member_change<&sheet::labels>;
    using sheet_change = history::variant_change<sheet, parts_change, wires_change, labels_change>;

    using sheet_history = history::stack<sheet, sheet_change>;

    static_assert(history::change_of<sheet_change, sheet>);
    static_assert(history::mergeable<sheet_change>);
    static_assert(history::commutable<sheet_change>);
    static_assert(history::emptiable<sheet_change>);
    static_assert(history::change_of<parts_change, sheet> && history::change_of<wires_change, sheet> &&
                  history::change_of<labels_change, sheet>);
    static_assert(history::mergeable<parts_change> && history::commutable<parts_change> &&
                  history::emptiable<parts_change>);

    // ---- naming items --------------------------------------------------------

    enum class item_kind : std::uint8_t
    {
        part,
        wire,
        label,
    };

    // An item of a sheet, for selections and hit results. The kind says
    // which table to look in; the id alone is unique already.
    struct item_ref
    {
        item_kind kind = item_kind::part;
        item_id id = 0;

        bool operator==(const item_ref &) const = default;
        auto operator<=>(const item_ref &) const = default;
    };

    // Whether the sheet holds the item (of that kind, under that id).
    bool contains(const sheet &s, item_ref item) noexcept;

} // namespace volt::schematic

template <>
struct std::hash<volt::schematic::item_ref>
{
    std::size_t operator()(const volt::schematic::item_ref &r) const noexcept
    {
        return std::hash<std::uint64_t>{}(static_cast<std::uint64_t>(r.kind) << 32 | r.id);
    }
};
