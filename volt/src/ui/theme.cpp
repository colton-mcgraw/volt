#include <ui/theme.hpp>

#include <gpu/color.hpp>

namespace volt::ui
{

    math::vec4f srgb(float r, float g, float b, float a) noexcept
    {
        const math::vec3f linear = gpu::srgb_to_linear(math::vec3f{r, g, b});
        return {linear[0], linear[1], linear[2], a};
    }

    math::vec4f hex(std::uint32_t rgb, float a) noexcept
    {
        const auto channel = [rgb](int shift) { return static_cast<float>((rgb >> shift) & 0xFFu) / 255.0f; };
        return srgb(channel(16), channel(8), channel(0), a);
    }

    math::vec4f faded(const math::vec4f &color, float alpha) noexcept
    {
        return {color[0], color[1], color[2], color[3] * alpha};
    }

    math::vec4f mix(const math::vec4f &a, const math::vec4f &b, float t) noexcept
    {
        return a + (b - a) * t;
    }

    theme theme::dark_theme()
    {
        theme t;
        t.name = "Dark";
        t.dark = true;

        t.background = srgb(0.09f, 0.10f, 0.12f);
        t.panel = srgb(0.13f, 0.14f, 0.17f);
        t.panel_header = srgb(0.155f, 0.165f, 0.20f);
        t.raised = srgb(0.16f, 0.17f, 0.21f);
        t.field = srgb(0.095f, 0.105f, 0.125f);
        t.border = srgb(0.22f, 0.235f, 0.28f);
        t.separator = srgb(0.20f, 0.215f, 0.255f);
        t.shadow = srgb(0.0f, 0.0f, 0.0f, 0.55f);
        t.backdrop = srgb(0.0f, 0.0f, 0.0f, 0.45f);

        t.text = srgb(0.88f, 0.90f, 0.93f);
        t.text_dim = srgb(0.60f, 0.63f, 0.69f);
        t.text_disabled = srgb(0.40f, 0.42f, 0.47f);
        t.text_on_accent = srgb(1.0f, 1.0f, 1.0f);

        t.accent = srgb(0.20f, 0.60f, 0.86f);
        t.accent_hover = srgb(0.29f, 0.67f, 0.92f);
        t.accent_pressed = srgb(0.16f, 0.50f, 0.74f);
        t.selection = srgb(0.20f, 0.60f, 0.86f, 0.38f);
        t.selection_inactive = srgb(0.30f, 0.32f, 0.38f, 0.55f);
        t.row_hover = srgb(1.0f, 1.0f, 1.0f, 0.05f);
        t.focus_ring = srgb(0.20f, 0.60f, 0.86f, 0.85f);

        t.warning = srgb(0.93f, 0.71f, 0.27f);
        t.error = srgb(0.95f, 0.42f, 0.40f);

        t.control = {srgb(0.19f, 0.20f, 0.245f), srgb(0.235f, 0.25f, 0.30f), srgb(0.15f, 0.16f, 0.195f),
                     srgb(0.15f, 0.16f, 0.19f)};
        t.flat = {srgb(1.0f, 1.0f, 1.0f, 0.0f), srgb(1.0f, 1.0f, 1.0f, 0.08f), srgb(1.0f, 1.0f, 1.0f, 0.04f),
                  srgb(1.0f, 1.0f, 1.0f, 0.0f)};
        t.control_border = srgb(0.26f, 0.28f, 0.33f);
        t.check = t.accent;
        t.track = srgb(0.23f, 0.245f, 0.29f);
        t.scrollbar_thumb = srgb(1.0f, 1.0f, 1.0f, 0.16f);
        t.scrollbar_thumb_hover = srgb(1.0f, 1.0f, 1.0f, 0.30f);
        t.tooltip = srgb(0.21f, 0.22f, 0.27f);

        t.canvas = srgb(0.07f, 0.08f, 0.10f);
        t.canvas_grid = srgb(1.0f, 1.0f, 1.0f, 0.08f);
        t.canvas_wire = srgb(0.78f, 0.80f, 0.84f);
        t.canvas_symbol = srgb(0.78f, 0.80f, 0.84f);
        return t;
    }

    theme theme::light_theme()
    {
        theme t;
        t.name = "Light";
        t.dark = false;

        t.background = srgb(0.89f, 0.90f, 0.92f);
        t.panel = srgb(0.955f, 0.96f, 0.97f);
        t.panel_header = srgb(0.92f, 0.925f, 0.94f);
        t.raised = srgb(0.99f, 0.99f, 1.0f);
        t.field = srgb(1.0f, 1.0f, 1.0f);
        t.border = srgb(0.78f, 0.79f, 0.82f);
        t.separator = srgb(0.84f, 0.85f, 0.87f);
        t.shadow = srgb(0.0f, 0.0f, 0.0f, 0.28f);
        t.backdrop = srgb(0.0f, 0.0f, 0.0f, 0.25f);

        t.text = srgb(0.11f, 0.12f, 0.14f);
        t.text_dim = srgb(0.40f, 0.42f, 0.47f);
        t.text_disabled = srgb(0.62f, 0.64f, 0.68f);
        t.text_on_accent = srgb(1.0f, 1.0f, 1.0f);

        t.accent = srgb(0.16f, 0.52f, 0.80f);
        t.accent_hover = srgb(0.22f, 0.58f, 0.86f);
        t.accent_pressed = srgb(0.12f, 0.44f, 0.70f);
        t.selection = srgb(0.20f, 0.60f, 0.86f, 0.30f);
        t.selection_inactive = srgb(0.0f, 0.0f, 0.0f, 0.09f);
        t.row_hover = srgb(0.0f, 0.0f, 0.0f, 0.045f);
        t.focus_ring = srgb(0.16f, 0.52f, 0.80f, 0.85f);

        t.warning = srgb(0.70f, 0.45f, 0.02f);
        t.error = srgb(0.80f, 0.16f, 0.15f);

        t.control = {srgb(0.97f, 0.975f, 0.98f), srgb(0.93f, 0.94f, 0.955f), srgb(0.88f, 0.89f, 0.91f),
                     srgb(0.93f, 0.935f, 0.94f)};
        t.flat = {srgb(0.0f, 0.0f, 0.0f, 0.0f), srgb(0.0f, 0.0f, 0.0f, 0.07f), srgb(0.0f, 0.0f, 0.0f, 0.12f),
                  srgb(0.0f, 0.0f, 0.0f, 0.0f)};
        t.control_border = srgb(0.74f, 0.75f, 0.78f);
        t.check = t.accent;
        t.track = srgb(0.80f, 0.81f, 0.84f);
        t.scrollbar_thumb = srgb(0.0f, 0.0f, 0.0f, 0.22f);
        t.scrollbar_thumb_hover = srgb(0.0f, 0.0f, 0.0f, 0.38f);
        t.tooltip = srgb(0.99f, 0.99f, 1.0f);

        // Paper, with dark ink: a sheet as it would be printed.
        t.canvas = srgb(0.985f, 0.985f, 0.975f);
        t.canvas_grid = srgb(0.0f, 0.0f, 0.0f, 0.10f);
        t.canvas_wire = srgb(0.16f, 0.18f, 0.22f);
        t.canvas_symbol = srgb(0.16f, 0.18f, 0.22f);
        return t;
    }

} // namespace volt::ui
