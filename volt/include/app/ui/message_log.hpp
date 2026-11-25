#pragma once

#include <app/ui/window_book.hpp>
#include <ui/signal.hpp>
#include <ui/widgets/log_view.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <string>
#include <utility>

// -------------------------------------------------------------------------
// message_log
// -------------------------------------------------------------------------
//
// Every status message any window has shown, kept: a status bar's message
// is gone after a few seconds, and a file that failed to load in a window
// since closed leaves no trace. The project manager shows this log.
//
// window_host::show_message() adds to it (so app_services::show_message()
// does too), at the level it is given: a message that reports a failure
// should say it is an error there, so that the log can tell it apart. A
// message the same as the last one from the same window counts on it
// (repeats) instead of adding an entry. The oldest go beyond `capacity`.
//
// Lives in app_services (messages()). Interface thread.

namespace volt::ui
{

    struct logged_message
    {
        std::chrono::system_clock::time_point when;
        window_kind from = window_kind::project_manager;
        log_level level = log_level::info;
        std::string text;
        std::size_t repeats = 1;
    };

    class message_log
    {
    public:
        static constexpr std::size_t capacity = 1000;

        void add(window_kind from, std::string text, log_level level = log_level::info)
        {
            if (text.empty())
                return;
            const auto now = std::chrono::system_clock::now();
            if (!m_messages.empty())
            {
                logged_message &last = m_messages.back();
                if (last.from == from && last.text == text)
                {
                    // Shown again: counted, at the more serious level of the
                    // two.
                    ++last.repeats;
                    last.when = now;
                    last.level = std::max(last.level, level);
                    added.emit(last, true);
                    return;
                }
            }
            m_messages.push_back({now, from, level, std::move(text), 1});
            if (m_messages.size() > capacity)
                m_messages.pop_front();
            added.emit(m_messages.back(), false);
        }

        const std::deque<logged_message> &messages() const noexcept { return m_messages; }

        void clear()
        {
            m_messages.clear();
            cleared.emit();
        }

        // A message was added; true when it repeated the last one, which
        // is then the one given, counted.
        signal<const logged_message &, bool> added;
        signal<> cleared;

    private:
        std::deque<logged_message> m_messages;
    };

} // namespace volt::ui
