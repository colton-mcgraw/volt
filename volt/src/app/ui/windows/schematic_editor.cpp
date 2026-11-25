#include <app/ui/windows/schematic_editor.hpp>

#include <app/ui/windows/menus.hpp>
#include <app/ui/windows/part_match.hpp>
#include <app/ui/windows/parts_panel.hpp>
#include <library/project.hpp>
#include <schematic/hit.hpp>
#include <schematic/io.hpp>
#include <schematic/symbol.hpp>
#include <ui/layout/box.hpp>
#include <ui/theme.hpp>
#include <ui/widgets/bars.hpp>
#include <ui/widgets/button.hpp>
#include <ui/widgets/checkbox.hpp>
#include <ui/widgets/label.hpp>
#include <ui/widgets/numeric_field.hpp>
#include <ui/widgets/panel.hpp>
#include <ui/widgets/property_grid.hpp>
#include <ui/widgets/radio_group.hpp>
#include <ui/widgets/scroll_area.hpp>
#include <ui/widgets/slider.hpp>
#include <ui/widgets/splitter.hpp>
#include <ui/widgets/tab_view.hpp>
#include <ui/widgets/tool_strip.hpp>
#include <ui/widgets/viewport.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <string_view>

namespace volt::ui
{
    namespace
    {
        using tool = schematic_editor::tool;
        using page = schematic_editor::page;

        // The editing tools: the strip, the Place menu, the shortcuts and the
        // status bar are made from this.
        struct tool_info
        {
            tool id;
            ui::icon icon;
            const char *name;
            const char *key;
            const char *command;
        };

        constexpr tool_info tools[] = {
            {tool::select, icon::pointer, "Select", "Esc", "tool.select"},
            {tool::wire, icon::wire, "Wire", "W", "tool.wire"},
            {tool::label, icon::tag, "Net Label", "L", "tool.label"},
            {tool::power, icon::power, "Power Port", "P", "tool.power"},
            {tool::part, icon::component, "Part", "A", "tool.part"},
            {tool::text, icon::text, "Text", "T", "tool.text"},
        };

        const tool_info &info(tool t)
        {
            return tools[static_cast<std::size_t>(t)];
        }

        constexpr const char *page_names[] = {"Properties", "Parts", "Simulation"};

        // The grid spacings the View menu offers; the field takes any.
        constexpr double grid_choices[] = {0.25, 0.5, 1.0, 2.0};

        // How long the sheet must stay as it is before it runs by itself.
        constexpr auto settle_time = std::chrono::milliseconds(300);

        // A read-only value in a property grid.
        std::unique_ptr<label> value(std::string text, text_role role = text_role::normal)
        {
            auto shown = std::make_unique<label>(std::move(text), role);
            shown->set_wrap(true, 2);
            return shown;
        }

        // "3 resistors", "1 resistor".
        std::string count_of(std::size_t n, const char *one, const char *many)
        {
            return std::format("{} {}", n, n == 1 ? one : many);
        }

        // "Error", "Warning", "Info".
        std::string severity_name(schematic::severity level)
        {
            std::string name(schematic::to_string(level));
            if (!name.empty() && name[0] >= 'a' && name[0] <= 'z')
                name[0] = static_cast<char>(name[0] - 'a' + 'A');
            return name;
        }

        // The tools' snap step from the grid's spacing in world units.
        int grid_step(float spacing)
        {
            return std::max(1, static_cast<int>(std::lround(spacing * schematic::units_per_grid)));
        }

        // A problem in the simulation page's list: clicking it selects its
        // items. Its labels let the pointer through to it.
        class problem_row final : public column
        {
        public:
            explicit problem_row(std::function<void()> chosen)
                : column(2.0f, insets::symmetric(4.0f, 3.0f)), m_chosen(std::move(chosen))
            {
                set_hit_testable(true);
            }

            cursor_shape cursor(const math::vec2f &) const override
            {
                return m_chosen ? cursor_shape::hand : cursor_shape::arrow;
            }

        protected:
            bool on_pointer_down(const pointer_event &e) override { return e.button == mouse_button::left; }
            bool on_click(const pointer_event &e) override
            {
                if (e.button != mouse_button::left || !m_chosen)
                    return false;
                m_chosen();
                return true;
            }

        private:
            std::function<void()> m_chosen;
        };
    } // namespace

    schematic_editor::schematic_editor(app_services &services)
        : window_content(services),
          m_properties(m_tools, selection_properties::hooks{
                                    .netlist = [this]() -> const schematic::netlist * {
                                        return &this->services().schematic().netlist();
                                    },
                                    .reveal = [this](const std::string &id) {
                                        this->services().reveal_in_library(id, true);
                                    },
                                    .can_reveal = [this] { return this->services().project() != nullptr; },
                                    .edited = [this] { tools_changed(); },
                                    .return_focus = [this] { return_to_canvas(); },
                                    .renamed =
                                        [this](schematic::label_kind kind, const std::string &text) {
                                            // Only while that kind is being placed.
                                            if (m_tools.active_tool() == tool::label &&
                                                kind == schematic::label_kind::net)
                                                m_tools.label_renamed(kind, text);
                                        },
                                    .assign = [this] { assign_part(); },
                                    .part_name = [this](const std::string &id) -> std::string {
                                        const library::project *project = this->services().project();
                                        const library::part *p = project != nullptr ? project->parts.find(id)
                                                                                    : nullptr;
                                        return p != nullptr ? p->name : std::string();
                                    },
                                }),
          m_tool_options(m_tools, tool_options::hooks{
                                      .edited = [this] { tools_changed(); },
                                      .return_focus = [this] { return_to_canvas(); },
                                      .choose_part = [this] { set_tool(tool::part); },
                                  })
    {
        // The camera starts where the scene does, and fits the sheet once
        // the view has a size.
        schematic_document &doc = services.schematic();
        m_controller.center = doc.scene.view.center;
        m_controller.zoom = doc.scene.view.zoom;

        // The tools edit the active sheet, and follow it for as long as
        // this object lives, window or not.
        m_tools.set_document(&doc.active());
        m_tools.set_grid(grid_step(doc.grid));
        m_tools.set_style(doc.style());
        // What connects is the project's: the tools move parts and cut
        // wires by its rules.
        m_tools.set_connectivity(doc.connectivity());
        m_tools.edit_requested = [this](schematic::item_ref item) { edit_item(item); };
        m_tools.placed = [this](schematic::item_ref item) {
            // A net label or a note is placed to be named: its text, ready
            // to type over. A power port has its name from the menu, and a
            // part is placed several at a time.
            if (item.kind != schematic::item_kind::label)
                return;
            const schematic::label *l = this->services().schematic().active().find_label(item.id);
            if (l != nullptr && l->kind != schematic::label_kind::power)
                edit_item(item);
        };
        m_view_controller.tools_changed = [this] { tools_changed(); };
        m_sheets_connection = doc.sheets_changed.connect([this] { sheets_changed(); });
        m_connectivity_connection = doc.connectivity_changed.connect([this] { connectivity_changed(); });
    }

    schematic_editor::~schematic_editor()
    {
        services().schematic().sheets_changed.disconnect(m_sheets_connection);
        services().schematic().connectivity_changed.disconnect(m_connectivity_connection);
        // A drag's transaction is not left open in a sheet that stays.
        m_tools.set_document(nullptr);
    }

    window_layout schematic_editor::build(window_host &host)
    {
        add_commands(host);
        m_changed_connection = services().schematic().changed.connect([this] { scene_changed(); });
        services().schematic().apply_theme(services().theme());
        m_tools.set_style(services().schematic().style());
        m_view_controller.pixel_scale = host.scale();
        m_pushed.reset();

        window_layout layout;
        // The project's sheets change here, and its parts (the Parts tab
        // adds to its library), so it can be saved from here too.
        menu_item save = command_item("Save Project", "file.save", icon::save);
        save.enabled = [this] { return services().project() != nullptr; };
        layout.file_items = {save};
        layout.menus.push_back(edit_menu());
        layout.menus.push_back(view_menu());
        layout.menus.push_back(place_menu());
        layout.menus.push_back(simulate_menu());

        // toolbar / [tools | sheets over canvas | dock]
        auto body = std::make_unique<column>(0.0f);
        body->add(build_toolbar(host));

        auto main = std::make_unique<row>(0.0f);
        main->layout().flex = 1.0f;
        main->add(build_tools());

        auto split = std::make_unique<splitter>(orientation::horizontal);
        split->set_name("schematic splitter");
        split->set_fixed(splitter::keep::second, m_dock_width);
        split->set_minimums(240.0f, 250.0f);
        split->layout().flex = 1.0f;
        split->set_first(build_canvas(host));
        split->set_second(build_dock(host));
        if (!m_dock_shown)
            split->set_collapsed(splitter::pane::second);
        m_split = split.get();
        main->add(std::move(split));
        body->add(std::move(main));
        layout.body = std::move(body);

        layout.status = [this](status_bar &bar) {
            m_cursor_status = &bar.add_segment("", 118.0f);
            m_cursor_status->set_name("cursor status");
            bar.add_separator();
            m_grid_status = &bar.add_segment("", 66.0f);
            m_grid_status->set_name("grid status");
            bar.add_separator();
            m_zoom_status = &bar.add_segment("", 84.0f);
            m_zoom_status->set_name("zoom status");
            bar.add_separator();
            m_tool_status = &bar.add_segment("", 96.0f);
            m_tool_status->set_name("tool status");
            bar.add_separator();
            m_simulation_status = &bar.add_segment("", 128.0f);
            m_simulation_status->set_name("simulation status");
            update_cursor_status();
            m_pushed.reset();
            tools_changed();
        };

        // A sheet edited while the window was closed runs when it opens.
        m_seen_key = services().schematic().active_key();
        schedule_simulation();
        return layout;
    }

