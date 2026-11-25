#include <app/ui/windows/library_browser.hpp>

#include <app/ui/windows/library_dialogs.hpp>
#include <app/ui/windows/library_widgets.hpp>
#include <app/ui/windows/menus.hpp>
#include <app/ui/windows/part_editor.hpp>
#include <app/ui/windows/part_editor_model.hpp>
#include <ui/layout/box.hpp>
#include <ui/painter.hpp>
#include <ui/theme.hpp>
#include <ui/widgets/bars.hpp>
#include <ui/widgets/button.hpp>
#include <ui/widgets/label.hpp>
#include <ui/widgets/list_view.hpp>
#include <ui/widgets/panel.hpp>
#include <ui/widgets/scroll_area.hpp>
#include <ui/widgets/splitter.hpp>
#include <ui/widgets/tab_view.hpp>
#include <ui/widgets/text_field.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <iostream>

namespace volt::ui
{
    namespace
    {
        // A search slower than this (search and sort, milliseconds) makes
        // the next one wait for a pause in the typing.
        constexpr double slow_search = 12.0;
        constexpr auto search_pause = std::chrono::milliseconds(90);

        // VOLT_LIBRARY_TIMING=1 prints how long each search took, for
        // measuring; nothing otherwise.
        bool log_timing()
        {
            static const bool on = [] {
                char *value = nullptr;
                std::size_t length = 0;
#ifdef _WIN32
                const bool set = _dupenv_s(&value, &length, "VOLT_LIBRARY_TIMING") == 0 && value != nullptr;
                std::free(value);
                return set;
#else
                static_cast<void>(value);
                static_cast<void>(length);
                return std::getenv("VOLT_LIBRARY_TIMING") != nullptr;
#endif
            }();
            return on;
        }

        std::vector<table_column> default_part_columns()
        {
            // Where the window is narrow, the library goes first, then the
            // manufacturer, the MPN and the package.
            // The value column shows the part's specs (spec_summary(): "1kΩ
            // ±1% 100mW"), its value first; sorted, it goes by the value.
            return {{"Name", 150.0f, 2.0f, align::start, true, 0},       {"Value", 140.0f, 2.0f, align::start, true, 0},
                    {"Package", 70.0f, 1.0f, align::start, true, 1},     {"Manufacturer", 96.0f, 1.2f, align::start, true, 3},
                    {"MPN", 124.0f, 1.8f, align::start, true, 2},        {"Library", 100.0f, 1.0f, align::start, true, 4}};
        }

        // The project's columns: a marker, then the texts.
        enum project_column : std::size_t
        {
            marker_column,
            name_column,
            value_column,
            package_column,
            source_column,
            status_column,
        };

        std::vector<table_column> default_project_columns()
        {
            return {{"", 24.0f, 0.0f, align::center, false, 0},       {"Name", 200.0f, 3.0f, align::start, false, 0},
                    {"Value", 70.0f, 1.0f, align::start, false, 0},      {"Package", 80.0f, 1.0f, align::start, false, 1},
                    {"Source", 190.0f, 2.0f, align::start, false, 2},    {"Status", 120.0f, 1.0f, align::start, false, 0}};
        }

        // A titled side panel's header text and padding, as the other
        // windows have them.
        std::unique_ptr<panel> side_panel(std::string title, surface s)
        {
            return std::make_unique<panel>(std::move(title), s, 6.0f, insets::uniform(8.0f));
        }

        // The status marker beside the status text in the details.
        class status_marker final : public widget
        {
        public:
            explicit status_marker(std::optional<library::part_status> status) : m_status(status)
            {
                set_hit_testable(false);
            }

        protected:
            math::vec2f on_measure(const constraints &) override { return {14.0f, 14.0f}; }
            void on_paint(painter &p) override { paint_status_marker(p, bounds(), m_status); }

        private:
            std::optional<library::part_status> m_status;
        };

        std::string project_display_name(const library::project &p, const std::filesystem::path &file)
        {
            if (!p.name.empty())
                return p.name;
            return library::path_to_utf8(file.stem());
        }
    } // namespace

    library_browser::library_browser(app_services &services)
        : window_content(services), m_part_columns(default_part_columns()),
          m_project_columns(default_project_columns()),
          m_searcher([&services](std::function<void()> task) { services.post(std::move(task)); })
    {
    }

    library_browser::~library_browser() = default;

    // ---- building ---------------------------------------------------------------------------

    window_layout library_browser::build(window_host &host)
    {
        add_commands(host);

        window_layout layout;
        layout.file_items.push_back(command_item("Save Project", "project.save", icon::save));
        layout.file_items.back().enabled = [this] { return services().project() != nullptr; };
        layout.menus = build_menus();

        auto tabs = std::make_unique<tab_view>();
        tabs->set_name("library tabs");
        m_tabs = tabs.get();
        tabs->add_tab("Parts library", build_parts_tab(host));
        tabs->add_tab("Project library", build_project_tab(host));
        tabs->select(static_cast<int>(m_tab));
        tabs->changed.connect([this](int index) {
            m_tab = index == 1 ? tab::project : tab::parts;
            if (m_tab == tab::project && m_project_list != nullptr)
                m_project_list->focus(focus_reason::programmatic);
            else if (m_list != nullptr)
                m_list->focus(focus_reason::programmatic);
            update_counts();
        });
        layout.body = std::move(tabs);

        layout.status = [this](status_bar &bar) {
            m_count_status = &bar.add_segment("", 190.0f);
            bar.add_separator();
            m_project_status = &bar.add_segment("", 230.0f);
        };

        app_services &s = services();
        m_connections = {s.catalog_changed.connect([this] { catalog_changed(); }),
                         s.catalog_status_changed.connect([this] { catalog_status_changed(); }),
                         s.project_changed.connect([this] { project_changed(); }),
                         s.library_reveal_requested.connect([this] { take_reveal(); })};

        // Whatever happened while the window was closed.
        if (m_parts.shared_catalog() != s.catalog())
            m_parts.set_catalog(s.catalog());
        rebuild_scope_tree();
        refresh_rows(false);
        catalog_status_changed();
        refresh_project();
        take_reveal();
        return layout;
    }

