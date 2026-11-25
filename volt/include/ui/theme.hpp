#pragma once

#include <ui/geometry.hpp>
#include <ui/text.hpp>

#include <math/vector.hpp>

#include <cstdint>
#include <string>

// -------------------------------------------------------------------------
// theme
// -------------------------------------------------------------------------
//
// Every colour, size and font the widgets are drawn with, in one value. A
// widget never makes up a colour of its own: it picks a role from here, so
// that switching themes switches everything.
//
// Colours are written as sRGB, the way designers and style sheets write
// them, and converted once, by srgb() and hex(), into the working space of
// <gpu/color.hpp> that the draw list takes. Alpha is not multiplied in.
//
// Sizes are logical units (see <ui/painter.hpp>): at an interface scale of 2
// everything comes out twice as many pixels.

namespace volt::ui
{

    // An sRGB colour, 0 to 1 per channel, as a working space colour.
    math::vec4f srgb(float r, float g, float b, float a = 1.0f) noexcept;

    // 0xRRGGBB, sRGB.
    math::vec4f hex(std::uint32_t rgb, float a = 1.0f) noexcept;

    // `color` with its alpha multiplied by `alpha`.
    math::vec4f faded(const math::vec4f &color, float alpha) noexcept;

    // Linear interpolation in the working space.
    math::vec4f mix(const math::vec4f &a, const math::vec4f &b, float t) noexcept;

    // The look of a control in each of its states.
    struct state_colors
    {
        math::vec4f normal;
        math::vec4f hover;
        math::vec4f pressed;
        math::vec4f disabled;
    };

    struct theme
    {
        std::string name;
        bool dark = true;

        // ---- surfaces ----------------------------------------------------
        math::vec4f background; // behind everything
        math::vec4f panel;      // side panels, bars
        math::vec4f panel_header;
        math::vec4f raised;     // popups, menus
        math::vec4f field;      // inside text fields, lists
        math::vec4f border;
        math::vec4f separator;
        math::vec4f shadow;
        math::vec4f backdrop;   // behind a modal dialog

        // ---- text --------------------------------------------------------
        math::vec4f text;
        math::vec4f text_dim;
        math::vec4f text_disabled;
        math::vec4f text_on_accent;

        // ---- accent and selection -------------------------------------------
        math::vec4f accent;
        math::vec4f accent_hover;
        math::vec4f accent_pressed;
        math::vec4f selection;          // selected text, selected rows while focused
        math::vec4f selection_inactive; // selected rows while not focused
        math::vec4f row_hover;
        math::vec4f focus_ring;

        // ---- state ----------------------------------------------------------
        // Marks, not surfaces: a dot, an icon, a short word saying how
        // something went. Readable as text on panel and field.
        math::vec4f warning;
        math::vec4f error;

        // ---- controls ----------------------------------------------------
        state_colors control;      // buttons, dropdowns, tab heads
        state_colors flat;         // toolbar buttons: transparent until hovered
        math::vec4f control_border;
        math::vec4f check;         // checkbox and radio fill when on
        math::vec4f track;         // slider and progress tracks
        math::vec4f scrollbar_thumb;
        math::vec4f scrollbar_thumb_hover;
        math::vec4f tooltip;

        // ---- canvases ------------------------------------------------------
        // What a document view draws its drawing with (the schematic
        // sheet): not widgets, but they sit among them and switch with them.
        math::vec4f canvas;        // the sheet behind the drawing
        math::vec4f canvas_grid;   // grid lines; alpha is how strongly they show
        math::vec4f canvas_wire;   // wires and junctions
        math::vec4f canvas_symbol; // the outlines of symbols

        // ---- fonts ---------------------------------------------------------
        font_id font = 0;       // everything
        font_id title_font = 0; // the odd heading or brand mark
        float font_size = 13.0f;
        float small_font_size = 11.5f;
        float title_font_size = 15.0f;

        // ---- metrics -------------------------------------------------------
        float spacing = 6.0f;       // between neighbours in a row or column
        float padding = 8.0f;       // inside panels
        float control_padding = 10.0f; // left and right inside buttons and fields
        float control_height = 26.0f;
        float row_height = 22.0f;   // list and tree rows, menu items
        float menu_bar_height = 26.0f;
        float toolbar_height = 36.0f;
        float status_bar_height = 24.0f;
        float tab_height = 28.0f;
        float radius = 4.0f;
        float small_radius = 3.0f;
        float border_width = 1.0f;
        float focus_width = 2.0f;
        float scrollbar_width = 10.0f;
        float splitter_width = 5.0f;
        float check_size = 15.0f;
        float indent = 16.0f;       // per tree level
        float icon_size = 16.0f;
        float shadow_blur = 12.0f;

        // Seconds.
        float caret_blink = 0.53f;
        float tooltip_delay = 0.6f;
        float transition = 0.12f; // hover fades

        ui::text_style body_style() const noexcept { return {font, font_size}; }
        ui::text_style small_style() const noexcept { return {font, small_font_size}; }
        ui::text_style title_style() const noexcept { return {title_font, title_font_size}; }

        // The colours of the default look.
        static theme dark_theme();
        static theme light_theme();
    };

} // namespace volt::ui