    void schematic_editor::detach()
    {
        // What the user was in the middle of ends with the window: a drag
        // or a rubber band is cancelled, a wire being drawn keeps what was
        // fixed, a paste is dropped. The tools stay with the sheet.
        for (int i = 0; i < 3 && (m_tools.in_gesture() || m_tools.pasting()); ++i)
            (void)m_tools.cancel();
        (void)m_tools.pointer_leave();
        services().schematic().set_overlay({});

        if (m_dock != nullptr && m_dock->bounds().width > 0.0f)
            m_dock_width = m_dock->bounds().width;
        if (m_dock_tabs != nullptr)
            m_dock_page = m_dock_tabs->selected();
        services().schematic().changed.disconnect(m_changed_connection);
        m_changed_connection = 0;
        m_problems_key.reset();
        m_properties.detach();
        m_tool_options.detach();
        m_run_timer = 0; // the screen's timers go with it
        m_pushed.reset();

        m_split = nullptr;
        m_dock = nullptr;
        m_dock_tabs = nullptr;
        m_parts_panel = nullptr;
        m_tool_strip = nullptr;
        m_sheet_holder = nullptr;
        m_sheet_strip = nullptr;
        m_sheet_note = nullptr;
        m_view = nullptr;
        m_hint = nullptr;
        m_auto_box = nullptr;
        m_undo_tool = nullptr;
        m_redo_tool = nullptr;
        m_sheet_name = nullptr;
        m_sheet_file = nullptr;
        m_sheet_parts = nullptr;
        m_sheet_wires = nullptr;
        m_sheet_labels = nullptr;
        m_sheet_nets = nullptr;
        m_sheet_unassigned = nullptr;
        m_select_unassigned = nullptr;
        m_connect_box = nullptr;
        m_circuit_nodes = nullptr;
        m_circuit_branches = nullptr;
        m_circuit_summary = nullptr;
        m_problems = nullptr;
        m_grid_box = nullptr;
        m_grid_tool = nullptr;
        m_potentials_tool = nullptr;
        m_grid_field = nullptr;
        m_colours = nullptr;
        m_run_state = nullptr;
        m_cursor_status = nullptr;
        m_grid_status = nullptr;
        m_zoom_status = nullptr;
        m_tool_status = nullptr;
        m_simulation_status = nullptr;
    }

    viewport *schematic_editor::view(offscreen_view view) noexcept
    {
        return view == offscreen_view::schematic ? m_view : nullptr;
    }

    void schematic_editor::before_frame()
    {
        // Whatever changed the tools or the sheet since the last event
        // (an edit in the properties, an undo from the menu) reaches the
        // scene before it is drawn.
        tools_changed();
        m_tool_options.update();
        m_properties.update();

        const schematic_document &doc = services().schematic();
        const schematic_document::run_state state = doc.simulation_state();
        const bool nothing = doc.circuit().network.nodes == 0;
        const bool stale = state == schematic_document::run_state::edited ||
                           state == schematic_document::run_state::other_sheet;

        // Setting the same text changes nothing, so these may run every
        // frame.
        if (m_simulation_status != nullptr)
        {
            std::string text;
            if (doc.simulation_busy && state == schematic_document::run_state::current)
                text = "Simulation running";
            else if (state == schematic_document::run_state::current)
                text = nothing ? "Nothing to simulate" : "Simulation settled";
            else if (state == schematic_document::run_state::edited)
                text = m_auto_run ? "Sheet changed" : "Sheet changed (F5)";
            else if (state == schematic_document::run_state::other_sheet)
                text = "Not this sheet (F5)";
            else
                text = "Not simulated";
            m_simulation_status->set_text(std::move(text));
        }
        if (m_run_state != nullptr)
        {
            std::string text;
            switch (state)
            {
            case schematic_document::run_state::current:
                text = doc.simulation_busy ? std::string("Running\xE2\x80\xA6")
                       : nothing           ? std::string("The sheet has nothing to simulate.")
                                           : std::string("Settled: the wires show the potentials of this sheet.");
                break;
            case schematic_document::run_state::edited:
                text = m_auto_run ? "The sheet changed: it runs again once the editing settles."
                                  : "The sheet changed since the last run, so the wires are drawn plain: Run "
                                    "simulation (F5) brings the potentials up to date.";
                break;
            case schematic_document::run_state::other_sheet:
                text = "The last run was of another sheet: Run simulation (F5) simulates this one.";
                break;
            case schematic_document::run_state::not_run:
                text = "Not run yet.";
                break;
            }
            m_run_state->set_text(std::move(text));
            m_run_state->set_role(stale ? text_role::accent : text_role::dim);
        }
        if (m_grid_status != nullptr)
            m_grid_status->set_text(doc.show_grid ? std::format("Grid {:g}", doc.grid) : std::string("Grid off"));

        update_sheet_info();
        update_problems();
    }

    // ---- menus --------------------------------------------------------------------------

    window_layout::menu schematic_editor::edit_menu()
    {
        // Enabled, and named, after the shown sheet's history and the
        // selection as the menu opens.
        menu_item undo = command_item("Undo", "edit.undo", icon::undo);
        undo.enabled = [this] { return m_tools.can_undo(); };
        undo.text_of = [this] {
            return m_tools.can_undo() ? std::format("Undo {}", m_tools.undo_label()) : std::string("Undo");
        };
        menu_item redo = command_item("Redo", "edit.redo", icon::redo);
        redo.enabled = [this] { return m_tools.can_redo(); };
        redo.text_of = [this] {
            return m_tools.can_redo() ? std::format("Redo {}", m_tools.redo_label()) : std::string("Redo");
        };

        const auto selected = [this] { return has_selection(); };
        const auto with_selection = [&](std::string text, std::string command) {
            menu_item item = command_item(std::move(text), std::move(command));
            item.enabled = selected;
            return item;
        };
        // Turning and mirroring also act on what is being placed.
        const auto turning = [this] { return has_selection() || m_tools.placing(); };
        const auto with_turning = [&](std::string text, std::string command) {
            menu_item item = command_item(std::move(text), std::move(command));
            item.enabled = turning;
            return item;
        };
        menu_item paste = command_item("Paste", "edit.paste");
        paste.enabled = [this] {
            const std::string text = services().system_clipboard().text();
            return m_tools.can_paste() || text.find(schematic::sheet_format) != std::string::npos;
        };

        // Assign Part… for placeholders; Change Part… for one assigned part.
        menu_item assign = action_item("Assign Part\xE2\x80\xA6", [this] { assign_part(); });
        assign.enabled = [this] { return selection_assignment().has_value(); };
        assign.text_of = [this] {
            const std::optional<part_assignment> what = selection_assignment();
            const schematic::document *d = m_tools.document();
            const schematic::part *p = what && d != nullptr && !what->items.empty() ? d->find_part(what->items[0])
                                                                                   : nullptr;
            return p != nullptr && !is_placeholder(*p) ? std::string("Change Part\xE2\x80\xA6")
                                                       : std::string("Assign Part\xE2\x80\xA6");
        };

        // The project's rule of connection (kept in the project file).
        menu_item along = command_item("Pins Connect Along Wires", "schematic.pins_along_wires");
        along.checked = [this] { return services().schematic().connectivity().pins_inside_wires; };

        return {"Edit",
                {undo, redo, menu_item::separator(), with_selection("Cut", "edit.cut"),
                 with_selection("Copy", "edit.copy"), paste, with_selection("Duplicate", "edit.duplicate"),
                 with_selection("Delete", "edit.delete"), menu_item::separator(),
                 command_item("Select All", "edit.select_all"), menu_item::separator(),
                 with_turning("Rotate", "edit.rotate"), with_turning("Rotate Clockwise", "edit.rotate_cw"),
                 with_turning("Mirror Horizontally", "edit.mirror_x"),
                 with_turning("Mirror Vertically", "edit.mirror_y"), menu_item::separator(), assign,
                 menu_item::separator(), along, menu_item::separator(),
                 command_item("Select Tool", "tool.select", icon::pointer)}};
    }

