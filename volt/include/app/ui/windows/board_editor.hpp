#pragma once

#include <app/board/scene.hpp>
#include <app/ui/windows/board_view.hpp>
#include <app/ui/windows/content.hpp>
#include <ui/controllers.hpp>

#include <model/model.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// board_editor
// -------------------------------------------------------------------------
//
// The board, in a window of its own, laid out as a board editor is:
//
//   menus        Edit (selection: hide, show, remove), View (camera,
//                framing, projection, what is shown), Place and Route (the
//                tools), Tools (select, measure)
//   toolbar      brand; the camera presets; frame all, frame the
//                selection; parallel projection; the selection's hide and
//                remove
//   tool strip   at the left of the view: select, route, via, zone,
//                measure. One is active; its shortcut and its state show in
//                the status bar. Esc (Edit > Select Nothing) goes back to
//                the Select tool with nothing selected, in one press
//   view         the board in 3D (offscreen_view::board): orbited, panned
//                and dollied with the mouse; a click selects what is under
//                it, a drag on a dropped model moves it on the board
//                (board_view_controller, <app/ui/windows/board_view.hpp>)
//   dock         at the right: tabs (Objects, Appearance) over the
//                selection's Properties
//   status       the tool, the selection, how many objects, the camera or
//                the point of the board under the pointer
//
// What works and what does not. The board is the demo board (<app/
// demo.hpp>) and the models dropped on it: every one of them is an object
// that can be selected (in the view or the list), framed, hidden, and its
// properties read; dropped models can also be moved (dragged in the view,
// or their position typed) and taken off. Measure measures on the board's
// top. There is no board document yet, so nothing routes or places: the
// route, via and zone tools are there as the tools a board editor has, and
// say plainly that they do nothing yet; so do Undo and Redo.
//
// Files dropped on this window: a model dropped on the board view goes
// where it was dropped (the point on the board's top under the pointer);
// one dropped elsewhere goes in a free corner. Either way it is selected and
// the camera frames it. Images open in the image viewer, as in every window.
//
// The board, its objects, what is shown and what is selected are
// app_services::board(), shared with every window. The camera, the tool,
// the dock's sizes and its tab are this object's, and survive the window
// closing.

namespace volt::ui
{

    class button;
    class checkbox;
    class label;
    class numeric_field;
    class property_grid;
    class radio_group;
    class scroll_area;
    class splitter;
    class tab_view;
    class tool_strip;
    class toggle_list;
    class viewport;
    class widget;

    class board_editor final : public window_content
    {
    public:
        explicit board_editor(app_services &services);
        ~board_editor() override;

        window_kind kind() const noexcept override { return window_kind::board_editor; }
        math::vec2i default_size() const noexcept override { return {1280, 780}; }

        // Narrow windows lose the less needed tools and status segments
        // (update_layout_for_width()); below this even those that stay
        // would not fit.
        math::vec2i min_size() const noexcept override { return {720, 480}; }

        window_layout build(window_host &host) override;
        void detach() override;
        void before_frame() override;
        viewport *view(offscreen_view view) noexcept override;
        void drop(std::span<const std::string> paths, math::vec2i position) override;
        void theme_changed() override;

    private:
        // The tools of the strip, in its order.
        enum class tool : int
        {
            select,
            route,
            via,
            zone,
            measure,
        };

        // A row of the objects list: an object, or a kind's heading.
        struct list_entry
        {
            board::object_id object = 0;
            int kind = -1;
        };

        // ---- building ------------------------------------------------------
        void add_commands(window_host &host);
        std::vector<window_layout::menu> build_menus();
        std::unique_ptr<widget> build_toolbar(window_host &host);
        std::unique_ptr<widget> build_tool_strip();
        std::unique_ptr<widget> build_view(window_host &host);
        std::unique_ptr<widget> build_dock();
        std::unique_ptr<widget> build_objects_page();
        std::unique_ptr<widget> build_appearance_page();
        std::unique_ptr<widget> build_properties();
        void build_status(status_bar &bar);

        // ---- camera --------------------------------------------------------
        void sync_camera();
        void set_preset(int preset);
        void camera_moved();
        void frame(const model::bounds &bounds);
        void frame_all();
        void frame_selection();
        void set_orthographic(bool orthographic);
        void set_fov(float radians);
        void apply_background();
        std::string camera_text() const;

