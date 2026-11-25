#include <ui/widgets/numeric_field.hpp>

#include <ui/painter.hpp>
#include <ui/theme.hpp>

#include <math/electrical/e_series.hpp>
#include <math/electrical/engineering.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>

namespace volt::ui
{
    namespace
    {
        constexpr float arrow_width = 18.0f;
        constexpr auto repeat_delay = std::chrono::milliseconds(400);
        constexpr auto repeat_period = std::chrono::milliseconds(60);

        // A plain number, with the same tolerance for spaces as the
        // engineering parser.
        std::optional<double> parse_plain(std::string_view text)
        {
            while (!text.empty() && text.front() == ' ')
                text.remove_prefix(1);
            while (!text.empty() && text.back() == ' ')
                text.remove_suffix(1);
            if (!text.empty() && text.front() == '+')
                text.remove_prefix(1);
            double value = 0.0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || text.empty())
                return std::nullopt;
            return value;
        }
    } // namespace

    numeric_field::numeric_field(double value, double minimum, double maximum, double step)
        : m_value(std::clamp(value, minimum, maximum)), m_minimum(minimum), m_maximum(std::max(minimum, maximum)),
          m_step(step)
    {
        set_text(format(m_value));
        // Digits, signs, the decimal point, exponents, prefixes and units.
        set_filter([](char32_t c) { return c != U'\t'; });
    }

    std::string numeric_field::format(double value) const
    {
        if (m_engineering)
            return math::electrical::format_engineering(value, {.significant_digits = m_digits, .unit = m_unit});
        // No "-0.00" for a value that rounds to zero.
        const int decimals = std::max(0, m_decimals);
        if (std::abs(value) < 0.5 * std::pow(10.0, -decimals))
            value = 0.0;
        return std::format("{:.{}f}", value, decimals);
    }

    void numeric_field::set_value(double value)
    {
        value = std::clamp(value, m_minimum, m_maximum);
        m_value = value;
        set_text(format(value));
    }

    void numeric_field::set_range(double minimum, double maximum)
    {
        m_minimum = minimum;
        m_maximum = std::max(minimum, maximum);
        set_value(m_value);
    }

    void numeric_field::set_engineering(std::string unit, int digits)
    {
        m_engineering = true;
        m_unit = std::move(unit);
        m_digits = digits;
        set_text(format(m_value));
    }

    void numeric_field::set_decimals(int decimals)
    {
        m_engineering = false;
        m_decimals = decimals;
        set_text(format(m_value));
    }

    void numeric_field::commit()
    {
        std::optional<double> read;
        if (m_engineering)
        {
            read = math::electrical::parse_engineering(text(), m_unit);
            if (!read)
            {
                if (const auto any = math::electrical::parse_engineering(text()))
                    read = any->value;
            }
        }
        else
        {
            read = parse_plain(text());
        }

        const double before = m_value;
        if (read && std::isfinite(*read))
            m_value = std::clamp(*read, m_minimum, m_maximum);

        // Shown the canonical way, whether it was read or put back.
        const std::string shown = format(m_value);
        if (shown != text())
            set_text(shown);
        if (m_value != before)
            value_changed.emit(m_value);
    }

    void numeric_field::step_by(int steps)
    {
        if (steps == 0)
            return;
        commit();

        double next = m_value;
        if (m_engineering && m_step <= 0.0)
        {
            using math::electrical::e_series;
            for (int i = 0; i < std::abs(steps); ++i)
            {
                if (next <= 0.0)
                    next = steps > 0 ? 1.0 : next;
                else
                    next = steps > 0 ? math::electrical::preferred_ceil(next * 1.0001, e_series::e12)
                                     : math::electrical::preferred_floor(next * 0.9999, e_series::e12);
            }
        }
        else
        {
            const double step = m_step > 0.0 ? m_step : 1.0;
            next = std::round((m_value + step * steps) / step) * step;
        }

        next = std::clamp(next, m_minimum, m_maximum);
        if (next != m_value)
        {
            m_value = next;
            set_text(format(next));
            select_all();
            value_changed.emit(m_value);
        }
    }

    rect numeric_field::arrows() const
    {
        const rect &b = bounds();
        return {b.right() - arrow_width - 1.0f, b.y + 1.0f, arrow_width, b.height - 2.0f};
    }

    rect numeric_field::text_area() const
    {
        rect area = text_field::text_area();
        area.width = std::max(0.0f, area.width - arrow_width);
        return area;
    }

    void numeric_field::on_paint(painter &p)
    {
        text_field::on_paint(p);

        const ui::theme &t = theme();
        const rect a = arrows();
        const float half = std::floor(a.height * 0.5f);
        const rect up{a.x, a.y, a.width, half};
        const rect down{a.x, a.y + half, a.width, a.height - half};
        const bool enabled = is_enabled();

        p.fill({a.x, a.y, 1.0f, a.height}, t.border);
        if (m_held > 0)
            p.fill(up, t.control.pressed);
        if (m_held < 0)
            p.fill(down, t.control.pressed);
        const math::vec4f ink = enabled ? t.text_dim : t.text_disabled;
        p.draw_icon(icon::chevron_up, {up.x + 4.0f, up.y + (up.height - 10.0f) * 0.5f + 1.0f, 10.0f, 10.0f}, ink);
        p.draw_icon(icon::chevron_down, {down.x + 4.0f, down.y + (down.height - 10.0f) * 0.5f - 1.0f, 10.0f, 10.0f},
                    ink);
    }

    void numeric_field::stop_repeat()
    {
        if (m_repeat != 0 && owner() != nullptr)
            owner()->cancel(m_repeat);
        m_repeat = 0;
    }

    bool numeric_field::on_pointer_down(const pointer_event &e)
    {
        if (e.button == mouse_button::left && arrows().contains(e.position))
        {
            m_held = e.position.y() < arrows().center().y() ? 1 : -1;
            step_by(m_held);
            invalidate();

            // Held: after a moment, again and again.
            stop_repeat();
            const auto again = [this](auto &&self) -> void {
                m_repeat = owner()->schedule_after(repeat_period, [this, self] {
                    step_by(m_held);
                    self(self);
                });
            };
            m_repeat = owner()->schedule_after(repeat_delay, [again] { again(again); });
            return true;
        }
        return text_field::on_pointer_down(e);
    }

    bool numeric_field::on_pointer_up(const pointer_event &e)
    {
        if (m_held != 0)
        {
            m_held = 0;
            stop_repeat();
            invalidate();
            return true;
        }
        return text_field::on_pointer_up(e);
    }

    bool numeric_field::on_scroll(const scroll_input &e)
    {
        if (!has_focus() || e.offset.y() == 0.0f)
            return false;
        step_by(e.offset.y() > 0.0f ? 1 : -1);
        return true;
    }

    bool numeric_field::on_key(const key_event &e)
    {
        if (e.action != action::release && (e.mods & held_modifiers) == modifiers::none)
        {
            switch (e.key)
            {
            case key::up:
                step_by(1);
                return true;
            case key::down:
                step_by(-1);
                return true;
            case key::page_up:
                step_by(10);
                return true;
            case key::page_down:
                step_by(-10);
                return true;
            case key::enter:
            case key::kp_enter:
                commit();
                select_all();
                break; // and on to `submitted`
            case key::escape:
                set_text(format(m_value)); // back to what it was
                select_all();
                break;
            default:
                break;
            }
        }
        return text_field::on_key(e);
    }

    void numeric_field::on_focus_changed(bool focused, focus_reason reason)
    {
        if (!focused)
        {
            commit();
            stop_repeat();
            m_held = 0;
        }
        text_field::on_focus_changed(focused, reason);
    }

    cursor_shape numeric_field::cursor(const math::vec2f &point) const
    {
        return arrows().contains(point) ? cursor_shape::arrow : text_field::cursor(point);
    }

} // namespace volt::ui
