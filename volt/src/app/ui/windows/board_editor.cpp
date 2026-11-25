#include <app/ui/windows/board_editor.hpp>

#include <app/ui/windows/menus.hpp>
#include <ui/layout/box.hpp>
#include <ui/theme.hpp>
#include <ui/widgets/bars.hpp>
#include <ui/widgets/button.hpp>
#include <ui/widgets/checkbox.hpp>
#include <ui/widgets/dropdown.hpp>
#include <ui/widgets/label.hpp>
#include <ui/widgets/numeric_field.hpp>
#include <ui/widgets/panel.hpp>
#include <ui/widgets/property_grid.hpp>
#include <ui/widgets/radio_group.hpp>
#include <ui/widgets/scroll_area.hpp>
#include <ui/widgets/slider.hpp>
#include <ui/widgets/splitter.hpp>
#include <ui/widgets/tab_view.hpp>
#include <ui/widgets/toggle_list.hpp>
#include <ui/widgets/tool_strip.hpp>
#include <ui/widgets/viewport.hpp>

#include <math/geometry.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace volt::ui
{
    namespace
    {
        // The camera presets: their names, and the keys that choose them.
        constexpr const char *preset_names[] = {"Isometric", "Top", "Bottom", "Front", "Back", "Left", "Right"};
        constexpr const char *preset_keys[] = {"Home", "1", "2", "3", "4", "5", "6"};
        constexpr int preset_count = 7;

        // The tools of the strip, in its order (board_editor::tool).
        struct tool_info
        {
            ui::icon icon;
            const char *name;
            const char *command;
            const char *key;
            bool works;
        };

        constexpr tool_info tools[] = {
            {icon::pointer, "Select", "tool.select", "Escape", true},
            {icon::route, "Route tracks", "tool.route", "X", false},
            {icon::via, "Place vias", "tool.via", "V", false},
            {icon::zone, "Draw zones", "tool.zone", "Z", false},
            {icon::ruler, "Measure", "tool.measure", "M", true},
        };

        constexpr board::object_kind kinds[] = {board::object_kind::board, board::object_kind::part_body,
                                                board::object_kind::part_model, board::object_kind::dropped_model};

        const char *group_name(board::object_kind kind)
        {
            switch (kind)
            {
            case board::object_kind::board:
                return "Board";
            case board::object_kind::part_body:
                return "Part bodies";
            case board::object_kind::part_model:
                return "Part models";
            case board::object_kind::dropped_model:
                return "Dropped models";
            }
            return "Objects";
        }

        ui::icon kind_icon(board::object_kind kind)
        {
            switch (kind)
            {
            case board::object_kind::board:
                return icon::layers;
            case board::object_kind::part_body:
                return icon::component;
            case board::object_kind::part_model:
                return icon::cube;
            case board::object_kind::dropped_model:
                return icon::file;
            }
            return icon::none;
        }

        // Window widths, logical units, below which the less needed bits
        // of the toolbar and the status bar step aside.
        constexpr float narrow_toolbar = 1000.0f;
        constexpr float narrow_camera_status = 1120.0f;
        constexpr float narrow_count_status = 960.0f;
        constexpr float narrow_selection_status = 780.0f;

        // Below this the view and the dock cannot both keep their minimum
        // widths: the dock is collapsed, and comes back as it was.
        constexpr float narrow_dock = 640.0f;
        constexpr float view_minimum = 320.0f;
        constexpr float dock_minimum = 260.0f;

        std::string mm(float value)
        {
            return std::format("{:.2f} mm", value);
        }

        std::string count_text(std::size_t count, const char *one, const char *many)
        {
            return std::format("{} {}", count, count == 1 ? one : many);
        }

        math::vec3f rgb(const math::vec4f &color)
        {
            return {color[0], color[1], color[2]};
        }
    } // namespace

    board_editor::board_editor(app_services &services) : window_content(services)
    {
        // The camera starts where the scene does. Close enough to frame a
        // part a millimeter long.
        board_document &doc = services.board();
        const board::scene &scene = doc.scene;
        m_controller.look(scene.view.eye, scene.view.target);
        m_controller.fov_y = scene.view.fov_y;
        m_controller.min_distance = 0.5f;
        m_preset_yaw = m_controller.yaw;
        m_preset_pitch = m_controller.pitch;
        m_board_background = scene.background;

        // The demo board's body and part bodies are boxes: objects from
        // here on, to list and pick like the rest.
        doc.adopt_boxes();

        m_view_controller.can_grab = [this](const math::vec2f &pixel, const math::vec2f &size) {
            return can_grab(pixel, size);
        };
        m_view_controller.clicked = [this](const math::vec2f &pixel, const math::vec2f &size, modifiers) {
            view_clicked(pixel, size);
        };
        m_view_controller.grabbed = [this](const math::vec2f &, const math::vec2f &to, const math::vec2f &size) {
            grab_moved(to, size);
        };
        m_view_controller.released = [this](bool cancelled) { grab_released(cancelled); };

        // A model placed from any window is framed, whether or not this one
        // is open: the camera is this object's.
        m_placed_connection = doc.model_placed.connect([this](const model::bounds &bounds) {
            m_preset = -1;
            frame(bounds);
        });
    }

    board_editor::~board_editor()
    {
        services().board().model_placed.disconnect(m_placed_connection);
    }

    window_layout board_editor::build(window_host &host)
    {
        add_commands(host);

        board_document &doc = services().board();
        doc.adopt_boxes();
        m_changed_connection = doc.changed.connect([this] {
            if (m_view != nullptr)
                m_view->invalidate();
        });
        m_objects_connection = doc.objects_changed.connect([this] { objects_changed(); });
        m_selection_connection = doc.selection_changed.connect([this] { selection_changed(); });

        window_layout layout;
        layout.menus = build_menus();

        // Toolbar over [tool strip | view | dock].
        auto body = std::make_unique<column>(0.0f);
        body->add(build_toolbar(host));
        auto &main = body->emplace<row>(0.0f);
        main.layout().flex = 1.0f;
        main.add(build_tool_strip());

        auto split = std::make_unique<splitter>(orientation::horizontal);
        split->set_name("board splitter");
        split->set_fixed(splitter::keep::second, m_dock_width);
        split->set_minimums(view_minimum, dock_minimum);
        split->layout().flex = 1.0f;
        split->set_first(build_view(host));
        split->set_second(build_dock());
        m_split = split.get();
        main.add(std::move(split));
        layout.body = std::move(body);

        layout.status = [this](status_bar &bar) { build_status(bar); };

        theme_changed();
        refresh_objects();
        refresh_layers();
        m_properties_dirty = true;
        sync_camera();
        return layout;
    }

    void board_editor::detach()
    {
        if (m_split != nullptr && m_split->second() != nullptr && m_split->second()->visible() &&
            m_split->second()->bounds().width > 0.0f)
            m_dock_width = m_split->second()->bounds().width;
        if (m_dock_split != nullptr && m_dock_split->second() != nullptr &&
            m_dock_split->second()->bounds().height > 0.0f)
            m_properties_height = m_dock_split->second()->bounds().height;
        if (m_dock_tabs != nullptr)
            m_dock_page = m_dock_tabs->selected();

        // A drag cut short by the window closing ends where it is.
        m_grabbed = 0;
        m_grab_candidate = 0;

        board_document &doc = services().board();
        doc.changed.disconnect(m_changed_connection);
        doc.objects_changed.disconnect(m_objects_connection);
        doc.selection_changed.disconnect(m_selection_connection);
        m_changed_connection = m_objects_connection = m_selection_connection = 0;

        m_split = nullptr;
        m_dock_split = nullptr;
        m_dock_tabs = nullptr;
        m_view = nullptr;
        m_tools = nullptr;
        m_objects = nullptr;
        m_layers = nullptr;
        m_properties = nullptr;
        m_projection_choice = nullptr;
        m_projection_tool = nullptr;
        m_frame_selection_tool = nullptr;
        m_hide_tool = nullptr;
        m_remove_tool = nullptr;
        m_narrow_hidden.clear();
        m_x_field = m_y_field = nullptr;
        m_x_label = m_y_label = nullptr;
        m_extent_label[0] = m_extent_label[1] = m_extent_label[2] = nullptr;
        m_visible_box = nullptr;
        m_remove_button = nullptr;
        m_tool_status = m_selection_status = m_count_status = m_camera_status = nullptr;
        m_status_separators[0] = m_status_separators[1] = m_status_separators[2] = nullptr;
        m_entries.clear();
        m_properties_dirty = true;
    }

    void board_editor::before_frame()
    {
        // Rebuilt here, between handlers: what is rebuilt may be what the
        // handler that asked for it belongs to (a Remove button).
        if (m_properties_dirty)
            refresh_properties();
        update_layout_for_width();

        const board_document &doc = services().board();
        if (m_tool_status != nullptr)
        {
            const tool_info &t = tools[static_cast<int>(m_tool)];
            m_tool_status->set_text(t.works ? std::format("Tool: {}", t.name)
                                            : std::format("{}: not implemented", t.name));
        }
        if (m_selection_status != nullptr)
        {
            const board::object *o = doc.find(doc.selected());
            m_selection_status->set_text(o == nullptr ? std::string("Nothing selected")
                                                      : std::format("{} \xC2\xB7 {}", o->name, board::kind_name(o->kind)));
        }
        if (m_count_status != nullptr)
        {
            std::string text = std::format("{} \xC2\xB7 {} dropped", count_text(doc.objects().size(), "object", "objects"),
                                           doc.models().size());
            if (const std::size_t loading = services().loading(); loading > 0)
                text += std::format(" \xC2\xB7 loading {}", loading);
            m_count_status->set_text(std::move(text));
        }
        update_pointer_status();
    }

    viewport *board_editor::view(offscreen_view view) noexcept
    {
        return view == offscreen_view::board ? m_view : nullptr;
    }

    void board_editor::drop(std::span<const std::string> paths, math::vec2i position)
    {
        // A model dropped on the board view goes where it was dropped.
        services().open_files(paths, kind(), board_point(position));
    }

    void board_editor::theme_changed()
    {
        const ui::theme t = services().theme();
        m_accent = rgb(t.accent);
        services().board().scene.selection_color = m_accent;
        apply_background();
        update_marks();
        services().board().mark_dirty();
    }

    // ---- building ------------------------------------------------------------------------

    void board_editor::add_commands(window_host &host)
    {
        ui::screen &s = host.screen();
        shortcut_table &keys = s.shortcuts();

        for (int i = 0; i < preset_count; ++i)
        {
            const std::string command = std::format("board.camera.{}", i);
            s.add_command(command, [this, i] { set_preset(i); });
            (void)keys.bind(preset_keys[i], command);
        }
        s.add_command("board.frame", [this] { frame_all(); });
        s.add_command("board.frame_selection", [this] { frame_selection(); });
        s.add_command("board.projection", [this] { set_orthographic(!m_orthographic); });
        s.add_command("board.hide_selected", [this] { hide_selected(); });
        s.add_command("board.show_all", [this] { show_all(); });
        s.add_command("board.remove_selected", [this] { remove_selected(); });
        s.add_command("board.remove_models", [this] { remove_models(); });
        s.add_command("view.dock", [this] { set_dock_shown(!m_dock_shown); });
        (void)keys.bind("Ctrl+Shift+D", "view.dock");
        (void)keys.bind("Ctrl+0", "board.frame");
        (void)keys.bind("F", "board.frame_selection");
        (void)keys.bind("P", "board.projection");
        (void)keys.bind("H", "board.hide_selected");
        (void)keys.bind("Shift+H", "board.show_all");
        (void)keys.bind("Delete", "board.remove_selected");
        (void)keys.bind("Ctrl+Delete", "board.remove_models");

        // Escape is one thing, and the Edit menu shows it: back to the
        // Select tool with nothing selected (Select Nothing). The Select
        // tool on its own (its button, Tools > Select) keeps the selection.
        s.add_command("board.select_none", [this] { escape(); });
        (void)keys.bind("Escape", "board.select_none");
        for (int i = 0; i < static_cast<int>(std::size(tools)); ++i)
        {
            s.add_command(tools[i].command, [this, i] { set_tool(static_cast<tool>(i)); });
            if (i != static_cast<int>(tool::select))
                (void)keys.bind(tools[i].key, tools[i].command);
        }
    }

    std::vector<window_layout::menu> board_editor::build_menus()
    {
        board_document &doc = services().board();
        std::vector<window_layout::menu> menus;

        // Edit: the selection. Undo and Redo are where they belong, and off:
        // nothing here edits the board in a way that could be undone yet.
        {
            menu_item undo = command_item("Undo", "", icon::undo);
            undo.enabled = [] { return false; };
            menu_item redo = command_item("Redo", "", icon::redo);
            redo.enabled = [] { return false; };
            menu_item hide = command_item("Hide Selection", "board.hide_selected", icon::eye_off);
            hide.enabled = [&doc] { return doc.selected() != 0; };
            menu_item show = command_item("Show Everything", "board.show_all", icon::eye);
            menu_item remove = command_item("Remove Selected Model", "board.remove_selected", icon::close);
            remove.enabled = [&doc] { return doc.is_dropped(doc.selected()); };
            menu_item remove_all = command_item("Remove All Dropped Models", "board.remove_models");
            remove_all.enabled = [&doc] { return !doc.models().empty(); };
            menu_item deselect = command_item("Select Nothing", "board.select_none");
            deselect.enabled = [this, &doc] { return doc.selected() != 0 || m_tool != tool::select; };
            menus.push_back({"Edit",
                             {undo, redo, menu_item::separator(), deselect, hide, show, menu_item::separator(), remove,
                              remove_all}});
        }

        // View: the camera, and what is drawn.
        {
            std::vector<menu_item> presets;
            for (int i = 0; i < preset_count; ++i)
            {
                menu_item item = command_item(preset_names[i], std::format("board.camera.{}", i),
                                              i == 0 ? icon::cube : icon::none);
                item.checked = [this, i] { return m_preset == i; };
                presets.push_back(std::move(item));
            }
            menu_item ortho = command_item("Parallel Projection", "board.projection");
            ortho.checked = [this] { return m_orthographic; };
            menu_item frame_selected = command_item("Frame Selection", "board.frame_selection", icon::target);
            frame_selected.enabled = [&doc] { return doc.selected() != 0; };

            std::vector<menu_item> shown;
            for (const board::object_kind k : kinds)
                shown.push_back(action_item(
                    group_name(k), [&doc, k] { doc.set_shown(k, !doc.shown(k)); }, [&doc, k] { return doc.shown(k); }));

            menu_item dock = command_item("Show Dock", "view.dock");
            dock.checked = [this] { return m_dock_shown; };

            menus.push_back({"View",
                             {dock, menu_item::separator(), submenu_item("Camera", std::move(presets)),
                              menu_item::separator(),
                              command_item("Frame Everything", "board.frame", icon::fit), frame_selected,
                              menu_item::separator(), ortho, menu_item::separator(),
                              submenu_item("Show", std::move(shown))}});
        }

        // The tools, under the menus a board editor keeps them in.
        const auto tool_item = [this](tool t) {
            const tool_info &info = tools[static_cast<int>(t)];
            menu_item item = command_item(info.works ? std::string(info.name) : std::string(info.name) + " (not implemented)",
                                          info.command, info.icon);
            item.checked = [this, t] { return m_tool == t; };
            return item;
        };
        menu_item model = action_item("3D Model: drop a STEP, OBJ or STL file on the view", [] {});
        model.enabled = [] { return false; };
        menus.push_back({"Place", {tool_item(tool::via), tool_item(tool::zone), menu_item::separator(), model}});
        menus.push_back({"Route", {tool_item(tool::route)}});
        menus.push_back({"Tools", {tool_item(tool::select), tool_item(tool::measure)}});
        return menus;
    }

    std::unique_ptr<widget> board_editor::build_toolbar(window_host &host)
    {
        auto bar = std::make_unique<toolbar>();
        bar->set_name("toolbar");
        add_brand(*bar, host);

        // The camera: the presets, framing, the projection.
        add_command_tool(*bar, host, icon::cube, "Isometric view (Home)", "board.camera.0", "Iso");
        for (int i = 1; i < preset_count; ++i)
        {
            button &preset = add_command_tool(*bar, host, icon::none,
                                              std::format("View from the {} ({})", preset_names[i], preset_keys[i]),
                                              std::format("board.camera.{}", i), preset_names[i]);
            m_narrow_hidden.push_back(&preset);
        }
        bar->add_separator();
        add_command_tool(*bar, host, icon::fit, "Frame everything (Ctrl+0)", "board.frame");
        m_frame_selection_tool = &add_command_tool(*bar, host, icon::target, "Frame the selection (F)",
                                                   "board.frame_selection");
        button &ortho = bar->add_tool(icon::none, "Parallel projection, no perspective (P)", "Parallel");
        ortho.set_name("projection tool");
        ortho.set_toggle(true);
        ortho.set_checked(m_orthographic);
        ortho.toggled.connect([this](bool on) { set_orthographic(on); });
        m_projection_tool = &ortho;

        // The selection.
        bar->add_spacer();
        m_hide_tool = &add_command_tool(*bar, host, icon::eye_off, "Hide the selection (H)", "board.hide_selected");
        add_command_tool(*bar, host, icon::eye, "Show everything (Shift+H)", "board.show_all");
        m_remove_tool = &add_command_tool(*bar, host, icon::close, "Remove the selected dropped model (Delete)",
                                          "board.remove_selected");
        return bar;
    }

    std::unique_ptr<widget> board_editor::build_tool_strip()
    {
        auto strip = std::make_unique<tool_strip>(orientation::vertical);
        strip->set_name("board tools");
        for (int i = 0; i < static_cast<int>(std::size(tools)); ++i)
        {
            if (i == static_cast<int>(tool::measure))
                strip->add_separator();
            const tool_info &t = tools[i];
            if (i == static_cast<int>(tool::select))
                strip->add_tool(t.icon, std::format("{} (Esc also clears the selection)", t.name));
            else
                strip->add_tool(t.icon, t.works ? std::format("{} ({})", t.name, t.key)
                                                : std::format("{} ({}): not implemented yet", t.name, t.key));
        }
        strip->select(static_cast<int>(m_tool));
        strip->changed.connect([this](int index) { set_tool(static_cast<tool>(index)); });
        m_tools = strip.get();
        return strip;
    }

    std::unique_ptr<widget> board_editor::build_view(window_host &host)
    {
        auto area = std::make_unique<panel>("", surface::background, 0.0f, insets::uniform(6.0f));
        area->set_name("board view area");
        auto view = std::make_unique<viewport>(services().view_image(offscreen_view::board));
        view->set_name("board view");
        view->set_radius(theme::dark_theme().radius + 2.0f);
        view->set_controller(&m_view_controller);
        view->layout().flex = 1.0f;
        view->changed.connect([this] { camera_moved(); });
        view->pointer_moved.connect([this](math::vec2f) { update_pointer_status(); });
        view->pointer_left.connect([this] { update_pointer_status(); });
        view->resized.connect([this](math::vec2i) { update_pointer_status(); });
        view->context_requested.connect([this, &host](math::vec2f at) {
            board_document &doc = services().board();
            menu_item frame_selected = command_item("Frame Selection", "board.frame_selection", icon::target);
            frame_selected.enabled = [&doc] { return doc.selected() != 0; };
            menu_item hide = command_item("Hide Selection", "board.hide_selected", icon::eye_off);
            hide.enabled = [&doc] { return doc.selected() != 0; };
            menu_item remove = command_item("Remove Selected Model", "board.remove_selected", icon::close);
            remove.enabled = [&doc] { return doc.is_dropped(doc.selected()); };
            open_context_menu(host.screen(),
                              {command_item("Frame Everything", "board.frame", icon::fit), frame_selected,
                               command_item("Isometric View", "board.camera.0", icon::cube), menu_item::separator(),
                               hide, command_item("Show Everything", "board.show_all", icon::eye),
                               menu_item::separator(), remove},
                              at);
        });
        m_view = view.get();
        area->add(std::move(view));
        return area;
    }

    std::unique_ptr<widget> board_editor::build_dock()
    {
        auto dock = std::make_unique<panel>("", surface::panel, 0.0f, insets{});
        dock->set_name("board dock");
        dock->set_edges(false, false, true, false);

        // Tabs over the properties; the properties keep their height as the
        // window changes size.
        auto &split = dock->emplace<splitter>(orientation::vertical);
        split.set_name("dock splitter");
        split.layout().flex = 1.0f;
        split.set_fixed(splitter::keep::second, m_properties_height);
        split.set_minimums(150.0f, 120.0f);
        m_dock_split = &split;

        auto tabs = std::make_unique<tab_view>();
        tabs->set_name("dock tabs");
        tabs->add_tab("Objects", build_objects_page());
        tabs->add_tab("Appearance", build_appearance_page());
        if (m_dock_page >= 0 && static_cast<std::size_t>(m_dock_page) < tabs->tab_count())
            tabs->select(m_dock_page);
        tabs->changed.connect([this](int page) { m_dock_page = page; });
        m_dock_tabs = tabs.get();
        split.set_first(std::move(tabs));
        split.set_second(build_properties());
        return dock;
    }

    std::unique_ptr<widget> board_editor::build_objects_page()
    {
        auto page = std::make_unique<column>(6.0f, insets::uniform(8.0f));
        page->set_name("objects page");

        auto list = std::make_unique<toggle_list>();
        list->set_name("objects list");
        list->layout().flex = 1.0f;
        list->selection_changed.connect([this](int row) {
            const board::object_id id =
                row >= 0 && static_cast<std::size_t>(row) < m_entries.size() ? m_entries[static_cast<std::size_t>(row)].object : 0;
            services().board().select(id);
        });
        list->activated.connect([this](int) { frame_selection(); });
        list->toggled.connect([this](int row, bool on) { toggle_object(row, on); });
        list->delete_requested.connect([this](int) { remove_selected(); });
        m_objects = list.get();
        page->add(std::move(list));
        return page;
    }

    std::unique_ptr<widget> board_editor::build_appearance_page()
    {
        auto page = std::make_unique<scroll_area>();
        page->set_name("appearance page");

        auto content = std::make_unique<column>(6.0f, insets::uniform(8.0f));

        auto &heading = content->emplace<label>("Objects shown", text_role::dim);
        heading.set_size(text_size::small);
        auto &layers = content->emplace<toggle_list>();
        layers.set_name("layers list");
        layers.set_tooltip("What the view draws, by kind of object");
        layers.toggled.connect([this](int row, bool on) { toggle_layer(row, on); });
        m_layers = &layers;

        auto grid = std::make_unique<property_grid>();
        grid->set_name("view options");
        grid->add_section("View");
        {
            auto projection = std::make_unique<radio_group>(std::vector<std::string>{"Perspective", "Parallel"},
                                                            m_orthographic ? 1 : 0);
            projection->set_name("projection");
            projection->changed.connect([this](int index) { set_orthographic(index == 1); });
            m_projection_choice = &grid->add_row("Projection", std::move(projection));

            constexpr float degrees = 180.0f / std::numbers::pi_v<float>;
            auto fov = std::make_unique<slider>(15.0, 80.0, std::round(m_controller.fov_y * degrees), 1.0);
            fov->set_name("field of view");
            fov->set_formatter([](double v) { return std::format("{:.0f}\xC2\xB0", v); });
            fov->changed.connect([this](double value) { set_fov(static_cast<float>(value) / degrees); });
            grid->add_row("Field of view", std::move(fov));

            auto background =
                std::make_unique<dropdown>(std::vector<std::string>{"Board dark", "Window theme"}, m_background);
            background->set_name("background");
            background->changed.connect([this](int index) {
                m_background = index;
                apply_background();
                services().board().mark_dirty();
            });
            grid->add_row("Background", std::move(background));
        }
        content->add(std::move(grid));
        page->set_content(std::move(content));
        return page;
    }

    std::unique_ptr<widget> board_editor::build_properties()
    {
        auto pane = std::make_unique<panel>("Properties", surface::panel, 0.0f, insets{});
        pane->set_name("properties pane");
        pane->set_edges(true, false, false, false);
        auto &scroller = pane->emplace<scroll_area>();
        scroller.set_name("properties scroll");
        scroller.layout().flex = 1.0f;
        m_properties = &scroller;
        m_properties_dirty = true;
        return pane;
    }

    void board_editor::build_status(status_bar &bar)
    {
        // The tool, then segments that step aside, each with the
        // separator before it, when the window is narrow.
        m_tool_status = &bar.add_segment("", 150.0f);
        m_tool_status->set_name("tool status");
        const auto optional = [&bar](const char *name, float width, widget *&separator) {
            bar.add_separator();
            separator = &bar.child(bar.child_count() - 1);
            label &segment = bar.add_segment("", width);
            segment.set_name(name);
            return &segment;
        };
        m_selection_status = optional("selection status", 230.0f, m_status_separators[0]);
        m_count_status = optional("count status", 190.0f, m_status_separators[1]);
        m_camera_status = optional("camera status", 190.0f, m_status_separators[2]);
    }

    // ---- camera ------------------------------------------------------------------------------

    void board_editor::sync_camera()
    {
        board_document &doc = services().board();
        board::camera &view = doc.scene.view;
        view.eye = m_controller.eye();
        view.target = m_controller.target;
        view.fov_y = m_controller.fov_y;
        view.orthographic = m_orthographic;

        // The depth range follows the distance, so that a part a millimeter
        // long, seen from a few millimeters, is not cut by the near plane,
        // and the board seen from afar keeps its depth precision.
        const float distance = m_controller.distance;
        view.z_near = std::clamp(distance * 0.01f, 0.01f, 1.0f);
        view.z_far = std::max(1000.0f, distance * 20.0f);

        // The marks are as thick as a pixel or two at the target's distance.
        update_marks();
        doc.mark_dirty();
        if (m_view != nullptr)
            m_view->set_caption(std::format("{} \xC2\xB7 {}", m_preset >= 0 ? preset_names[m_preset] : "Free view",
                                            m_orthographic ? "parallel" : "perspective"));
    }

    void board_editor::set_preset(int preset)
    {
        if (preset < 0 || preset >= preset_count)
            return;
        m_preset = preset;
        orbit_controller &c = m_controller;
        const float quarter = std::numbers::pi_v<float> * 0.5f;
        const float overhead = quarter - 0.01f; // straight down, within the orbit's limit
        switch (preset)
        {
        case 1: // top
            c.yaw = -quarter;
            c.pitch = overhead;
            break;
        case 2: // bottom
            c.yaw = -quarter;
            c.pitch = -overhead;
            break;
        case 3: // front, from -y
            c.yaw = -quarter;
            c.pitch = 0.0f;
            break;
        case 4: // back
            c.yaw = quarter;
            c.pitch = 0.0f;
            break;
        case 5: // left, from -x
            c.yaw = std::numbers::pi_v<float>;
            c.pitch = 0.0f;
            break;
        case 6: // right
            c.yaw = 0.0f;
            c.pitch = 0.0f;
            break;
        default: // isometric, where the demo started
            c.look({55.0f, -95.0f, 80.0f}, {0.0f, 0.0f, 0.0f});
            break;
        }
        m_preset_yaw = c.yaw;
        m_preset_pitch = c.pitch;

        // Then the whole board, from there.
        model::bounds all;
        for (const board::object &o : services().board().objects())
            all.add(o.bounds);
        if (all.empty())
            sync_camera();
        else
            frame(all);
    }

    void board_editor::camera_moved()
    {
        // Dollying and panning keep the direction a preset looks from.
        if (m_preset >= 0 && (std::abs(m_controller.yaw - m_preset_yaw) > 1.0e-4f ||
                              std::abs(m_controller.pitch - m_preset_pitch) > 1.0e-4f))
            m_preset = -1;
        sync_camera();
    }

    void board_editor::frame(const model::bounds &bounds)
    {
        if (bounds.empty())
            return;

        // Looking at the box's centre from where the camera looks now: as
        // close as every corner of the box allows, with a little room. A
        // corner off to the side wants the eye this far back along the view
        // for the half field of view to reach it; a corner nearer the eye
        // needs less, one further away more. Without a window the view has
        // no size; square then.
        const math::vec2i pixels = m_view != nullptr ? m_view->pixel_size() : math::vec2i{};
        const float aspect =
            pixels.x() > 0 && pixels.y() > 0 ? static_cast<float>(pixels.x()) / static_cast<float>(pixels.y()) : 1.0f;
        const float tan_y = std::tan(m_controller.fov_y * 0.5f);
        const float tan_x = tan_y * aspect;

        m_controller.target = bounds.center();
        const math::vec3f back = math::normalized(m_controller.eye() - m_controller.target);
        const math::vec3f forward = math::vec3f{0.0f, 0.0f, 0.0f} - back;
        const math::vec3f right = math::normalized(math::cross(forward, math::vec3f{0.0f, 0.0f, 1.0f}));
        const math::vec3f up = math::cross(right, forward);

        float distance = 0.0f;
        for (int i = 0; i < 8; ++i)
        {
            const math::vec3f corner{(i & 1) != 0 ? bounds.max[0] : bounds.min[0],
                                     (i & 2) != 0 ? bounds.max[1] : bounds.min[1],
                                     (i & 4) != 0 ? bounds.max[2] : bounds.min[2]};
            const math::vec3f d = corner - m_controller.target;
            // A parallel view is as wide at every depth.
            const float deeper = m_orthographic ? 0.0f : math::dot(d, forward);
            distance = std::max({distance, std::abs(math::dot(d, right)) / tan_x - deeper,
                                 std::abs(math::dot(d, up)) / tan_y - deeper});
        }
        const math::vec3f size = bounds.size();
        const float radius = std::max(0.5f * std::sqrt(math::dot(size, size)), 0.25f);
        m_controller.distance = std::clamp(std::max(distance * 1.08f, radius * 1.05f), m_controller.min_distance,
                                           m_controller.max_distance);
        sync_camera();
    }

    void board_editor::frame_all()
    {
        model::bounds all;
        const board_document &doc = services().board();
        for (const board::object &o : doc.objects())
        {
            if (doc.scene.drawn(o.id))
                all.add(o.bounds);
        }
        if (all.empty())
        {
            set_preset(0);
            return;
        }
        frame(all);
    }

    void board_editor::frame_selection()
    {
        const board_document &doc = services().board();
        const board::object *o = doc.find(doc.selected());
        if (o == nullptr)
        {
            say("Nothing selected to frame: Ctrl+0 frames everything");
            return;
        }
        frame(o->bounds);
    }

    void board_editor::set_orthographic(bool orthographic)
    {
        m_orthographic = orthographic;
        if (m_projection_tool != nullptr)
            m_projection_tool->set_checked(orthographic);
        if (m_projection_choice != nullptr)
            m_projection_choice->set_selected(orthographic ? 1 : 0);
        sync_camera();
    }

    void board_editor::set_fov(float radians)
    {
        m_controller.fov_y = std::clamp(radians, 0.1f, 2.0f);
        sync_camera();
    }

    void board_editor::apply_background()
    {
        board_document &doc = services().board();
        doc.scene.background = m_background == 1 ? rgb(services().theme().background) : m_board_background;
    }

    std::string board_editor::camera_text() const
    {
        return std::format("{} \xC2\xB7 {:.0f} mm away", m_orthographic ? "Parallel" : "Perspective",
                           m_controller.distance);
    }

    // ---- objects and selection ------------------------------------------------------------------

    void board_editor::objects_changed()
    {
        refresh_objects();
        refresh_layers();
        update_properties();
        update_marks();
    }

    void board_editor::selection_changed()
    {
        sync_selection_widgets();
        m_properties_dirty = true;
        if (m_properties != nullptr)
            m_properties->invalidate();
        update_marks();
    }

    void board_editor::sync_selection_widgets()
    {
        const board_document &doc = services().board();
        const board::object_id id = doc.selected();
        if (m_objects != nullptr)
        {
            int row = -1;
            for (std::size_t i = 0; i < m_entries.size(); ++i)
            {
                if (id != 0 && m_entries[i].object == id)
                    row = static_cast<int>(i);
            }
            m_objects->set_selected(row);
        }

        const bool any = id != 0;
        if (m_frame_selection_tool != nullptr)
            m_frame_selection_tool->set_enabled(any);
        if (m_hide_tool != nullptr)
            m_hide_tool->set_enabled(any);
        if (m_remove_tool != nullptr)
            m_remove_tool->set_enabled(doc.is_dropped(id));
    }

    void board_editor::refresh_objects()
    {
        if (m_objects == nullptr)
            return;
        const board_document &doc = services().board();

        std::vector<toggle_row> rows;
        m_entries.clear();
        for (const board::object_kind k : kinds)
        {
            const bool shown = doc.shown(k);
            const std::size_t count = doc.scene.count(k);
            toggle_row heading;
            heading.text = group_name(k);
            heading.detail = std::format("{}", count);
            heading.icon = kind_icon(k);
            heading.header = true;
            heading.on = shown;
            rows.push_back(std::move(heading));
            m_entries.push_back({0, static_cast<int>(k)});

            for (const board::object &o : doc.objects())
            {
                if (o.kind != k)
                    continue;
                toggle_row r;
                r.text = o.name;
                if (k == board::object_kind::board)
                {
                    const math::vec3f s = o.bounds.size();
                    r.detail = std::format("{:.0f} \xC3\x97 {:.0f} mm", s[0], s[1]);
                }
                else
                {
                    r.detail = std::format("{:.1f}, {:.1f}", o.position[0], o.position[1]);
                }
                r.on = o.visible;
                r.dimmed = !shown;
                rows.push_back(std::move(r));
                m_entries.push_back({o.id, -1});
            }

            if (k == board::object_kind::dropped_model && count == 0)
            {
                toggle_row note;
                note.text = "Drop STEP, OBJ or STL files on the view";
                note.switchable = false;
                note.selectable = false;
                rows.push_back(std::move(note));
                m_entries.push_back({0, -1});
            }
        }
        m_objects->set_rows(std::move(rows));
        sync_selection_widgets();
    }

    void board_editor::refresh_layers()
    {
        if (m_layers == nullptr)
            return;
        const board_document &doc = services().board();
        std::vector<toggle_row> rows;
        for (const board::object_kind k : kinds)
        {
            toggle_row r;
            r.text = group_name(k);
            r.detail = std::format("{}", doc.scene.count(k));
            r.icon = kind_icon(k);
            r.on = doc.shown(k);
            r.selectable = false;
            rows.push_back(std::move(r));
        }
        m_layers->set_rows(std::move(rows));
    }

    void board_editor::refresh_properties()
    {
        m_properties_dirty = false;
        if (m_properties == nullptr)
            return;

        m_x_field = m_y_field = nullptr;
        m_x_label = m_y_label = nullptr;
        m_extent_label[0] = m_extent_label[1] = m_extent_label[2] = nullptr;
        m_visible_box = nullptr;
        m_remove_button = nullptr;

        board_document &doc = services().board();
        const board::object *o = doc.find(doc.selected());
        m_properties_for = o != nullptr ? o->id : 0;

        auto content = std::make_unique<column>(6.0f, insets{0.0f, 0.0f, 0.0f, 8.0f});
        auto grid = std::make_unique<property_grid>();
        grid->set_name("property grid");
        const auto text = [](std::string value) {
            auto l = std::make_unique<label>(std::move(value), text_role::dim);
            return l;
        };

        if (o == nullptr)
        {
            // Nothing selected: the board as a whole.
            grid->add_section("Board");
            model::bounds board_bounds;
            for (const board::object &b : doc.objects())
            {
                if (b.kind == board::object_kind::board)
                    board_bounds.add(b.bounds);
            }
            if (!board_bounds.empty())
            {
                const math::vec3f s = board_bounds.size();
                grid->add_row("Size", text(std::format("{:.1f} \xC3\x97 {:.1f} mm", s[0], s[1])));
                grid->add_row("Thickness", text(mm(s[2])));
            }
            grid->add_row("Objects", text(count_text(doc.objects().size(), "object", "objects")));
            grid->add_row("Dropped", text(count_text(doc.models().size(), "model", "models")));
            content->add(std::move(grid));

            auto &hint = content->emplace<label>(
                "Click something in the view or in Objects to see its properties. Double-click a row to frame "
                "it; an eye shows or hides it. Drag a dropped model in the view to move it.",
                text_role::dim);
            hint.set_size(text_size::small);
            hint.set_wrap(true, 4);
            content->set_padding(insets{8.0f, 0.0f, 8.0f, 8.0f});
            m_properties->set_content(std::move(content));
            return;
        }

        const bool dropped = doc.is_dropped(o->id);
        grid->add_section(board::kind_name(o->kind));
        grid->add_row("Name", text(o->name));
        if (!o->file.empty())
            grid->add_row("File", text(o->file));
        else if (o->kind == board::object_kind::part_model)
            grid->add_row("File", text("Demo board part"));
        grid->add_row("Triangles", text(std::format("{}", o->triangles)));

        grid->add_section("Placement");
        if (dropped)
        {
            const board::object_id id = o->id;
            const auto field = [this, id](float value, int axis) {
                auto f = std::make_unique<numeric_field>(value, -1000.0, 1000.0, 0.5);
                f->set_decimals(2);
                f->set_name(axis == 0 ? "x position" : "y position");
                f->value_changed.connect([this, id, axis](double v) {
                    board_document &d = services().board();
                    const board::object *now = d.find(id);
                    if (now == nullptr)
                        return;
                    const float by = static_cast<float>(v) - now->position[static_cast<std::size_t>(axis)];
                    d.move(id, axis == 0 ? math::vec2f{by, 0.0f} : math::vec2f{0.0f, by});
                });
                return f;
            };
            m_x_field = &grid->add_row("X (mm)", field(o->position[0], 0));
            m_y_field = &grid->add_row("Y (mm)", field(o->position[1], 1));
        }
        else
        {
            m_x_label = &grid->add_row("X", text(mm(o->position[0])));
            m_y_label = &grid->add_row("Y", text(mm(o->position[1])));
        }
        constexpr float degrees = 180.0f / std::numbers::pi_v<float>;
        grid->add_row("Rotation", text(std::format("{:.1f}\xC2\xB0", o->rotation * degrees)));

        grid->add_section("Bounds");
        m_extent_label[0] = &grid->add_row("X", text(""));
        m_extent_label[1] = &grid->add_row("Y", text(""));
        m_extent_label[2] = &grid->add_row("Z", text(""));

        grid->add_section("Display");
        {
            auto visible = std::make_unique<checkbox>("Visible", o->visible);
            visible->set_name("visible checkbox");
            visible->layout().cross = align::start;
            const board::object_id id = o->id;
            visible->changed.connect([this, id](bool on) { services().board().set_visible(id, on); });
            m_visible_box = &grid->add_row("Shown", std::move(visible));
        }
        content->add(std::move(grid));

        // What can be done with it.
        auto &actions = content->emplace<row>(8.0f, insets{8.0f, 0.0f, 8.0f, 0.0f});
        auto &frame_button = actions.emplace<button>("Frame", icon::target);
        frame_button.set_name("frame selection");
        frame_button.set_tooltip("Frame the selection (F)");
        frame_button.clicked.connect([this] { frame_selection(); });
        auto &remove = actions.emplace<button>("Remove", icon::close);
        remove.set_name("remove selection");
        remove.set_enabled(dropped);
        remove.set_tooltip(dropped ? "Take this model off the board (Delete)"
                                   : "Only dropped models can be removed: this is part of the demo board");
        remove.clicked.connect([this] { remove_selected(); });
        m_remove_button = &remove;

        m_properties->set_content(std::move(content));
        update_properties();
    }

    void board_editor::update_properties()
    {
        const board_document &doc = services().board();
        const board::object *o = doc.find(m_properties_for);
        if (m_properties_for != 0 && o == nullptr)
        {
            // It is gone.
            m_properties_dirty = true;
            if (m_properties != nullptr)
                m_properties->invalidate();
            return;
        }
        if (o == nullptr)
        {
            // The board's summary counts objects.
            if (m_properties != nullptr && doc.selected() == 0)
            {
                m_properties_dirty = true;
                m_properties->invalidate();
            }
            return;
        }

        if (m_x_field != nullptr && !m_x_field->has_focus())
            m_x_field->set_value(static_cast<double>(o->position[0]));
        if (m_y_field != nullptr && !m_y_field->has_focus())
            m_y_field->set_value(static_cast<double>(o->position[1]));
        if (m_x_label != nullptr)
            m_x_label->set_text(mm(o->position[0]));
        if (m_y_label != nullptr)
            m_y_label->set_text(mm(o->position[1]));
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            if (m_extent_label[axis] != nullptr)
                m_extent_label[axis]->set_text(std::format("{:.2f} to {:.2f} mm ({:.2f})", o->bounds.min[axis],
                                                           o->bounds.max[axis],
                                                           o->bounds.max[axis] - o->bounds.min[axis]));
        }
        if (m_visible_box != nullptr)
            m_visible_box->set_checked(o->visible);
    }

    void board_editor::update_marks()
    {
        board_document &doc = services().board();
        board::scene &scene = doc.scene;
        scene.marks.clear();

        // About a pixel and a half at the target's distance, at the
        // window's scale.
        const math::vec2i pixels = m_view != nullptr ? m_view->pixel_size() : math::vec2i{};
        const float height = pixels.y() > 0 ? static_cast<float>(pixels.y()) : 800.0f;
        const float per_pixel = scene.view.ortho_height() / height;
        const float width = std::max(per_pixel * 2.0f, 0.005f);

        if (const board::object *o = doc.find(doc.selected()); o != nullptr && scene.drawn(o->id))
        {
            model::bounds around = o->bounds;
            const math::vec3f pad{width, width, width};
            around.min -= pad;
            around.max += pad;
            board::outline(scene.marks, around, width, m_accent);
        }

        if (m_measure_from)
        {
            const float lift = width; // just above the board's top
            const math::vec2f a = *m_measure_from;
            const math::vec2f b = m_measure_to.value_or(a);
            const float dot = width * 4.0f;
            for (const math::vec2f &p : {a, b})
                scene.marks.push_back({{p.x(), p.y(), lift}, {dot, dot, dot}, 0.0f, m_accent});
            if (m_measure_to)
                board::bar(scene.marks, a, b, lift, width * 1.5f, m_accent);
        }
    }

    void board_editor::toggle_object(int row, bool on)
    {
        if (row < 0 || static_cast<std::size_t>(row) >= m_entries.size())
            return;
        const list_entry &e = m_entries[static_cast<std::size_t>(row)];
        board_document &doc = services().board();
        if (e.kind >= 0)
            doc.set_shown(static_cast<board::object_kind>(e.kind), on);
        else if (e.object != 0)
            doc.set_visible(e.object, on);
    }

    void board_editor::toggle_layer(int row, bool on)
    {
        if (row >= 0 && static_cast<std::size_t>(row) < std::size(kinds))
            services().board().set_shown(kinds[static_cast<std::size_t>(row)], on);
    }

    void board_editor::hide_selected()
    {
        board_document &doc = services().board();
        const board::object *o = doc.find(doc.selected());
        if (o == nullptr)
        {
            say("Nothing selected to hide");
            return;
        }
        const std::string name = o->name;
        doc.set_visible(o->id, false);
        say(name + " hidden: Shift+H shows everything again");
    }

    void board_editor::show_all()
    {
        board_document &doc = services().board();
        std::vector<board::object_id> hidden;
        for (const board::object &o : doc.objects())
        {
            if (!o.visible)
                hidden.push_back(o.id);
        }
        for (const board::object_id id : hidden)
            doc.set_visible(id, true);
        for (const board::object_kind k : kinds)
            doc.set_shown(k, true);
    }

    void board_editor::remove_selected()
    {
        board_document &doc = services().board();
        const board::object *o = doc.find(doc.selected());
        if (o == nullptr)
        {
            say("Nothing selected to remove");
            return;
        }
        if (!doc.is_dropped(o->id))
        {
            say(o->name + " is part of the demo board and stays; only dropped models can be removed");
            return;
        }
        const std::string name = o->name;
        if (doc.remove(o->id))
            say("Removed " + name);
    }

    void board_editor::remove_models()
    {
        const std::size_t count = services().board().remove_models();
        if (count == 0)
            say("No dropped models to remove");
        else
            say(std::format("Removed {}", count_text(count, "model", "models")));
    }

    // ---- tools and the view ------------------------------------------------------------------------

    void board_editor::set_tool(tool t)
    {
        m_tool = t;
        if (m_tools != nullptr)
            m_tools->select(static_cast<int>(t));
        if (t != tool::measure && m_measure_from)
        {
            m_measure_from.reset();
            m_measure_to.reset();
            update_marks();
            services().board().mark_dirty();
        }

        const tool_info &info = tools[static_cast<int>(t)];
        if (!info.works)
            say(std::format("{} is not implemented yet: the view still orbits, pans and selects nothing", info.name));
        else if (t == tool::measure)
            say("Measure: click two points on the board's top");
        if (m_tool_status != nullptr)
            m_tool_status->invalidate();
    }

    void board_editor::set_dock_shown(bool shown)
    {
        m_dock_shown = shown;
        update_layout_for_width();
    }

    void board_editor::escape()
    {
        // Out of the tool and out of the selection at once: one press of
        // Esc always ends in the same place, whatever came before.
        if (m_tool != tool::select)
            set_tool(tool::select);
        services().board().select(0);
    }

    void board_editor::view_clicked(const math::vec2f &pixel, const math::vec2f &size)
    {
        board_document &doc = services().board();
        switch (m_tool)
        {
        case tool::select:
        {
            const auto r = board::view_ray(doc.scene.view, pixel, size);
            doc.select(r ? board::pick(doc.scene, *r) : 0);
            return;
        }
        case tool::measure:
        {
            const auto at = board::point_on_board(doc.scene.view, pixel, size);
            if (!at)
            {
                say("Measure on the board: that point is off its plane");
                return;
            }
            if (!m_measure_from || m_measure_to)
            {
                m_measure_from = at;
                m_measure_to.reset();
                say(std::format("From {:.2f}, {:.2f} mm: click the second point", at->x(), at->y()));
            }
            else
            {
                m_measure_to = at;
                const math::vec2f d = *at - *m_measure_from;
                say(std::format("{:.2f} mm  (\xCE\x94 {:.2f}, {:.2f})",
                                std::sqrt(d.x() * d.x() + d.y() * d.y()), d.x(), d.y()));
            }
            update_marks();
            doc.mark_dirty();
            return;
        }
        case tool::route:
        case tool::via:
        case tool::zone:
            say(std::format("{} is not implemented yet: there is no board document to edit",
                            tools[static_cast<int>(m_tool)].name));
            return;
        }
    }

    bool board_editor::can_grab(const math::vec2f &pixel, const math::vec2f &size)
    {
        m_grab_candidate = 0;
        if (m_tool != tool::select)
            return false;
        board_document &doc = services().board();
        const auto r = board::view_ray(doc.scene.view, pixel, size);
        const board::object_id id = r ? board::pick(doc.scene, *r) : 0;
        const auto at = board::point_on_board(doc.scene.view, pixel, size);
        if (!doc.is_dropped(id) || !at)
            return false;
        m_grab_candidate = id;
        m_grab_start = *at;
        m_grab_moved = {0.0f, 0.0f};
        return true;
    }

    void board_editor::grab_moved(const math::vec2f &to, const math::vec2f &size)
    {
        board_document &doc = services().board();
        if (m_grabbed == 0)
        {
            if (m_grab_candidate == 0)
                return;
            m_grabbed = m_grab_candidate;
            doc.select(m_grabbed);
        }
        const auto at = board::point_on_board(doc.scene.view, to, size);
        if (!at)
            return; // above the horizon: it stays where it last was
        const math::vec2f offset = *at - m_grab_start;
        if (doc.move(m_grabbed, offset - m_grab_moved))
            m_grab_moved = offset;
    }

    void board_editor::grab_released(bool cancelled)
    {
        board_document &doc = services().board();
        if (m_grabbed != 0)
        {
            const board::object *o = doc.find(m_grabbed);
            if (cancelled)
            {
                doc.move(m_grabbed, math::vec2f{0.0f, 0.0f} - m_grab_moved);
                say("Move cancelled");
            }
            else if (o != nullptr)
            {
                say(std::format("Moved {} to {:.2f}, {:.2f} mm", o->name, o->position[0], o->position[1]));
            }
        }
        m_grabbed = 0;
        m_grab_candidate = 0;
        m_grab_moved = {0.0f, 0.0f};
    }

    void board_editor::update_pointer_status()
    {
        if (m_camera_status == nullptr)
            return;
        if (m_view != nullptr && m_view->has_pointer())
        {
            const auto at = board::point_on_board(services().board().scene.view, m_view->pointer_pixels(),
                                                  m_view->pixel_size().cast<float>());
            if (at)
            {
                m_camera_status->set_text(std::format("X {:.2f}   Y {:.2f} mm", at->x(), at->y()));
                return;
            }
        }
        m_camera_status->set_text(camera_text());
    }

    void board_editor::update_layout_for_width()
    {
        if (host() == nullptr || host()->screen().root() == nullptr)
            return;
        // The screen's size, which the frame has set already, not the
        // root's bounds, which are the last layout's: deciding by those
        // left a window that had just been made narrow with the wide
        // layout, and nothing asked for the frame that would correct it.
        const float width = host()->screen().size().x();
        if (width <= 0.0f)
            return;

        const auto show = [](widget *w, bool shown) {
            if (w != nullptr && w->visible() != shown)
                w->set_visible(shown);
        };
        for (widget *w : m_narrow_hidden)
            show(w, width >= narrow_toolbar);

        // The dock is collapsed when the user hid it or there is no room:
        // gone with its divider, so that nothing drags a pane that is not
        // there, and back at the width it had.
        if (m_split != nullptr)
        {
            const bool docked = m_dock_shown && width >= narrow_dock;
            m_split->set_collapsed(docked ? splitter::pane::none : splitter::pane::second);
        }
        const bool selection = width >= narrow_selection_status;
        const bool count = width >= narrow_count_status;
        const bool camera = width >= narrow_camera_status;
        show(m_selection_status, selection);
        show(m_status_separators[0], selection);
        show(m_count_status, count);
        show(m_status_separators[1], count);
        show(m_camera_status, camera);
        show(m_status_separators[2], camera);
    }

    void board_editor::say(std::string text)
    {
        if (host() != nullptr)
            host()->show_message(std::move(text));
    }

    std::optional<math::vec2f> board_editor::board_point(const math::vec2i &window_pixels) const
    {
        if (m_view == nullptr || host() == nullptr)
            return std::nullopt;

        const math::vec2f logical = window_pixels.cast<float>() / host()->scale();
        if (!m_view->visible() || !m_view->bounds().contains(logical))
            return std::nullopt;
        return board::point_on_board(services().board().scene.view, m_view->to_pixels(logical),
                                     m_view->pixel_size().cast<float>());
    }

} // namespace volt::ui
