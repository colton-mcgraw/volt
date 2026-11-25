#pragma once

#include <schematic/draw.hpp>
#include <schematic/sheet.hpp>
#include <schematic/symbol.hpp>

#include <math/vector.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// -------------------------------------------------------------------------
// hit testing
// -------------------------------------------------------------------------
//
// Which items of a sheet are under the pointer or in a rectangle, which wire
// end a drag picks up, what a wire being drawn snaps to, and the boxes of
// items and selections. GPU-free and widget-free, like the drawing it
// matches (<schematic/draw.hpp>).
//
// Units. Everything here is in sheet units (100 to a grid square), y up:
// positions under the pointer as math::vec2f, since the pointer falls
// between the integer points; tolerances as a float distance in sheet
// units (a view turns a few pixels into one: pixels / zoom * 100); boxes as
// integer points, rounded outwards.
//
// What counts as being under the pointer:
//   - a part: its body, the box of its symbol as placed (part_bounds(),
//     <schematic/symbol.hpp>), pins and leads included, grown by the
//     tolerance. Its reference, value and pin text are not part of it.
//   - a wire: within the tolerance of the segment.
//   - a label: the box of its text (and a power port's glyph, and a net
//     label's flag), label_bounds(), grown by the tolerance.
//
// Priority, when several are under the pointer: small things over big
// things, since a big thing has room elsewhere to be clicked on and a small
// one may not. Labels first (they sit on wires and pins), the later in the
// sheet first; then wires, the nearest first; then parts, the smallest
// body first, then the later in the sheet (drawn on top). hit_test_all()
// lists them all in that order, for clicking again to cycle through them.

namespace volt::schematic
{

    // A box in sheet units, both corners included.
    struct rect
    {
        point min{};
        point max{};

        point center() const noexcept { return point{(min.x() + max.x()) / 2, (min.y() + max.y()) / 2}; }
        bool operator==(const rect &) const = default;
    };

    // The topmost item under the point (see the top of this file), if any.
    std::optional<item_ref> hit_test(const sheet &s, math::vec2f at, float tolerance, const draw_style &style = {});

    // Every item under the point, in priority order.
    std::vector<item_ref> hit_test_all(const sheet &s, math::vec2f at, float tolerance,
                                       const draw_style &style = {});

    // The items in the rectangle between two corners (in either order):
    // those wholly inside it, or with `crossing` those it touches at all
    // (a wire that passes through it, a part whose body it overlaps). Parts
    // by their bodies, wires by their ends, labels by their boxes. Parts,
    // then wires, then labels, each in the sheet's order.
    std::vector<item_ref> items_in_rect(const sheet &s, point a, point b, bool crossing,
                                        const draw_style &style = {});

    // ---- wire ends --------------------------------------------------------------------

    // One end of a wire: `b` when end_b, else `a`.
    struct wire_end_hit
    {
        item_id wire = 0;
        bool end_b = false;

        bool operator==(const wire_end_hit &) const = default;
    };

    // The wire end nearest the point within the tolerance, for dragging it;
    // of several at the same point, the wire later in the sheet.
    std::optional<wire_end_hit> hit_wire_end(const sheet &s, math::vec2f at, float tolerance);

    // Every wire end at exactly that point, in the sheet's order: what a
    // drag of a shared end should move together.
    std::vector<wire_end_hit> wire_ends_at(const sheet &s, point at);

    // ---- snapping ---------------------------------------------------------------------

    enum class connection_kind : std::uint8_t
    {
        pin,         // a pin's end
        wire_end,    // an end of a wire
        wire_inside, // a point inside a wire (a T joins there)
        label,       // the point of a net label or power port
    };

    // A point a wire can end on and be connected there.
    struct connection_point
    {
        point at{};
        connection_kind kind = connection_kind::pin;
        // The part, wire or label it belongs to.
        item_ref item{};
        // For a pin, the symbol's pin (valid for the life of the program).
        const schematic::pin *pin = nullptr;
        // For a wire end, which one.
        bool end_b = false;
    };

    // The connection point nearest the pointer within the tolerance, by the
    // distance to the point itself; of several equally near, a pin, then a
    // wire end, then a label, then a wire's inside. A wire's inside offers
    // the grid point inside it nearest the pointer; a wire with an end off
    // the grid, when no grid point inside it is near, offers the nearest
    // point of exact integer coordinates on it instead. Never its ends,
    // which are wire ends.
    //
    // Items in `exclude` offer nothing (their pins, ends and insides are
    // passed over): the wires whose end is being dragged, so that the end
    // does not snap to itself. The same whatever the connectivity rules:
    // every point offered connects a wire ending there under all of them.
    std::optional<connection_point> nearest_connection(const sheet &s, math::vec2f at, float tolerance,
                                                       std::span<const item_ref> exclude = {});

    // ---- boxes ---------------------------------------------------------------------------

    // The box of an item, as hit tests see it (a part's body, a wire's
    // ends, a label's box); nullopt for an item not on the sheet.
    std::optional<rect> item_bounds(const sheet &s, item_ref item, const draw_style &style = {});

    // The box of all the items (those not on the sheet ignored); nullopt
    // when there are none.
    std::optional<rect> selection_bounds(const sheet &s, std::span<const item_ref> items,
                                         const draw_style &style = {});

    // What a selection turns about: the centre of its box, snapped to the
    // grid, so that items on the grid stay on it.
    std::optional<point> rotation_pivot(const sheet &s, std::span<const item_ref> items,
                                        const draw_style &style = {});

} // namespace volt::schematic
