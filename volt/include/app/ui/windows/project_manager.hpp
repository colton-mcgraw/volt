#pragma once

#include <app/ui/project_history.hpp>
#include <app/ui/windows/content.hpp>
#include <ui/widgets/list_view.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// project_manager
// -------------------------------------------------------------------------
//
// The main window: open from start to exit, and closing it (or File > Exit
// in any window) ends the application. A launcher and an overview of the
// project, not an editor:
//
//   menu         File: New Project..., Open..., Save Project, Save Project
//                As..., Close Project (then the frame's items); History:
//                Take Snapshot..., Switch Branch (opens the branch list),
//                New Branch..., Rename Branch..., Delete Branch..., Restore
//                Selected Snapshot, New Branch from Selected Snapshot...,
//                Check for Changes (F5)
//   toolbar      brand; New, Open, Save; the three editors
//   left         PROJECT: the project's files as a tree (its sheets, its
//                board, its project library and the parts in it), filtered
//                by the field above it. Activating a sheet opens the
//                Schematic Editor, the board the Board Editor, the library
//                or a part the Library Browser.
//   right        the project's identity (name, location, saved or not);
//                EDITORS, a tile per editor saying whether it is open and
//                opening (or raising) it; HISTORY, the project's snapshots
//                and branches (below); MESSAGES, every status message any
//                window has shown (app_services::messages()), since they
//                vanish from the status bars after seconds
//
// With no project open it says so, and the tree shows what stands in for
// one: the built-in demo (its sheet, the demo board and the models dropped
// on it, the parts catalog), labelled as the demo.
//
// HISTORY (<app/ui/project_history.hpp>, which does the work): the current
// branch, in a list to switch from, with New, Rename and Delete beside it;
// Snapshot..., which asks for a message; whether the files differ from the
// last snapshot, and which; the branch's snapshots, newest first, the one
// the project is at marked; and for the one selected, Restore (its files
// back, on the same branch: the head stays, and they show as changes until
// the next snapshot) and New Branch from Here... A snapshot is of what is
// on disk, so a project with unsaved changes is saved first, and the
// message says so. Whatever overwrites the files (switching, restoring,
// branching from an older snapshot) first asks what would be lost: the
// part editor's work meant for the project (app_services::project_guards(),
// as closing the project does), then, if the project has unsaved changes or
// files that differ from the last snapshot, offers to take a snapshot of
// them first or to cancel; nothing is overwritten without being in a
// snapshot. After files were put back the project is opened again from
// disk, and every window follows project_changed; if it cannot be, it is
// closed rather than left in memory to be saved over the files. With no
// project, or one not on disk, the area says so and its actions are off.
//
// The ways in: File > Open... (Ctrl+O) shows the system's file dialog
// (app_services::open_with_dialog()), and what is chosen opens as a drop
// would; where the system has none, it asks for a typed path in the window
// instead. A .voltproj dropped on the window, or named with --open (which
// arrives as a drop), opens through app_services::open_files(), as a drop
// on any window does. File > New Project... asks for a name and a folder
// (typed, or chosen with Browse...); File > Save Project As... asks the
// system's save dialog where. They go to app_services (new_project(),
// open_project(), save_project(), save_project_as(), close_project()) and
// follow project_changed. Whatever would replace or close a project with
// unsaved changes asks first, with app_services::confirm_discard(), as
// every other window does.
//
// Kept while closed: the filter text, the side panel's width and the
// snapshot selected (the history itself is read again when it opens).

namespace volt::ui
{

    class button;
    class column_view;
    class dropdown;
    class label;
    class log_view;
    class splitter;
    class text_field;
    class tree_view;
    class widget;

    class project_manager final : public window_content
    {
    public:
        explicit project_manager(app_services &services);
        ~project_manager() override;

        window_kind kind() const noexcept override { return window_kind::project_manager; }
        math::vec2i default_size() const noexcept override { return {1100, 700}; }
        math::vec2i min_size() const noexcept override { return {640, 420}; }

        window_layout build(window_host &host) override;
        void detach() override;
        void update(time_point now) override;
        void theme_changed() override;

