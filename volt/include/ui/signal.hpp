#pragma once

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

// -------------------------------------------------------------------------
// signal
// -------------------------------------------------------------------------
//
// How a widget tells the application that something happened to it: a
// button's `clicked`, a slider's `changed`. A signal is a list of callbacks,
// called in the order they were connected, synchronously, on the thread the
// interface lives on.
//
// Why not the event bus (<events/bus.hpp>)? The bus is keyed by event type
// and shared: a listener for "button clicked" would hear every button and
// have to filter by sender, and the bus pays for being callable from any
// thread. A widget's events belong to that widget, are raised and handled
// on one thread, and are few; a plain list on the widget says exactly that.
// What does go through the bus is input, before it reaches the widgets.
//
// Connecting or disconnecting from inside a callback is allowed: a callback
// connected during an emit is first called by the next one, one
// disconnected is not called again, even later in the same emit.

namespace volt::ui
{

    template <typename... Args>
    class signal
    {
    public:
        using slot = std::function<void(Args...)>;

        // Returns the id to disconnect() with.
        std::size_t connect(slot callback)
        {
            m_slots.push_back({++m_last_id, std::move(callback)});
            return m_last_id;
        }

        void disconnect(std::size_t id)
        {
            for (auto &entry : m_slots)
            {
                if (entry.id == id)
                    entry.callback = nullptr;
            }
            if (m_emitting == 0)
                compact();
        }

        void disconnect_all()
        {
            for (auto &entry : m_slots)
                entry.callback = nullptr;
            if (m_emitting == 0)
                m_slots.clear();
        }

        bool empty() const noexcept
        {
            for (const auto &entry : m_slots)
            {
                if (entry.callback)
                    return false;
            }
            return true;
        }

        void emit(Args... args)
        {
            ++m_emitting;
            const std::size_t count = m_slots.size();
            for (std::size_t i = 0; i < count; ++i)
            {
                // By index and a copy: a callback may connect (growing the
                // vector) or disconnect itself while it runs.
                if (m_slots[i].callback)
                {
                    const slot running = m_slots[i].callback;
                    running(args...);
                }
            }
            if (--m_emitting == 0)
                compact();
        }

        void operator()(Args... args) { emit(std::forward<Args>(args)...); }

    private:
        struct entry
        {
            std::size_t id = 0;
            slot callback;
        };

        void compact()
        {
            std::erase_if(m_slots, [](const entry &e) { return !e.callback; });
        }

        std::vector<entry> m_slots;
        std::size_t m_last_id = 0;
        int m_emitting = 0;
    };

} // namespace volt::ui
