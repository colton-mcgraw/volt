#pragma once

#include <ui/screen.hpp>
#include <ui/signal.hpp>
#include <ui/widgets/text_field.hpp>

#include <string>

// -------------------------------------------------------------------------
// numeric_field
// -------------------------------------------------------------------------
//
// A text field for a number, with up and down arrows: a spin box. It knows
// engineering notation (<math/electrical/engineering.hpp>): with a unit set
// it shows 4700 ohms as "4.7kΩ" and reads back what an engineer types,
// "4k7", "4.7 k", "4700", "2M2", "100n". Without one it shows the plain
// number.
//
// The text is read when the user presses Enter or the field loses the focus;
// a value it cannot read puts the previous one back. The value is clamped
// to [minimum, maximum]. The arrows, Up/Down and the wheel (while focused)
// step it: by `step`, or with engineering notation and no step, through the
// E12 decade (1, 1.2, 1.5, ... 8.2, 10) as component values go. Holding an
// arrow repeats.

namespace volt::ui
{

    class numeric_field : public text_field
    {
    public:
        explicit numeric_field(double value = 0.0, double minimum = -1.0e300, double maximum = 1.0e300,
                               double step = 1.0);

        double value() const noexcept { return m_value; }
        void set_value(double value); // clamped; does not emit

        void set_range(double minimum, double maximum);
        void set_step(double step) noexcept { m_step = step; }

        // Shows and reads the value in engineering notation with this unit
        // ("Ω", "F", "V"; "" for a bare prefix).
        void set_engineering(std::string unit, int digits = 3);

        // Plain notation with this many decimals.
        void set_decimals(int decimals);

        // Steps up (+1) or down (-1); emits if the value changed.
        void step_by(int steps);

        // Reads the text now; emits if the value changed.
        void commit();

        // The value as the field would show it.
        std::string format(double value) const;

        signal<double> value_changed;

        cursor_shape cursor(const math::vec2f &point) const override;

    protected:
        void on_paint(painter &p) override;
        bool on_pointer_down(const pointer_event &e) override;
        bool on_pointer_up(const pointer_event &e) override;
        bool on_scroll(const scroll_input &e) override;
        bool on_key(const key_event &e) override;
        void on_focus_changed(bool focused, focus_reason reason) override;
        rect text_area() const override;
        void on_detached() override
        {
            stop_repeat();
            text_field::on_detached();
        }

    private:
        rect arrows() const;
        void stop_repeat();

        double m_value;
        double m_minimum;
        double m_maximum;
        double m_step;

        bool m_engineering = false;
        std::string m_unit;
        int m_digits = 3;
        int m_decimals = 2;

        int m_held = 0; // the arrow held down: +1, -1 or 0
        screen::timer_id m_repeat = 0;
    };

} // namespace volt::ui
