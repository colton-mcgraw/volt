#pragma once

#include <ui/painter.hpp>
#include <ui/widgets/row_view.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// log_view
// -------------------------------------------------------------------------
//
// A running record of what happened, newest at the bottom: one row per
// entry, with the time, where it came from, and the text, coloured by how
// serious it is. A row view (<ui/widgets/row_view.hpp>), so it paints only
// the screenful it shows however long it gets.
//
// It follows the end: while the last row is in view, appending scrolls the
// new one into view; scrolled back to read something, it stays put. An
// entry the same as the last one (same source, level and text) is not
// added again: the last row counts it instead ("x3").
//
// At most capacity() entries are kept; the oldest go first. Ctrl+C copies
// the selected entry as a line of text. With no entries it shows its
// placeholder.

namespace volt::ui
{

    enum class log_level : std::uint8_t
    {
        info,
        warning,
        error,
    };

    struct log_entry
    {
        std::string time;   // as shown: "14:02:37"
        std::string source; // where it came from: "Board Editor"; may be empty
        std::string text;
        log_level level = log_level::info;

        // How many times in a row it happened.
        std::size_t count = 1;
    };

    class log_view : public row_view
    {
    public:
        explicit log_view(std::size_t capacity = 1000);

        // Adds an entry at the end, or counts it on the last one if it is
        // the same; see above.
        void append(log_entry entry);

        // Replaces every entry, showing the end.
        void set_entries(std::vector<log_entry> entries);
        void clear();

        const std::deque<log_entry> &entries() const noexcept { return m_entries; }
        std::size_t capacity() const noexcept { return m_capacity; }

        // Shown while there are no entries.
        void set_placeholder(std::string text);

        // An entry as one line of text, as Ctrl+C copies it.
        static std::string line(const log_entry &entry);

        std::size_t row_count() const override { return m_entries.size(); }

    protected:
        void paint_row(painter &p, std::size_t index, const rect &area) override;
        bool row_key(const key_event &e) override;
        void on_arrange(const rect &bounds) override;
        void on_paint(painter &p) override;

    private:
        // Whether the last row is in view (or there is nothing to scroll).
        bool at_end() const;
        void follow_end();

        std::deque<log_entry> m_entries;
        std::size_t m_capacity;
        std::string m_placeholder;

        // Scroll to the end at the next arrange: rows were added while the
        // end was in view.
        bool m_follow = true;

        // The source column's width, measured from the entries; negative
        // when they changed since.
        float m_source_width = -1.0f;
    };

} // namespace volt::ui