    void library_browser::detach()
    {
        app_services &s = services();
        s.catalog_changed.disconnect(m_connections[0]);
        s.catalog_status_changed.disconnect(m_connections[1]);
        s.project_changed.disconnect(m_connections[2]);
        s.library_reveal_requested.disconnect(m_connections[3]);
        m_connections.clear();

        if (m_outer != nullptr)
            m_scope_width = m_outer->position();
        if (m_inner != nullptr && m_inner->second() != nullptr)
            m_details_width = m_inner->second()->bounds().width;
        if (m_project_split != nullptr && m_project_split->second() != nullptr)
            m_project_details_width = m_project_split->second()->bounds().width;
        if (m_list != nullptr)
            m_part_columns = m_list->columns();
        if (m_project_list != nullptr)
            m_project_columns = m_project_list->columns();

        // The timer dies with the screen.
        m_search_timer = 0;

        m_tabs = nullptr;
        m_outer = nullptr;
        m_inner = nullptr;
        m_project_split = nullptr;
        m_banner = nullptr;
        m_banner_text = nullptr;
        m_banner_button = nullptr;
        m_scope_tree = nullptr;
        m_search_field = nullptr;
        m_result_count = nullptr;
        m_list = nullptr;
        m_details = nullptr;
        m_add = nullptr;
        m_copy = nullptr;
        m_show_in_project = nullptr;
        m_edit = nullptr;
        m_duplicate = nullptr;
        m_delete = nullptr;
        m_add_hint = nullptr;
        m_count_status = nullptr;
        m_project_status = nullptr;
        m_project_title = nullptr;
        m_project_file = nullptr;
        m_save = nullptr;
        m_project_list = nullptr;
        m_project_details = nullptr;
        m_update = nullptr;
        m_remove = nullptr;
        m_reveal = nullptr;
        m_project_edit = nullptr;
        m_project_hint = nullptr;
    }

    void library_browser::drop(std::span<const std::string> paths, math::vec2i position)
    {
        if (host() != nullptr && part_editor_drop(*host(), paths))
            return;
        window_content::drop(paths, position);
    }

    void library_browser::add_commands(window_host &window)
    {
        ui::screen &s = window.screen();
        s.add_command("library.find", [this] {
            show_tab(tab::parts);
            if (m_search_field != nullptr)
            {
                m_search_field->focus(focus_reason::keyboard);
                m_search_field->select_all();
            }
        });
        s.add_command("library.clear", [this] {
            if (m_tab != tab::parts || m_search_field == nullptr)
                return;
            if (!m_search.empty())
            {
                m_search_field->set_text("");
                search_edited("");
            }
        });
        s.add_command("library.add", [this] { add_selected(); });
        s.add_command("library.copy_mpn", [this] { copy_mpn(); });
        s.add_command("library.rescan", [this] {
            services().rescan_catalog();
            if (host() != nullptr)
                host()->show_message("Scanning the parts libraries again\xE2\x80\xA6");
        });
        s.add_command("library.problems", [this] {
            const auto &report = services().catalog_report();
            if (host() == nullptr || !report)
                return;
            std::vector<std::string> lines;
            for (const library::scan_report::file &f : report->files)
            {
                const std::string file = library::path_to_utf8(f.path.filename());
                if (f.shadowed)
                    lines.push_back(file + ": " + f.error);
                else if (!f.loaded)
                    lines.push_back(file + ": not loaded: " + f.error);
                for (const library::load_report::problem &problem : f.report.problems)
                {
                    std::string where = file;
                    if (problem.part != library::load_report::problem::no_part)
                        where += std::format(", part {}", problem.part + 1);
                    if (!problem.id.empty())
                        where += " (" + problem.id + ")";
                    lines.push_back(where + ": " + problem.message + (problem.skipped ? " (left out)" : ""));
                }
            }
            if (lines.empty())
                lines.push_back("No problems: every library loaded as it is.");
            std::string summary = std::format("{} librar{} with {} parts from {} file{}.", report->libraries,
                                              report->libraries == 1 ? "y" : "ies", grouped(report->parts),
                                              report->files.size(), report->files.size() == 1 ? "" : "s");
            open_report_dialog(*host(), "Parts library scan", std::move(summary), std::move(lines));
        });
        s.add_command("project.save", [this] { save_project(); });
        s.add_command("project.update", [this] { update_from_source(); });
        s.add_command("project.remove", [this] {
            if (m_tab == tab::project)
                remove_selected();
        });
        s.add_command("project.reveal", [this] { reveal_selected(); });
        s.add_command("part.new", [this] { new_part(); });
        s.add_command("part.edit", [this] {
            // The tab's part: a catalog part, or the project's.
            if (m_tab == tab::parts)
            {
                edit_selected();
                return;
            }
            if (host() != nullptr && selected_project_row() != nullptr)
                open_edit_project_part(*host(), m_selected_project);
        });
        s.add_command("part.duplicate", [this] { duplicate_selected(); });
        s.add_command("part.delete", [this] { delete_selected(); });

        shortcut_table &keys = s.shortcuts();
        (void)keys.bind("Ctrl+F", "library.find");
        (void)keys.bind("Escape", "library.clear");
        (void)keys.bind("Ctrl+Shift+C", "library.copy_mpn");
        (void)keys.bind("F5", "library.rescan");
        (void)keys.bind("Ctrl+S", "project.save");
        (void)keys.bind("Delete", "project.remove");
        (void)keys.bind("F2", "part.edit");
    }

    std::vector<window_layout::menu> library_browser::build_menus()
    {
        std::vector<menu_item> sort;
        menu_item relevance = action_item(
            "Relevance", [this] { sort_by(std::nullopt, true); }, [this] { return !m_parts.sort_column(); });
        sort.push_back(std::move(relevance));
        sort.push_back(menu_item::separator());
        for (std::size_t i = 0; i < part_column_count; ++i)
        {
            const auto column = static_cast<part_column>(i);
            sort.push_back(action_item(
                std::string(column_title(column)),
                [this, column] {
                    const bool again = m_parts.sort_column() == column;
                    sort_by(column, again ? !m_parts.sort_ascending() : true);
                },
                [this, column] { return m_parts.sort_column() == column; }));
        }

        std::vector<menu_item> library;
        library.push_back(command_item("Find Part", "library.find", icon::zoom_in));
        library.push_back(command_item("Clear Search", "library.clear"));
        library.push_back(submenu_item("Sort By", std::move(sort)));
        library.push_back(menu_item::separator());
        library.push_back(command_item("Add to Project", "library.add", icon::plus));
        library.back().enabled = [this] {
            const library::part *p = selected_part();
            return p != nullptr && services().project() != nullptr && services().project_part_of(m_selected).empty();
        };
        library.push_back(command_item("Copy MPN", "library.copy_mpn"));
        library.back().enabled = [this] {
            const library::part *p = selected_part();
            return p != nullptr && !p->mpn.empty();
        };
        library.push_back(menu_item::separator());
        library.push_back(command_item("New Part\xE2\x80\xA6", "part.new", icon::plus));
        library.push_back(command_item("Edit Part\xE2\x80\xA6", "part.edit"));
        library.back().enabled = [this] { return m_tab == tab::parts && selected_part() != nullptr; };
        library.push_back(command_item("Duplicate Part\xE2\x80\xA6", "part.duplicate"));
        library.back().enabled = [this] { return m_tab == tab::parts && selected_part() != nullptr; };
        library.push_back(command_item("Delete Part\xE2\x80\xA6", "part.delete"));
        library.back().enabled = [this] { return m_tab == tab::parts && selected_writable_library() != nullptr; };
        library.push_back(menu_item::separator());
        library.push_back(command_item("Rescan Libraries", "library.rescan"));
        library.back().enabled = [this] { return !services().catalog_loading(); };
        library.push_back(command_item("Scan Report\xE2\x80\xA6", "library.problems"));
        library.back().enabled = [this] { return services().catalog_report() != nullptr; };

        std::vector<menu_item> project;
        project.push_back(command_item("Save Project", "project.save", icon::save));
        project.back().enabled = [this] { return services().project() != nullptr; };
        project.push_back(menu_item::separator());
        project.push_back(command_item("Update from Source\xE2\x80\xA6", "project.update"));
        project.back().enabled = [this] {
            const project_row *row = selected_project_row();
            return row != nullptr && row->status && *row->status != library::part_status::up_to_date &&
                   *row->status != library::part_status::source_missing &&
                   *row->status != library::part_status::unlinked;
        };
        project.push_back(command_item("Remove from Project", "project.remove", icon::minus));
        project.back().enabled = [this] { return m_tab == tab::project && selected_project_row() != nullptr; };
        project.push_back(action_item("New Part in Project\xE2\x80\xA6", [this] {
            if (host() != nullptr)
                open_new_part(*host(), true);
        }));
        project.back().enabled = [this] { return services().project() != nullptr; };
        project.push_back(command_item("Edit Part\xE2\x80\xA6", "part.edit"));
        project.back().enabled = [this] { return m_tab == tab::project && selected_project_row() != nullptr; };
        project.push_back(command_item("Show in Parts Library", "project.reveal"));
        project.back().enabled = [this] {
            const project_row *row = selected_project_row();
            return row != nullptr && !row->source.empty() && services().catalog() &&
                   services().catalog()->find(row->source).has_value();
        };

        return {{"Library", std::move(library)}, {"Project", std::move(project)}};
    }

