#include <gpu/retire.hpp>

#include <utility>

namespace volt::gpu
{

    void retire_queue::retire(std::uint64_t last_use, std::move_only_function<void()> release)
    {
        m_entries.push_back({last_use, std::move(release)});
    }

    void retire_queue::collect(const frame &frame)
    {
        // The presenter waited for frame `number - frames_in_flight` before
        // handing this one out, and frames finish in order.
        const std::uint64_t through =
            frame.number > presenter::frames_in_flight ? frame.number - presenter::frames_in_flight : 0;

        // Run them in the order they came: one function may free what an
        // earlier one's object still pointed at.
        std::vector<entry> waiting;
        for (entry &e : m_entries)
        {
            if (e.last_use <= through)
                e.release();
            else
                waiting.push_back(std::move(e));
        }
        m_entries = std::move(waiting);
    }

    void retire_queue::flush()
    {
        // A release that retires something else in turn lands in the list
        // being walked; take the list first.
        while (!m_entries.empty())
        {
            std::vector<entry> entries = std::move(m_entries);
            m_entries.clear();
            for (entry &e : entries)
                e.release();
        }
    }

} // namespace volt::gpu
