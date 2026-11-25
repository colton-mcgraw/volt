#pragma once

#include <schematic/sheet.hpp>

#include <span>
#include <utility>
#include <vector>

// -------------------------------------------------------------------------
// wires following what moves
// -------------------------------------------------------------------------
//
// When items of a sheet move (a drag, a nudge, a turn), what they were
// connected to should stay connected to them: the wires attached to a moved
// part's pins follow the pins, and the routing stays tidy (orthogonal wires
// stay orthogonal). plan_follow() works out how, from the sheet before the
// move and after it (the moved items moved, nothing else changed); the
// document applies the plan (document::follow_wires(), <schematic/document.hpp>).
// Plain data, no history: a drag plans again from the sheet it started
// with at every pointer move, so the path never collects jogs as the
// pointer wanders.
//
// What moves. A moving point is where something moved was: a pin of a
// moved part, an end of a moved wire, the point of a moved net label or
// power port, and every point strictly inside a moved wire. Each goes where
// that thing went (with several things there, the first in the sheet's
// order, parts then wires then labels, decides; under one rigid move they
// all agree). A wire not moved itself that has an end on a moving point is
// attached; a net label or power port not moved that sits on one is carried
// to where the point went.
//
// Held points. A moving point is held when something there stays: a pin of
// a part not moved, or a wire not moved and not attached that the point is
// strictly inside (a T onto it) and that the things at the point connect to
// (a wire end, a label, or a pin when pins connect inside wires). Unless
// the point's new place is still on that wire, nothing attached there
// follows: the wires keep their ends there, connected to what stays, and
// when a pin or a wire end moved away from it, a new wire is drawn from the
// held point to where the pin or end went (one segment when the two line
// up, else two by a corner, the first leg across any horizontal wire there,
// along it otherwise). So connections survive even pin-to-pin and T joins.
// A label moved off a held point leaves alone: labels draw no wires.
//
// An attached wire with both ends on moving points moves with them, whole:
// each end goes where its point went.
//
// An attached wire with one end E on a moving point (from p to q) and the
// other end F staying:
//
//   - A diagonal wire (neither horizontal nor vertical) just has E moved to
//     q. So does any wire when the move is along it (stretching or
//     shrinking it; through F and beyond, too).
//   - A horizontal or vertical wire, the move having a component across it:
//     the wire slides sideways by that component, F with it, when everything
//     at F can take that by stretching: F holds no pin and no label, is not
//     inside another wire, and every other wire ending at F is not moved, not
//     attached, not slid already, and runs across the wire (along the slide),
//     so that it stretches or shrinks (to no length at most, never past its
//     other end). Nothing may be left behind or reached: no pin, label or
//     unmoved wire end may lie strictly inside the wire, on its new place
//     (other than at q), or on the stretch of a bent wire between F and its
//     new end; F's new place may not be on an unmoved wire. One level only:
//     the wires at F stretch, nothing beyond them moves. A point F is slid
//     once, a wire bent once, the first attached wire in the sheet's order
//     first.
//   - Otherwise a jog: E moves along the wire by the move's component along
//     it, and a new wire goes from there across to q (a corner at the pin).
//     When the wire or the jog would then run over another moved pin (the
//     part's own other pin, after a move along the part's axis), the jog is
//     made a grid square further along the wire (or short of it) and comes
//     back to the pin with a third leg, round the part.
//
// What the routing must not do is join nets. The plan is checked: if, with
// it, pins that were on different nets before the move are on one net after
// it (two routes meeting, a stretched wire over a moved pin), and the move
// itself did not put a moved pin, wire end or label onto something of
// another net (a connection the user made), the plan is made again with
// every attached end taken straight to where its point went (diagonally if
// need be); and if even that joins nets, nothing follows: the wires stay
// where they are, as in a detached move, and the pins are left unconnected,
// which the sheet shows.
//
// Wires of no length and duplicates may come out (a jog of a wire moved
// back where it was, a wire stretched to nothing); the document tidies them
// when the move ends (document::finish_move()).
//
// Where routing comes out less than tidy: several attached wires at one
// pin each make their own jog (they then meet at the pin from two
// directions); a pin lying strictly inside an attached wire (not at its
// end) does not pull it; wires attached to the inside of a wire that
// follows whole (not moved itself) are left where they are; an attached
// wire shrunk past a T or a pin inside it lets go of it.

namespace volt::schematic
{

    struct follow_plan
    {
        // Wires of the sheet with new ends, in the sheet's order.
        std::vector<std::pair<item_id, wire>> wires;
        // New wires: jogs at moved pins and wires from held points.
        std::vector<wire> added;
        // Labels carried with the point they sit on, in the sheet's order.
        std::vector<std::pair<item_id, label>> labels;

        bool empty() const noexcept { return wires.empty() && added.empty() && labels.empty(); }
    };

    // How the wires attached to the moved items follow them (see the top
    // of this file). `before` is the sheet before the move, `after` the
    // same sheet with the items in `moved` moved (turned, mirrored) and
    // nothing else changed. Items not on both sheets are ignored.
    follow_plan plan_follow(const sheet &before, const sheet &after, std::span<const item_ref> moved,
                            const connectivity &rules = {});

} // namespace volt::schematic
