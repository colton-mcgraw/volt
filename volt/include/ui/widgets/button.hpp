#pragma once

#include <ui/draw_list.hpp>
#include <ui/painter.hpp>
#include <ui/signal.hpp>
#include <ui/widget.hpp>

#include <cstdint>
#include <string>

// -------------------------------------------------------------------------
// button
// -------------------------------------------------------------------------
//
// Text, an icon, or both, that does something when clicked: a press and a
// release on it (pressing, moving off and releasing does nothing), or Space
// or Enter while it has the focus. A toggle button stays down while checked
// and flips on each activation.
//
// Kinds: `normal` is a framed control; `primary` is filled with the accent,
// for the one action a panel is about; `flat` has no frame until hovered,
// for toolbars.

namespace volt::ui
{

    enum class button_kind : std::uint8_t
    {
        normal,
        primary,
        flat,
    };

    class button : public widget
    {
    public:
        explicit button(std::string text = {}, ui::icon icon = icon::none, button_kind kind = button_kind::normal);

        const std::string &text() const noexcept { return m_text; }
        void set_text(std::string text);

        void set_icon(ui::icon icon);

        // An image to show in place of the built-in icon, `size` units
        // square: a texture the renderer knows (see <ui/draw_list.hpp>).
        void set_image(texture_id image, float size);

        void set_kind(button_kind kind);

        void set_toggle(bool toggle) noexcept { m_toggle = toggle; }
        bool checked() const noexcept { return m_checked; }
        void set_checked(bool checked); // does not emit

        // Activates it as a click would.
        void activate();

        signal<> clicked;
        signal<bool> toggled;

        bool is_pressed() const noexcept { return m_pressed && m_inside; }

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_paint(painter &p) override;

        bool on_pointer_down(const pointer_event &e) override;
        bool on_pointer_up(const pointer_event &e) override;
        void on_pointer_move(const pointer_event &e) override;
        void on_pointer_enter() override;
        void on_pointer_leave() override;
        bool on_key(const key_event &e) override;
        bool on_animate(float seconds) override;

    private:
        std::string m_text;
        ui::icon m_icon;
        texture_id m_image;
        float m_image_size = 16.0f;
        button_kind m_kind;

        bool m_toggle = false;
        bool m_checked = false;
        bool m_pressed = false; // the press began on it and is held
        bool m_inside = false;  // ...and the pointer is still on it
        fade m_hover;
    };

} // namespace volt::ui