    window_layout::menu schematic_editor::view_menu()
    {
        menu_item grid = command_item("Show Grid", "view.grid", icon::grid);
        grid.checked = [this] { return services().schematic().show_grid; };

        std::vector<menu_item> spacings;
        for (double spacing : grid_choices)
            spacings.push_back(action_item(
                std::format("{:g}", spacing), [this, spacing] { set_grid_spacing(spacing); },
                [this, spacing] { return std::abs(static_cast<double>(services().schematic().grid) - spacing) < 1.0e-6; }));

        std::vector<menu_item> items{grid,
                                     submenu_item("Grid Spacing", std::move(spacings)),
                                     menu_item::separator(),
                                     command_item("Zoom In", "view.zoom_in", icon::zoom_in),
                                     command_item("Zoom Out", "view.zoom_out", icon::zoom_out),
                                     command_item("Fit to View", "view.fit", icon::fit),
                                     menu_item::separator()};
        menu_item dock = command_item("Show Dock", "view.dock");
        dock.checked = [this] { return m_dock_shown; };
        items.push_back(std::move(dock));
        for (std::size_t i = 0; i < std::size(page_names); ++i)
        {
            menu_item panel = action_item(std::format("{} Panel", page_names[i]),
                                          [this, i] { show_page(static_cast<page>(i)); },
                                          [this, i] { return m_dock_tabs != nullptr && m_dock_tabs->selected() ==
                                                                                           static_cast<int>(i); });
            items.push_back(std::move(panel));
        }
        return {"View", std::move(items)};
    }

    window_layout::menu schematic_editor::place_menu()
    {
        const auto tool_item = [this](tool t, std::string text) {
            menu_item item = command_item(std::move(text), info(t).command, info(t).icon);
            item.checked = [this, t] { return m_tools.active_tool() == t; };
            return item;
        };

        std::vector<menu_item> power{tool_item(tool::power, "Power Port"), menu_item::separator()};
        for (const std::string_view name : standard_power_ports())
            power.push_back(action_item(std::string(name), [this, name] { place_power(std::string(name)); }));

        // Parts, generic ones too, come from the Parts tab (Part…).
        return {"Place",
                {tool_item(tool::wire, "Wire"), tool_item(tool::label, "Net Label"), tool_item(tool::text, "Text"),
                 menu_item::separator(), tool_item(tool::part, "Part\xE2\x80\xA6"),
                 submenu_item("Power Port", std::move(power))}};
    }

    window_layout::menu schematic_editor::simulate_menu()
    {
        menu_item potentials = command_item("Colour Wires by Potential", "simulate.potentials", icon::net);
        potentials.checked = [this] { return services().schematic().show_potentials; };
        menu_item automatic = command_item("Run Automatically", "simulate.auto");
        automatic.checked = [this] { return m_auto_run; };
        return {"Simulate",
                {command_item("Run Simulation", "simulate.run", icon::play), automatic, potentials,
                 menu_item::separator(),
                 action_item("Circuit and Problems\xE2\x80\xA6", [this] { show_page(page::simulation); })}};
    }

    // ---- building -----------------------------------------------------------------------

    void schematic_editor::add_commands(window_host &host)
    {
        ui::screen &s = host.screen();
        s.add_command("edit.undo", [this] { undo(); });
        s.add_command("edit.redo", [this] { redo(); });
        s.add_command("edit.cut", [this] { cut(); });
        s.add_command("edit.copy", [this] { copy(); });
        s.add_command("edit.paste", [this] { paste(); });
        s.add_command("edit.duplicate", [this] {
            const bool armed = m_tools.duplicate();
            after_command(armed);
        });
        s.add_command("edit.delete", [this] {
            (void)m_tools.delete_selection();
            after_command();
        });
        s.add_command("edit.select_all", [this] {
            m_tools.select_all();
            after_command();
        });
        s.add_command("edit.rotate", [this] {
            (void)m_tools.rotate_selection(false);
            after_command();
        });
        s.add_command("edit.rotate_cw", [this] {
            (void)m_tools.rotate_selection(true);
            after_command();
        });
        s.add_command("edit.mirror_x", [this] {
            (void)m_tools.mirror_selection(true);
            after_command();
        });
        s.add_command("edit.mirror_y", [this] {
            (void)m_tools.mirror_selection(false);
            after_command();
        });
        s.add_command("edit.cancel", [this] {
            (void)m_tools.cancel();
            after_command();
        });
        s.add_command("edit.wire_corner", [this] {
            (void)m_tools.toggle_wire_corner();
            after_command();
        });
        const std::pair<const char *, std::pair<int, int>> nudges[] = {
            {"edit.nudge_left", {-1, 0}}, {"edit.nudge_right", {1, 0}}, {"edit.nudge_up", {0, 1}},
            {"edit.nudge_down", {0, -1}}};
        for (const auto &[id, step] : nudges)
            s.add_command(id, [this, step] {
                (void)m_tools.nudge(step.first, step.second);
                after_command();
            });

        s.add_command("view.grid", [this] { set_grid_shown(!services().schematic().show_grid); });
        s.add_command("view.zoom_in", [this] { zoom_by(1.25f); });
        s.add_command("view.zoom_out", [this] { zoom_by(0.8f); });
        s.add_command("view.fit", [this] { fit_sheet(); });
        s.add_command("view.dock", [this] { set_dock_shown(!m_dock_shown); });
        s.add_command("simulate.run", [this] { run_simulation(true); });
        s.add_command("simulate.auto", [this] { set_auto_run(!m_auto_run); });
        s.add_command("file.save", [&host] {
            app_services &services = host.services();
            if (services.project() == nullptr)
            {
                host.show_message("No project is open: there is nothing to save (the demo sheet is never saved)");
                return;
            }
            if (auto saved = services.save_project(); !saved)
                host.show_message("Cannot save the project: " + saved.error(), std::chrono::seconds(10),
                                  log_level::error);
            else
                host.show_message("Saved " + services.project()->name);
        });
        s.add_command("simulate.potentials", [this] { set_potentials(!services().schematic().show_potentials); });
        for (const tool_info &t : tools)
            s.add_command(t.command, [this, id = t.id] { set_tool(id); });
        // P (again, too): the port last used, and the pick of ports.
        s.add_command("tool.power", [this] { power_tool(); });
        s.add_command("schematic.pins_along_wires", [this] {
            set_connectivity(!services().schematic().connectivity().pins_inside_wires);
        });

        // Single keys reach the canvas first while it has the focus (the
        // tools take them there); elsewhere they are these shortcuts,
        // which a text field being typed in never sees (it claims plain
        // keys).
        shortcut_table &keys = s.shortcuts();
        (void)keys.bind("Ctrl+Z", "edit.undo");
        (void)keys.bind("Ctrl+Y", "edit.redo");
        (void)keys.bind("Ctrl+Shift+Z", "edit.redo");
        (void)keys.bind("Ctrl+X", "edit.cut");
        (void)keys.bind("Ctrl+C", "edit.copy");
        (void)keys.bind("Ctrl+V", "edit.paste");
        (void)keys.bind("Ctrl+D", "edit.duplicate");
        (void)keys.bind("Delete", "edit.delete");
        (void)keys.bind("Ctrl+A", "edit.select_all");
        (void)keys.bind("R", "edit.rotate");
        (void)keys.bind("Shift+R", "edit.rotate_cw");
        (void)keys.bind("X", "edit.mirror_x");
        (void)keys.bind("Y", "edit.mirror_y");
        (void)keys.bind("Space", "edit.wire_corner");
        (void)keys.bind("Escape", "edit.cancel");
        (void)keys.bind("Left", "edit.nudge_left");
        (void)keys.bind("Right", "edit.nudge_right");
        (void)keys.bind("Up", "edit.nudge_up");
        (void)keys.bind("Down", "edit.nudge_down");
        (void)keys.bind("Ctrl+G", "view.grid");
        (void)keys.bind("Ctrl+=", "view.zoom_in");
        (void)keys.bind("Ctrl+-", "view.zoom_out");
        (void)keys.bind("Ctrl+0", "view.fit");
        (void)keys.bind("Ctrl+Shift+D", "view.dock");
        (void)keys.bind("F5", "simulate.run");
        (void)keys.bind("Ctrl+S", "file.save");
        for (const tool_info &t : tools)
        {
            if (t.id != tool::select)
                (void)keys.bind(t.key, t.command);
        }
    }

