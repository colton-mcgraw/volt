#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// -------------------------------------------------------------------------
// window kinds, and the book the interface keeps of them
// -------------------------------------------------------------------------
//
// The application's top-level windows are of four kinds, at most one of
// each open at a time (KiCad's model: separate windows for separate parts
// of the program, so that two monitors can show the schematic and the board
// side by side):
//
//   project_manager    the main window; open from start to exit, and
//                      closing it exits the application
//   schematic_editor   opened from the project manager or any Window menu
//   board_editor
//   library_browser
//
// window_book is the interface thread's record of which kinds are open, and
// what a request to open or close one means given that: plain bookkeeping,
// with no windows in it, so that the rules are tested on their own
// (volt/tests/app). The window manager (<app/ui/window_manager.hpp>) does
// what the book says.
//
// Every window the book hands out gets a serial number. The platform
// thread's messages name a window by kind and serial, so that a message
// about a window that has since been closed (and perhaps opened again: a
// new serial) is recognized as stale and dropped.
//
// A slot goes
//
//     closed --request_open--> opening --created--> open
//        ^                        |                   |
//        +------ failed ----------+                   |
//        +------ request_close -----------------------+
//        +------ request_close (while opening: the window is retired as
//                soon as created() reports it)
//
// and once exiting, nothing opens any more.

namespace volt::ui
{

    enum class window_kind : std::uint8_t
    {
        project_manager,
        schematic_editor,
        board_editor,
        library_browser,
    };

    inline constexpr std::size_t window_kind_count = 4;

    inline constexpr std::array<window_kind, window_kind_count> all_window_kinds{
        window_kind::project_manager, window_kind::schematic_editor, window_kind::board_editor,
        window_kind::library_browser};

    constexpr std::size_t index_of(window_kind kind) noexcept
    {
        return static_cast<std::size_t>(kind);
    }

    // "Schematic Editor": what menus and messages call it.
    constexpr std::string_view window_name(window_kind kind) noexcept
    {
        switch (kind)
        {
        case window_kind::project_manager:
            return "Project Manager";
        case window_kind::schematic_editor:
            return "Schematic Editor";
        case window_kind::board_editor:
            return "Board Editor";
        case window_kind::library_browser:
            return "Library Browser";
        }
        return "Window";
    }

    // "schematic": its name on the command line (--windows) and in its
    // command id ("window.schematic").
    constexpr std::string_view window_key(window_kind kind) noexcept
    {
        switch (kind)
        {
        case window_kind::project_manager:
            return "project";
        case window_kind::schematic_editor:
            return "schematic";
        case window_kind::board_editor:
            return "board";
        case window_kind::library_browser:
            return "library";
        }
        return "";
    }

    constexpr std::optional<window_kind> window_kind_from_key(std::string_view key) noexcept
    {
        for (window_kind kind : all_window_kinds)
        {
            if (window_key(kind) == key)
                return kind;
        }
        return std::nullopt;
    }

    class window_book
    {
    public:
        enum class state : std::uint8_t
        {
            closed,
            opening, // asked of the platform thread, not reported yet
            open,
        };

        enum class open_action : std::uint8_t
        {
            none,   // already on its way, or the application is exiting
            create, // ask the platform thread for a window with `serial`
            focus,  // it is open: raise it
        };

        struct open_result
        {
            open_action action = open_action::none;
            std::uint64_t serial = 0;
        };

        enum class close_action : std::uint8_t
        {
            none,      // not open, or the request is about an older window
            tear_down, // it was open: take its interface down, then retire it
            forget,    // it was still opening: retire it when it is reported
        };

        open_result request_open(window_kind kind) noexcept
        {
            slot &s = m_slots[index_of(kind)];
            if (m_exiting)
                return {};
            switch (s.current)
            {
            case state::closed:
                s.current = state::opening;
                s.serial = ++m_last_serial;
                return {open_action::create, s.serial};
            case state::opening:
                return {open_action::none, s.serial};
            case state::open:
                return {open_action::focus, s.serial};
            }
            return {};
        }

        // The platform thread made window `serial`. True if it is the one
        // the slot waits for, which is now open; false if it was closed
        // meanwhile, or the application is exiting: retire it at once.
        bool created(window_kind kind, std::uint64_t serial) noexcept
        {
            slot &s = m_slots[index_of(kind)];
            if (m_exiting || s.current != state::opening || s.serial != serial)
                return false;
            s.current = state::open;
            return true;
        }

        // Making window `serial` failed; the slot is free again.
        void failed(window_kind kind, std::uint64_t serial) noexcept
        {
            slot &s = m_slots[index_of(kind)];
            if (s.current == state::opening && s.serial == serial)
                s.current = state::closed;
        }

        // A request to close window `serial` (the user's close button), or
        // whichever window of the kind there is (serial 0: the interface's
        // own requests). A second request for the same window finds it
        // closed and does nothing.
        close_action request_close(window_kind kind, std::uint64_t serial = 0) noexcept
        {
            slot &s = m_slots[index_of(kind)];
            if (s.current == state::closed || (serial != 0 && s.serial != serial))
                return close_action::none;
            const bool was_open = s.current == state::open;
            s.current = state::closed;
            return was_open ? close_action::tear_down : close_action::forget;
        }

        // From now on nothing opens; what is open stays in the book until
        // closed.
        void begin_exit() noexcept { m_exiting = true; }
        bool exiting() const noexcept { return m_exiting; }

        state state_of(window_kind kind) const noexcept { return m_slots[index_of(kind)].current; }
        bool is_open(window_kind kind) const noexcept { return state_of(kind) == state::open; }

        // The serial of the window the slot holds or waits for; 0 if it
        // never had one.
        std::uint64_t serial_of(window_kind kind) const noexcept { return m_slots[index_of(kind)].serial; }

    private:
        struct slot
        {
            state current = state::closed;
            std::uint64_t serial = 0;
        };

        std::array<slot, window_kind_count> m_slots{};
        std::uint64_t m_last_serial = 0;
        bool m_exiting = false;
    };

} // namespace volt::ui