    std::unique_ptr<widget> library_browser::build_parts_tab(window_host & /*host*/)
    {
        const ui::theme t = services().theme();
        auto page = std::make_unique<column>(0.0f);
        page->set_name("parts page");

        // ---- the banner: scanning, or problems
        auto banner = std::make_unique<panel>("", surface::header, 8.0f, insets::symmetric(12.0f, 6.0f));
        banner->set_edges(false, true, false, false);
        auto &banner_row = banner->emplace<row>(10.0f);
        banner_row.set_cross_align(align::center);
        m_banner_text = &banner_row.emplace<label>("");
        m_banner_text->layout().flex = 1.0f;
        m_banner_button = &banner_row.emplace<button>("Show Problems");
        m_banner_button->set_name("library problems");
        m_banner_button->clicked.connect([this] {
            if (host() != nullptr)
                host()->screen().run_command("library.problems");
        });
        banner->set_visible(false);
        m_banner = &page->add(std::move(banner));

        auto outer = std::make_unique<splitter>(orientation::horizontal);
        outer->set_name("library outer splitter");
        outer->set_fixed(splitter::keep::first, m_scope_width);
        outer->set_minimums(160.0f, 480.0f);
        outer->layout().flex = 1.0f;
        m_outer = outer.get();

        // ---- left: libraries and categories
        auto scopes = side_panel("LIBRARIES", surface::panel);
        scopes->set_edges(false, false, false, true);
        auto &tree = scopes->emplace<tree_view>();
        tree.set_name("library scopes");
        tree.layout().flex = 1.0f;
        tree.selection_changed.connect([this](int) {
            if (m_scope_tree == nullptr)
                return;
            if (const tree_node *n = m_scope_tree->selected_node(); n != nullptr)
            {
                auto [library, category] = parse_scope_id(n->id);
                choose_scope(library, category);
            }
        });
        m_scope_tree = &tree;
        outer->set_first(std::move(scopes));

        // ---- centre and right
        auto inner = std::make_unique<splitter>(orientation::horizontal);
        inner->set_name("library inner splitter");
        inner->set_fixed(splitter::keep::second, m_details_width);
        inner->set_minimums(250.0f, 225.0f);
        m_inner = inner.get();

        auto centre = std::make_unique<column>(t.spacing, insets::uniform(t.padding));
        centre->set_name("library centre");
        auto &search_row = centre->emplace<row>(t.spacing * 1.5f);
        search_row.set_cross_align(align::center);
        auto &search = search_row.emplace<search_field>(m_search, "Search name, value, MPN, package\xE2\x80\xA6");
        search.set_name("library search");
        // The field and the count share the row, so that neither pushes
        // the other out where the pane is narrow: the count is cut first.
        search.layout().width = 0.0f;
        search.layout().flex = 3.0f;
        search.layout().min_size = {120.0f, 0.0f};
        search.set_tooltip("Search the parts (Ctrl+F). Every word must match. Escape clears.");
        search.changed.connect([this](const std::string &text) { search_edited(text); });
        search.submitted.connect([this](const std::string &) {
            // Enter in the field: the selected part goes in, or the first
            // hit is selected to start from.
            if (m_list == nullptr)
                return;
            if (m_list->selected() >= 0)
                add_selected();
            else if (m_list->row_count() > 0)
                select_row(0);
        });
        m_search_field = &search;
        m_result_count = &search_row.emplace<label>("", text_role::dim);
        m_result_count->set_name("library result count");
        m_result_count->set_align(align::end);
        m_result_count->layout().width = 0.0f;
        m_result_count->layout().flex = 1.0f;
        auto &new_button = search_row.emplace<button>("New Part", icon::plus);
        new_button.set_name("library new part");
        new_button.set_tooltip("Make a part in the part editor, for My Parts or the open project");
        new_button.clicked.connect([this] { new_part(); });

        auto &list = centre->emplace<column_view>();
        list.set_name("library list");
        list.layout().flex = 1.0f;
        list.set_columns(m_part_columns);
        list.selection_changed.connect([this](int index) { select_row(index); });
        list.activated.connect([this](int) { add_selected(); });
        list.sort_requested.connect([this](int column, bool ascending) {
            sort_by(static_cast<part_column>(column), ascending);
        });
        m_list = &list;

        // Up and Down in the search field go to the list, so that typing
        // and choosing need no switch of focus.
        search.navigate = [this](key k) {
            if (m_list == nullptr)
                return false;
            const int count = static_cast<int>(m_list->row_count());
            if (count == 0)
                return false;
            const int at = m_list->selected();
            int next = at;
            switch (k)
            {
            case key::down:
                next = std::min(count - 1, at + 1);
                break;
            case key::up:
                next = std::max(0, at - 1);
                break;
            case key::page_down:
                next = std::min(count - 1, std::max(0, at) + 10);
                break;
            case key::page_up:
                next = std::max(0, at - 10);
                break;
            default:
                return false;
            }
            m_list->set_selected(next);
            select_row(next);
            return true;
        };
        inner->set_first(std::move(centre));

        // ---- right: the selected part
        auto details = side_panel("DETAILS", surface::panel);
        details->set_edges(false, false, true, false);
        auto &scroll = details->emplace<scroll_area>(true, false);
        scroll.set_name("library details scroll");
        scroll.layout().flex = 1.0f;
        auto &content = scroll.set_content(std::make_unique<column>(t.spacing * 1.5f, insets{0.0f, 0.0f, 4.0f, 0.0f}));
        m_details = &content;

        details->emplace<separator>();
        // One action a line where the pane is narrow: Add across the
        // width, the two lesser ones sharing the next.
        m_add = &details->emplace<button>("Add to Project", icon::plus, button_kind::primary);
        m_add->set_name("library add");
        m_add->set_tooltip("Copy this part into the open project's library (Enter)");
        m_add->clicked.connect([this] { add_selected(); });
        auto &actions = details->emplace<row>(t.spacing);
        m_copy = &actions.emplace<button>("Copy MPN");
        m_copy->set_name("library copy mpn");
        m_copy->set_tooltip("Copy the manufacturer part number to the clipboard (Ctrl+Shift+C)");
        m_copy->layout().width = 0.0f;
        m_copy->layout().flex = 1.0f;
        m_copy->clicked.connect([this] { copy_mpn(); });
        m_show_in_project = &actions.emplace<button>("Show in Project");
        m_show_in_project->set_name("library show in project");
        m_show_in_project->set_tooltip("Show the project's copy in the Project library tab");
        m_show_in_project->layout().width = 0.0f;
        m_show_in_project->layout().flex = 1.0f;

        // Changing parts, in the part editor.
        auto &editing = details->emplace<row>(t.spacing);
        const auto edit_button = [&](std::string text, std::string name, std::function<void()> action) -> button & {
            auto &b = editing.emplace<button>(std::move(text));
            b.set_name(std::move(name));
            b.layout().width = 0.0f;
            b.layout().flex = 1.0f;
            b.clicked.connect(std::move(action));
            return b;
        };
        m_edit = &edit_button("Edit", "library edit part", [this] { edit_selected(); });
        m_duplicate = &edit_button("Duplicate", "library duplicate part", [this] { duplicate_selected(); });
        m_delete = &edit_button("Delete", "library delete part", [this] { delete_selected(); });

        m_add_hint = &details->emplace<label>("", text_role::dim);
        m_add_hint->set_wrap(true, 3);
        m_add_hint->set_size(text_size::small);
        m_show_in_project->clicked.connect([this] {
            const std::string id = services().project_part_of(m_selected);
            if (id.empty())
                return;
            m_selected_project = id;
            show_tab(tab::project);
            refresh_project();
            if (m_project_list != nullptr)
                m_project_list->focus(focus_reason::keyboard);
        });
        inner->set_second(std::move(details));

        outer->set_second(std::move(inner));
        page->add(std::move(outer));
        return page;
    }