    std::unique_ptr<widget> schematic_editor::build_toolbar(window_host &host)
    {
        auto bar = std::make_unique<toolbar>();
        bar->set_name("toolbar");
        add_brand(*bar, host);

        // Enabled, and their tooltips naming the step, by before_frame().
        m_undo_tool = &add_command_tool(*bar, host, icon::undo, "Undo (Ctrl+Z)", "edit.undo");
        m_undo_tool->set_name("undo tool");
        m_redo_tool = &add_command_tool(*bar, host, icon::redo, "Redo (Ctrl+Y)", "edit.redo");
        m_redo_tool->set_name("redo tool");
        bar->add_separator();
        add_command_tool(*bar, host, icon::zoom_in, "Zoom in (Ctrl+=)", "view.zoom_in");
        add_command_tool(*bar, host, icon::zoom_out, "Zoom out (Ctrl+-)", "view.zoom_out");
        add_command_tool(*bar, host, icon::fit, "Fit to view (Ctrl+0)", "view.fit");

        button &grid = bar->add_tool(icon::grid, "Show grid (Ctrl+G)");
        grid.set_name("grid tool");
        grid.set_toggle(true);
        grid.set_checked(services().schematic().show_grid);
        grid.toggled.connect([this](bool on) { set_grid_shown(on); });
        m_grid_tool = &grid;
        bar->add_separator();

        button &run = add_command_tool(*bar, host, icon::play, "Run simulation (F5)", "simulate.run", "Simulate");
        run.set_name("run tool");
        button &potentials = bar->add_tool(icon::net, "Colour wires by potential");
        potentials.set_name("potentials tool");
        potentials.set_toggle(true);
        potentials.set_checked(services().schematic().show_potentials);
        potentials.toggled.connect([this](bool on) { set_potentials(on); });
        m_potentials_tool = &potentials;

        bar->add_spacer();
        add_command_tool(*bar, host, icon::cube, "Board Editor (Ctrl+3)", window_command(window_kind::board_editor));
        add_command_tool(*bar, host, icon::component, "Library Browser (Ctrl+4)",
                         window_command(window_kind::library_browser));
        return bar;
    }

    std::unique_ptr<widget> schematic_editor::build_tools()
    {
        auto strip = std::make_unique<tool_strip>(orientation::vertical);
        strip->set_name("tool strip");
        for (const tool_info &t : tools)
        {
            const int index = strip->add_tool(t.icon, std::format("{} ({})", t.name, t.key));
            strip->tool(static_cast<std::size_t>(index)).set_name(std::string("tool ") + t.command);
            if (t.id == tool::select)
                strip->add_separator();
        }
        strip->select(static_cast<int>(m_tools.active_tool()));
        strip->changed.connect([this](int index) {
            // The power button opens the pick by itself (below), chosen or
            // clicked again.
            if (static_cast<tool>(index) != tool::power)
                set_tool(static_cast<tool>(index));
        });
        button &power = strip->tool(static_cast<std::size_t>(tool::power));
        power.clicked.connect([this, &power] { power_tool(&power); });
        m_tool_strip = strip.get();
        return strip;
    }

    std::unique_ptr<widget> schematic_editor::build_sheets()
    {
        // The open sheets: the project's, or the scratch sheet.
        const schematic_document &doc = services().schematic();
        auto strip = std::make_unique<tool_strip>(orientation::horizontal);
        strip->set_name("sheet strip");
        for (std::size_t i = 0; i < doc.sheet_count(); ++i)
        {
            const std::string &file = doc.sheet_file(i);
            std::string tooltip =
                doc.scratch() ? std::string("A demo sheet, a resistor ladder: editable, never saved. Open or "
                                            "create a project to keep a schematic.")
                : !doc.sheet_loaded(i) ? std::format("{}: did not load, and is not saved over", file)
                                       : file;
            strip->add_tool(icon::sheet, std::move(tooltip), doc.sheet_name(i));
        }
        strip->select(static_cast<int>(doc.active_index()));
        strip->changed.connect([this](int index) {
            if (index >= 0)
                services().schematic().set_active(static_cast<std::size_t>(index));
        });
        m_sheet_strip = strip.get();
        m_strip_generation = doc.active_key().generation;

        // Takes what is left at the right, and is cut short when that is
        // not enough: what is special about the shown sheet.
        auto &note = strip->emplace<label>("", text_role::dim);
        note.set_name("sheet note");
        note.set_size(text_size::small);
        note.set_align(align::end);
        note.layout().flex = 1.0f;
        m_sheet_note = &note;
        return strip;
    }

    std::unique_ptr<widget> schematic_editor::build_canvas(window_host &host)
    {
        auto area = std::make_unique<column>(0.0f);
        area->set_name("canvas area");

        auto holder = std::make_unique<column>(0.0f);
        holder->add(build_sheets());
        m_sheet_holder = holder.get();
        area->add(std::move(holder));

        auto frame = std::make_unique<panel>("", surface::background, 0.0f, insets::uniform(6.0f));
        frame->layout().flex = 1.0f;
        auto view =
            std::make_unique<schematic_viewport>(services().view_image(offscreen_view::schematic), m_view_controller);
        view->set_name("schematic view");
        view->set_caption(services().schematic().sheet_name(services().schematic().active_index()));
        view->set_radius(host.screen().theme().radius + 2.0f);
        view->set_controller(&m_view_controller);
        view->layout().flex = 1.0f;
        view->changed.connect([this] { sync_camera(); });
        view->resized.connect([this](math::vec2i) {
            // The first time the view has a size for these sheets, the
            // shown one is fitted into it: the camera's zoom is in the
            // target's pixels, which at a display scale of 2.5 would show
            // the sheet tiny. After that the camera stays as the user leaves
            // it, until another sheet is shown.
            const schematic_document::sheet_key key = services().schematic().active_key();
            if (m_fitted != std::pair{key.generation, key.index})
                fit_sheet();
            update_cursor_status();
        });
        view->pointer_moved.connect([this](math::vec2f) { update_cursor_status(); });
        view->pointer_left.connect([this] {
            m_view_controller.leave();
            update_cursor_status();
        });
        view->context_requested.connect([this, &host](math::vec2f at) { open_context_menu_at(host, at); });
        m_view = view.get();
        frame->add(std::move(view));
        area->add(std::move(frame));

        // What the tool does now, in a line under the canvas: long, and
        // changing with every gesture, so not in the status bar's message.
        // Cut short when the canvas is narrower than it.
        auto &hint_row = area->emplace<row>(0.0f, insets{10.0f, 0.0f, 10.0f, 5.0f});
        auto &hint = hint_row.emplace<label>("", text_role::dim);
        hint.set_name("canvas hint");
        hint.set_size(text_size::small);
        hint.layout().flex = 1.0f;
        m_hint = &hint;
        return area;
    }

    void schematic_editor::open_context_menu_at(window_host &host, math::vec2f at)
    {
        // A right press the tools took (ending a wire, cancelling a drag)
        // opens nothing.
        if (!m_view_controller.context_menu_allowed() || m_view == nullptr)
            return;

        // With the select tool, the item under the pointer becomes the
        // selection, unless it is in it already.
        if (m_tools.active_tool() == tool::select && !m_tools.pasting() && m_tools.document() != nullptr)
        {
            const math::vec2f pixel = m_view->to_pixels(at);
            const math::vec2f size = m_view->pixel_size().cast<float>();
            const math::vec2f sheet_at = m_view_controller.to_sheet(pixel, size);
            const auto hit = schematic::hit_test(m_tools.document()->sheet(), sheet_at, m_view_controller.tolerance(),
                                                 services().schematic().style());
            if (hit && !m_tools.is_selected(*hit))
            {
                m_tools.select(*hit);
                tools_changed();
            }
        }

        std::vector<menu_item> items;
        if (has_selection())
        {
            items = {command_item("Cut", "edit.cut"),
                     command_item("Copy", "edit.copy"),
                     command_item("Delete", "edit.delete"),
                     menu_item::separator(),
                     command_item("Rotate", "edit.rotate"),
                     command_item("Rotate Clockwise", "edit.rotate_cw"),
                     command_item("Mirror Horizontally", "edit.mirror_x"),
                     command_item("Mirror Vertically", "edit.mirror_y"),
                     menu_item::separator(),
                     action_item("Properties", [this] { show_page(page::properties); }),
                     menu_item::separator()};
            if (const std::optional<part_assignment> what = selection_assignment())
            {
                const schematic::part *p = m_tools.document()->find_part(what->items.front());
                const bool change = p != nullptr && !is_placeholder(*p);
                items.insert(items.end() - 1,
                             action_item(change ? "Change Part\xE2\x80\xA6" : "Assign Part\xE2\x80\xA6",
                                         [this] { assign_part(); }));
            }
        }
        menu_item paste = command_item("Paste", "edit.paste");
        paste.enabled = [this] {
            const std::string text = services().system_clipboard().text();
            return m_tools.can_paste() || text.find(schematic::sheet_format) != std::string::npos;
        };
        items.push_back(std::move(paste));
        items.push_back(command_item("Select All", "edit.select_all"));
        items.push_back(menu_item::separator());

        menu_item grid = command_item("Show Grid", "view.grid", icon::grid);
        grid.checked = [this] { return services().schematic().show_grid; };
        menu_item potentials = command_item("Colour Wires by Potential", "simulate.potentials", icon::net);
        potentials.checked = [this] { return services().schematic().show_potentials; };
        items.push_back(command_item("Fit to View", "view.fit", icon::fit));
        items.push_back(std::move(grid));
        items.push_back(menu_item::separator());
        items.push_back(command_item("Run Simulation", "simulate.run", icon::play));
        items.push_back(std::move(potentials));
        if (m_tools.active_tool() != tool::select)
        {
            items.push_back(menu_item::separator());
            items.push_back(command_item("Select Tool", "tool.select", icon::pointer));
        }
        open_context_menu(host.screen(), std::move(items), at);
    }

