#pragma once

#include <ui/signal.hpp>
#include <ui/widget.hpp>

#include <functional>
#include <string>

// -------------------------------------------------------------------------
// slider
// -------------------------------------------------------------------------
//
// A value between a minimum and a maximum, picked by dragging a thumb along
// a track. Pressing the track jumps the thumb there and keeps dragging; the
// value follows the pointer past either end (the press captured it). With a
// step, values are multiples of it from the minimum. Keys while focused:
// Left/Down and Right/Up by a step (a hundredth of the range without one),
// Page Up/Down by a tenth, Home and End to the ends.
//
// With a formatter, the value is written to the right of the track.

namespace volt::ui
{

    class slider : public widget
    {
    public:
        slider(double minimum = 0.0, double maximum = 1.0, double value = 0.0, double step = 0.0);

        double value() const noexcept { return m_value; }
        void set_value(double value); // clamped and stepped; does not emit

        double minimum() const noexcept { return m_minimum; }
        double maximum() const noexcept { return m_maximum; }
        void set_range(double minimum, double maximum, double step = 0.0);

        void set_formatter(std::function<std::string(double)> format);

        signal<double> changed;

        // Where the track runs, in logical units: for tests and for drawing
        // ticks over it.
        float track_left() const noexcept;
        float track_right() const noexcept;

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_paint(painter &p) override;
        bool on_pointer_down(const pointer_event &e) override;
        void on_pointer_move(const pointer_event &e) override;
        bool on_pointer_up(const pointer_event &e) override;
        void on_pointer_enter() override { invalidate(); }
        void on_pointer_leave() override { invalidate(); }
        bool on_key(const key_event &e) override;

    private:
        double snapped(double value) const noexcept;
        void move_to(double value); // emits if it changed
        void follow(float x);
        float label_width() const noexcept { return m_format ? 52.0f : 0.0f; }

        double m_minimum;
        double m_maximum;
        double m_value;
        double m_step;
        std::function<std::string(double)> m_format;
        bool m_dragging = false;
    };

} // namespace volt::ui