    std::unique_ptr<widget> library_browser::build_project_tab(window_host & /*host*/)
    {
        const ui::theme t = services().theme();
        auto page = std::make_unique<column>(0.0f);
        page->set_name("project page");

        // ---- which project
        auto header = std::make_unique<panel>("", surface::header, 0.0f, insets::symmetric(12.0f, 8.0f));
        header->set_edges(false, true, false, false);
        auto &header_row = header->emplace<row>(12.0f);
        header_row.set_cross_align(align::center);
        auto &names = header_row.emplace<column>(2.0f);
        names.layout().flex = 1.0f;
        m_project_title = &names.emplace<label>("");
        m_project_file = &names.emplace<label>("", text_role::dim);
        m_project_file->set_size(text_size::small);
        auto &new_button = header_row.emplace<button>("New Part", icon::plus);
        new_button.set_name("project new part");
        new_button.set_tooltip("Make a part for this project's library in the part editor");
        new_button.clicked.connect([this] { new_part(); });
        m_save = &header_row.emplace<button>("Save Project", icon::save);
        m_save->set_name("project save");
        m_save->set_tooltip("Write the project and its parts library (Ctrl+S)");
        m_save->clicked.connect([this] { save_project(); });
        page->add(std::move(header));

        auto split = std::make_unique<splitter>(orientation::horizontal);
        split->set_name("project splitter");
        split->set_fixed(splitter::keep::second, m_project_details_width);
        split->set_minimums(300.0f, 225.0f);
        split->layout().flex = 1.0f;
        m_project_split = split.get();

        auto centre = std::make_unique<column>(t.spacing, insets::uniform(t.padding));
        auto &list = centre->emplace<column_view>();
        list.set_name("project list");
        list.layout().flex = 1.0f;
        list.set_columns(m_project_columns);
        list.set_cell_painter([this](painter &p, std::size_t row, std::size_t column, const rect &area) {
            if (column != marker_column || row >= m_project_rows.size())
                return false;
            paint_status_marker(p, area, m_project_rows[row].status);
            return true;
        });
        list.selection_changed.connect([this](int index) { select_project_row(index); });
        list.activated.connect([this](int) { reveal_selected(); });
        m_project_list = &list;
        split->set_first(std::move(centre));

        auto details = side_panel("DETAILS", surface::panel);
        details->set_edges(false, false, true, false);
        auto &scroll = details->emplace<scroll_area>(true, false);
        scroll.layout().flex = 1.0f;
        auto &content = scroll.set_content(std::make_unique<column>(t.spacing * 1.5f, insets{0.0f, 0.0f, 4.0f, 0.0f}));
        m_project_details = &content;

        details->emplace<separator>();
        m_update = &details->emplace<button>("Update from Source");
        m_update->set_name("project update");
        m_update->set_tooltip("Replace the project's copy with the part as the library has it now");
        m_update->clicked.connect([this] { update_from_source(); });
        auto &actions = details->emplace<row>(t.spacing);
        m_project_edit = &actions.emplace<button>("Edit");
        m_project_edit->set_name("project edit part");
        m_project_edit->set_tooltip("Change the project's copy in the part editor (F2)");
        m_project_edit->layout().width = 0.0f;
        m_project_edit->layout().flex = 1.0f;
        m_project_edit->clicked.connect([this] {
            if (host() != nullptr && selected_project_row() != nullptr)
                open_edit_project_part(*host(), m_selected_project);
        });
        m_remove = &actions.emplace<button>("Remove", icon::minus);
        m_remove->set_name("project remove");
        m_remove->set_tooltip("Take the part out of the project's library (Delete)");
        m_remove->layout().width = 0.0f;
        m_remove->layout().flex = 1.0f;
        m_remove->clicked.connect([this] { remove_selected(); });
        m_reveal = &actions.emplace<button>("Show Source");
        m_reveal->set_name("project reveal");
        m_reveal->set_tooltip("Show the part it was copied from in the Parts library tab (Enter)");
        m_reveal->layout().width = 0.0f;
        m_reveal->layout().flex = 1.0f;
        m_reveal->clicked.connect([this] { reveal_selected(); });
        m_project_hint = &details->emplace<label>("", text_role::dim);
        m_project_hint->set_wrap(true, 3);
        m_project_hint->set_size(text_size::small);
        split->set_second(std::move(details));

        page->add(std::move(split));
        return page;
    }

    // ---- the parts library tab --------------------------------------------------------------------

    void library_browser::catalog_changed()
    {
        m_parts.set_catalog(services().catalog());

        // The library chosen may have gone with the rescan.
        const part_filter &f = m_parts.filter();
        if (!f.library.empty() && (m_parts.catalog() == nullptr || m_parts.catalog()->find_library(f.library) == nullptr))
            choose_scope({}, {});

        rebuild_scope_tree();
        refresh_rows(true);
        refresh_project(); // statuses are against the catalog
    }