        // ---- objects and selection ------------------------------------------
        void objects_changed();
        void selection_changed();
        void sync_selection_widgets();
        void refresh_objects();
        void refresh_layers();
        void refresh_properties();
        void update_properties();
        void update_marks();
        void toggle_object(int row, bool on);
        void toggle_layer(int row, bool on);
        void hide_selected();
        void show_all();
        void remove_selected();
        void remove_models();

        // ---- tools and the view ------------------------------------------------
        void set_tool(tool t);
        void set_dock_shown(bool shown);

        // Esc, Edit > Select Nothing: the Select tool, nothing selected.
        void escape();
        void view_clicked(const math::vec2f &pixel, const math::vec2f &size);
        bool can_grab(const math::vec2f &pixel, const math::vec2f &size);
        void grab_moved(const math::vec2f &to, const math::vec2f &size);
        void grab_released(bool cancelled);
        void update_pointer_status();
        void update_layout_for_width();
        void say(std::string text);

        // The point on the board under `window_pixels`, if that is over the
        // board view and the board.
        std::optional<math::vec2f> board_point(const math::vec2i &window_pixels) const;

        // ---- state kept while the window is closed -----------------------------
        orbit_controller m_controller;
        board_view_controller m_view_controller{m_controller};
        int m_preset = 0; // -1 once the camera is turned by hand
        float m_preset_yaw = 0.0f;
        float m_preset_pitch = 0.0f;
        bool m_orthographic = false;
        tool m_tool = tool::select;
        int m_background = 0; // 0 the board's dark, 1 the theme's
        math::vec3f m_board_background{};

        // The theme's accent, for what the view marks (working space); set
        // from the theme in build() and theme_changed().
        math::vec3f m_accent{};

        float m_dock_width = 330.0f;
        float m_properties_height = 240.0f;
        int m_dock_page = 0;

        // The user wants the dock (View > Show Dock, Ctrl+Shift+D). It is
        // collapsed (splitter::set_collapsed()) when not, and also while
        // the window is too narrow for it and the view both.
        bool m_dock_shown = true;

        // A measurement on the board's top: its first point, then its second.
        std::optional<math::vec2f> m_measure_from;
        std::optional<math::vec2f> m_measure_to;

        // A dropped model being dragged: which, where the press was on the
        // board, and how far it has been moved so far.
        board::object_id m_grab_candidate = 0;
        board::object_id m_grabbed = 0;
        math::vec2f m_grab_start{};
        math::vec2f m_grab_moved{};

        std::size_t m_changed_connection = 0;
        std::size_t m_placed_connection = 0;
        std::size_t m_objects_connection = 0;
        std::size_t m_selection_connection = 0;

        // The objects list's rows, and whether the dock needs them again.
        std::vector<list_entry> m_entries;
        bool m_properties_dirty = true;
        board::object_id m_properties_for = 0;

        // ---- widgets talked to after building them; owned by the screen's
        // tree, null while the window is closed --------------------------------
        splitter *m_split = nullptr;
        splitter *m_dock_split = nullptr;
        tab_view *m_dock_tabs = nullptr;
        viewport *m_view = nullptr;
        tool_strip *m_tools = nullptr;
        toggle_list *m_objects = nullptr;
        toggle_list *m_layers = nullptr;
        scroll_area *m_properties = nullptr;
        radio_group *m_projection_choice = nullptr;
        button *m_projection_tool = nullptr;
        button *m_frame_selection_tool = nullptr;
        button *m_hide_tool = nullptr;
        button *m_remove_tool = nullptr;
        std::vector<widget *> m_narrow_hidden; // toolbar buttons that go when the window is narrow

        // The selection's properties that change while it stays selected.
        numeric_field *m_x_field = nullptr;
        numeric_field *m_y_field = nullptr;
        label *m_x_label = nullptr;
        label *m_y_label = nullptr;
        label *m_extent_label[3] = {nullptr, nullptr, nullptr};
        checkbox *m_visible_box = nullptr;
        button *m_remove_button = nullptr;

        label *m_tool_status = nullptr;
        label *m_selection_status = nullptr;
        label *m_count_status = nullptr;
        label *m_camera_status = nullptr;
        widget *m_status_separators[3] = {nullptr, nullptr, nullptr};
    };

} // namespace volt::ui
