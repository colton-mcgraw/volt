#pragma once

#include <ui/signal.hpp>
#include <ui/widget.hpp>

#include <string>
#include <vector>

// -------------------------------------------------------------------------
// radio_group
// -------------------------------------------------------------------------
//
// One choice of several, shown as a column of round buttons. The group is
// one stop in the Tab order, as is conventional; Up and Down (or Left and
// Right) move the choice while it has the focus.

namespace volt::ui
{

    class radio_group : public widget
    {
    public:
        explicit radio_group(std::vector<std::string> options = {}, int selected = 0);

        const std::vector<std::string> &options() const noexcept { return m_options; }
        void set_options(std::vector<std::string> options);

        int selected() const noexcept { return m_selected; }
        void set_selected(int index); // does not emit

        signal<int> changed;

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_paint(painter &p) override;
        bool on_pointer_down(const pointer_event &e) override;
        void on_pointer_move(const pointer_event &e) override;
        void on_pointer_leave() override;
        bool on_key(const key_event &e) override;

    private:
        float row_height() const;
        int option_at(const math::vec2f &point) const;
        void choose(int index);

        std::vector<std::string> m_options;
        int m_selected;
        int m_hot = -1;
    };

} // namespace volt::ui