    std::unique_ptr<widget> schematic_editor::build_dock(window_host &host)
    {
        auto dock = std::make_unique<panel>("", surface::panel, 0.0f, insets{});
        dock->set_name("schematic dock");
        auto &tabs = dock->emplace<tab_view>();
        tabs.set_name("dock tabs");
        tabs.layout().flex = 1.0f;
        tabs.add_tab(page_names[0], build_properties());

        parts_panel_options parts;
        parts.place = [this](const library::part &part, const std::string &project_id) {
            place_library_part(part, project_id);
        };
        parts.place_symbol = [this](const std::string &symbol, const std::string &v) { place_generic(symbol, v); };
        parts.place_power = [this](const std::string &name) { place_power(name); };
        parts.assign = [this](const part_assignment &what, const library::part &part, const std::string &project_id) {
            apply_assignment(what, part, project_id);
        };
        auto parts_panel = make_parts_panel(host, std::move(parts));
        m_parts_panel = parts_panel.get();
        tabs.add_tab(page_names[1], std::move(parts_panel));
        tabs.add_tab(page_names[2], build_simulation());
        if (m_dock_page >= 0 && static_cast<std::size_t>(m_dock_page) < tabs.tab_count())
            tabs.select(m_dock_page);
        tabs.changed.connect([this](int index) { m_dock_page = index; });
        m_dock_tabs = &tabs;
        m_dock = dock.get();
        return dock;
    }

    std::unique_ptr<widget> schematic_editor::build_properties()
    {
        schematic_document &doc = services().schematic();

        auto scroller = std::make_unique<scroll_area>();
        scroller->set_name("properties scroll");
        auto content = std::make_unique<column>(0.0f);

        // ---- what the armed tool places, then what is selected; made by
        // the pages, in before_frame()
        content->add(m_tool_options.build());
        content->add(m_properties.build());

        auto grid = std::make_unique<property_grid>();
        grid->set_name("property grid");

        // ---- the sheet shown; filled in by update_sheet_info()
        grid->add_section("Sheet");
        m_sheet_name = &grid->add_row("Name", value(""));
        m_sheet_name->set_name("sheet name");
        m_sheet_file = &grid->add_row("File", value("", text_role::dim));
        m_sheet_file->set_name("sheet file");
        m_sheet_parts = &grid->add_row("Parts", value(""));
        m_sheet_parts->set_name("sheet parts");
        m_sheet_wires = &grid->add_row("Wires", value(""));
        m_sheet_wires->set_name("sheet wires");
        m_sheet_labels = &grid->add_row("Labels", value(""));
        m_sheet_labels->set_name("sheet labels");
        m_sheet_nets = &grid->add_row("Nets", value(""));
        m_sheet_nets->set_name("sheet nets");
        {
            // The placeholders, and a way to select them for Assign Part….
            auto line = std::make_unique<row>(6.0f);
            line->set_cross_align(align::center);
            auto &count = line->emplace<label>("");
            count.set_name("sheet unassigned");
            count.layout().flex = 1.0f;
            auto &select = line->emplace<button>("Select");
            select.set_name("sheet select unassigned");
            select.set_tooltip("Select every part with no library part assigned, to assign parts to them (Edit > "
                               "Assign Part\xE2\x80\xA6, a kind and value at a time)");
            select.clicked.connect([this] {
                const schematic::document *d = m_tools.document();
                if (d == nullptr)
                    return;
                const std::vector<schematic::item_ref> parts = unassigned_parts(d->sheet());
                m_tools.select(parts);
                tools_changed();
            });
            m_sheet_unassigned = &count;
            m_select_unassigned = &select;
            grid->add_row("Unassigned", std::move(line));
        }
        {
            // The project's rule (the demo sheet's for the session).
            auto box = std::make_unique<checkbox>("along wires", doc.connectivity().pins_inside_wires);
            box->set_name("pins along wires");
            box->set_tooltip("On: a wire that passes over a pin connects to it. Off: only a wire that ends on a "
                             "pin connects to it. Kept in the project, for all its sheets "
                             "(Edit > Pins Connect Along Wires)");
            box->layout().cross = align::start;
            box->changed.connect([this](bool on) { set_connectivity(on); });
            m_connect_box = &grid->add_row("Pins connect", std::move(box));
        }

        // ---- how it is drawn
        grid->add_section("Display");
        {
            auto box = std::make_unique<checkbox>("", doc.show_grid);
            box->set_name("grid checkbox");
            box->layout().cross = align::start;
            box->changed.connect([this](bool on) { set_grid_shown(on); });
            m_grid_box = &grid->add_row("Show grid", std::move(box));

            auto spacing = std::make_unique<numeric_field>(doc.grid, 0.25, 10.0, 0.25);
            spacing->set_name("grid spacing");
            spacing->set_decimals(2);
            spacing->set_enabled(doc.show_grid);
            spacing->value_changed.connect([this](double v) { set_grid_spacing(v); });
            m_grid_field = &grid->add_row("Grid spacing", std::move(spacing));

            auto wire = std::make_unique<slider>(0.03, 0.40, doc.wire_width, 0.01);
            wire->set_name("wire width");
            wire->set_formatter([](double v) { return std::format("{:.2f}", v); });
            wire->changed.connect([this](double v) {
                schematic_document &d = services().schematic();
                d.wire_width = static_cast<float>(v);
                d.apply_wire_width();
                m_tools.set_style(d.style());
                m_pushed.reset();
            });
            grid->add_row("Wire width", std::move(wire));
        }
        content->add(std::move(grid));
        scroller->set_content(std::move(content));
        return scroller;
    }

    std::unique_ptr<widget> schematic_editor::build_simulation()
    {
        schematic_document &doc = services().schematic();

        auto page_box = std::make_unique<column>(8.0f);
        page_box->set_name("simulation page");
        auto &scroller = page_box->emplace<scroll_area>();
        scroller.set_name("simulation scroll");
        scroller.layout().flex = 1.0f;

        auto content = std::make_unique<column>(6.0f);
        property_grid *grid = &content->emplace<property_grid>();
        grid->set_name("simulation grid");

        // ---- what goes in: the shown sheet's circuit, as its netlist
        // makes it; filled in by update_sheet_info().
        grid->add_section("Circuit");
        m_circuit_summary = &grid->add_row("Made of", value(""));
        m_circuit_summary->set_name("circuit summary");
        m_circuit_nodes = &grid->add_row("Nodes", value(""));
        m_circuit_nodes->set_name("circuit nodes");
        m_circuit_branches = &grid->add_row("Branches", value(""));
        m_circuit_branches->set_name("circuit branches");

        grid->add_section("Display");
        {
            auto colours = std::make_unique<radio_group>(std::vector<std::string>{"By potential", "Plain"},
                                                         doc.show_potentials ? 0 : 1);
            colours->set_name("wire colours");
            colours->changed.connect([this](int index) { set_potentials(index == 0); });
            m_colours = &grid->add_row("Wire colours", std::move(colours));

            auto automatic = std::make_unique<checkbox>("", m_auto_run);
            automatic->set_name("run automatically");
            automatic->set_tooltip("Run the simulation again a moment after every change of the sheet");
            automatic->layout().cross = align::start;
            automatic->changed.connect([this](bool on) { set_auto_run(on); });
            m_auto_box = &grid->add_row("Auto-run", std::move(automatic));
        }

        // ---- what the netlist and the circuit found; filled in by
        // update_problems().
        auto &section = content->emplace<column>(4.0f, insets{8.0f, 0.0f, 8.0f, 8.0f});
        auto &problems_title = section.emplace<label>("PROBLEMS", text_role::dim);
        problems_title.set_size(text_size::small);
        auto &problems = section.emplace<column>(2.0f);
        problems.set_name("simulation problems");
        m_problems = &problems;
        scroller.set_content(std::move(content));

        // What the results are, and the one action the page is about.
        auto &foot = page_box->emplace<column>(8.0f, insets{8.0f, 0.0f, 8.0f, 8.0f});
        foot.add(std::make_unique<separator>());
        m_run_state = &foot.emplace<label>("", text_role::dim);
        m_run_state->set_name("simulation state");
        m_run_state->set_size(text_size::small);
        m_run_state->set_wrap(true, 3);
        auto &run = foot.emplace<button>("Run simulation", icon::play, button_kind::primary);
        run.set_name("run simulation");
        run.set_tooltip("Solves the shown sheet's circuit as it is (F5)");
        run.clicked.connect([this] { run_simulation(true); });
        return page_box;
    }

    // ---- bindings -------------------------------------------------------------------------