    void library_browser::catalog_status_changed()
    {
        app_services &s = services();
        if (m_banner == nullptr)
            return;

        const auto &report = s.catalog_report();
        std::size_t problems = 0;
        std::size_t not_loaded = 0;
        if (report)
        {
            for (const library::scan_report::file &f : report->files)
            {
                not_loaded += f.loaded || f.shadowed ? 0 : 1;
                problems += f.report.problems.size();
            }
        }

        if (s.catalog_loading())
        {
            m_banner_text->set_text(s.scan_total() > 0
                                        ? std::format("Scanning the parts libraries\xE2\x80\xA6 {} of {} files",
                                                      s.scan_done(), s.scan_total())
                                        : std::string("Scanning the parts libraries\xE2\x80\xA6"));
            m_banner_button->set_visible(false);
            m_banner->set_visible(true);
        }
        else if (report && (problems > 0 || not_loaded > 0))
        {
            std::string text;
            if (not_loaded > 0)
                text = std::format("{} librar{} could not be loaded", not_loaded, not_loaded == 1 ? "y" : "ies");
            if (problems > 0)
                text += std::format("{}{} problem{} in the libraries (those parts may be incomplete or left out)",
                                    text.empty() ? "" : "; ", problems, problems == 1 ? "" : "s");
            m_banner_text->set_text(std::move(text));
            m_banner_button->set_visible(true);
            m_banner->set_visible(true);
        }
        else
        {
            m_banner->set_visible(false);
        }
        refresh_rows(true);
    }

    void library_browser::search_edited(const std::string &text)
    {
        m_search = text;
        ui::screen *s = host() != nullptr ? &host()->screen() : nullptr;
        if (s != nullptr && m_search_timer != 0)
        {
            s->cancel(m_search_timer);
            m_search_timer = 0;
        }

        // A large catalog is searched on the search thread, so that typing
        // never waits for it: in a Debug build a search of 50 000 parts
        // takes tens of milliseconds, the first letter's (most parts match)
        // over a hundred.
        if (part_search::wanted(m_parts))
        {
            search_in_background();
            return;
        }
        const double last = m_parts.search_milliseconds() + m_parts.sort_milliseconds();
        if (last < slow_search || s == nullptr)
        {
            apply_search();
            return;
        }
        m_search_timer = s->schedule_after(search_pause, [this] {
            m_search_timer = 0;
            apply_search();
        });
    }

    void library_browser::search_in_background()
    {
        part_filter f = m_parts.filter();
        f.text = m_search;
        m_searcher.start(m_parts.shared_catalog(), std::move(f),
                         [this](const std::shared_ptr<const library::catalog> &catalog, part_filter filter,
                                std::vector<library::part_ref> ranked, double ms) {
                             finish_search(catalog, std::move(filter), std::move(ranked), ms);
                         });
    }

    void library_browser::finish_search(const std::shared_ptr<const library::catalog> &catalog, part_filter filter,
                                        std::vector<library::part_ref> ranked, double milliseconds)
    {
        const bool started = m_parts.filter().text.empty() && !filter.text.empty();
        if (started && m_parts.sort_column())
            m_parts.set_sort(std::nullopt, true);
        if (!m_parts.set_results(catalog, std::move(filter), std::move(ranked), milliseconds))
            return;
        refresh_rows(false);
        if (log_timing())
            std::cerr << std::format("library search \"{}\" (search thread): {} hits, search {:.2f} ms, sort {:.2f} ms",
                                     m_parts.filter().text, m_parts.size(), m_parts.search_milliseconds(),
                                     m_parts.sort_milliseconds())
                      << std::endl;
    }

    void library_browser::apply_search()
    {
        part_filter f = m_parts.filter();
        const bool started = f.text.empty() && !m_search.empty();
        f.text = m_search;
        // A new search is read best first; a column chosen before goes.
        if (started && m_parts.sort_column())
            m_parts.set_sort(std::nullopt, true);
        if (!m_parts.set_filter(std::move(f)))
            return;
        refresh_rows(false);
        if (log_timing())
            std::cerr << std::format("library search \"{}\": {} hits, search {:.2f} ms, sort {:.2f} ms", m_search,
                                     m_parts.size(), m_parts.search_milliseconds(), m_parts.sort_milliseconds())
                      << std::endl;
    }

    void library_browser::refresh_rows(bool keep_scroll)
    {
        if (m_list == nullptr)
            return;
        app_services &s = services();
        const float scroll = m_list->scroll_offset();
        m_list->set_rows(m_parts.size(), [this](std::size_t row, std::size_t column) {
            const auto c = static_cast<part_column>(column);
            return c == part_column::value ? m_parts.specs(row) : m_parts.cell(row, c);
        });
        m_list->set_sort(m_parts.sort_column() ? static_cast<int>(*m_parts.sort_column()) : -1,
                         m_parts.sort_ascending());

        const auto row = m_parts.find(m_selected);
        m_list->set_selected(row ? static_cast<int>(*row) : -1);
        if (!row)
            m_list->scroll_to(keep_scroll ? scroll : 0.0f);

        // What an empty list says.
        std::string empty;
        if (m_parts.catalog() == nullptr)
            empty = s.catalog_loading() ? "Loading the parts library\xE2\x80\xA6" : "The parts library is not loaded.";
        else if (m_parts.total() == 0)
        {
            const std::filesystem::path user = app_services::user_library_directory();
            empty = "No parts libraries were found. Put .voltlib files in " +
                    (user.empty() ? std::string("a library directory") : library::path_to_utf8(user)) +
                    ", drop one on a window, or start Volt with --library-dir.";
        }
        else if (!m_parts.filter().text.empty())
            empty = std::format("No parts match \xE2\x80\x9C{}\xE2\x80\x9D{}. Press Escape to clear the search{}.",
                                m_parts.filter().text, m_parts.filter().library.empty() ? "" : " in this library",
                                m_parts.filter().library.empty() && m_parts.filter().category.empty()
                                    ? ""
                                    : ", or choose All libraries");
        else
            empty = "This category has no parts.";
        m_list->set_empty_text(std::move(empty));

        update_counts();
        show_part_details();
    }

