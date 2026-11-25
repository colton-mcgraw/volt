#include <ui/widgets/tool_strip.hpp>

#include <ui/theme.hpp>
#include <ui/widgets/panel.hpp>

#include <algorithm>

namespace volt::ui
{

    tool_strip::tool_strip(ui::orientation orientation)
        : box(orientation, 2.0f,
              orientation == orientation::vertical ? insets::symmetric(4.0f, 6.0f) : insets::symmetric(6.0f, 3.0f))
    {
        set_cross_align(orientation == orientation::vertical ? align::stretch : align::center);
    }

    int tool_strip::add_tool(ui::icon icon, std::string tooltip, std::string text)
    {
        button &tool = emplace<button>(std::move(text), icon, button_kind::flat);
        tool.set_tooltip(std::move(tooltip));
        tool.set_toggle(true);
        tool.set_focusable(false); // tools do not take the focus from the document

        const int index = static_cast<int>(m_tools.size());
        m_tools.push_back(&tool);

        // A toggle button flips itself before it says it was clicked; the
        // strip then puts every tool's state right, the active one down
        // even when it was clicked again.
        tool.clicked.connect([this, index] { choose(index); });
        if (m_selected < 0)
            m_selected = index;
        sync_checks();
        return index;
    }

    void tool_strip::add_separator()
    {
        const ui::orientation across =
            orientation() == orientation::vertical ? orientation::horizontal : orientation::vertical;
        separator &line = emplace<separator>(across, 4.0f);
        line.layout().cross = align::stretch;
    }

    void tool_strip::select(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_tools.size()))
            return;
        m_selected = index;
        sync_checks();
    }

    void tool_strip::choose(int index)
    {
        const int before = m_selected;
        select(index);
        if (m_selected != before)
            changed.emit(m_selected);
    }

    void tool_strip::sync_checks()
    {
        for (std::size_t i = 0; i < m_tools.size(); ++i)
            m_tools[i]->set_checked(static_cast<int>(i) == m_selected);
    }

    math::vec2f tool_strip::on_measure(const constraints &limits)
    {
        // A vertical strip is as wide as a toolbar is high, so that the two
        // meet in a square corner and the icons line up across both.
        math::vec2f size = box::on_measure(limits);
        if (orientation() == orientation::vertical)
            size[0] = std::max(size.x(), theme().toolbar_height);
        return size;
    }

    void tool_strip::on_paint(painter &p)
    {
        const ui::theme &t = theme();
        const rect &b = bounds();
        p.fill(b, t.panel);
        if (orientation() == orientation::vertical)
            p.fill({b.right() - 1.0f, b.y, 1.0f, b.height}, t.separator);
        else
            p.fill({b.x, b.bottom() - 1.0f, b.width, 1.0f}, t.separator);
        paint_children(p);
    }

} // namespace volt::ui