    void schematic_editor::set_tool(tool t)
    {
        m_tools.set_tool(t);
        // A part is picked from the parts library: typing goes to its
        // search, the part armed before (if any) still armed meanwhile.
        if (t == tool::part)
        {
            show_page(page::parts);
            if (m_parts_panel != nullptr)
                focus_parts_search(*m_parts_panel);
        }
        tools_changed();
    }

    void schematic_editor::power_tool(const button *anchor)
    {
        // Armed with the port it had (GND to begin with), and the pick
        // offered on top: dismissing it keeps that one.
        m_tools.set_tool(tool::power);
        tools_changed();
        open_power_pick(anchor);
    }

    void schematic_editor::open_power_pick(const button *anchor)
    {
        if (host() == nullptr || m_tools.document() == nullptr)
            return;
        ui::screen &s = host()->screen();
        const std::string armed = m_tools.label_text(schematic::label_kind::power);
        const std::vector<std::string> names = power_port_names(m_tools.document()->sheet());

        // The usual ports, then those on the sheet, then any other name.
        std::vector<menu_item> items;
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            if (i == standard_power_ports().size())
                items.push_back(menu_item::separator());
            items.push_back(action_item(
                names[i], [this, name = names[i]] { place_power(name); },
                [armed, name = names[i]] { return name == armed; }));
        }
        items.push_back(menu_item::separator());
        items.push_back(action_item("Other\xE2\x80\xA6", [this] { name_power_port(); }));