    void library_browser::update_counts()
    {
        std::string text;
        if (m_parts.catalog() == nullptr)
            text = services().catalog_loading() ? "Loading\xE2\x80\xA6" : "";
        else if (m_parts.size() == m_parts.total())
            text = std::format("{} parts", grouped(m_parts.total()));
        else
            text = std::format("{} of {} parts", grouped(m_parts.size()), grouped(m_parts.total()));
        if (m_result_count != nullptr)
            m_result_count->set_text(text);

        if (m_count_status != nullptr)
        {
            if (m_tab == tab::project)
            {
                const library::project *p = services().project();
                m_count_status->set_text(p == nullptr ? std::string("No project")
                                                      : std::format("{} project part{}", m_project_rows.size(),
                                                                    m_project_rows.size() == 1 ? "" : "s"));
            }
            else
            {
                std::string order = "catalog order";
                if (m_parts.sort_column())
                    order = std::format("by {}", column_title(*m_parts.sort_column()));
                else if (!m_parts.filter().text.empty())
                    order = "by relevance";
                m_count_status->set_text(text.empty() ? text : text + ", " + order);
            }
        }

        if (m_project_status != nullptr)
        {
            const library::project *p = services().project();
            if (p == nullptr)
                m_project_status->set_text("No project open");
            else
                m_project_status->set_text(std::format("Project {}{}", project_display_name(*p, services().project_path()),
                                                       services().project_modified() ? " \xE2\x80\x94 not saved" : ""));
        }
    }

    void library_browser::rebuild_scope_tree()
    {
        if (m_scope_tree == nullptr)
            return;
        if (m_parts.catalog() == nullptr)
        {
            m_scope_tree->set_roots({});
            return;
        }
        std::vector<tree_node> nodes = scope_nodes(scope_tree(*m_parts.catalog()));

        // Open the way to the chosen scope, and select it.
        const std::string wanted = scope_id(m_parts.filter().library, m_parts.filter().category);
        const auto open_to = [&](auto &&self, std::vector<tree_node> &list) -> bool {
            for (tree_node &n : list)
            {
                if (n.id == wanted)
                    return true;
                if (self(self, n.children))
                {
                    n.expanded = true;
                    return true;
                }
            }
            return false;
        };
        open_to(open_to, nodes);
        m_scope_tree->set_roots(std::move(nodes));
        for (std::size_t i = 0; i < m_scope_tree->row_count(); ++i)
        {
            if (m_scope_tree->node(i).id == wanted)
            {
                m_scope_tree->set_selected(static_cast<int>(i));
                break;
            }
        }
    }

    void library_browser::choose_scope(const std::string &library, const std::string &category)
    {
        part_filter f = m_parts.filter();
        f.library = library;
        f.category = category;
        if (m_parts.set_filter(std::move(f)))
            refresh_rows(false);
    }

    void library_browser::sort_by(std::optional<part_column> column, bool ascending)
    {
        m_parts.set_sort(column, ascending);
        refresh_rows(false);
        // The selection stays where it is in the new order, in view.
        if (m_list != nullptr && m_list->selected() >= 0)
            m_list->ensure_visible(m_list->selected());
    }

    void library_browser::select_row(int row)
    {
        if (row < 0 || static_cast<std::size_t>(row) >= m_parts.size())
        {
            // Deselected (a press below the rows): nothing is chosen.
            m_selected.clear();
            show_part_details();
            return;
        }
        m_selected = m_parts.qualified_id(static_cast<std::size_t>(row));
        show_part_details();
    }

    const library::part *library_browser::selected_part() const
    {
        const library::catalog *c = m_parts.catalog();
        if (c == nullptr || m_selected.empty())
            return nullptr;
        const auto ref = c->find(m_selected);
        return ref ? &c->get(*ref) : nullptr;
    }

    void library_browser::show_part_details()
    {
        if (m_details == nullptr)
            return;
        m_details->clear_children();

        const library::catalog *c = m_parts.catalog();
        const library::part *p = selected_part();
        if (p == nullptr)
        {
            auto &none = m_details->emplace<label>(c == nullptr ? std::string()
                                                                : std::string("Select a part to see its fields, "
                                                                              "parameters, datasheet and 3D model."),
                                                   text_role::dim);
            none.set_wrap(true);
            update_part_actions();
            return;
        }

        const library::library &lib = c->library_of(*c->find(m_selected));
        auto &name = m_details->emplace<label>(p->name.empty() ? p->id : p->name);
        name.set_wrap(true, 3);
        auto &id = m_details->emplace<label>(m_selected, text_role::dim);
        id.set_size(text_size::small);

        std::string source(lib.display_name());
        if (!lib.version.empty())
            source += " " + lib.version;
        add_part_details(*m_details, *p, source, services().models_directory(),
                         [this](const std::string &url) { services().open_link(url, kind()); }, &lib);
        update_part_actions();
    }

    void library_browser::update_part_actions()
    {
        if (m_add == nullptr)
            return;
        app_services &s = services();
        const library::part *p = selected_part();
        const std::string in_project = p != nullptr ? s.project_part_of(m_selected) : std::string();

        std::string hint;
        bool can_add = false;
        if (p == nullptr)
            hint = "";
        else if (s.project() == nullptr)
            hint = "No project is open. Create or open one in the Project Manager (Ctrl+1) to add parts to it.";
        else if (!in_project.empty())
            hint = std::format("Already in the project\xE2\x80\x99s library, as {}.", in_project);
        else
        {
            can_add = true;
            hint = std::format("Adds a copy to {}\xE2\x80\x99s library, linked to this source.",
                               project_display_name(*s.project(), s.project_path()));
        }
        m_add->set_text(in_project.empty() ? "Add to Project" : "In Project");
        m_add->set_icon(in_project.empty() ? icon::plus : icon::check);
        m_add->set_enabled(can_add);
        m_add->set_tooltip(can_add ? "Copy this part into the open project's library (Enter)" : hint);
        m_copy->set_enabled(p != nullptr && !p->mpn.empty());
        m_copy->set_tooltip(p != nullptr && p->mpn.empty() ? "This part has no MPN"
                                                           : "Copy the manufacturer part number to the clipboard "
                                                             "(Ctrl+Shift+C)");
        m_add_hint->set_text(std::move(hint));
        m_add_hint->set_visible(p != nullptr);
        m_show_in_project->set_enabled(!in_project.empty());

        // Edit and Delete change the library file itself, which a library
        // shipped with Volt does not allow: Edit then offers a duplicate.
        const bool writable = p != nullptr && selected_writable_library() != nullptr;
        m_edit->set_enabled(p != nullptr);
        m_edit->set_tooltip(p == nullptr  ? std::string()
                            : writable    ? std::string("Change this part in its library file (F2)")
                                          : std::string("This library ships with Volt: Edit offers to duplicate the "
                                                        "part into My Parts and edit the copy"));
        m_duplicate->set_enabled(p != nullptr);
        m_duplicate->set_tooltip("Copy this part into My Parts (or the open project) as a new part, and edit the "
                                 "copy");
        m_delete->set_enabled(writable);
        m_delete->set_tooltip(p == nullptr ? std::string()
                              : writable   ? std::string("Delete this part from its library file (asks first)")
                                           : std::string("A library shipped with Volt cannot be changed"));
    }

    // ---- making and changing parts -------------------------------------------------------------------

    const library::library *library_browser::selected_writable_library() const
    {
        const library::catalog *c = m_parts.catalog();
        if (c == nullptr || m_selected.empty())
            return nullptr;
        const auto ref = c->find(m_selected);
        if (!ref)
            return nullptr;
        const library::library &lib = c->library_of(*ref);
        return is_writable_library(lib.path, services().assets_directory()) ? &lib : nullptr;
    }

