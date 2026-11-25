#include <ui/widgets/log_view.hpp>

#include <ui/screen.hpp>
#include <ui/theme.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace volt::ui
{
    namespace
    {
        // The source column never takes more than this, whatever the
        // entries say.
        constexpr float max_source_width = 160.0f;

        // The width of the mark before a warning or an error.
        constexpr float mark_width = 12.0f;
    } // namespace

    log_view::log_view(std::size_t capacity) : m_capacity(std::max<std::size_t>(capacity, 1))
    {
    }

    std::string log_view::line(const log_entry &entry)
    {
        std::string text = entry.time;
        if (!entry.source.empty())
            text += (text.empty() ? "" : "  ") + entry.source;
        if (entry.level == log_level::warning)
            text += (text.empty() ? "" : "  ") + std::string("Warning:");
        else if (entry.level == log_level::error)
            text += (text.empty() ? "" : "  ") + std::string("Error:");
        text += (text.empty() ? "" : "  ") + entry.text;
        if (entry.count > 1)
            text += std::format(" (\xC3\x97{})", entry.count);
        return text;
    }

    bool log_view::at_end() const
    {
        if (owner() == nullptr || m_entries.empty())
            return true;
        const float content = static_cast<float>(m_entries.size()) * row_height();
        const float view = std::max(0.0f, bounds().height - 2.0f);
        return scroll_offset() + view >= content - 0.5f;
    }

    void log_view::append(log_entry entry)
    {
        if (!m_entries.empty())
        {
            log_entry &last = m_entries.back();
            if (last.level == entry.level && last.source == entry.source && last.text == entry.text)
            {
                last.count += entry.count;
                last.time = std::move(entry.time);
                invalidate();
                return;
            }
        }

        m_follow = m_follow || at_end();
        m_entries.push_back(std::move(entry));
        if (m_entries.size() > m_capacity)
        {
            m_entries.pop_front();

            // What was shown moved up a row: keep showing it, and the
            // selection on the same entry.
            if (!m_follow && owner() != nullptr)
                scroll_to(scroll_offset() - row_height());
            if (selected() >= 0)
                set_selected(selected() - 1);
        }
        m_source_width = -1.0f;
        rows_changed();
    }

    void log_view::set_entries(std::vector<log_entry> entries)
    {
        m_entries.clear();
        const std::size_t skip = entries.size() > m_capacity ? entries.size() - m_capacity : 0;
        for (std::size_t i = skip; i < entries.size(); ++i)
            m_entries.push_back(std::move(entries[i]));
        set_selected(-1);
        m_follow = true;
        m_source_width = -1.0f;
        rows_changed();
    }

    void log_view::clear()
    {
        set_entries({});
    }

    void log_view::set_placeholder(std::string text)
    {
        m_placeholder = std::move(text);
        invalidate();
    }

    void log_view::follow_end()
    {
        scroll_to(static_cast<float>(m_entries.size()) * row_height());
    }

    void log_view::on_arrange(const rect &bounds)
    {
        row_view::on_arrange(bounds);
        if (m_follow)
        {
            follow_end();
            m_follow = false;
        }
    }

    void log_view::on_paint(painter &p)
    {
        row_view::on_paint(p);
        if (m_entries.empty() && !m_placeholder.empty())
        {
            const ui::theme &t = theme();
            p.text(row_rect(0).inset(insets::symmetric(8.0f, 0.0f)), m_placeholder, t.body_style(), t.text_dim);
        }
    }

    void log_view::paint_row(painter &p, std::size_t index, const rect &area)
    {
        const ui::theme &t = theme();
        const log_entry &entry = m_entries[index];
        const text_style small = t.small_style();
        const text_style body = t.body_style();

        if (m_source_width < 0.0f)
        {
            m_source_width = 0.0f;
            for (const log_entry &e : m_entries)
            {
                if (!e.source.empty())
                    m_source_width = std::max(m_source_width, std::ceil(fonts().measure(e.source, small)));
            }
            m_source_width = std::min(m_source_width, max_source_width);
        }

        rect rest = area.inset(insets::symmetric(8.0f, 0.0f));
        const bool enabled = is_enabled();

        // The mark: a dot for a warning or an error, nothing for the rest.
        if (entry.level != log_level::info)
        {
            const float dot = 6.0f;
            const math::vec4f color = !enabled                         ? t.text_disabled
                                      : entry.level == log_level::error ? t.error
                                                                        : t.warning;
            p.fill({rest.x + 1.0f, std::round(rest.y + (rest.height - dot) * 0.5f), dot, dot}, color, dot * 0.5f);
        }
        rest.x += mark_width;
        rest.width = std::max(0.0f, rest.width - mark_width);

        const auto column = [&](const std::string &text, float width) {
            if (width <= 0.0f)
                return;
            const float taken = std::min(width, rest.width);
            p.text({rest.x, rest.y, taken, rest.height}, text, small, enabled ? t.text_dim : t.text_disabled);
            rest.x += taken + t.spacing * 1.5f;
            rest.width = std::max(0.0f, rest.width - taken - t.spacing * 1.5f);
        };
        if (!entry.time.empty())
            column(entry.time, std::ceil(fonts().measure(entry.time, small)));
        column(entry.source, m_source_width);

        // The count, at the right, before the text is cut to fit.
        if (entry.count > 1)
        {
            const std::string count = std::format("\xC3\x97{}", entry.count);
            const float width = std::ceil(fonts().measure(count, small));
            p.text({rest.right() - width, rest.y, width, rest.height}, count, small,
                   enabled ? t.text_dim : t.text_disabled);
            rest.width = std::max(0.0f, rest.width - width - t.spacing);
        }

        const math::vec4f ink = !enabled                           ? t.text_disabled
                                : entry.level == log_level::error   ? t.error
                                : entry.level == log_level::warning ? t.warning
                                                                    : t.text;
        p.text(rest, entry.text, body, ink);
    }

    bool log_view::row_key(const key_event &e)
    {
        const modifiers held = e.mods & held_modifiers;
        if (e.action == action::press && held == modifiers::control && e.key == key::c && selected() >= 0 &&
            owner() != nullptr)
        {
            owner()->clipboard().set_text(line(m_entries[static_cast<std::size_t>(selected())]));
            return true;
        }
        return false;
    }

} // namespace volt::ui
