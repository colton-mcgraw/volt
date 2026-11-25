#pragma once

#include <image/image.hpp>
#include <image/path.hpp>
#include <image/raster.hpp>

#include <math/matrix.hpp>
#include <math/vector.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// svg
// -------------------------------------------------------------------------
//
// SVG files as icons and logos use them, parsed into a list of paths with
// paints that can be rasterized here or drawn as vectors by a renderer.
//
// Read: the outer and nested <svg> with viewBox and preserveAspectRatio;
// <g>, <a> and <switch> (its first child); <defs>, <symbol> and <use>;
// <path> with the full path grammar, <rect> (rounded too), <circle>,
// <ellipse>, <line>, <polyline>, <polygon>; transforms; fill and stroke with
// their opacities, rules, widths, caps, joins, miter limits and dashes;
// opacity on any element (composited as a group); colors as #rgb, #rgba,
// #rrggbb, #rrggbbaa, rgb()/rgba(), hsl()/hsla(), the CSS named colors,
// currentColor and transparent; linear and radial gradients with stops,
// gradientUnits, gradientTransform, spreadMethod, focal points, and
// inheritance through href; presentation attributes, the style attribute,
// and <style> sheets as far as simple selectors go (type, .class, #id, *,
// and compounds of them, no combinators); units px, pt, pc, mm, cm, in,
// em, ex and %. Compressed .svgz is read too.
//
// Skipped, without failing: text, images, clipping paths, masks, patterns,
// markers, filters, animation, and any element or property not listed. A
// path whose data turns invalid is drawn up to that point, as the
// specification says. Only XML that is not well-formed, or a document
// without an <svg> root, is an error.

namespace volt::image::svg
{

    // A path to draw, with how. Fills come before strokes.
    struct shape
    {
        volt::image::path path; // in user units

        // User units to the document's viewport, whose units are CSS pixels
        // with (0, 0) at its top left: every transform, viewBox and nested
        // viewport above the shape, combined.
        math::mat3f transform = math::mat3f::identity();

        // Gradients in the paints are already resolved to user units (bounding
        // box units turned into the shape's box); their transforms map to the
        // same user space as the path.
        volt::image::paint fill;
        float fill_opacity = 1.0f;
        volt::image::fill_rule fill_rule = volt::image::fill_rule::nonzero;

        volt::image::paint stroke;
        float stroke_opacity = 1.0f;
        volt::image::stroke_style stroke_style; // width and dashes in user units
    };

    // An entry of the drawing, in painting order.
    struct item
    {
        enum class kind : std::uint8_t
        {
            shape,
            // What follows, up to the matching end_group, is drawn on its own
            // and composited with `opacity`.
            begin_group,
            end_group,
        };

        kind type = kind::shape;
        svg::shape shape;      // kind::shape
        float opacity = 1.0f; // kind::begin_group
    };

    struct document
    {
        // The intrinsic size in CSS pixels: from the root's width and height,
        // or its viewBox where those are missing or relative.
        float width = 0.0f;
        float height = 0.0f;
        std::vector<item> items;
    };

    struct parse_options
    {
        // What a root without width, height or viewBox is taken to be, and
        // what percentages of nothing come to.
        float default_width = 300.0f;
        float default_height = 150.0f;
    };

    // Whether the data looks like an SVG file (an <svg element near the
    // start, past any XML declaration, comments and DOCTYPE), or is gzip,
    // which is how .svgz arrives.
    bool is_svg(std::span<const std::uint8_t> data) noexcept;

    std::expected<document, std::string> parse(std::string_view text, const parse_options &options = {});

    // As above, from file contents: UTF-8, optionally gzip-compressed.
    std::expected<document, std::string> parse(std::span<const std::uint8_t> data, const parse_options &options = {});

    struct rasterize_options
    {
        static constexpr float max_intrinsic = 4096.0f;

        // The output size in pixels. Both zero: the document's size times
        // `scale`, but no more than max_intrinsic on the longer side (a file
        // that says it is a million pixels wide gets scaled down, not a
        // canvas of that size). One zero: the other one decides, keeping
        // the aspect ratio. Both set: the drawing is scaled to fit inside,
        // centered.
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        float scale = 1.0f;

        // Blend and interpolate gradients in linear light rather than in
        // sRGB-encoded values as browsers do (see <image/raster.hpp>).
        bool linear_light = false;

        // How far flattened curves may stray from the true ones, in pixels.
        float tolerance = 0.1f;
    };

    // The document as rgba8, sRGB-encoded, straight alpha, on transparent.
    std::expected<image, std::string> rasterize(const document &source, const rasterize_options &options = {});

    // Draws the document into a canvas, its viewport mapped by `transform`
    // to the canvas' pixels.
    void draw(const document &source, canvas &target, const math::mat3f &transform, float tolerance = 0.1f);

} // namespace volt::image::svg
