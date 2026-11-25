#pragma once

#include <ui/signal.hpp>
#include <ui/widget.hpp>

#include <memory>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// tab_view
// -------------------------------------------------------------------------
//
// Pages one at a time under a row of tabs naming them. Clicking a tab shows
// its page; Ctrl+Page Up and Ctrl+Page Down (or Ctrl+Tab) move between
// tabs from anywhere inside the view. Only the shown page is laid out,
// painted and hit; the others are kept, hidden, with their state.

namespace volt::ui
{

    class tab_view : public widget
    {
    public:
        tab_view();

        // Adds a page under a tab and returns it.
        template <typename W>
        W &add_tab(std::string title, std::unique_ptr<W> page)
        {
            W &result = *page;
            add_page(std::move(title), std::move(page));
            return result;
        }

        std::size_t tab_count() const noexcept { return m_titles.size(); }
        const std::string &title(std::size_t index) const { return m_titles[index]; }

        int selected() const noexcept { return m_selected; }
        void select(int index); // does not emit

        widget *page(std::size_t index) const { return &child(index); }

        // The rectangle of a tab, as laid out.
        rect tab_rect(std::size_t index) const;

        signal<int> changed;

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_arrange(const rect &bounds) override;
        void on_paint(painter &p) override;
        bool on_pointer_down(const pointer_event &e) override;
        void on_pointer_move(const pointer_event &e) override;
        void on_pointer_leave() override;
        bool on_key(const key_event &e) override;

    private:
        void add_page(std::string title, std::unique_ptr<widget> page);
        int tab_at(const math::vec2f &point) const;
        void choose(int index);

        std::vector<std::string> m_titles;
        std::vector<float> m_widths;
        int m_selected = -1;
        int m_hot = -1;
    };

} // namespace volt::ui
