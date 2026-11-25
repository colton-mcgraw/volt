#pragma once

#include <ui/painter.hpp>
#include <ui/screen.hpp>
#include <ui/signal.hpp>
#include <ui/widget.hpp>

#include <functional>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// menus
// -------------------------------------------------------------------------
//
// A menu is a list of items shown in a popup: a menu bar's, a context
// menu, a dropdown's list. An item runs a command (by the screen's command
// id, which is also where its shortcut text comes from) or a function, or
// opens a submenu to its right; it may show a check mark and be disabled.
//
// Pointer: hovering an item highlights it and opens its submenu, if it has
// one; clicking an item activates it, which closes the whole chain of menus
// before the item's action runs, so the action finds the interface as it
// was. Keys: Up/Down move between items (separators and disabled items are
// skipped), Right opens a submenu, Left closes one, Enter/Space activate,
// Escape closes the menu (the screen does that). On a menu bar's menu,
// Left/Right at the top level move to the neighbouring menu.
//
// The menu widgets are popup contents: they belong to the screen while
// open, and are destroyed when closed. open_menu() and open_context_menu()
// are how menus are shown.

namespace volt::ui
{

    struct menu_item
    {
        std::string text;

        // A screen command to run (screen::add_command()); its shortcut is
        // shown on the right.
        std::string command;

        // Or a function to run.
        std::function<void()> action;

        std::vector<menu_item> submenu;

        // A check mark beside the item while this returns true.
        std::function<bool()> checked;

        // The item is greyed out while this returns false.
        std::function<bool()> enabled;

        ui::icon icon = icon::none;
        bool is_separator = false;

        // When set, the text is this one's answer, asked each time the menu
        // opens (an Undo that names what it would undo): `text` then only
        // stands until then.
        std::function<std::string()> text_of;

        static menu_item separator()
        {
            menu_item item;
            item.is_separator = true;
            return item;
        }
    };

    class menu : public widget
    {
    public:
        explicit menu(std::vector<menu_item> items, menu *parent = nullptr);

        const std::vector<menu_item> &items() const noexcept { return m_items; }

        // The highlighted item, -1 for none.
        int hot() const noexcept { return m_hot; }
        void set_hot(int index);

        // The open submenu, if any.
        menu *submenu() const noexcept { return m_child; }
        menu *parent_menu() const noexcept { return m_parent; }

        // Runs item `index` as a click would.
        void activate(int index);

        // Where item `index` is, as laid out.
        rect item_rect(std::size_t index) const;

        // Left (-1) or Right (+1) at the top level: for the menu bar.
        signal<int> navigate;

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_paint(painter &p) override;
        bool on_pointer_down(const pointer_event &e) override;
        bool on_pointer_up(const pointer_event &e) override;
        void on_pointer_move(const pointer_event &e) override;
        bool on_key(const key_event &e) override;

    private:
        int item_at(const math::vec2f &point) const;
        bool usable(int index) const;
        int step(int from, int direction) const;
        void open_submenu(int index, bool focus_first);
        void close_submenu();
        menu &root();

        std::vector<menu_item> m_items;
        menu *m_parent;
        menu *m_child = nullptr;
        int m_child_index = -1;
        int m_hot = -1;
        int m_pressed = -1;
    };

    // Opens `items` as a menu by `anchor` and returns it. `owner` is the
    // widget that opened it (see popup_options::owner).
    menu &open_menu(screen &screen, std::vector<menu_item> items, const rect &anchor, placement where,
                    widget *owner = nullptr, std::function<void()> closed = {});

    // A context menu with its corner at `point`.
    menu &open_context_menu(screen &screen, std::vector<menu_item> items, const math::vec2f &point);

    // ---------------------------------------------------------------------
    // menu_bar
    // ---------------------------------------------------------------------
    //
    // The row of menu titles at the top of the window. Clicking a title opens
    // its menu below it; while one is open, moving over another title opens
    // that one instead, as desktop menu bars do.
    class menu_bar : public widget
    {
    public:
        menu_bar();

        void add_menu(std::string title, std::vector<menu_item> items);

        std::size_t menu_count() const noexcept { return m_menus.size(); }

        // The title's rectangle, as laid out.
        rect title_rect(std::size_t index) const;

        // Which menu is open, -1 for none.
        int open_index() const noexcept { return m_open_index; }
        menu *open_menu_widget() const noexcept { return m_open; }

        void open(int index, bool focus_first = false);
        void close();

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_paint(painter &p) override;
        bool on_pointer_down(const pointer_event &e) override;
        void on_pointer_move(const pointer_event &e) override;
        void on_pointer_leave() override;
        void on_detached() override { close(); }

    private:
        int title_at(const math::vec2f &point) const;

        struct entry
        {
            std::string title;
            std::vector<menu_item> items;
            float width = 0.0f;
        };

        std::vector<entry> m_menus;
        menu *m_open = nullptr;
        int m_open_index = -1;
        int m_hot = -1;
    };

} // namespace volt::ui
