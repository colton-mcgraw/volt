#pragma once

#include <ui/signal.hpp>
#include <ui/widget.hpp>

#include <string>
#include <vector>

// -------------------------------------------------------------------------
// dropdown
// -------------------------------------------------------------------------
//
// One choice of several, shown closed as the current choice and opened, on
// a click, as a list below it (a menu, <ui/widgets/menu.hpp>, at least as
// wide as the dropdown, the current choice checked and highlighted).
// Choosing from the list emits `changed`; Escape or a click elsewhere
// closes it without a change. While closed and focused, Up and Down change
// the choice directly; Space, Enter, Alt+Down and F4 open the list.

namespace volt::ui
{

    class menu;

    class dropdown : public widget
    {
    public:
        explicit dropdown(std::vector<std::string> items = {}, int selected = 0);

        const std::vector<std::string> &items() const noexcept { return m_items; }
        void set_items(std::vector<std::string> items);

        int selected() const noexcept { return m_selected; }
        void set_selected(int index); // does not emit

        const std::string &selected_text() const;

        bool is_open() const noexcept { return m_list != nullptr; }
        void open();
        void close();

        // The open list, for tests.
        menu *list() const noexcept { return m_list; }

        signal<int> changed;

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_paint(painter &p) override;
        bool on_pointer_down(const pointer_event &e) override;
        void on_pointer_enter() override { invalidate(); }
        void on_pointer_leave() override { invalidate(); }
        bool on_key(const key_event &e) override;
        void on_detached() override { close(); }

    private:
        void choose(int index);

        std::vector<std::string> m_items;
        int m_selected;
        menu *m_list = nullptr;
    };

} // namespace volt::ui
