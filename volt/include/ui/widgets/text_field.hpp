#pragma once

#include <ui/screen.hpp>
#include <ui/signal.hpp>
#include <ui/widget.hpp>

#include <font/text.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

// -------------------------------------------------------------------------
// text_field
// -------------------------------------------------------------------------
//
// One line of editable text.
//
// Positions in the text are byte offsets into its UTF-8, always on code
// point boundaries (<font/utf8.hpp>). The selection runs between the anchor,
// where it started, and the caret, where it ends; they are equal when
// nothing is selected.
//
// Keys: Left/Right by a character, with Ctrl by a word; Home/End; Shift with
// any of them extends the selection; Backspace/Delete (Ctrl: a word); Ctrl+A
// selects all; Ctrl+C, Ctrl+X, Ctrl+V (and Ctrl+Insert, Shift+Delete,
// Shift+Insert) copy, cut and paste through the screen's clipboard; Enter
// emits `submitted`; Escape emits `cancelled` and is not consumed, so a
// dialog still closes. Mouse: a press puts the caret there (Shift: extends),
// dragging selects, a double click selects a word, a triple click all of it.
//
// When the text is wider than the field it scrolls sideways to keep the
// caret in view. The caret blinks while the field has the focus, on a timer
// that asks for one frame per blink, not a frame per display refresh.

namespace volt::ui
{

    class text_field : public widget
    {
    public:
        explicit text_field(std::string text = {}, std::string placeholder = {});
        ~text_field() override;

        const std::string &text() const noexcept { return m_text; }

        // Replaces the text and puts the caret at its end; does not emit.
        void set_text(std::string text);

        const std::string &placeholder() const noexcept { return m_placeholder; }
        void set_placeholder(std::string placeholder);

        // Which characters typing may insert; all printable ones by default.
        void set_filter(std::function<bool(char32_t)> accept) { m_filter = std::move(accept); }

        void set_read_only(bool read_only) noexcept { m_read_only = read_only; }
        bool read_only() const noexcept { return m_read_only; }

        // ---- caret and selection ----------------------------------------------

        std::size_t caret() const noexcept { return m_caret; }
        std::size_t anchor() const noexcept { return m_anchor; }
        bool has_selection() const noexcept { return m_caret != m_anchor; }

        // [first, second), ordered.
        std::pair<std::size_t, std::size_t> selection() const noexcept;
        std::string selected_text() const;

        // Moves the caret to `caret`, the anchor to `anchor`.
        void select(std::size_t anchor, std::size_t caret);
        void select_all();

        // ---- editing, as the keys do it ------------------------------------------

        // Replaces the selection with `text` (filtered); emits `changed`.
        void insert(std::string_view text);
        void erase_backward(bool word);
        void erase_forward(bool word);
        void copy() const;
        void cut();
        void paste();

        // How far the text is scrolled to the left, logical units.
        float scroll() const noexcept { return m_scroll; }

        signal<const std::string &> changed;
        signal<const std::string &> submitted;
        signal<> cancelled;

        cursor_shape cursor(const math::vec2f &point) const override;

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_arrange(const rect &bounds) override;
        void on_paint(painter &p) override;

        bool on_pointer_down(const pointer_event &e) override;
        void on_pointer_move(const pointer_event &e) override;
        bool on_pointer_up(const pointer_event &e) override;
        void on_pointer_enter() override { invalidate(); }
        void on_pointer_leave() override { invalidate(); }
        bool on_key(const key_event &e) override;
        bool on_text(const text_event &e) override;
        void on_focus_changed(bool focused, focus_reason reason) override;
        void on_detached() override;

        // Where the text runs: the field less its padding, and less whatever
        // a subclass draws at the sides (a spin box's arrows).
        virtual rect text_area() const;

        // Called after every change of the text by the user, before
        // `changed` is emitted.
        virtual void on_edited() {}

        // Paints the frame; subclasses add to it.
        void paint_field(painter &p);

    private:
        const font::shaped_run &run();
        std::size_t offset_at(float x);
        float caret_position(std::size_t offset);
        void keep_caret_visible();
        void move_caret(std::size_t to, bool extend);
        void restart_blink();
        void stop_blink();
        void replace(std::size_t first, std::size_t last, std::string_view text);

        std::size_t word_left(std::size_t from) const;
        std::size_t word_right(std::size_t from) const;
        std::pair<std::size_t, std::size_t> word_at(std::size_t offset) const;

        std::string m_text;
        std::string m_placeholder;
        std::function<bool(char32_t)> m_filter;
        bool m_read_only = false;

        std::size_t m_caret = 0;
        std::size_t m_anchor = 0;
        float m_scroll = 0.0f;
        bool m_selecting = false;
        std::pair<std::size_t, std::size_t> m_word_drag{0, 0}; // a double click drags by words

        font::shaped_run m_run;
        bool m_run_valid = false;

        bool m_caret_on = true;
        screen::timer_id m_blink = 0;
    };

} // namespace volt::ui
