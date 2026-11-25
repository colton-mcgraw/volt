#pragma once

#include <math/vector.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// -------------------------------------------------------------------------
// geometry
// -------------------------------------------------------------------------
//
// The few shapes the interface is laid out with. Units are whatever the
// caller works in: widgets use logical units (the window's pixels divided by
// the interface scale, see <ui/painter.hpp>), the draw list uses pixels. x
// grows to the right and y downwards, from the top-left of the window.

namespace volt::ui
{

    // No limit, for a constraint that has none.
    inline constexpr float unbounded = std::numeric_limits<float>::infinity();

    // Where something goes along an axis, in the space it is given.
    enum class align : std::uint8_t
    {
        start, // left, or top
        center,
        end,     // right, or bottom
        stretch, // fills the space (layout only; text treats it as start)
    };

    // Space kept free inside an edge: padding, margins.
    struct insets
    {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;

        static constexpr insets uniform(float all) noexcept { return {all, all, all, all}; }
        static constexpr insets symmetric(float horizontal, float vertical) noexcept
        {
            return {horizontal, vertical, horizontal, vertical};
        }

        constexpr float horizontal() const noexcept { return left + right; }
        constexpr float vertical() const noexcept { return top + bottom; }
        constexpr math::vec2f total() const noexcept { return {horizontal(), vertical()}; }

        friend constexpr bool operator==(const insets &, const insets &) = default;
    };

    // An axis-aligned rectangle: top-left corner and size.
    struct rect
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;

        static constexpr rect from(const math::vec2f &position, const math::vec2f &size) noexcept
        {
            return {position.x(), position.y(), size.x(), size.y()};
        }

        // The rectangle between two corners, in any order.
        static constexpr rect between(const math::vec2f &a, const math::vec2f &b) noexcept
        {
            const float left = std::min(a.x(), b.x());
            const float top = std::min(a.y(), b.y());
            return {left, top, std::max(a.x(), b.x()) - left, std::max(a.y(), b.y()) - top};
        }

        constexpr math::vec2f position() const noexcept { return {x, y}; }
        constexpr math::vec2f size() const noexcept { return {width, height}; }
        constexpr math::vec2f center() const noexcept { return {x + width * 0.5f, y + height * 0.5f}; }
        constexpr float right() const noexcept { return x + width; }
        constexpr float bottom() const noexcept { return y + height; }

        constexpr bool empty() const noexcept { return !(width > 0.0f) || !(height > 0.0f); }

        // Half-open: the right and bottom edges are outside, so rectangles
        // that share an edge never both contain a point on it.
        constexpr bool contains(const math::vec2f &point) const noexcept
        {
            return point.x() >= x && point.y() >= y && point.x() < x + width && point.y() < y + height;
        }

        constexpr bool intersects(const rect &other) const noexcept
        {
            return x < other.right() && other.x < right() && y < other.bottom() && other.y < bottom();
        }

        // The overlap; empty (with zero size) when there is none.
        constexpr rect intersect(const rect &other) const noexcept
        {
            const float left = std::max(x, other.x);
            const float top = std::max(y, other.y);
            const float r = std::min(right(), other.right());
            const float b = std::min(bottom(), other.bottom());
            if (r <= left || b <= top)
                return {left, top, 0.0f, 0.0f};
            return {left, top, r - left, b - top};
        }

        // Smaller by `edges` on each side; never negative in size.
        constexpr rect inset(const insets &edges) const noexcept
        {
            return {x + edges.left, y + edges.top, std::max(0.0f, width - edges.horizontal()),
                    std::max(0.0f, height - edges.vertical())};
        }

        constexpr rect inset(float all) const noexcept { return inset(insets::uniform(all)); }

        constexpr rect outset(float all) const noexcept
        {
            return {x - all, y - all, width + 2.0f * all, height + 2.0f * all};
        }

        constexpr rect translated(const math::vec2f &offset) const noexcept
        {
            return {x + offset.x(), y + offset.y(), width, height};
        }

        friend constexpr bool operator==(const rect &, const rect &) = default;
    };

    // What a parent allows a child in measure(): a size between min and max
    // on each axis. A max of `unbounded` means the child may be as large as
    // it likes (inside a scroll area, along the scrolling axis).
    struct constraints
    {
        math::vec2f min{0.0f, 0.0f};
        math::vec2f max{unbounded, unbounded};

        static constexpr constraints tight(const math::vec2f &size) noexcept { return {size, size}; }
        static constexpr constraints loose(const math::vec2f &size) noexcept { return {{0.0f, 0.0f}, size}; }

        constexpr math::vec2f constrain(const math::vec2f &size) const noexcept
        {
            return {std::clamp(size.x(), min.x(), std::max(min.x(), max.x())),
                    std::clamp(size.y(), min.y(), std::max(min.y(), max.y()))};
        }

        // What is left for the content once `edges` are taken off.
        constexpr constraints deflate(const insets &edges) const noexcept
        {
            const auto shrink = [](float value, float by) { return std::max(0.0f, value - by); };
            return {{shrink(min.x(), edges.horizontal()), shrink(min.y(), edges.vertical())},
                    {shrink(max.x(), edges.horizontal()), shrink(max.y(), edges.vertical())}};
        }
    };

} // namespace volt::ui
