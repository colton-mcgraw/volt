#pragma once

#include <schematic/document.hpp>
#include <schematic/scene.hpp>
#include <schematic/sheet.hpp>

#include <math/vector.hpp>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

// -------------------------------------------------------------------------
// drawing a sheet
// -------------------------------------------------------------------------
//
// A sheet (<schematic/sheet.hpp>) as the segments the schematic view draws
// (<schematic/scene.hpp>): wires, the junction dots where they join, parts
// as their symbols' strokes with their references and values, the pin
// numbers and names of boxes, a mark on every pin end nothing is attached
// to, and labels. Then the overlays the editing tools draw on top: a ghost
// of what is being placed or pasted, the wire being drawn, the rubber-band
// rectangle, a snap marker. No GPU and no widgets: the result is a list of
// segments, rebuilt when the sheet or the selection changes.
//
// Units. The sheet is in sheet units (100 to a grid square); segments are in
// world units, a grid square each, so a sheet point p is drawn at p / 100.
// Widths and text heights in draw_style are world units.
//
// Junctions are not stored: a dot is drawn wherever three or more wire ends
// and pins meet at a point, or a wire end or pin lies inside another wire (a
// T, which joins them, <schematic/sheet.hpp>). Two wires meeting end to end
// (a corner, or a straight run) get none, and neither do wires that only
// cross. Under connectivity rules where pins connect at wire ends only
// (draw_options::rules), a pin inside a wire is no T: it gets no dot, and
// its free-pin mark, unless something else is attached there.
//
// Text stays readable in every orientation (<schematic/stroke_font.hpp>): a
// part's reference and value are centred on its symbol's anchors and turn
// with the part, upright or reading upwards, never upside down; labels read
// from their point in the direction of their rotation, along the side above
// (or, upright, to the left of) a wire there.
//
// What a selected item is drawn with: the selection colour, and its lines
// `selection_widening` wider; the same segments otherwise, so a selection
// changes colours and widths, never the number or order of segments. A
// selected wire has no node (-1), so the selection shows over a
// simulation's colours.
//
// The order, later on top: parts, wires, junction dots, labels.

namespace volt::schematic
{

    // Colours (working space, alpha not multiplied in) and sizes (world
    // units) to draw with. The defaults match the dark theme; the
    // application fills the colours from its theme.
    struct draw_style
    {
        math::vec4f wire{0.571f, 0.604f, 0.674f, 1.0f};
        math::vec4f symbol{0.571f, 0.604f, 0.674f, 1.0f};
        math::vec4f text{0.571f, 0.604f, 0.674f, 1.0f};         // reference, value, a box's name
        math::vec4f pin_text{0.355f, 0.375f, 0.434f, 1.0f};     // pin numbers and names
        math::vec4f unconnected{0.890f, 0.147f, 0.133f, 1.0f};  // the mark on a free pin end
        math::vec4f net_label{0.171f, 0.604f, 0.262f, 1.0f};
        math::vec4f power_label{0.571f, 0.604f, 0.674f, 1.0f};
        math::vec4f note{0.319f, 0.355f, 0.434f, 1.0f};
        math::vec4f selection{0.069f, 0.406f, 0.831f, 1.0f};
        math::vec4f ghost{0.571f, 0.604f, 0.674f, 0.5f};        // placing, pasting, a wire being drawn
        math::vec4f rubber_band{0.069f, 0.406f, 0.831f, 0.9f};  // the selection rectangle
        math::vec4f marker{0.855f, 0.462f, 0.060f, 1.0f};       // where a wire would snap to

        float wire_width = 0.12f;
        float symbol_width = 0.08f;
        float junction_diameter = 0.36f;
        float text_stroke = 0.06f;
        float pin_text_stroke = 0.045f;
        float selection_widening = 0.04f; // added to the width of a selected item's lines
        float unconnected_size = 0.16f;   // the side of the open square on a free pin end
        float rubber_band_width = 0.03f;
        float marker_radius = 0.2f;
        float marker_width = 0.05f;

        // Cap heights.
        float reference_height = 0.5f; // a part's reference and value, and a box's name
        float pin_number_height = 0.35f;
        float pin_name_height = 0.4f;
        float label_height = 0.5f;

        bool show_pin_numbers = true;
        bool show_pin_names = true;
    };

    struct draw_options
    {
        draw_style style;

        // Drawn in the selection colour. Items not on the sheet are ignored.
        std::span<const item_ref> selection;

        // The simulation's node of each wire (segment::node), which its
        // segments and the junction dots on it carry; null, or a wire
        // missing from it, for -1. Symbols and text are always -1.
        const std::unordered_map<item_id, std::int32_t> *wire_nodes = nullptr;

        // What connects (<schematic/sheet.hpp>): where junction dots and
        // free-pin marks go.
        connectivity rules;

        // When set, gets one entry per segment appended: the item it draws,
        // or {item_kind::wire, 0} for a junction dot, which belongs to no
        // one item. For a view that highlights an item without a rebuild,
        // and for tests.
        std::vector<item_ref> *owners = nullptr;
    };

    // Appends the sheet's segments to `out`.
    void append_sheet(std::vector<segment> &out, const sheet &s, const draw_options &options);

    // The points where append_sheet() draws a junction dot (see the top of
    // this file), in sheet units, sorted (by x, then y), each once.
    std::vector<point> junctions(const sheet &s, const connectivity &rules = {});

    // ---- overlays ------------------------------------------------------------------
    //
    // Appended after the sheet, by the tools.

    // A ghost of items being placed, pasted or dragged: the parts, wires and
    // labels moved by `offset` (sheet units), every line in `color` (the
    // style's ghost colour, usually), node -1. No junction dots and no marks
    // on free pins.
    void append_fragment(std::vector<segment> &out, std::span<const part> parts, std::span<const wire> wires,
                         std::span<const label> labels, point offset, const draw_style &style,
                         const math::vec4f &color);
    void append_fragment(std::vector<segment> &out, const fragment &items, point offset, const draw_style &style,
                         const math::vec4f &color);

    // A wire being drawn: a path through the points, as wide as a wire, in
    // the ghost colour. Nothing for fewer than two points.
    void append_wire_preview(std::vector<segment> &out, std::span<const point> path, const draw_style &style);

    // The outline of the rectangle between two corners (in either order),
    // thin, in the rubber-band colour.
    void append_rect(std::vector<segment> &out, point a, point b, const draw_style &style);

    // A ring about a point, in the marker colour: where a wire being drawn
    // would attach.
    void append_marker(std::vector<segment> &out, point at, const draw_style &style);

    // ---- extents -------------------------------------------------------------------

    // The box everything drawn of the sheet lies in (text and the width of
    // the lines included), in sheet units, rounded outwards; false, and the
    // two left alone, for an empty sheet. For "fit to view".
    bool sheet_bounds(const sheet &s, point &min, point &max, const draw_style &style = {});

    // The box a label covers as drawn (its text, a power port's glyph), in
    // sheet units: what hit tests and rectangle selection go by.
    void label_bounds(const label &l, const draw_style &style, math::vec2f &min, math::vec2f &max);

} // namespace volt::schematic
