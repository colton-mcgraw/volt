#pragma once

#include <app/ui/windows/content.hpp>
#include <app/ui/windows/library_model.hpp>
#include <app/ui/windows/part_search.hpp>
#include <ui/screen.hpp>
#include <ui/widgets/column_view.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// library_browser
// -------------------------------------------------------------------------
//
// The parts library, in a window of its own, in two tabs:
//
//   Parts library     every part in the catalog (app_services::catalog())
//     banner          while the libraries are scanned, and after a scan
//                     that was not clean (with the problems a click away)
//     left            "All libraries" and each library, with their
//                     category trees and part counts; choosing one
//                     restricts the list to it
//     centre          a search field (Ctrl+F; Escape clears it; Up and Down
//                     move through the list while typing) over the parts,
//                     a column_view of name, value, package, manufacturer,
//                     MPN and library: by relevance while searching, or by
//                     the column whose header was clicked
//     right           the selected part: its fields, parameters, keywords,
//                     datasheet and 3D model; Add to Project (Enter in the
//                     list), Copy MPN; Edit (F2), Duplicate and Delete, in
//                     the part editor (<app/ui/windows/part_editor.hpp>):
//                     a part of a library shipped with Volt is not edited
//                     or deleted in place, Edit offers to duplicate it into
//                     My Parts instead
//     New Part        beside the search field, and in the Library menu
//
//   Project library   the open project's parts (app_services::project())
//     header          the project's name and file, modified or not, New
//                     Part (into the project), Save
//     centre          its parts with their status against the catalog
//     right           the selected one's details; Update from Source (asks
//                     first when edits made in the project would be lost),
//                     Edit (F2), Remove, Show in Parts Library
//
//   status            how many parts the list shows of how many; the project
//
// Search runs as the user types. For catalogs of up to 20 000 parts on the
// interface thread: catalog::search is a few milliseconds even in a Debug
// build for the shipped libraries, and when the last search and sort took
// longer than a frame's worth the next one waits until typing pauses
// (debounce). Bigger catalogs are searched on a thread of the browser's
// own, the newest query winning, so typing never waits for a search.
//
// Kept while closed: the tab, the search text, the library and category,
// the selection (by qualified id, which survives a rescan), the sort, the
// panes' widths and the columns' widths.

namespace volt::ui
{

    class button;
    class label;
    class panel;
    class splitter;
    class tab_view;
    class text_field;
    class tree_view;
    class widget;

    class library_browser final : public window_content
    {
    public:
        explicit library_browser(app_services &services);
        ~library_browser() override;

        window_kind kind() const noexcept override { return window_kind::library_browser; }
        math::vec2i default_size() const noexcept override { return {1180, 720}; }
        math::vec2i min_size() const noexcept override { return {780, 480}; }

        window_layout build(window_host &host) override;
        void detach() override;

        // While the part editor is open, a 3D model dropped on the window
        // is its part's model; otherwise files open as in any window.
        void drop(std::span<const std::string> paths, math::vec2i position) override;

    private:
        enum class tab : int
        {
            parts = 0,
            project = 1,
        };

        std::unique_ptr<widget> build_parts_tab(window_host &host);
        std::unique_ptr<widget> build_project_tab(window_host &host);
        void add_commands(window_host &host);
        std::vector<window_layout::menu> build_menus();

        // ---- the parts library tab
        void catalog_changed();
        void catalog_status_changed();
        void search_edited(const std::string &text);
        void apply_search();
        void search_in_background();
        void finish_search(const std::shared_ptr<const library::catalog> &catalog, part_filter filter,
                           std::vector<library::part_ref> ranked, double milliseconds);
        void refresh_rows(bool keep_scroll);
        void select_row(int row);
        void show_part_details();
        void update_part_actions();
        void update_counts();
        void rebuild_scope_tree();
        void choose_scope(const std::string &library, const std::string &category);
        void sort_by(std::optional<part_column> column, bool ascending);
        void add_selected();
        void copy_mpn();

        // ---- making and changing parts (the part editor,
        // <app/ui/windows/part_editor.hpp>)
        void new_part();
        void edit_selected();
        void duplicate_selected();
        void delete_selected();
        // The selected catalog part's library, if its file may be changed
        // in place (not shipped with Volt).
        const library::library *selected_writable_library() const;

        // ---- the project library tab
        void project_changed();
        void refresh_project();
        void select_project_row(int row);
        void show_project_details();
        void update_from_source();
        void remove_selected();
        void reveal_selected();
        void save_project();

        // Shows a part asked for through app_services::reveal_in_library().
        void take_reveal();
        void show_tab(tab which);

        const library::part *selected_part() const;
        const project_row *selected_project_row() const;

        // ---- kept while closed
        tab m_tab = tab::parts;
        part_list m_parts;
        std::string m_search;
        std::string m_selected;         // qualified id
        std::string m_selected_project; // id in the project library
        std::vector<table_column> m_part_columns;
        std::vector<table_column> m_project_columns;
        float m_scope_width = 250.0f;
        float m_details_width = 340.0f;
        float m_project_details_width = 360.0f;

        std::vector<project_row> m_project_rows;

        // The search waits for typing to pause when searching is slow.
        screen::timer_id m_search_timer = 0;
        std::vector<std::size_t> m_connections;


        // Widgets talked to after building them; owned by the screen's tree,
        // null while the window is closed.
        tab_view *m_tabs = nullptr;
        splitter *m_outer = nullptr;
        splitter *m_inner = nullptr;
        splitter *m_project_split = nullptr;
        panel *m_banner = nullptr;
        label *m_banner_text = nullptr;
        button *m_banner_button = nullptr;
        tree_view *m_scope_tree = nullptr;
        text_field *m_search_field = nullptr;
        label *m_result_count = nullptr;
        column_view *m_list = nullptr;
        widget *m_details = nullptr;
        button *m_add = nullptr;
        button *m_copy = nullptr;
        button *m_show_in_project = nullptr;
        button *m_edit = nullptr;
        button *m_duplicate = nullptr;
        button *m_delete = nullptr;
        label *m_add_hint = nullptr;
        label *m_count_status = nullptr;
        label *m_project_status = nullptr;

        label *m_project_title = nullptr;
        label *m_project_file = nullptr;
        button *m_save = nullptr;
        column_view *m_project_list = nullptr;
        widget *m_project_details = nullptr;
        button *m_update = nullptr;
        button *m_remove = nullptr;
        button *m_reveal = nullptr;
        button *m_project_edit = nullptr;
        label *m_project_hint = nullptr;

        // Searches of a large catalog, on a thread of their own, newest
        // wins. Last, so that it stops (joins) before the rest of the
        // browser goes.
        part_search m_searcher;
    };

} // namespace volt::ui
