#pragma once

#include <ui/widget.hpp>

#include <string>

// -------------------------------------------------------------------------
// progress_bar
// -------------------------------------------------------------------------
//
// How far along something is, 0 to 1; or, while indeterminate, that it is
// busy at all, with a segment sweeping along the bar. The sweep animates,
// so an indeterminate bar asks for frames for as long as it is shown.

namespace volt::ui
{

    class progress_bar : public widget
    {
    public:
        explicit progress_bar(float value = 0.0f) : m_value(value) {}

        float value() const noexcept { return m_value; }
        void set_value(float value);

        bool indeterminate() const noexcept { return m_indeterminate; }
        void set_indeterminate(bool indeterminate);

        // Written over the bar, e.g. "42%".
        void set_text(std::string text);

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_paint(painter &p) override;
        bool on_animate(float seconds) override;
        void on_attached() override;

    private:
        float m_value;
        bool m_indeterminate = false;
        float m_phase = 0.0f;
        std::string m_text;
    };

} // namespace volt::ui