    private:
        // A launcher tile's widgets, for saying whether its editor is open.
        struct tile
        {
            window_kind kind = window_kind::schematic_editor;
            label *state = nullptr;
            button *open = nullptr;
            bool shown_open = false;
        };

        std::unique_ptr<widget> build_toolbar(window_host &host);
        std::unique_ptr<widget> build_project_panel();
        std::unique_ptr<widget> build_overview(window_host &host);
        std::unique_ptr<widget> build_tile(window_host &host, window_kind kind, ui::icon icon,
                                           std::string description);
        std::unique_ptr<widget> build_messages();
        std::unique_ptr<widget> build_history(window_host &host);
        void add_commands(window_host &host);
        std::vector<menu_item> history_menu();

        // The dialogs of New Project..., and of Open... where the system has
        // no file dialog (a typed path).
        void ask_new_project();
        void ask_open_project();

        void save_project();
        void save_project_as();
        void close_project();

        void activate(const tree_node &node);
        void filter_tree(std::string_view text);
        void refresh_tree();
        void refresh_identity();
        void refresh_tiles(bool force);

        // ---- history ----------------------------------------------------------------
        //
        // The dialogs and what they lead to. Each that overwrites files
        // goes through confirm_overwrite() first, and finish_checkout()
        // after.

        void ask_snapshot();
        void ask_new_branch(std::string from);
        void ask_rename_branch();
        void ask_delete_branch();
        void switch_branch(std::string name);
        void restore_snapshot(std::string id);

        // Runs `then` once nothing would be lost by overwriting the
        // project's files: the project guards have let go, and the project
        // is saved and its files are in a snapshot; if they are not, asks
        // whether to take one (with `snapshot_message`) first, or cancel.
        // `doing` names what waits: "Switch to two-layer". Not at all if
        // the project changes meanwhile.
        void confirm_overwrite(std::string doing, std::string snapshot_message, std::function<void()> then);

        // Saves the project if it has unsaved changes, then takes a
        // snapshot; says what came of it in the status bar. False if it
        // failed.
        bool snapshot_now(const std::string &message);

        // After files were put back (or may have been): the project opened
        // again from disk (or closed, if it cannot be), then `done`, or why
        // not, in the status bar.
        void finish_checkout(const project_history::checkout &result, const std::string &failed,
                             const std::string &done);

        // Reads the history again (`reread`: the repository and the files'
        // status; otherwise only what the project's unsaved state changes)
        // and shows it.
        void refresh_history(bool reread);
        void show_history();
        void show_selection();
        const history_entry *selected_snapshot() const;
        // The branches other than the current one: those that can be
        // deleted.
        std::vector<std::string> other_branches() const;

        // The open project's history, followed while the window is closed
        // too (refreshed when it opens).
        project_history m_history;
        std::filesystem::path m_history_file; // the project file it follows
        std::string m_selected_snapshot;      // id

        // The model the tree shows, unfiltered, and the filter.
        std::vector<tree_node> m_project;
        std::string m_filter;

        // The side panel's width, logical units, as the user left it.
        float m_side_width = 320.0f;

        // Connections to shared signals while the window is open.
        std::size_t m_models_connection = 0;
        std::size_t m_project_connection = 0;
        std::size_t m_catalog_connection = 0;
        std::size_t m_message_connection = 0;
        std::size_t m_cleared_connection = 0;

        // Widgets talked to after building them; owned by the screen's tree,
        // null while the window is closed.
        splitter *m_split = nullptr;
        tree_view *m_tree = nullptr;
        label *m_name = nullptr;
        label *m_location = nullptr;
        label *m_state = nullptr;
        button *m_save_tool = nullptr;
        log_view *m_log = nullptr;
        std::array<tile, 3> m_tiles{};

        dropdown *m_branch_choice = nullptr;
        std::vector<std::string> m_branch_names; // as the dropdown lists them; empty for "no branch"
        button *m_new_branch = nullptr;
        button *m_rename_branch = nullptr;
        button *m_delete_branch = nullptr;
        button *m_snapshot = nullptr;
        label *m_history_state = nullptr;
        label *m_history_changes = nullptr;
        column_view *m_snapshot_list = nullptr;
        button *m_restore = nullptr;
        button *m_branch_from = nullptr;
        label *m_selected_text = nullptr;
    };

} // namespace volt::ui
