#include <ui/widgets/text_field.hpp>

#include <ui/painter.hpp>
#include <ui/theme.hpp>

#include <font/utf8.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace volt::ui
{
    namespace
    {
        enum class char_class
        {
            space,
            word,
            other,
        };

        char_class class_of(char32_t c) noexcept
        {
            if (c == U' ' || c == U'\t' || c == 0x00A0)
                return char_class::space;
            if ((c >= U'0' && c <= U'9') || (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || c == U'_' ||
                c >= 0x80)
                return char_class::word;
            return char_class::other;
        }

        char32_t code_point_before(std::string_view text, std::size_t offset) noexcept
        {
            return font::decode_utf8(text, font::previous_boundary(text, offset)).code_point;
        }
    } // namespace

    text_field::text_field(std::string text, std::string placeholder)
        : m_text(std::move(text)), m_placeholder(std::move(placeholder))
    {
        set_focusable(true);
        m_caret = m_anchor = m_text.size();
    }

    text_field::~text_field()
    {
        stop_blink();
    }

    void text_field::set_text(std::string text)
    {
        if (text == m_text)
            return;
        m_text = std::move(text);
        m_run_valid = false;
        m_caret = m_anchor = m_text.size();
        keep_caret_visible();
        invalidate();
    }

    void text_field::set_placeholder(std::string placeholder)
    {
        m_placeholder = std::move(placeholder);
        invalidate();
    }

    std::pair<std::size_t, std::size_t> text_field::selection() const noexcept
    {
        return {std::min(m_caret, m_anchor), std::max(m_caret, m_anchor)};
    }

    std::string text_field::selected_text() const
    {
        const auto [first, last] = selection();
        return m_text.substr(first, last - first);
    }

    void text_field::select(std::size_t anchor, std::size_t caret)
    {
        // Onto boundaries: an offset inside a code point counts as its start.
        const auto snap = [this](std::size_t offset) {
            offset = std::min(offset, m_text.size());
            while (offset > 0 && offset < m_text.size() &&
                   (static_cast<unsigned char>(m_text[offset]) & 0xC0u) == 0x80u)
                --offset;
            return offset;
        };
        m_anchor = snap(anchor);
        m_caret = snap(caret);
        keep_caret_visible();
        restart_blink();
        invalidate();
    }

    void text_field::select_all()
    {
        select(0, m_text.size());
    }

    // ---- geometry -------------------------------------------------------------

    rect text_field::text_area() const
    {
        const float padding = theme().control_padding * 0.7f;
        return bounds().inset(insets::symmetric(padding, 0.0f));
    }

    const font::shaped_run &text_field::run()
    {
        if (!m_run_valid)
        {
            m_run = fonts().shape(m_text, theme().body_style());
            m_run_valid = true;
        }
        return m_run;
    }

    float text_field::caret_position(std::size_t offset)
    {
        return font::caret_x(run(), offset);
    }

    std::size_t text_field::offset_at(float x)
    {
        return font::hit_test(run(), x - text_area().x + m_scroll);
    }

    void text_field::keep_caret_visible()
    {
        if (owner() == nullptr)
            return;
        const float width = text_area().width;
        const float total = run().width;
        const float x = caret_position(m_caret);

        // A little room past the caret, so it is not glued to the edge.
        const float margin = std::min(width * 0.25f, 24.0f);
        if (x - m_scroll > width - 1.0f)
            m_scroll = x - width + 1.0f + margin;
        else if (x - m_scroll < 0.0f)
            m_scroll = x - margin;
        m_scroll = std::clamp(m_scroll, 0.0f, std::max(0.0f, total - width + 1.0f));
        m_scroll = std::round(m_scroll);
    }

    // ---- words ----------------------------------------------------------------------

    std::size_t text_field::word_left(std::size_t from) const
    {
        std::size_t at = from;
        while (at > 0 && class_of(code_point_before(m_text, at)) == char_class::space)
            at = font::previous_boundary(m_text, at);
        if (at == 0)
            return 0;
        const char_class kind = class_of(code_point_before(m_text, at));
        while (at > 0 && class_of(code_point_before(m_text, at)) == kind)
            at = font::previous_boundary(m_text, at);
        return at;
    }

    std::size_t text_field::word_right(std::size_t from) const
    {
        std::size_t at = from;
        if (at < m_text.size())
        {
            const char_class kind = class_of(font::decode_utf8(m_text, at).code_point);
            if (kind != char_class::space)
            {
                while (at < m_text.size() && class_of(font::decode_utf8(m_text, at).code_point) == kind)
                    at = font::next_boundary(m_text, at);
            }
        }
        while (at < m_text.size() && class_of(font::decode_utf8(m_text, at).code_point) == char_class::space)
            at = font::next_boundary(m_text, at);
        return at;
    }

    std::pair<std::size_t, std::size_t> text_field::word_at(std::size_t offset) const
    {
        if (m_text.empty())
            return {0, 0};
        std::size_t at = std::min(offset, m_text.size());
        // Past the end, or after the word: take the word before.
        if (at == m_text.size())
            at = font::previous_boundary(m_text, at);
        const char_class kind = class_of(font::decode_utf8(m_text, at).code_point);

        std::size_t first = at;
        while (first > 0 && class_of(code_point_before(m_text, first)) == kind)
            first = font::previous_boundary(m_text, first);
        std::size_t last = at;
        while (last < m_text.size() && class_of(font::decode_utf8(m_text, last).code_point) == kind)
            last = font::next_boundary(m_text, last);
        return {first, last};
    }

    // ---- editing ------------------------------------------------------------------------

    void text_field::replace(std::size_t first, std::size_t last, std::string_view text)
    {
        m_text.replace(first, last - first, text);
        m_caret = m_anchor = first + text.size();
        m_run_valid = false;
        keep_caret_visible();
        restart_blink();
        invalidate();
        on_edited();
        changed.emit(m_text);
    }

    void text_field::insert(std::string_view text)
    {
        if (m_read_only)
            return;

        // Typed or pasted, the text is one line of acceptable characters.
        std::string accepted;
        for (std::size_t at = 0; at < text.size();)
        {
            const font::decoded d = font::decode_utf8(text, at);
            at += std::max<std::uint32_t>(d.length, 1);
            char32_t c = d.code_point;
            if (c == U'\n' || c == U'\r' || c == U'\t')
                c = U' ';
            if (c < 0x20 || c == 0x7F)
                continue;
            if (m_filter && !m_filter(c))
                continue;
            font::append_utf8(accepted, c);
        }

        const auto [first, last] = selection();
        if (accepted.empty() && first == last)
            return;
        replace(first, last, accepted);
    }

    void text_field::erase_backward(bool word)
    {
        if (m_read_only)
            return;
        if (has_selection())
        {
            const auto [first, last] = selection();
            replace(first, last, {});
            return;
        }
        if (m_caret == 0)
            return;
        const std::size_t from = word ? word_left(m_caret) : font::previous_boundary(m_text, m_caret);
        replace(from, m_caret, {});
    }

    void text_field::erase_forward(bool word)
    {
        if (m_read_only)
            return;
        if (has_selection())
        {
            const auto [first, last] = selection();
            replace(first, last, {});
            return;
        }
        if (m_caret >= m_text.size())
            return;
        const std::size_t to = word ? word_right(m_caret) : font::next_boundary(m_text, m_caret);
        replace(m_caret, to, {});
    }

    void text_field::copy() const
    {
        if (has_selection() && owner() != nullptr)
            owner()->clipboard().set_text(selected_text());
    }

    void text_field::cut()
    {
        if (!has_selection() || m_read_only)
            return;
        copy();
        const auto [first, last] = selection();
        replace(first, last, {});
    }

    void text_field::paste()
    {
        if (owner() != nullptr)
            insert(owner()->clipboard().text());
    }

    void text_field::move_caret(std::size_t to, bool extend)
    {
        m_caret = to;
        if (!extend)
            m_anchor = to;
        keep_caret_visible();
        restart_blink();
        invalidate();
    }

    // ---- caret blink ----------------------------------------------------------------------

    void text_field::restart_blink()
    {
        stop_blink();
        m_caret_on = true;
        if (!has_focus() || owner() == nullptr)
            return;

        const auto period =
            std::chrono::duration_cast<clock::duration>(std::chrono::duration<float>(theme().caret_blink));
        // Each tick schedules the next: one frame per blink.
        const auto tick = [this, period](auto &&self) -> void {
            m_blink = owner()->schedule_after(period, [this, self] {
                m_caret_on = !m_caret_on;
                invalidate();
                self(self);
            });
        };
        tick(tick);
    }

    void text_field::stop_blink()
    {
        if (m_blink != 0 && owner() != nullptr)
            owner()->cancel(m_blink);
        m_blink = 0;
    }

    void text_field::on_detached()
    {
        stop_blink();
    }

    void text_field::on_focus_changed(bool focused, focus_reason reason)
    {
        if (focused)
        {
            // Tabbing into a field selects what is there, ready to replace.
            if (reason == focus_reason::keyboard)
                select_all();
            restart_blink();
        }
        else
        {
            stop_blink();
            m_selecting = false;
        }
        invalidate();
    }

    // ---- layout and painting ----------------------------------------------------------------

    math::vec2f text_field::on_measure(const constraints &)
    {
        return {160.0f, theme().control_height};
    }

    void text_field::on_arrange(const rect &)
    {
        m_run_valid = false;
        keep_caret_visible();
    }

    void text_field::paint_field(painter &p)
    {
        const ui::theme &t = theme();
        const bool enabled = is_enabled();
        const math::vec4f border = !enabled     ? t.control_border
                                   : has_focus() ? t.accent
                                   : hovered()   ? mix(t.control_border, t.text_dim, 0.5f)
                                                 : t.control_border;
        p.frame(bounds(), enabled ? t.field : t.control.disabled, border, t.radius);
        if (has_focus() && owner()->focus_visible())
            p.focus_ring(bounds(), t.radius);
    }

    void text_field::on_paint(painter &p)
    {
        const ui::theme &t = theme();
        const text_style style = t.body_style();
        paint_field(p);

        const rect area = text_area();
        const float baseline = p.baseline(bounds().y, bounds().height, style);
        const text_metrics metrics = fonts().metrics(style);

        clip_scope clip(p, area.outset(1.0f));

        if (m_text.empty())
        {
            if (!m_placeholder.empty())
                p.text(area, m_placeholder, style, t.text_disabled);
        }
        else
        {
            if (has_selection())
            {
                const auto [first, last] = selection();
                const float left = area.x - m_scroll + caret_position(first);
                const float right = area.x - m_scroll + caret_position(last);
                const float top = baseline - metrics.ascent - 1.0f;
                p.fill({left, top, right - left, metrics.height() + 2.0f},
                       has_focus() ? t.selection : t.selection_inactive, 2.0f);
            }
            p.run(run(), style, {area.x - m_scroll, baseline}, is_enabled() ? t.text : t.text_disabled);
        }

        if (has_focus() && m_caret_on && !m_read_only)
        {
            const float x = std::round((area.x - m_scroll + caret_position(m_caret)) * p.scale()) / p.scale();
            const float top = baseline - metrics.ascent - 1.0f;
            p.fill({x, top, 1.0f, metrics.height() + 2.0f}, t.text);
        }
    }

    cursor_shape text_field::cursor(const math::vec2f &point) const
    {
        return text_area().outset(4.0f).contains(point) ? cursor_shape::ibeam : cursor_shape::arrow;
    }

    // ---- input ------------------------------------------------------------------------------

    bool text_field::on_pointer_down(const pointer_event &e)
    {
        if (e.button != mouse_button::left)
            return false;

        const std::size_t at = offset_at(e.position.x());
        if (e.clicks >= 3)
        {
            select_all();
            m_selecting = false;
            return true;
        }
        if (e.clicks == 2)
        {
            m_word_drag = word_at(at);
            select(m_word_drag.first, m_word_drag.second);
            m_selecting = true;
            return true;
        }

        m_word_drag = {at, at};
        move_caret(at, has(e.mods, modifiers::shift));
        m_selecting = true;
        return true;
    }

    void text_field::on_pointer_move(const pointer_event &e)
    {
        if (!m_selecting)
            return;
        const std::size_t at = offset_at(e.position.x());

        // Dragging after a double click selects whole words.
        if (m_word_drag.first != m_word_drag.second)
        {
            const auto word = word_at(at);
            if (at < m_word_drag.first)
                select(m_word_drag.second, word.first);
            else
                select(m_word_drag.first, std::max(word.second, m_word_drag.second));
            return;
        }
        if (at != m_caret)
            move_caret(at, true);
    }

    bool text_field::on_pointer_up(const pointer_event &e)
    {
        if (e.button != mouse_button::left)
            return false;
        m_selecting = false;
        return true;
    }

    bool text_field::on_key(const key_event &e)
    {
        if (e.action == action::release)
            return false;

        const modifiers held = e.mods & held_modifiers;
        const bool shift = has(held, modifiers::shift);
        const bool control = has(held, modifiers::control);
        const bool alt = has(held, modifiers::alt);
        if (alt)
            return false;

        switch (e.key)
        {
        case key::left:
            if (has_selection() && !shift)
                move_caret(selection().first, false);
            else
                move_caret(control ? word_left(m_caret) : font::previous_boundary(m_text, m_caret), shift);
            return true;
        case key::right:
            if (has_selection() && !shift)
                move_caret(selection().second, false);
            else
                move_caret(control ? word_right(m_caret) : font::next_boundary(m_text, m_caret), shift);
            return true;
        case key::home:
            move_caret(0, shift);
            return true;
        case key::end:
            move_caret(m_text.size(), shift);
            return true;
        case key::backspace:
            erase_backward(control);
            return true;
        case key::del:
            if (shift && !control)
                cut();
            else
                erase_forward(control);
            return true;
        case key::insert:
            if (control && !shift)
                copy();
            else if (shift && !control)
                paste();
            return control || shift;
        case key::enter:
        case key::kp_enter:
            submitted.emit(m_text);
            return true;
        case key::escape:
            cancelled.emit();
            return false;
        case key::a:
            if (control && !shift)
            {
                select_all();
                return true;
            }
            break;
        case key::c:
            if (control && !shift)
            {
                copy();
                return true;
            }
            break;
        case key::x:
            if (control && !shift)
            {
                cut();
                return true;
            }
            break;
        case key::v:
            if (control && !shift)
            {
                paste();
                return true;
            }
            break;
        default:
            break;
        }

        // Plain keys are text and arrive as text events; claim them here so
        // that no single-key shortcut fires while typing.
        return !control && e.key != key::tab && e.key != key::escape &&
               !(e.key >= key::f1 && e.key <= key::f12);
    }

    bool text_field::on_text(const text_event &e)
    {
        if (has(e.mods, modifiers::control) && !has(e.mods, modifiers::alt))
            return false;
        std::string typed;
        font::append_utf8(typed, e.codepoint);
        insert(typed);
        return true;
    }

} // namespace volt::ui