        if (anchor == nullptr && m_view != nullptr && m_view->has_pointer())
        {
            (void)open_context_menu(s, std::move(items), s.pointer());
            return;
        }
        // From the strip's button, or with the pointer elsewhere: beside the
        // button.
        if (anchor == nullptr && m_tool_strip != nullptr)
            anchor = &m_tool_strip->tool(static_cast<std::size_t>(tool::power));
        if (anchor != nullptr)
            (void)open_menu(s, std::move(items), anchor->bounds(), placement::right);
        else
            (void)open_context_menu(s, std::move(items), s.size() * 0.5f);
    }

    void schematic_editor::name_power_port()
    {
        m_tools.set_tool(tool::power);
        tools_changed();
        show_page(page::properties);
        (void)m_tool_options.focus_field();
    }

    void schematic_editor::return_to_canvas()
    {
        // The selection and the tool stay: the next click places the next
        // one, its ghost under the pointer.
        tools_changed();
        if (m_view != nullptr)
            m_view->focus();
    }

    void schematic_editor::set_connectivity(bool pins_inside_wires)
    {
        schematic::connectivity rules = services().schematic().connectivity();
        rules.pins_inside_wires = pins_inside_wires;
        // connectivity_changed() follows, from the document.
        services().schematic().set_connectivity(rules);
        if (host() != nullptr)
            host()->show_message(pins_inside_wires
                                     ? std::string("Pins connect to a wire that passes over them")
                                     : std::string("Pins connect only to wires that end on them"));
    }

    void schematic_editor::connectivity_changed()
    {
        const schematic::connectivity &rules = services().schematic().connectivity();
        m_tools.set_connectivity(rules);
        if (m_connect_box != nullptr)
            m_connect_box->set_checked(rules.pins_inside_wires);
        m_pushed.reset();
        tools_changed();
    }

    void schematic_editor::theme_changed()
    {
        services().schematic().apply_theme(services().theme());
        m_tools.set_style(services().schematic().style());
        m_pushed.reset();
        tools_changed();
    }

    void schematic_editor::scale_changed(float scale)
    {
        m_view_controller.pixel_scale = scale;
    }

    void schematic_editor::show_page(page p)
    {
        if (m_dock_tabs == nullptr)
            return;
        set_dock_shown(true);
        m_dock_tabs->select(static_cast<int>(p));
        m_dock_page = m_dock_tabs->selected();
    }

    void schematic_editor::set_dock_shown(bool shown)
    {
        m_dock_shown = shown;
        if (m_split != nullptr)
            m_split->set_collapsed(shown ? splitter::pane::none : splitter::pane::second);
    }

    void schematic_editor::set_grid_shown(bool shown)
    {
        schematic_document &doc = services().schematic();
        doc.show_grid = shown;
        if (m_grid_box != nullptr)
            m_grid_box->set_checked(shown);
        if (m_grid_tool != nullptr)
            m_grid_tool->set_checked(shown);
        if (m_grid_field != nullptr)
            m_grid_field->set_enabled(shown);
        doc.apply_grid();
    }

    void schematic_editor::set_grid_spacing(double spacing)
    {
        schematic_document &doc = services().schematic();
        doc.grid = static_cast<float>(spacing);
        if (m_grid_field != nullptr)
            m_grid_field->set_value(spacing);
        doc.apply_grid();
        // What is placed snaps to the grid shown, whether it is shown or
        // not (Alt lets go of it while held).
        m_tools.set_grid(grid_step(doc.grid));
        tools_changed();
    }

    void schematic_editor::set_potentials(bool shown)
    {
        schematic_document &doc = services().schematic();
        doc.show_potentials = shown;
        if (m_potentials_tool != nullptr)
            m_potentials_tool->set_checked(shown);
        if (m_colours != nullptr)
            m_colours->set_selected(shown ? 0 : 1);
        doc.mark_dirty();
    }

    void schematic_editor::set_auto_run(bool on)
    {
        m_auto_run = on;
        if (m_auto_box != nullptr)
            m_auto_box->set_checked(on);
        if (on)
            schedule_simulation();
    }

    void schematic_editor::zoom_by(float factor)
    {
        if (m_view == nullptr)
            return;
        const math::vec2f size = m_view->pixel_size().cast<float>();
        m_controller.zoom_about(size * 0.5f, size, factor);
        sync_camera();
    }

    void schematic_editor::sync_camera()
    {
        schematic_document &doc = services().schematic();
        doc.scene.view.center = m_controller.center;
        doc.scene.view.zoom = m_controller.zoom;
        doc.mark_dirty();
        update_cursor_status();
    }

    // ---- the tools ----------------------------------------------------------------------------

    void schematic_editor::attach_tools()
    {
        schematic::document *active = &services().schematic().active();
        if (m_tools.document() != active)
            m_tools.set_document(active);
    }

    void schematic_editor::tools_changed()
    {
        schematic_document &doc = services().schematic();
        attach_tools();

        const std::pair<std::uint64_t, schematic_document::sheet_key> now{m_tools.revision(), doc.active_key()};
        if (m_pushed == now)
            return;
        // Noted first: pushing marks the scene dirty, whose `changed` comes
        // back here.
        m_pushed = now;

        // Only while a window shows them: a closed window's tools draw
        // nothing.
        if (host() == nullptr)
            return;
        doc.set_selection(m_tools.selection());
        std::vector<schematic::segment> overlay;
        m_tools.append_overlay(overlay, doc.style());
        doc.set_overlay(std::move(overlay));

        const tool t = m_tools.active_tool();
        if (m_tool_strip != nullptr)
            m_tool_strip->select(static_cast<int>(t));
        if (m_tool_status != nullptr)
            m_tool_status->set_text(m_tools.pasting() ? std::string("Paste") : std::string(info(t).name));
        if (m_hint != nullptr)
            m_hint->set_text(m_tools.hint());
        update_cursor_status();
    }

    void schematic_editor::after_command(bool focus_canvas)
    {
        tools_changed();
        if (focus_canvas && m_view != nullptr)
            m_view->focus();
    }

    bool schematic_editor::has_selection() const
    {
        return !m_tools.selection().empty();
    }

    void schematic_editor::undo()
    {
        const std::string step(m_tools.undo_label());
        const bool done = m_tools.undo();
        after_command();
        if (host() != nullptr)
            host()->show_message(done ? "Undone: " + step : std::string("Nothing to undo"));
    }

    void schematic_editor::redo()
    {
        const std::string step(m_tools.redo_label());
        const bool done = m_tools.redo();
        after_command();
        if (host() != nullptr)
            host()->show_message(done ? "Redone: " + step : std::string("Nothing to redo"));
    }

    void schematic_editor::copy()
    {
        if (!m_tools.copy())
            return;
        // On the system's clipboard as well, as a sheet: it pastes into
        // another session, or a text editor.
        services().system_clipboard().set_text(m_tools.clipboard_text());
        after_command();
        if (host() != nullptr)
            host()->show_message(
                std::format("Copied {}", selection_summary(m_tools.document()->sheet(), m_tools.selection())));
    }

    void schematic_editor::cut()
    {
        if (m_tools.document() == nullptr || !has_selection())
            return;
        const std::string what = selection_summary(m_tools.document()->sheet(), m_tools.selection());
        if (!m_tools.cut())
            return;
        services().system_clipboard().set_text(m_tools.clipboard_text());
        after_command();
        if (host() != nullptr)
            host()->show_message("Cut " + what);
    }

    void schematic_editor::paste()
    {
        // A sheet on the system's clipboard (copied here, or in another
        // session) comes first; anything else there leaves the tools' own.
        const std::string text = services().system_clipboard().text();
        if (text.find(schematic::sheet_format) != std::string::npos)
            (void)m_tools.set_clipboard_text(text);
        const bool armed = m_tools.paste();
        after_command(armed);
        if (!armed && host() != nullptr)
            host()->show_message("Nothing to paste: copy something on a sheet first");
    }

    void schematic_editor::place_generic(std::string symbol, std::string value)
    {
        // As begin_place_symbol() makes it, with a value to start from.
        schematic::part p;
        p.device = schematic::find_symbol(symbol).device;
        p.symbol = std::move(symbol);
        p.value = std::move(value);
        m_tools.begin_place_part(std::move(p));
        after_command(true);
    }

    void schematic_editor::place_power(std::string name)
    {
        m_tools.begin_place_power(std::move(name));
        after_command(true);
    }

    void schematic_editor::place_library_part(const library::part &part, const std::string &project_id)
    {
        schematic::part t = schematic_tools::part_template(part);
        // The project library's id, or none: a part of the scratch sheet.
        t.library_part = project_id;
        m_tools.begin_place_part(std::move(t));
        after_command(true);
    }

    std::optional<part_assignment> schematic_editor::selection_assignment(std::size_t *others) const
    {
        if (others != nullptr)
            *others = 0;
        const schematic::document *d = m_tools.document();
        if (d == nullptr)
            return std::nullopt;

        // The selected placeholders (anything else selected is left out),
        // grouped by device and value, in the order first selected.
        struct group
        {
            part_assignment what;
            std::vector<std::string> references;
        };
        std::vector<group> groups;
        std::size_t placeholders = 0;
        for (const schematic::item_ref &item : m_tools.selection())
        {
            const schematic::part *p = item.kind == schematic::item_kind::part ? d->find_part(item.id) : nullptr;
            if (p == nullptr || !is_placeholder(*p))
                continue;
            ++placeholders;
            auto at = std::ranges::find_if(
                groups, [&](const group &g) { return g.what.device == p->device && g.what.value == p->value; });
            if (at == groups.end())
            {
                groups.push_back({part_assignment{{}, p->device, p->value, p->symbol, {}}, {}});
                at = groups.end() - 1;
            }
            at->what.items.push_back(item.id);
            at->references.push_back(p->reference);
        }

        const auto name_them = [](const std::vector<std::string> &references) {
            std::string named;
            for (std::size_t i = 0; i < references.size() && i < 4; ++i)
                named += (i == 0 ? "" : ", ") + references[i];
            if (references.size() > 4)
                named += std::format(" and {} more", references.size() - 4);
            return named;
        };

        if (groups.empty())
        {
            // Change Part…: one assigned part, selected alone.
            const std::span<const schematic::item_ref> selection = m_tools.selection();
            if (selection.size() != 1 || selection[0].kind != schematic::item_kind::part)
                return std::nullopt;
            const schematic::part *p = d->find_part(selection[0].id);
            if (p == nullptr || p->library_part.empty())
                return std::nullopt;
            part_assignment what{{}, p->device, p->value, p->symbol, {selection[0].id}};
            what.title = std::format("Change the part of {} ({})", p->reference,
                                     describe(placeholder{what.device, what.value, what.symbol}));
            return what;
        }

        // The largest group; the first selected among equals.
        const group *best = &groups.front();
        for (const group &g : groups)
        {
            if (g.what.items.size() > best->what.items.size())
                best = &g;
        }
        if (others != nullptr)
            *others = placeholders - best->what.items.size();
        part_assignment what = best->what;
        what.title = std::format("Assign a part to {} ({})", name_them(best->references),
                                 describe(placeholder{what.device, what.value, what.symbol}));
        return what;
    }

    void schematic_editor::assign_part()
    {
        std::size_t others = 0;
        std::optional<part_assignment> what = selection_assignment(&others);
        if (host() == nullptr)
            return;
        if (!what)
        {
            host()->show_message("Select placeholders (parts with no library part) to assign a part to them");
            return;
        }
        if (services().project() == nullptr)
        {
            host()->show_message("Open a project to assign parts: an assigned part is kept in the project's library",
                                 std::chrono::seconds(6), log_level::warning);
            return;
        }
        // Placeholders of several kinds: the largest group first; the
        // others stay selected for the next Assign Part….
        if (others > 0)
            host()->show_message(std::format("Assigning to {} first: {} other selected placeholder{} of another "
                                             "kind or value, for the next Assign Part\xE2\x80\xA6",
                                             describe(placeholder{what->device, what->value, what->symbol}),
                                             others, others == 1 ? " is" : "s are"),
                                 std::chrono::seconds(8));
        show_page(page::parts);
        if (m_parts_panel != nullptr && begin_part_assignment(*m_parts_panel, std::move(*what)))
            focus_parts_search(*m_parts_panel);
    }

    void schematic_editor::apply_assignment(const part_assignment &what, const library::part &part,
                                            const std::string &project_id)
    {
        schematic::document *d = m_tools.document();
        if (d == nullptr)
            return;
        // Those still there and still of that device and value: the
        // placeholders, or the part whose part is being changed.
        std::vector<std::pair<schematic::item_id, schematic::part>> changes;
        bool changing = false;
        for (const schematic::item_id id : what.items)
        {
            const schematic::part *p = d->find_part(id);
            if (p == nullptr || p->device != what.device || p->value != what.value)
                continue;
            if (!is_placeholder(*p) && p->library_part.empty())
                continue;
            changing = changing || !is_placeholder(*p);
            changes.emplace_back(id, assigned(*p, part, project_id));
        }
        if (changes.empty())
        {
            if (host() != nullptr)
                host()->show_message("Nothing assigned: the parts are gone, or are not what they were");
            return;
        }
        const std::string step =
            changing ? (changes.size() == 1 ? "Change part of " + changes[0].second.reference
                                            : std::format("Change part of {} parts", changes.size()))
            : changes.size() == 1 ? "Assign part to " + changes[0].second.reference
                                  : std::format("Assign part to {} parts", changes.size());
        d->begin(step);
        for (auto &[id, p] : changes)
            (void)d->replace_part(id, std::move(p));
        d->commit();
        after_command(true);
        if (host() != nullptr)
            host()->show_message(std::format("{}: {}", step, part.name.empty() ? part.id : part.name));
    }

    void schematic_editor::edit_item(schematic::item_ref item)
    {
        if (host() == nullptr)
            return;
        show_page(page::properties);
        (void)m_properties.focus_text(item);
    }

    void schematic_editor::show_items(std::span<const schematic::item_ref> items)
    {
        m_tools.select(items);
        tools_changed();
        const schematic::document *d = m_tools.document();
        if (d == nullptr || items.empty())
            return;
        // The view centred on them, at the zoom it has.
        if (const auto box = schematic::selection_bounds(d->sheet(), items, services().schematic().style()))
        {
            const schematic::point c = box->center();
            const float unit = 1.0f / static_cast<float>(schematic::units_per_grid);
            m_controller.center = math::vec2f{static_cast<float>(c.x()), static_cast<float>(c.y())} * unit;
            sync_camera();
        }
    }

    // ---- keeping up ----------------------------------------------------------------------------

    void schematic_editor::scene_changed()
    {
        // The view's target is drawn again in the next frame that draws this
        // window; this asks for that frame. The tools' overlay and the
        // selection reach the scene in before_frame().
        if (m_view != nullptr)
            m_view->invalidate();

        // An edit (not the camera, not the selection): the simulation runs
        // again once the sheet settles, and the demo sheet says once that
        // it is not saved.
        schematic_document &doc = services().schematic();
        const schematic_document::sheet_key key = doc.active_key();
        if (m_seen_key == key)
            return;
        const bool same_sheet =
            m_seen_key && m_seen_key->generation == key.generation && m_seen_key->index == key.index;
        m_seen_key = key;
        schedule_simulation();
        if (same_sheet && doc.scratch() && doc.active().modified() && !m_scratch_told && host() != nullptr)
        {
            m_scratch_told = true;
            // The automatic run that follows keeps quiet while this shows.
            m_quiet_until = clock::now() + std::chrono::seconds(10);
            host()->show_message("The demo sheet is never saved: open or create a project in the Project Manager "
                                 "to keep a schematic",
                                 std::chrono::seconds(10));
        }
    }

    void schematic_editor::sheets_changed()
    {
        // The tools first, window or not: the sheets they edited may be on
        // their way out. A project opens with its own rules of connection
        // (without connectivity_changed: they are the project's already).
        attach_tools();
        m_pushed.reset();

        const schematic_document &doc = services().schematic();
        m_tools.set_connectivity(doc.connectivity());
        if (m_connect_box != nullptr)
            m_connect_box->set_checked(doc.connectivity().pins_inside_wires);
        const schematic_document::sheet_key key = doc.active_key();
        m_seen_key = key;

        // Other sheets: the strip is made again. Only another sheet shown
        // (the strip's own click, perhaps, which is still being handled):
        // the strip only follows.
        if (m_sheet_holder != nullptr && m_sheet_strip != nullptr)
        {
            if (key.generation == m_strip_generation)
            {
                m_sheet_strip->select(static_cast<int>(key.index));
            }
            else
            {
                m_sheet_strip = nullptr;
                m_sheet_note = nullptr;
                m_sheet_holder->clear_children();
                m_sheet_holder->add(build_sheets());
            }
        }
        if (m_view != nullptr)
            m_view->set_caption(doc.sheet_name(key.index));
        m_problems_key.reset();
        fit_sheet();
        tools_changed();
        schedule_simulation();
    }

    void schematic_editor::schedule_simulation()
    {
        if (!m_auto_run || host() == nullptr)
            return;
        if (services().schematic().simulation_current())
            return;
        ui::screen &s = host()->screen();
        if (m_run_timer != 0)
            s.cancel(m_run_timer);
        // From the clock: the screen's time may be long past after a sleep.
        m_run_timer = s.schedule(clock::now() + settle_time, [this] {
            m_run_timer = 0;
            run_when_settled();
        });
    }

    void schematic_editor::run_when_settled()
    {
        if (!m_auto_run || services().schematic().simulation_current())
            return;
        // Not in the middle of a drag or a wire: once it ends.
        if (m_tools.in_gesture() || services().schematic().active().in_transaction())
        {
            schedule_simulation();
            return;
        }
        run_simulation(false);
    }

    void schematic_editor::run_simulation(bool manual)
    {
        std::string text = services().run_simulation();
        // The automatic run says something only when it found something
        // else than the last time.
        if (host() != nullptr && (manual || (text != m_last_run && clock::now() >= m_quiet_until)))
            host()->show_message(text, std::chrono::seconds(8));
        m_last_run = std::move(text);
    }

    void schematic_editor::fit_sheet()
    {
        const schematic_document &doc = services().schematic();
        schematic::point low;
        schematic::point high;
        if (doc.sheet_bounds(low, high))
        {
            // Sheet units to world units, and a little room around it.
            const float unit = 1.0f / static_cast<float>(schematic::units_per_grid);
            m_controller.fit_min = math::vec2f{static_cast<float>(low.x()), static_cast<float>(low.y())} * unit -
                                   math::vec2f{1.0f, 1.0f};
            m_controller.fit_max = math::vec2f{static_cast<float>(high.x()), static_cast<float>(high.y())} * unit +
                                   math::vec2f{1.0f, 1.0f};
        }
        else
        {
            // An empty sheet: a stretch about the origin.
            m_controller.fit_min = {-10.0f, -5.0f};
            m_controller.fit_max = {10.0f, 5.0f};
        }
        if (m_view == nullptr)
            return;
        const math::vec2i pixels = m_view->pixel_size();
        if (pixels.x() < 1 || pixels.y() < 1)
            return; // when the view has a size (resized)
        const schematic_document::sheet_key key = doc.active_key();
        m_fitted = std::pair{key.generation, key.index};
        m_controller.fit(pixels.cast<float>());
        sync_camera();
    }

    void schematic_editor::update_sheet_info()
    {
        const schematic_document &doc = services().schematic();
        const std::size_t index = doc.active_index();
        const schematic::document &d = doc.active();
        const schematic::sheet &s = d.sheet();

        if (m_undo_tool != nullptr)
        {
            m_undo_tool->set_enabled(m_tools.can_undo());
            m_undo_tool->set_tooltip(m_tools.can_undo() ? std::format("Undo {} (Ctrl+Z)", m_tools.undo_label())
                                                        : std::string("Nothing to undo"));
        }
        if (m_redo_tool != nullptr)
        {
            m_redo_tool->set_enabled(m_tools.can_redo());
            m_redo_tool->set_tooltip(m_tools.can_redo() ? std::format("Redo {} (Ctrl+Y)", m_tools.redo_label())
                                                        : std::string("Nothing to redo"));
        }

        if (m_sheet_note != nullptr)
        {
            m_sheet_note->set_text(doc.scratch()               ? "Demo sheet: never saved"
                                   : !doc.sheet_loaded(index) ? "Did not load: not saved over its file"
                                   : d.modified()             ? "Modified"
                                                              : "");
        }
        if (m_sheet_name != nullptr)
            m_sheet_name->set_text(doc.sheet_name(index));
        if (m_sheet_file != nullptr)
            m_sheet_file->set_text(doc.scratch()               ? std::string("Not saved: demo")
                                   : !doc.sheet_loaded(index) ? doc.sheet_file(index) + " (did not load)"
                                                              : doc.sheet_file(index));
        if (m_sheet_parts != nullptr)
            m_sheet_parts->set_text(std::format("{}", s.parts.size()));
        if (m_sheet_wires != nullptr)
            m_sheet_wires->set_text(std::format("{}", s.wires.size()));
        if (m_sheet_labels != nullptr)
            m_sheet_labels->set_text(std::format("{}", s.labels.size()));
        if (m_sheet_nets != nullptr)
            m_sheet_nets->set_text(std::format("{}", doc.netlist().nets.size()));
        if (m_sheet_unassigned != nullptr)
        {
            const std::size_t unassigned = unassigned_parts(s).size();
            m_sheet_unassigned->set_text(unassigned == 0 ? std::string("None") : std::format("{}", unassigned));
            if (m_select_unassigned != nullptr)
                m_select_unassigned->set_enabled(unassigned > 0);
        }

        const schematic::circuit &c = doc.circuit();
        if (m_circuit_summary != nullptr)
            m_circuit_summary->set_text(c.network.nodes == 0 ? std::string("Nothing to simulate") : c.summary);
        if (m_circuit_nodes != nullptr)
            m_circuit_nodes->set_text(std::format("{}", c.network.nodes));
        if (m_circuit_branches != nullptr)
            m_circuit_branches->set_text(
                std::format("{}, {}, {}", count_of(c.network.branches.size(), "branch", "branches"),
                            count_of(c.network.holds.size(), "node held", "nodes held"),
                            count_of(c.network.sources.size(), "current fed", "currents fed")));
    }

    void schematic_editor::update_problems()
    {
        if (m_problems == nullptr || host() == nullptr)
            return;
        const schematic_document &doc = services().schematic();
        const schematic_document::sheet_key key = doc.active_key();
        if (m_problems_key == key)
            return;
        m_problems_key = key;

        // The circuit's first (what was left out of it), then the
        // netlist's. A click on one selects what it is about.
        const ui::theme &t = host()->screen().theme();
        m_problems->clear_children();
        std::size_t count = 0;
        for (const std::vector<schematic::problem> *list : {&doc.circuit().problems, &doc.netlist().problems})
        {
            for (const schematic::problem &p : *list)
            {
                std::function<void()> chosen;
                if (!p.items.empty())
                    chosen = [this, items = p.items] { show_items(items); };
                auto &entry = m_problems->emplace<problem_row>(std::move(chosen));
                if (!p.items.empty())
                    entry.set_tooltip("Select what this is about");
                auto &line = entry.emplace<label>(std::format("{}: {}", severity_name(p.level), p.message),
                                                  text_role::normal);
                line.set_wrap(true, 3);
                line.set_size(text_size::small);
                if (p.level == schematic::severity::error)
                    line.set_color(t.error);
                else if (p.level == schematic::severity::warning)
                    line.set_color(t.warning);
                else
                    line.set_role(text_role::dim);
                ++count;
            }
        }
        if (count == 0)
        {
            auto &none = m_problems->emplace<label>("None found.", text_role::dim);
            none.set_wrap(true, 2);
            none.set_size(text_size::small);
        }
    }

    void schematic_editor::update_cursor_status()
    {
        if (m_cursor_status == nullptr || m_view == nullptr)
            return;
        if (m_view->has_pointer())
        {
            const math::vec2f size = m_view->pixel_size().cast<float>();
            const math::vec2f at = m_view_controller.to_sheet(m_view->pointer_pixels(), size);
            const bool snapping = m_tools.placing() || m_tools.active_tool() == tool::wire;
            if (snapping)
            {
                // Where a click would put it: a connection point the tools
                // would attach to, else the grid point.
                schematic::point p = m_tools.snap_point(at);
                if (const auto &c = m_tools.hover_connection(); c && m_tools.active_tool() != tool::part &&
                                                                m_tools.active_tool() != tool::text)
                    p = c->at;
                m_cursor_status->set_text(std::format("X {}  Y {}", grid_units(p.x()), grid_units(p.y())));
            }
            else
            {
                const float unit = 1.0f / static_cast<float>(schematic::units_per_grid);
                m_cursor_status->set_text(std::format("X {:.2f}  Y {:.2f}", at.x() * unit, at.y() * unit));
            }
        }
        else
        {
            m_cursor_status->set_text("X \xE2\x80\x94  Y \xE2\x80\x94");
        }
        if (m_zoom_status != nullptr)
            m_zoom_status->set_text(std::format("Zoom {:.0f} px/u", m_controller.zoom));
    }

} // namespace volt::ui
