#pragma once

#include <font/outline.hpp>
#include <font/raster.hpp>

#include <cmath>
#include <expected>
#include <string>

namespace volt::font::detail
{

    // The whole pixels `shape` (font units, y up) touches at `scale` pixels
    // per unit after moving it right by `shift` pixels, y down, grown by
    // `padding` pixels on every side. False if there is nothing to draw; an
    // error if the image would be absurdly large or the outline is broken.
    inline std::expected<bool, std::string> pixel_box(const outline &shape, float scale, float shift, float padding,
                                                      math::vec2i &origin, math::vec2i &size)
    {
        const box bounds = shape.bounds();
        if (bounds.empty() || !(scale > 0.0f))
            return false;

        const double left = std::floor(static_cast<double>(bounds.min[0] * scale + shift - padding));
        const double right = std::ceil(static_cast<double>(bounds.max[0] * scale + shift + padding));
        const double top = std::floor(static_cast<double>(-bounds.max[1] * scale - padding));
        const double bottom = std::ceil(static_cast<double>(-bounds.min[1] * scale + padding));
        if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(top) || !std::isfinite(bottom))
            return std::unexpected("the outline has coordinates that are not finite");
        if (right - left > max_image_size || bottom - top > max_image_size)
            return std::unexpected("the glyph image would be larger than " + std::to_string(max_image_size) +
                                   " pixels");
        if (right <= left || bottom <= top)
            return false;

        origin = {static_cast<int>(left), static_cast<int>(top)};
        size = {static_cast<int>(right - left), static_cast<int>(bottom - top)};
        return true;
    }

} // namespace volt::font::detail
