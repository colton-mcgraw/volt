#pragma once

#include <image/path.hpp>

// Flattening with a limit on how far the direction may turn between two
// points, which strokes need on top of the distance tolerance: the outside
// of a wide stroke is further out than the curve, and a polyline that is
// close enough to the curve can still give it visible corners there.
// Internal to the image library.

namespace volt::image::detail
{

    // As image::flatten(), and no segment of a curve turns more than
    // `max_turn` radians (0 for no limit).
    std::vector<contour> flatten(const path &source, const math::mat3f &transform, float tolerance, float max_turn);

    // The geometry of a full turn in polygon steps: the angle per step for
    // a circle of `radius` to stay within `tolerance`.
    float arc_step(float radius, float tolerance) noexcept;

} // namespace volt::image::detail