    void library_browser::new_part()
    {
        if (host() != nullptr)
            open_new_part(*host(), m_tab == tab::project && services().project() != nullptr);
    }

    void library_browser::edit_selected()
    {
        const library::part *p = selected_part();
        const library::catalog *c = m_parts.catalog();
        if (p == nullptr || c == nullptr || host() == nullptr)
            return;
        if (const library::library *lib = selected_writable_library(); lib != nullptr)
        {
            open_edit_part(*host(), *lib, *p);
            return;
        }
        const library::library &lib = c->library_of(*c->find(m_selected));
        open_choice_dialog(*host(), std::format("{} ships with Volt", lib.display_name()),
                           std::format("Its parts are not changed in place: the next version of Volt replaces the "
                                       "library, and the change would be lost. Duplicate {} into My Parts and edit "
                                       "the copy there instead; the copy remembers which part it came from.",
                                       p->name.empty() ? p->id : p->name),
                           {{"Duplicate into My Parts", button_kind::primary, [this] { duplicate_selected(); }},
                            {"Cancel", button_kind::normal, {}}});
    }

    void library_browser::duplicate_selected()
    {
        const library::part *p = selected_part();
        const library::catalog *c = m_parts.catalog();
        if (p == nullptr || c == nullptr || host() == nullptr)
            return;
        open_duplicate_part(*host(), c->library_of(*c->find(m_selected)), *p);
    }

    void library_browser::delete_selected()
    {
        const library::part *p = selected_part();
        if (p == nullptr || host() == nullptr)
            return;
        const library::library *lib = selected_writable_library();
        if (lib == nullptr)
        {
            host()->show_message("This part's library ships with Volt and cannot be changed", std::chrono::seconds(6),
                                 log_level::warning);
            return;
        }
        confirm_delete_part(*host(), *lib, *p);
    }

    void library_browser::add_selected()
    {
        app_services &s = services();
        const library::part *p = selected_part();
        if (p == nullptr || host() == nullptr)
            return;
        if (s.project() == nullptr)
        {
            host()->show_message("No project is open: create or open one in the Project Manager first",
                                 std::chrono::seconds(6), log_level::warning);
            return;
        }
        if (const std::string there = s.project_part_of(m_selected); !there.empty())
        {
            host()->show_message("Already in the project as " + there);
            return;
        }
        auto added = s.add_to_project(m_selected);
        if (!added)
        {
            host()->show_message("Not added: " + added.error(), std::chrono::seconds(8), log_level::error);
            return;
        }
        host()->show_message(std::format("Added {} to the project as {} (not saved yet: Ctrl+S)",
                                         p->name.empty() ? p->id : p->name, *added),
                             std::chrono::seconds(6));
    }

    void library_browser::copy_mpn()
    {
        const library::part *p = selected_part();
        if (p == nullptr || p->mpn.empty() || host() == nullptr)
            return;
        ui::clipboard &board = services().system_clipboard();
        board.set_text(p->mpn);
        host()->show_message("Copied " + p->mpn + " to the clipboard");
    }

    // ---- the project library tab --------------------------------------------------------------------

    void library_browser::project_changed()
    {
        refresh_project();
        update_part_actions();
        update_counts();
    }

    void library_browser::refresh_project()
    {
        app_services &s = services();
        const library::project *p = s.project();
        m_project_rows = p != nullptr ? project_rows(*p, s.catalog().get()) : std::vector<project_row>{};

        if (m_project_title != nullptr)
        {
            if (p == nullptr)
            {
                m_project_title->set_text("No project open");
                m_project_file->set_text("Projects are created and opened in the Project Manager (Ctrl+1).");
            }
            else
            {
                m_project_title->set_text(project_display_name(*p, s.project_path()) +
                                          (s.project_modified() ? "  \xE2\x80\x94  modified" : ""));
                m_project_file->set_text(library::path_to_utf8(s.project_path()) + "  \xC2\xB7  library " +
                                         p->library_file);
            }
            m_save->set_enabled(p != nullptr && s.project_modified());
            m_save->set_tooltip(p == nullptr            ? "No project is open"
                                : s.project_modified() ? "Write the project and its parts library (Ctrl+S)"
                                                       : "Saved: nothing has changed since");
        }

        if (m_project_list != nullptr)
        {
            m_project_list->set_rows(m_project_rows.size(), [this](std::size_t row, std::size_t column) -> std::string_view {
                const project_row &r = m_project_rows[row];
                switch (column)
                {
                case name_column:
                    return r.name;
                case value_column:
                    return r.value;
                case package_column:
                    return r.package;
                case source_column:
                    return r.source.empty() ? std::string_view("(made in the project)") : std::string_view(r.source);
                case status_column:
                    return r.status_text;
                default:
                    return {};
                }
            });
            if (p == nullptr)
                m_project_list->set_empty_text("No project is open.\nCreate or open one in the Project Manager "
                                               "(Window > Project Manager, Ctrl+1); its parts show here.");
            else
                m_project_list->set_empty_text("This project has no parts yet.\nFind them in the Parts library tab "
                                               "and choose Add to Project, or press Enter on one.");

            int selected = -1;
            for (std::size_t i = 0; i < m_project_rows.size(); ++i)
            {
                if (m_project_rows[i].id == m_selected_project)
                    selected = static_cast<int>(i);
            }
            m_project_list->set_selected(selected);
        }
        show_project_details();
        update_counts();
    }

    const project_row *library_browser::selected_project_row() const
    {
        for (const project_row &r : m_project_rows)
        {
            if (r.id == m_selected_project)
                return &r;
        }
        return nullptr;
    }

    void library_browser::select_project_row(int row)
    {
        if (row < 0 || static_cast<std::size_t>(row) >= m_project_rows.size())
        {
            m_selected_project.clear();
            show_project_details();
            return;
        }
        m_selected_project = m_project_rows[static_cast<std::size_t>(row)].id;
        show_project_details();
    }

