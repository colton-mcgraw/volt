#pragma once

#include <ui/signal.hpp>
#include <ui/widget.hpp>

#include <string>

// -------------------------------------------------------------------------
// checkbox
// -------------------------------------------------------------------------
//
// A box that is ticked or not, with its text beside it. Clicking it, its
// text included, or Space while it has the focus, flips it and emits
// `changed`.

namespace volt::ui
{

    class checkbox : public widget
    {
    public:
        explicit checkbox(std::string text = {}, bool checked = false);

        bool checked() const noexcept { return m_checked; }

        // Does not emit `changed`: that is for the user's changes.
        void set_checked(bool checked);

        const std::string &text() const noexcept { return m_text; }
        void set_text(std::string text);

        void toggle();

        signal<bool> changed;

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_paint(painter &p) override;
        bool on_pointer_down(const pointer_event &e) override;
        bool on_pointer_up(const pointer_event &e) override;
        void on_pointer_enter() override { invalidate(); }
        void on_pointer_leave() override { invalidate(); }
        bool on_key(const key_event &e) override;

    private:
        std::string m_text;
        bool m_checked;
        bool m_pressed = false;
    };

} // namespace volt::ui