    void library_browser::show_project_details()
    {
        if (m_project_details == nullptr)
            return;
        m_project_details->clear_children();

        app_services &s = services();
        const library::project *p = s.project();
        const project_row *row = selected_project_row();
        const library::part *part = p != nullptr && row != nullptr ? p->parts.find(row->id) : nullptr;

        std::string hint;
        if (part == nullptr)
        {
            if (p != nullptr && !m_project_rows.empty())
            {
                auto &none = m_project_details->emplace<label>("Select a part to see it and compare it with its "
                                                               "source.",
                                                               text_role::dim);
                none.set_wrap(true);
            }
        }
        else
        {
            auto &name = m_project_details->emplace<label>(row->name);
            name.set_wrap(true, 3);

            auto &status = m_project_details->emplace<ui::row>(6.0f);
            status.set_cross_align(align::center);
            status.emplace<status_marker>(row->status);
            auto &status_text = status.emplace<label>(row->status_text, text_role::dim);
            status_text.layout().flex = 1.0f;

            std::string explain;
            if (row->status)
            {
                switch (*row->status)
                {
                case library::part_status::unlinked:
                    explain = "Made in this project; it has no source to compare with.";
                    break;
                case library::part_status::source_missing:
                    explain = "Its source, " + row->source +
                              ", is not in the parts library now. The project keeps its own copy.";
                    break;
                case library::part_status::up_to_date:
                    explain = "The same as its source in the parts library.";
                    break;
                case library::part_status::source_changed:
                    explain = "The library's part has changed since it was copied. Update from Source takes the "
                              "new version.";
                    break;
                case library::part_status::locally_modified:
                    explain = "Edited in this project since it was copied; the source has not changed.";
                    break;
                case library::part_status::diverged:
                    explain = "Edited in this project, and the library's part has changed too. Updating loses "
                              "the project's edits.";
                    break;
                }
            }
            else
            {
                explain = "Compared with its source once the parts library has loaded.";
            }
            auto &why = m_project_details->emplace<label>(std::move(explain), text_role::dim);
            why.set_wrap(true, 4);

            add_part_details(*m_project_details, *part, {}, s.models_directory(),
                             [this](const std::string &url) { services().open_link(url, kind()); }, &p->parts);
        }

        // The actions, and why one cannot be used.
        const bool linked = row != nullptr && row->status && *row->status != library::part_status::unlinked &&
                            *row->status != library::part_status::source_missing;
        const bool can_update = linked && *row->status != library::part_status::up_to_date;
        const bool in_catalog = row != nullptr && !row->source.empty() && s.catalog() &&
                                s.catalog()->find(row->source).has_value();
        if (m_update != nullptr)
        {
            m_update->set_enabled(can_update);
            m_remove->set_enabled(row != nullptr);
            m_project_edit->set_enabled(row != nullptr);
            m_reveal->set_enabled(in_catalog);
            if (row == nullptr)
                hint = "";
            else if (!row->status)
                hint = "Waiting for the parts library to load to compare.";
            else if (*row->status == library::part_status::up_to_date)
                hint = "Nothing to update: it is the same as its source.";
            else if (*row->status == library::part_status::source_missing)
                hint = "Cannot update: the source is not in the parts library.";
            else if (*row->status == library::part_status::unlinked)
                hint = "Cannot update: it was not copied from a library.";
            else if (update_loses_edits(*row->status))
                hint = "Updating replaces the edits made in this project; you will be asked first.";
            if (!hint.empty() && s.project_modified())
                hint += " Changes are not saved until Save Project (Ctrl+S).";
            else if (hint.empty() && s.project_modified())
                hint = "Changes are not saved until Save Project (Ctrl+S).";
            m_project_hint->set_text(std::move(hint));
        }
    }

    void library_browser::update_from_source()
    {
        app_services &s = services();
        const project_row *row = selected_project_row();
        if (row == nullptr || !row->status || s.project() == nullptr || !s.catalog() || host() == nullptr)
            return;
        const std::string id = row->id;
        const std::string name = row->name;
        const auto run = [this, id, name] {
            app_services &services_ = services();
            if (services_.project() == nullptr || !services_.catalog())
                return;
            auto updated = library::update_from_source(*services_.project(), id, *services_.catalog());
            if (host() == nullptr)
                return;
            if (!updated)
            {
                host()->show_message("Not updated: " + updated.error(), std::chrono::seconds(8), log_level::error);
                return;
            }
            services_.mark_project_modified();
            host()->show_message("Updated " + name + " from its source (not saved yet: Ctrl+S)");
        };

        if (update_loses_edits(*row->status))
        {
            open_choice_dialog(*host(), std::format("Update {} from its source?", name),
                               std::format("{} was edited in this project. Updating replaces it with the library's "
                                           "current version ({}), and the edits are lost.",
                                           name, row->source),
                               {{"Update and Lose Edits", button_kind::normal, run}, {"Cancel", button_kind::primary, {}}});
            return;
        }
        run();
    }

    void library_browser::remove_selected()
    {
        app_services &s = services();
        const project_row *row = selected_project_row();
        if (row == nullptr || s.project() == nullptr || host() == nullptr)
            return;
        const std::string name = row->name;
        // The next row takes the selection, so that removing several goes
        // one key at a time.
        std::string next;
        for (std::size_t i = 0; i < m_project_rows.size(); ++i)
        {
            if (m_project_rows[i].id == row->id)
            {
                if (i + 1 < m_project_rows.size())
                    next = m_project_rows[i + 1].id;
                else if (i > 0)
                    next = m_project_rows[i - 1].id;
            }
        }
        if (!library::remove_part(*s.project(), row->id))
            return;
        m_selected_project = next;
        s.mark_project_modified();
        host()->show_message("Removed " + name + " from the project (not saved yet: Ctrl+S)");
    }

    void library_browser::reveal_selected()
    {
        const project_row *row = selected_project_row();
        if (row == nullptr || row->source.empty())
            return;
        const library::catalog *c = services().catalog().get();
        if (c == nullptr || !c->find(row->source))
        {
            if (host() != nullptr)
                host()->show_message("Its source, " + row->source + ", is not in the parts library",
                                     std::chrono::seconds(4), log_level::warning);
            return;
        }
        m_selected = row->source;
        show_tab(tab::parts);
        if (!m_parts.find(m_selected))
        {
            // Not shown under the current search or scope: show everything.
            m_search.clear();
            if (m_search_field != nullptr)
                m_search_field->set_text("");
            m_parts.set_filter({});
            rebuild_scope_tree();
        }
        refresh_rows(false);
        if (m_list != nullptr)
            m_list->focus(focus_reason::keyboard);
    }

    void library_browser::save_project()
    {
        app_services &s = services();
        if (host() == nullptr)
            return;
        if (s.project() == nullptr)
        {
            host()->show_message("No project is open");
            return;
        }
        if (auto saved = s.save_project(); !saved)
        {
            host()->show_message("Not saved: " + saved.error(), std::chrono::seconds(10), log_level::error);
            return;
        }
        host()->show_message("Saved " + library::path_to_utf8(s.project_path().filename()));
    }

    // ---- both ---------------------------------------------------------------------------------------

    void library_browser::show_tab(tab which)
    {
        m_tab = which;
        if (m_tabs != nullptr)
            m_tabs->select(static_cast<int>(which));
        update_counts();
    }

    void library_browser::take_reveal()
    {
        auto reveal = services().take_library_reveal();
        if (!reveal)
            return;
        if (reveal->project)
        {
            m_selected_project = reveal->id;
            show_tab(tab::project);
            refresh_project();
            if (m_project_list != nullptr)
                m_project_list->focus(focus_reason::keyboard);
            return;
        }

        m_selected = reveal->id;
        show_tab(tab::parts);
        if (m_parts.catalog() != nullptr && !m_parts.find(m_selected))
        {
            m_search.clear();
            if (m_search_field != nullptr)
                m_search_field->set_text("");
            m_parts.set_filter({});
            rebuild_scope_tree();
        }
        refresh_rows(false);
        if (m_list != nullptr)
            m_list->focus(focus_reason::keyboard);
    }

} // namespace volt::ui
