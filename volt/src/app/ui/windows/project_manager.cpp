#include <app/ui/windows/project_manager.hpp>

#include <app/ui/windows/library_dialogs.hpp>
#include <app/ui/windows/menus.hpp>
#include <library/catalog.hpp>
#include <library/project.hpp>
#include <ui/layout/box.hpp>
#include <ui/theme.hpp>
#include <ui/widgets/bars.hpp>
#include <ui/widgets/button.hpp>
#include <ui/widgets/checkbox.hpp>
#include <ui/widgets/column_view.hpp>
#include <ui/widgets/dropdown.hpp>
#include <ui/widgets/image_view.hpp>
#include <ui/widgets/label.hpp>
#include <ui/widgets/log_view.hpp>
#include <ui/widgets/panel.hpp>
#include <ui/widgets/property_grid.hpp>
#include <ui/widgets/scroll_area.hpp>
#include <ui/widgets/splitter.hpp>
#include <ui/widgets/text_field.hpp>
#include <ui/widgets/tile_grid.hpp>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>

namespace volt::ui
{
    namespace
    {
        // The middle dot between the parts of a tree row.
        constexpr const char *dot = "  \xC2\xB7  ";

        tree_node node(std::string text, ui::icon icon, std::string id, std::vector<tree_node> children = {},
                       bool expanded = false)
        {
            tree_node n;
            n.text = std::move(text);
            n.icon = icon;
            n.id = std::move(id);
            n.children = std::move(children);
            n.expanded = expanded;
            return n;
        }

        std::string lower(std::string s)
        {
            for (char &c : s)
                c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            return s;
        }

        // A path as the interface shows it: UTF-8, with the system's
        // separators.
        std::string shown(std::filesystem::path path)
        {
            path.make_preferred();
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        std::string plural(std::size_t count, std::string_view one, std::string_view many)
        {
            return std::format("{} {}", count, count == 1 ? one : many);
        }

        // What the tree shows: the open project's files, or the demo that
        // stands in for a project. Node ids say what activating one opens:
        // "sheet", "board", "library", "part".
        std::vector<tree_node> project_model(app_services &services)
        {
            std::vector<tree_node> roots;
            if (const library::project *p = services.project())
            {
                std::vector<tree_node> sheets;
                for (const library::sheet &sheet : p->sheets)
                    sheets.push_back(node(sheet.name + dot + sheet.file, icon::sheet, "sheet"));

                std::vector<tree_node> parts;
                for (const library::part &part : p->parts.parts)
                    parts.push_back(node(part.name.empty() ? part.id : part.id + dot + part.name, icon::component,
                                         "part"));

                std::vector<tree_node> files;
                const std::string sheets_title = std::format("Sheets ({})", sheets.size());
                files.push_back(node(sheets_title, icon::folder, "sheet", std::move(sheets), true));
                if (!p->board.empty())
                    files.push_back(node("Board" + std::string(dot) + p->board, icon::cube, "board"));
                files.push_back(node(std::format("Project library{}{} ({})", dot, p->library_file,
                                                 plural(p->parts.parts.size(), "part", "parts")),
                                     icon::component, "library", std::move(parts), false));
                roots.push_back(node(p->name, icon::folder, "", std::move(files), true));
                return roots;
            }

            // No project: the built-in demo, saying so.
            std::vector<tree_node> models;
            for (const board_document::placed_model &placed : services.board().models())
                models.push_back(node(placed.file, icon::cube, "board"));
            const bool any_models = !models.empty();

            std::vector<tree_node> files;
            files.push_back(node("Sheets", icon::folder, "sheet",
                                 {node("Ladder" + std::string(dot) + "demo sheet, not a file", icon::sheet, "sheet")},
                                 true));
            files.push_back(node(any_models ? std::format("Demo board ({} dropped)", plural(models.size(), "model",
                                                                                           "models"))
                                            : std::string("Demo board"),
                                 icon::cube, "board", std::move(models), any_models));
            const auto &catalog = services.catalog();
            files.push_back(node(catalog ? std::format("Parts catalog ({})",
                                                       plural(catalog->part_count(), "part", "parts"))
                                         : std::string("Parts catalog (loading)"),
                                 icon::component, "library"));
            roots.push_back(node("Ladder demo (built in)", icon::folder, "", std::move(files), true));
            return roots;
        }

        // The nodes whose text contains `text` (any case), with the folders
        // on the way to them, opened.
        bool filter(const tree_node &from, std::string_view text, tree_node &out)
        {
            const bool match = lower(from.text).find(lower(std::string(text))) != std::string::npos;

            out = from;
            out.children.clear();
            for (const tree_node &child : from.children)
            {
                tree_node kept;
                if (filter(child, text, kept))
                    out.children.push_back(std::move(kept));
            }
            out.expanded = !out.children.empty();
            if (match && out.children.empty())
            {
                // A match keeps what is under it, folded.
                out.children = from.children;
                out.expanded = false;
            }
            return match || !out.children.empty();
        }

        // "14:02:37", local time.
        std::string clock_text(std::chrono::system_clock::time_point when)
        {
            const std::time_t t = std::chrono::system_clock::to_time_t(when);
            std::tm local{};
#ifdef _WIN32
            if (localtime_s(&local, &t) != 0)
                return {};
#else
            if (localtime_r(&t, &local) == nullptr)
                return {};
#endif
            return std::format("{:02}:{:02}:{:02}", local.tm_hour, local.tm_min, local.tm_sec);
        }

        log_entry entry_of(const logged_message &m)
        {
            return {clock_text(m.when), std::string(window_name(m.from)), m.text, m.level, m.repeats};
        }

        // Where a new project goes unless the user says otherwise: a Volt
        // folder in the user's documents.
        std::filesystem::path default_project_folder()
        {
#ifdef _WIN32
            wchar_t *profile = nullptr;
            std::size_t length = 0;
            std::filesystem::path home;
            if (_wdupenv_s(&profile, &length, L"USERPROFILE") == 0 && profile != nullptr)
            {
                home = profile;
                std::free(profile);
            }
#else
            const char *value = std::getenv("HOME");
            const std::filesystem::path home = value != nullptr ? value : "";
#endif
            if (home.empty())
                return {};
            return home / "Documents" / "Volt";
        }

        // A path typed or pasted: without the quotes Explorer's "Copy as
        // path" puts around it, or the spaces around those.
        std::string tidy_path(std::string text)
        {
            const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
            while (!text.empty() && blank(text.front()))
                text.erase(text.begin());
            while (!text.empty() && blank(text.back()))
                text.pop_back();
            if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                text = text.substr(1, text.size() - 2);
            return text;
        }

        // The frame of a modal dialog, with its heading.
        std::unique_ptr<panel> dialog_frame(std::string name, std::string heading)
        {
            auto dialog = std::make_unique<panel>("", surface::raised, 10.0f, insets::uniform(18.0f));
            dialog->set_name(std::move(name));
            dialog->set_border(true);
            dialog->layout().width = 500.0f;
            auto &title = dialog->emplace<label>(std::move(heading));
            title.set_size(text_size::title);
            return dialog;
        }

        // A line for what went wrong in a dialog, hidden until something
        // does.
        label &error_line(panel &dialog, const ui::theme &theme)
        {
            auto &line = dialog.emplace<label>("");
            line.set_color(theme.error);
            line.set_wrap(true);
            line.set_visible(false);
            return line;
        }

        void show_error(label &line, std::string text)
        {
            line.set_text(std::move(text));
            line.set_visible(true);
        }

        widget &open_dialog(window_host &host, std::unique_ptr<panel> dialog)
        {
            popup_options options;
            options.placement = placement::centered;
            options.modal = true;
            return host.screen().open_popup(std::move(dialog), std::move(options));
        }

        // ---- history ----------------------------------------------------------------------

        // Without the spaces around it.
        std::string trimmed(std::string_view text)
        {
            const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
            while (!text.empty() && blank(text.front()))
                text.remove_prefix(1);
            while (!text.empty() && blank(text.back()))
                text.remove_suffix(1);
            return std::string(text);
        }

        // Quotation marks around a snapshot's message.
        std::string in_quotes(std::string_view text)
        {
            return "\xE2\x80\x9C" + std::string(text) + "\xE2\x80\x9D";
        }

        // a1b2c3d4 "Routed the power rails", in curly quotes.
        std::string snapshot_name(const history_entry &e)
        {
            return e.message.empty() ? e.short_id : e.short_id + " " + in_quotes(e.message);
        }

        // What putting files back did, for the message after.
        std::string restored_summary(const history::restored_files &r)
        {
            if (r.written.empty() && r.removed.empty())
                return "the files were as in it already";
            std::string text = plural(r.written.size(), "file written", "files written");
            if (!r.removed.empty())
                text += ", " + plural(r.removed.size(), "file removed", "files removed");
            return text;
        }

        std::string upper_first(std::string text)
        {
            if (!text.empty() && text[0] >= 'a' && text[0] <= 'z')
                text[0] = static_cast<char>(text[0] - 'a' + 'A');
            return text;
        }

        // A dropdown of names: the dialogs choose a branch with one.
        dropdown &branch_dropdown(widget &parent, std::vector<std::string> names, int selected)
        {
            auto &choice = parent.emplace<dropdown>(std::move(names), selected);
            choice.layout().width = 0.0f;
            choice.layout().flex = 1.0f;
            return choice;
        }

        // The snapshot list's columns.
        constexpr std::size_t head_column = 0;
        constexpr std::size_t message_column = 1;
        constexpr std::size_t when_column = 2;
        constexpr std::size_t id_column = 3;

        std::vector<table_column> snapshot_columns()
        {
            std::vector<table_column> columns(4);
            columns[head_column] = {"", 22.0f, 0.0f, align::center, false, 0};
            columns[message_column] = {"Message", 160.0f, 1.0f, align::start, false, 0};
            columns[when_column] = {"When", 118.0f, 0.0f, align::start, false, 1};
            columns[id_column] = {"Snapshot", 76.0f, 0.0f, align::start, false, 2};
            return columns;
        }
    } // namespace

    project_manager::project_manager(app_services &services) : window_content(services)
    {
        refresh_tree();
    }

    project_manager::~project_manager() = default;

    window_layout project_manager::build(window_host &host)
    {
        add_commands(host);

        // The tree follows the project, the catalog and the models on the
        // board, wherever they changed; the log every window's messages.
        app_services &s = services();
        m_models_connection = s.board().models_changed.connect([this] { refresh_tree(); });
        m_catalog_connection = s.catalog_changed.connect([this] { refresh_tree(); });
        m_project_connection = s.project_changed.connect([this] {
            refresh_tree();
            refresh_identity();
            // The files on disk change with a save, an open or a close,
            // after each of which the project is not modified; marking it
            // modified changes only what is in memory, and the files need
            // not be read again for that.
            refresh_history(!services().project_modified() || services().project_path() != m_history_file);
        });
        m_message_connection = s.messages().added.connect([this](const logged_message &m, bool) {
            // A repeat comes back as the last one, counted; the view counts
            // it on its last row the same way.
            if (m_log == nullptr)
                return;
            log_entry e = entry_of(m);
            e.count = 1;
            m_log->append(std::move(e));
        });
        m_cleared_connection = s.messages().cleared.connect([this] {
            if (m_log != nullptr)
                m_log->clear();
        });

        window_layout layout;
        const auto has_project = [this] { return services().project() != nullptr; };
        menu_item save = command_item("Save Project", "file.save", icon::save);
        save.enabled = has_project;
        menu_item save_as = command_item("Save Project As\xE2\x80\xA6", "file.save_as");
        save_as.enabled = has_project;
        menu_item close = command_item("Close Project", "file.close_project");
        close.enabled = has_project;
        layout.file_items = {command_item("New Project\xE2\x80\xA6", "file.new", icon::file),
                             command_item("Open\xE2\x80\xA6", "file.open", icon::open), save, save_as, close};
        layout.menus.push_back({"History", history_menu()});

        auto body = std::make_unique<column>(0.0f);
        body->add(build_toolbar(host));

        auto split = std::make_unique<splitter>(orientation::horizontal);
        split->set_name("project splitter");
        split->set_fixed(splitter::keep::first, m_side_width);
        split->set_minimums(200.0f, 340.0f);
        split->layout().flex = 1.0f;
        split->set_first(build_project_panel());
        split->set_second(build_overview(host));
        m_split = split.get();
        body->add(std::move(split));

        layout.body = std::move(body);
        refresh_identity();
        refresh_tiles(true);
        // Not followed while the window was closed: read afresh.
        refresh_history(true);
        return layout;
    }

    void project_manager::detach()
    {
        if (m_split != nullptr)
            m_side_width = m_split->position();
        app_services &s = services();
        s.board().models_changed.disconnect(m_models_connection);
        s.catalog_changed.disconnect(m_catalog_connection);
        s.project_changed.disconnect(m_project_connection);
        s.messages().added.disconnect(m_message_connection);
        s.messages().cleared.disconnect(m_cleared_connection);
        m_models_connection = m_catalog_connection = m_project_connection = 0;
        m_message_connection = m_cleared_connection = 0;

        m_split = nullptr;
        m_tree = nullptr;
        m_name = nullptr;
        m_location = nullptr;
        m_state = nullptr;
        m_save_tool = nullptr;
        m_log = nullptr;
        m_tiles = {};

        m_branch_choice = nullptr;
        m_new_branch = nullptr;
        m_rename_branch = nullptr;
        m_delete_branch = nullptr;
        m_snapshot = nullptr;
        m_history_state = nullptr;
        m_history_changes = nullptr;
        m_snapshot_list = nullptr;
        m_restore = nullptr;
        m_branch_from = nullptr;
        m_selected_text = nullptr;
    }

    void project_manager::update(time_point /*now*/)
    {
        // Windows open and close from anywhere (their own close boxes, the
        // Window menu of another); this runs at every wake-up, and changes
        // the tiles only when that changed something.
        refresh_tiles(false);
    }

    void project_manager::theme_changed()
    {
        // The history's notes are coloured from the theme (warning, error).
        show_history();
    }

    // ---- building -------------------------------------------------------------------------

    void project_manager::add_commands(window_host &host)
    {
        ui::screen &s = host.screen();
        s.add_command("file.new", [this] { ask_new_project(); });
        // The standard Open... (menus.hpp), with a typed path to fall back
        // on where the system has no file dialog.
        s.add_command("file.open", [this] { services().open_with_dialog(kind(), [this] { ask_open_project(); }); });
        s.add_command("file.save", [this] { save_project(); });
        s.add_command("file.save_as", [this] { save_project_as(); });
        s.add_command("file.close_project", [this] { close_project(); });

        s.add_command("history.snapshot", [this] { ask_snapshot(); });
        s.add_command("history.switch", [this] {
            // The branch list in HISTORY, opened.
            if (m_branch_choice != nullptr && m_branch_choice->is_enabled())
            {
                m_branch_choice->focus(focus_reason::keyboard);
                m_branch_choice->open();
            }
        });
        s.add_command("history.new_branch", [this] { ask_new_branch({}); });
        s.add_command("history.rename_branch", [this] { ask_rename_branch(); });
        s.add_command("history.delete_branch", [this] { ask_delete_branch(); });
        // (`host` here is the parameter; the window's, later, is this->host().)
        s.add_command("history.restore", [this] {
            if (const history_entry *e = selected_snapshot())
                restore_snapshot(e->id);
            else if (this->host() != nullptr)
                this->host()->show_message("Select a snapshot in the HISTORY list first");
        });
        s.add_command("history.branch_from", [this] {
            if (const history_entry *e = selected_snapshot())
                ask_new_branch(e->id);
            else if (this->host() != nullptr)
                this->host()->show_message("Select a snapshot in the HISTORY list first");
        });
        s.add_command("history.refresh", [this] {
            refresh_history(true);
            if (this->host() != nullptr && m_history.state() == history_state::ready)
                this->host()->show_message(m_history.view().changed()
                                               ? "Not in a snapshot yet: " + describe_changes(m_history.view().changes)
                                               : std::string("The project's files are as in the last snapshot"));
        });

        shortcut_table &keys = s.shortcuts();
        (void)keys.bind("Ctrl+N", "file.new");
        (void)keys.bind("Ctrl+S", "file.save");
        (void)keys.bind("Ctrl+Shift+S", "file.save_as");
        (void)keys.bind("F5", "history.refresh");
    }

    std::vector<menu_item> project_manager::history_menu()
    {
        const auto can_snapshot = [this] { return services().project() != nullptr && m_history.can_snapshot(); };
        const auto has_snapshots = [this] { return services().project() != nullptr && m_history.has_snapshots(); };
        const auto has_selection = [this] {
            return services().project() != nullptr && m_history.has_snapshots() && selected_snapshot() != nullptr;
        };

        std::vector<menu_item> items;
        items.push_back(command_item("Take Snapshot\xE2\x80\xA6", "history.snapshot", icon::save));
        items.back().enabled = can_snapshot;
        items.push_back(menu_item::separator());
        items.push_back(command_item("Switch Branch", "history.switch"));
        items.back().enabled = [this] { return m_branch_choice != nullptr && m_branch_choice->is_enabled(); };
        items.push_back(command_item("New Branch\xE2\x80\xA6", "history.new_branch", icon::plus));
        items.back().enabled = has_snapshots;
        items.push_back(command_item("Rename Branch\xE2\x80\xA6", "history.rename_branch"));
        items.back().enabled = [this] {
            return services().project() != nullptr && m_history.state() == history_state::ready &&
                   !m_history.view().branches.empty();
        };
        items.push_back(command_item("Delete Branch\xE2\x80\xA6", "history.delete_branch", icon::minus));
        items.back().enabled = [this] { return services().project() != nullptr && !other_branches().empty(); };
        items.push_back(menu_item::separator());
        items.push_back(command_item("Restore Selected Snapshot", "history.restore", icon::undo));
        items.back().enabled = has_selection;
        items.push_back(command_item("New Branch from Selected Snapshot\xE2\x80\xA6", "history.branch_from"));
        items.back().enabled = has_selection;
        items.push_back(menu_item::separator());
        items.push_back(command_item("Check for Changes", "history.refresh"));
        items.back().enabled = [this] { return services().project() != nullptr; };
        return items;
    }

    std::unique_ptr<widget> project_manager::build_toolbar(window_host &host)
    {
        auto bar = std::make_unique<toolbar>();
        bar->set_name("toolbar");
        add_brand(*bar, host);

        add_command_tool(*bar, host, icon::file, "New Project (Ctrl+N)", "file.new");
        add_command_tool(*bar, host, icon::open, "Open a project, library, part package, model or image (Ctrl+O)",
                         "file.open");
        m_save_tool = &add_command_tool(*bar, host, icon::save, "Save Project (Ctrl+S)", "file.save");
        m_save_tool->set_name("save tool");
        bar->add_separator();
        add_command_tool(*bar, host, icon::sheet, "Schematic Editor (Ctrl+2)",
                         window_command(window_kind::schematic_editor), "Schematic");
        add_command_tool(*bar, host, icon::cube, "Board Editor (Ctrl+3)", window_command(window_kind::board_editor),
                         "Board");
        add_command_tool(*bar, host, icon::component, "Library Browser (Ctrl+4)",
                         window_command(window_kind::library_browser), "Library");
        bar->add_spacer();
        return bar;
    }

    std::unique_ptr<widget> project_manager::build_project_panel()
    {
        auto side = std::make_unique<panel>("PROJECT", surface::panel, 6.0f, insets::uniform(8.0f));
        side->set_name("project panel");

        auto &search = side->emplace<text_field>(m_filter, "Filter files\xE2\x80\xA6");
        search.set_name("tree filter");
        search.set_tooltip("Show only the files whose names contain this");
        search.changed.connect([this](const std::string &text) { filter_tree(text); });

        auto &tree = side->emplace<tree_view>(m_project);
        tree.set_name("project tree");
        tree.layout().flex = 1.0f;
        tree.activated.connect([this](int) {
            if (const tree_node *n = m_tree != nullptr ? m_tree->selected_node() : nullptr)
                activate(*n);
        });
        m_tree = &tree;
        filter_tree(std::string(m_filter));
        return side;
    }

    std::unique_ptr<widget> project_manager::build_overview(window_host &host)
    {
        const ui::theme &t = host.screen().theme();
        auto area = std::make_unique<panel>("", surface::background, 12.0f, insets::uniform(t.padding * 2.0f));
        area->set_name("overview");

        // Who the project is.
        auto &heading = area->emplace<row>(12.0f);
        heading.set_cross_align(align::center);
        auto &logo = heading.emplace<image_view>(texture_id{}, math::vec2f{40.0f, 40.0f});
        host.track_logo(logo, 40.0f);
        auto &names = heading.emplace<column>(2.0f);
        names.layout().flex = 1.0f;
        m_name = &names.emplace<label>("");
        m_name->set_name("project name");
        m_name->set_size(text_size::title);
        m_location = &names.emplace<label>("", text_role::dim);
        m_location->set_name("project location");
        m_state = &area->emplace<label>("", text_role::dim);
        m_state->set_name("project state");
        m_state->set_size(text_size::small);
        m_state->set_wrap(true);

        area->emplace<separator>();

        // The editors.
        auto &editors = area->emplace<label>("EDITORS", text_role::dim);
        editors.set_size(text_size::small);
        auto &tiles = area->emplace<tile_grid>(200.0f, t.spacing * 1.5f);
        tiles.set_name("editor tiles");
        tiles.add(build_tile(host, window_kind::schematic_editor, icon::sheet,
                             "The project's sheets and their simulation; a demo sheet when no project is open."));
        tiles.add(build_tile(host, window_kind::board_editor, icon::cube,
                             "The board in 3D. Drop STEP, OBJ or STL models on it to place them."));
        tiles.add(build_tile(host, window_kind::library_browser, icon::component,
                             "The parts in the libraries and in the project's library."));

        area->add(build_history(host));

        auto messages = build_messages();
        messages->layout().flex = 1.0f;
        area->add(std::move(messages));

        // A scroll area gives its content at least its own height, so with
        // room to spare the log takes it; in a short window the overview
        // scrolls instead of running out under the status bar.
        auto scroller = std::make_unique<scroll_area>();
        scroller->set_name("overview scroll");
        scroller->set_content(std::move(area));
        return scroller;
    }

    std::unique_ptr<widget> project_manager::build_tile(window_host &host, window_kind kind, ui::icon icon,
                                                        std::string description)
    {
        const std::string key(window_key(kind));
        auto card = std::make_unique<panel>("", surface::panel, 8.0f, insets::uniform(12.0f));
        card->set_name(key + " tile");
        card->set_border(true);

        auto &top = card->emplace<row>(8.0f);
        top.set_cross_align(align::center);
        top.emplace<image_view>(icon, 18.0f);
        auto &name = top.emplace<label>(std::string(window_name(kind)));
        name.layout().flex = 1.0f;
        auto &state = top.emplace<label>("", text_role::dim);
        state.set_name(key + " state");
        state.set_size(text_size::small);

        auto &text = card->emplace<label>(std::move(description), text_role::dim);
        text.set_wrap(true);
        text.set_size(text_size::small);
        text.layout().flex = 1.0f;

        auto &bottom = card->emplace<row>(8.0f);
        bottom.set_cross_align(align::center);
        auto &open = bottom.emplace<button>("Open", icon::none, button_kind::normal);
        open.set_name("launch " + key);
        open.clicked.connect([&host, kind] { host.services().open_window(kind); });
        bottom.emplace<spacer>();
        auto &keys = bottom.emplace<label>(host.screen().shortcut_text(window_command(kind)), text_role::dim);
        keys.set_size(text_size::small);

        for (tile &t : m_tiles)
        {
            if (t.state == nullptr)
            {
                t = {kind, &state, &open, false};
                break;
            }
        }
        return card;
    }

    std::unique_ptr<widget> project_manager::build_messages()
    {
        auto box = std::make_unique<column>(6.0f);
        box->set_name("messages");

        auto &head = box->emplace<row>(8.0f);
        head.set_cross_align(align::center);
        auto &title = head.emplace<label>("MESSAGES", text_role::dim);
        title.set_size(text_size::small);
        head.emplace<spacer>();
        auto &clear = head.emplace<button>("Clear", icon::none, button_kind::flat);
        clear.set_name("clear messages");
        clear.set_tooltip("Forget the messages listed here");
        clear.clicked.connect([this] { services().messages().clear(); });

        auto &log = box->emplace<log_view>(message_log::capacity);
        log.set_name("message log");
        log.set_placeholder("Nothing yet. Files opened, saved or refused are listed here.");
        log.layout().flex = 1.0f;
        log.layout().min_size = {0.0f, 120.0f};
        std::vector<log_entry> entries;
        for (const logged_message &m : services().messages().messages())
            entries.push_back(entry_of(m));
        log.set_entries(std::move(entries));
        m_log = &log;
        return box;
    }

    std::unique_ptr<widget> project_manager::build_history(window_host &host)
    {
        const ui::theme &t = host.screen().theme();
        auto box = std::make_unique<column>(6.0f);
        box->set_name("history");

        // The heading, with the one action the area is about.
        auto &head = box->emplace<row>(8.0f);
        head.set_cross_align(align::center);
        auto &title = head.emplace<label>("HISTORY", text_role::dim);
        title.set_size(text_size::small);
        head.emplace<spacer>();
        m_snapshot = &head.emplace<button>("Snapshot\xE2\x80\xA6", icon::save, button_kind::primary);
        m_snapshot->set_name("take snapshot");
        m_snapshot->set_tooltip("Keep the project's files as they are now, with a message saying what changed");
        m_snapshot->clicked.connect([this] { ask_snapshot(); });

        // The branch, to switch from, and what can be done with branches.
        auto &branches = box->emplace<row>(t.spacing);
        branches.set_cross_align(align::center);
        branches.emplace<label>("Branch", text_role::dim);
        m_branch_choice = &branch_dropdown(branches, {}, -1);
        m_branch_choice->set_name("branch choice");
        m_branch_choice->set_tooltip("The branch the project is on. Choose another to switch to it: its files replace "
                                     "the project's.");
        m_branch_choice->changed.connect([this](int index) {
            const std::string name = index >= 0 && static_cast<std::size_t>(index) < m_branch_names.size()
                                         ? m_branch_names[static_cast<std::size_t>(index)]
                                         : std::string();
            // Shown as it is until the switch is done: it may be cancelled.
            // The switch itself waits for the list to have closed, since
            // what it leads to rebuilds the dropdown.
            const auto current = std::ranges::find(m_branch_names, m_history.view().branch);
            if (m_branch_choice != nullptr)
                m_branch_choice->set_selected(
                    current != m_branch_names.end() ? static_cast<int>(current - m_branch_names.begin()) : -1);
            if (!name.empty())
                services().post([this, name] { switch_branch(name); });
        });
        m_new_branch = &branches.emplace<button>("New\xE2\x80\xA6", icon::plus);
        m_new_branch->set_name("new branch");
        m_new_branch->set_tooltip("Start a branch at the latest snapshot and go on there; the files stay as they are");
        m_new_branch->clicked.connect([this] { ask_new_branch({}); });
        m_rename_branch = &branches.emplace<button>("Rename\xE2\x80\xA6");
        m_rename_branch->set_name("rename branch");
        m_rename_branch->set_tooltip("Give a branch another name");
        m_rename_branch->clicked.connect([this] { ask_rename_branch(); });
        m_delete_branch = &branches.emplace<button>("Delete\xE2\x80\xA6", icon::minus);
        m_delete_branch->set_name("delete branch");
        m_delete_branch->set_tooltip("Delete a branch other than the current one");
        m_delete_branch->clicked.connect([this] { ask_delete_branch(); });

        // Where the project stands against its last snapshot.
        m_history_state = &box->emplace<label>("", text_role::dim);
        m_history_state->set_name("history state");
        m_history_state->set_size(text_size::small);
        m_history_state->set_wrap(true);
        m_history_changes = &box->emplace<label>("", text_role::dim);
        m_history_changes->set_name("history changes");
        m_history_changes->set_size(text_size::small);
        m_history_changes->set_wrap(true);

        // The branch's snapshots, the one the project is at marked.
        auto &list = box->emplace<column_view>();
        list.set_name("snapshot list");
        list.set_columns(snapshot_columns());
        list.layout().height = 168.0f;
        list.set_cell_painter([this](painter &p, std::size_t row, std::size_t column, const rect &area) {
            if (column != head_column)
                return false;
            const std::vector<history_entry> &snapshots = m_history.view().snapshots;
            if (row < snapshots.size() && snapshots[row].head)
            {
                const float size = 8.0f;
                const rect marker{area.x + (area.width - size) * 0.5f, area.y + (area.height - size) * 0.5f, size,
                                  size};
                p.fill(marker, p.theme().accent, size * 0.5f);
            }
            return true;
        });
        list.selection_changed.connect([this](int index) {
            const std::vector<history_entry> &snapshots = m_history.view().snapshots;
            m_selected_snapshot = index >= 0 && static_cast<std::size_t>(index) < snapshots.size()
                                      ? snapshots[static_cast<std::size_t>(index)].id
                                      : std::string();
            show_selection();
        });
        m_snapshot_list = &list;

        // What can be done with the snapshot selected.
        auto &actions = box->emplace<row>(t.spacing);
        actions.set_cross_align(align::center);
        m_restore = &actions.emplace<button>("Restore", icon::undo);
        m_restore->set_name("restore snapshot");
        m_restore->set_tooltip("Put the project's files back as they were in this snapshot, staying on this branch: "
                               "they show as changes, and the next snapshot records them");
        m_restore->clicked.connect([this] {
            if (const history_entry *e = selected_snapshot())
                restore_snapshot(e->id);
        });
        m_branch_from = &actions.emplace<button>("New Branch from Here\xE2\x80\xA6");
        m_branch_from->set_name("branch from snapshot");
        m_branch_from->set_tooltip("Start a branch at this snapshot: its files replace the project's, and you go on "
                                   "from there");
        m_branch_from->clicked.connect([this] {
            if (const history_entry *e = selected_snapshot())
                ask_new_branch(e->id);
        });
        m_selected_text = &actions.emplace<label>("", text_role::dim);
        m_selected_text->set_name("selected snapshot");
        m_selected_text->set_size(text_size::small);
        m_selected_text->layout().flex = 1.0f;
        return box;
    }

    // ---- the project ------------------------------------------------------------------------

    void project_manager::ask_new_project()
    {
        window_host *h = host();
        if (h == nullptr)
            return;
        services().confirm_discard(kind(), "New Project", [this] {
            window_host *host_now = host();
            if (host_now == nullptr)
                return;
            window_host &host = *host_now;

            auto dialog = dialog_frame("new project dialog", "New Project");
            auto &grid = dialog->emplace<property_grid>();
            grid.set_name_share(0.28f);
            grid.add_section("Project");
            auto &name = grid.add_row("Name", std::make_unique<text_field>("", "Blinky"));
            name.set_name("project name field");

            // Where the last one went this session, next to the open
            // project, or in the user's documents. Typed, or chosen in the
            // system's dialog.
            std::filesystem::path folder = services().file_folder(file_purpose::project_folder);
            if (folder.empty())
                folder = default_project_folder();
            auto folder_row = std::make_unique<row>(6.0f);
            folder_row->set_cross_align(align::center);
            auto &place = folder_row->emplace<text_field>(shown(folder), "Where it goes");
            place.set_name("project folder field");
            place.layout().flex = 1.0f;
            auto &browse = folder_row->emplace<button>("Browse\xE2\x80\xA6", icon::open);
            browse.set_name("browse project folder");
            browse.set_tooltip("Choose the folder in the system's file dialog");
            grid.add_row("Folder", std::move(folder_row));
            auto &own = grid.add_row("", std::make_unique<checkbox>("In a folder of its own, named after it", true));
            own.set_name("own folder");

            auto &preview = dialog->emplace<label>("", text_role::dim);
            preview.set_name("new project preview");
            preview.set_size(text_size::small);
            preview.set_wrap(true);
            label &error = error_line(*dialog, host.screen().theme());

            auto &buttons = dialog->emplace<row>(8.0f);
            buttons.emplace<spacer>();
            auto &cancel = buttons.emplace<button>("Cancel");
            auto &create = buttons.emplace<button>("Create", icon::file, button_kind::primary);
            create.set_name("create project");

            // Where the project file lands, as app_services::new_project()
            // names it: after the project, with what a file name cannot
            // hold replaced.
            const auto target = [&name, &place, &own]() -> std::optional<std::filesystem::path> {
                const std::string typed = tidy_path(name.text());
                if (typed.empty())
                    return std::nullopt;
                const std::filesystem::path stem =
                    library::path_from_utf8(library::new_project(typed).library_file).stem();
                std::filesystem::path directory = library::path_from_utf8(tidy_path(place.text()));
                if (own.checked())
                    directory /= stem;
                std::filesystem::path file = directory / stem;
                file += library::project_extension;
                return file;
            };
            const auto refresh = [&preview, &create, target] {
                const auto file = target();
                create.set_enabled(file.has_value());
                preview.set_text(file ? "Makes " + shown(*file) + ", with its project library beside it."
                                      : std::string("Give the project a name."));
            };
            refresh();
            name.changed.connect([refresh](const std::string &) { refresh(); });
            place.changed.connect([refresh](const std::string &) { refresh(); });
            own.changed.connect([refresh](bool) { refresh(); });

            widget &shown_dialog = open_dialog(host, std::move(dialog));
            const auto make = [this, &host, &shown_dialog, &name, &error, target] {
                const auto file = target();
                if (!file)
                {
                    show_error(error, "Give the project a name.");
                    return;
                }
                if (file->parent_path().empty() || !file->is_absolute())
                {
                    show_error(error, "Give the whole path of the folder, from the drive.");
                    return;
                }
                auto made = services().new_project(file->parent_path(), tidy_path(name.text()));
                if (!made)
                {
                    show_error(error, made.error());
                    host.show_message("Cannot make the project: " + made.error(), std::chrono::seconds(10),
                                      log_level::error);
                    return;
                }
                host.screen().close_popup(shown_dialog);
                host.show_message("Made " + shown(services().project_path()));
            };
            cancel.clicked.connect([&host, &shown_dialog] { host.screen().close_popup(shown_dialog); });
            create.clicked.connect(make);
            name.submitted.connect([make](const std::string &) { make(); });
            place.submitted.connect([make](const std::string &) { make(); });

            // The answer comes later, when this dialog may be gone (the
            // window closed with it): only a dialog still open is filled in.
            browse.clicked.connect([this, &shown_dialog, &place, &error, refresh] {
                platform::file_dialog chooser = file_dialog_for(file_purpose::project_folder);
                chooser.directory = library::path_from_utf8(tidy_path(place.text()));
                services().show_file_dialog(
                    kind(), file_purpose::project_folder, std::move(chooser),
                    [this, &shown_dialog, &place, &error, refresh](platform::file_dialog_result result) {
                        // (`host` here is the window the form was made in.)
                        window_host *now = this->host();
                        if (now == nullptr || !now->screen().is_open(shown_dialog))
                            return;
                        if (!result)
                        {
                            show_error(error, result.error().unavailable
                                                  ? "There is no file dialog on this system (" +
                                                        result.error().message + "): type the folder's path."
                                                  : "The file dialog did not open: " + result.error().message);
                            return;
                        }
                        if (result->empty())
                            return;
                        place.set_text(shown(result->front()));
                        refresh();
                    });
            });
            name.focus(focus_reason::keyboard);
        });
    }

    void project_manager::ask_open_project()
    {
        window_host *h = host();
        if (h == nullptr)
            return;
        services().confirm_discard(kind(), "Open Project", [this] {
            window_host *host_now = host();
            if (host_now == nullptr)
                return;
            window_host &host = *host_now;

            auto dialog = dialog_frame("open project dialog", "Open Project");
            auto &note = dialog->emplace<label>(
                "This system has no file dialog (on Linux, zenity or kdialog shows one). Type or paste the path of "
                "a .voltproj file; dropping the file on this window opens it too.",
                text_role::dim);
            note.set_wrap(true);
            const std::string current = services().project_path().empty() ? std::string()
                                                                            : shown(services().project_path());
            auto &field = dialog->emplace<text_field>(current, "C:\\Projects\\Blinky\\Blinky.voltproj");
            field.set_name("project path field");
            label &error = error_line(*dialog, host.screen().theme());

            auto &buttons = dialog->emplace<row>(8.0f);
            buttons.emplace<spacer>();
            auto &cancel = buttons.emplace<button>("Cancel");
            auto &open = buttons.emplace<button>("Open", icon::open, button_kind::primary);
            open.set_name("open project");

            widget &shown_dialog = open_dialog(host, std::move(dialog));
            const auto go = [this, &host, &shown_dialog, &field, &error] {
                const std::string path = tidy_path(field.text());
                if (path.empty())
                {
                    show_error(error, "Type the path of a .voltproj file.");
                    return;
                }
                if (lower(library::path_to_utf8(library::path_from_utf8(path).extension())) !=
                    library::project_extension)
                {
                    show_error(error, "That is not a Volt project: its name should end in .voltproj.");
                    return;
                }
                auto opened = services().open_project(library::path_from_utf8(path));
                if (!opened)
                {
                    show_error(error, opened.error());
                    host.show_message("Cannot open the project: " + opened.error(), std::chrono::seconds(10),
                                      log_level::error);
                    return;
                }
                host.screen().close_popup(shown_dialog);
                host.show_message("Opened " + shown(services().project_path()));
            };
            cancel.clicked.connect([&host, &shown_dialog] { host.screen().close_popup(shown_dialog); });
            open.clicked.connect(go);
            field.submitted.connect([go](const std::string &) { go(); });
            field.focus(focus_reason::keyboard);
        });
    }

    void project_manager::save_project()
    {
        window_host *h = host();
        if (h == nullptr)
            return;
        if (services().project() == nullptr)
        {
            h->show_message("No project is open: there is nothing to save");
            return;
        }
        auto saved = services().save_project();
        if (!saved)
        {
            h->show_message("Cannot save the project: " + saved.error(), std::chrono::seconds(10), log_level::error);
            return;
        }
        h->show_message("Saved " + shown(services().project_path()));
    }

    void project_manager::save_project_as()
    {
        window_host *h = host();
        if (h == nullptr)
            return;
        if (services().project() == nullptr)
        {
            h->show_message("No project is open: there is nothing to save");
            return;
        }

        // Where it is now, under its own name, for the user to change.
        platform::file_dialog dialog = file_dialog_for(file_purpose::save_project);
        dialog.directory = services().project_path().parent_path();
        dialog.file_name = platform::path_to_utf8(services().project_path().filename());
        // The other windows go on while the dialog is up: a project dropped
        // on one of them replaces this one, which is then not the project
        // the user chose a new place for.
        services().show_file_dialog(kind(), file_purpose::save_project, std::move(dialog),
                                    [this, asked_for = services().project_path()](platform::file_dialog_result result) {
                                        window_host *now = host();
                                        if (result && !result->empty() &&
                                            (services().project() == nullptr || services().project_path() != asked_for))
                                        {
                                            services().show_message(kind(),
                                                                    "Not saved elsewhere: the project was closed while "
                                                                    "the dialog was open",
                                                                    std::chrono::seconds(10), log_level::warning);
                                            return;
                                        }
                                        if (!result)
                                        {
                                            const std::string why =
                                                result.error().unavailable
                                                    ? "there is no file dialog on this system: " +
                                                          result.error().message
                                                    : result.error().message;
                                            services().show_message(kind(), "Not saved elsewhere: " + why,
                                                                    std::chrono::seconds(10), log_level::warning);
                                            return;
                                        }
                                        if (result->empty())
                                            return;
                                        auto saved = services().save_project_as(result->front());
                                        if (!saved)
                                        {
                                            services().show_message(kind(), "Cannot save the project: " + saved.error(),
                                                                    std::chrono::seconds(10), log_level::error);
                                            return;
                                        }
                                        if (now != nullptr)
                                            now->show_message("Saved as " + shown(services().project_path()));
                                    });
    }

    void project_manager::close_project()
    {
        window_host *h = host();
        if (h == nullptr || services().project() == nullptr)
            return;
        services().confirm_discard(kind(), "Close Project", [this] {
            const std::string name = services().project() != nullptr ? services().project()->name : std::string();
            services().close_project();
            if (host() != nullptr)
                host()->show_message("Closed " + name);
        });
    }

    // ---- the views ----------------------------------------------------------------------------

    void project_manager::activate(const tree_node &n)
    {
        // A sheet opens the schematic, the board the board, the library or
        // a part the library browser.
        if (n.id == "sheet")
            services().open_window(window_kind::schematic_editor);
        else if (n.id == "board")
            services().open_window(window_kind::board_editor);
        else if (n.id == "library" || n.id == "part")
            services().open_window(window_kind::library_browser);
        else if (m_tree != nullptr && m_tree->selected() >= 0)
            m_tree->toggle(static_cast<std::size_t>(m_tree->selected()));
    }

    void project_manager::filter_tree(std::string_view text)
    {
        m_filter = std::string(text);
        if (m_tree == nullptr)
            return;
        if (text.empty())
        {
            m_tree->set_roots(m_project);
            return;
        }
        std::vector<tree_node> kept;
        for (const tree_node &root : m_project)
        {
            tree_node out;
            if (filter(root, text, out))
                kept.push_back(std::move(out));
        }
        m_tree->set_roots(std::move(kept));
    }

    void project_manager::refresh_tree()
    {
        m_project = project_model(services());
        filter_tree(std::string(m_filter));
    }

    void project_manager::refresh_identity()
    {
        if (m_name == nullptr)
            return;
        app_services &s = services();
        const library::project *p = s.project();
        if (p != nullptr)
        {
            m_name->set_text(p->name);
            // The folder: the file's name is the project's, and in the tree.
            m_location->set_text(shown(s.project_path().parent_path()));
            m_state->set_text(s.project_modified()
                                  ? "Changed since it was last saved: File > Save Project (Ctrl+S) keeps the changes."
                                  : std::format("Saved. {}, {}, {} in its library.",
                                                plural(p->sheets.size(), "sheet", "sheets"),
                                                p->board.empty() ? "no board" : "a board",
                                                plural(p->parts.parts.size(), "part", "parts")));
        }
        else
        {
            m_name->set_text("No project open");
            m_location->set_text("Showing the built-in ladder demo");
            m_state->set_text("Open a project with File > Open (Ctrl+O), by dropping a .voltproj file on this "
                              "window, or with --open on the command line. File > New Project (Ctrl+N) makes one.");
        }
        if (m_save_tool != nullptr)
            m_save_tool->set_enabled(p != nullptr);
    }

    void project_manager::refresh_tiles(bool force)
    {
        for (tile &t : m_tiles)
        {
            if (t.state == nullptr)
                continue;
            const bool open = services().is_open(t.kind);
            if (!force && open == t.shown_open)
                continue;
            t.shown_open = open;
            t.state->set_text(open ? "Open" : "Closed");
            t.state->set_role(open ? text_role::accent : text_role::dim);
            t.open->set_text(open ? "Show" : "Open");
            t.open->set_tooltip(open ? std::format("Brings the {} to the front", window_name(t.kind))
                                     : std::format("Opens the {}", window_name(t.kind)));
        }
    }

    // ---- history ------------------------------------------------------------------------------

    void project_manager::refresh_history(bool reread)
    {
        app_services &s = services();
        const library::project *p = s.project();
        const std::filesystem::path file = p != nullptr ? s.project_path() : std::filesystem::path();
        if (reread || file != m_history_file)
        {
            m_history.set_project(file, p);
            m_history_file = file;
        }
        show_history();
    }

    const history_entry *project_manager::selected_snapshot() const
    {
        return m_selected_snapshot.empty() ? nullptr : m_history.find(m_selected_snapshot);
    }

    std::vector<std::string> project_manager::other_branches() const
    {
        std::vector<std::string> names;
        if (m_history.state() != history_state::ready)
            return names;
        for (const history_branch &b : m_history.view().branches)
            if (!b.current)
                names.push_back(b.name);
        return names;
    }

    void project_manager::show_history()
    {
        if (m_history_state == nullptr || host() == nullptr)
            return;
        app_services &s = services();
        const ui::theme &t = host()->screen().theme();
        const history_view &v = m_history.view();
        const bool open = s.project() != nullptr;
        const bool modified = open && s.project_modified();
        const bool ready = v.state == history_state::ready;

        // The branches, the current one chosen. A head on no branch is
        // not something Volt makes (another tool may have), and is shown
        // for what it is.
        m_branch_names.clear();
        std::vector<std::string> items;
        int current = 0;
        if (ready)
        {
            if (v.branch.empty())
            {
                m_branch_names.emplace_back();
                items.push_back(std::format("(no branch: at {})", short_id(v.head)));
            }
            for (const history_branch &b : v.branches)
            {
                if (b.current)
                    current = static_cast<int>(items.size());
                m_branch_names.push_back(b.name);
                items.push_back(b.name);
            }
        }
        else
        {
            items.emplace_back(v.state == history_state::no_history ? std::string(history::default_branch)
                                                                    : std::string("\xE2\x80\x94"));
        }
        m_branch_choice->set_items(std::move(items));
        m_branch_choice->set_selected(current);
        m_branch_choice->set_enabled(open && !other_branches().empty());

        m_snapshot->set_enabled(open && m_history.can_snapshot());
        m_new_branch->set_enabled(open && m_history.has_snapshots());
        m_rename_branch->set_enabled(open && ready && !v.branches.empty());
        m_delete_branch->set_enabled(open && !other_branches().empty());

        // Where the project stands against its last snapshot.
        std::string state;
        std::vector<std::string> notes;
        bool warn = false;
        switch (v.state)
        {
        case history_state::no_project:
            state = "No project is open. A project's history is kept with it, in its folder.";
            break;
        case history_state::not_on_disk:
            state = "The project is not on disk, so it has no history: save it (Ctrl+S) first.";
            break;
        case history_state::no_history:
            state = "No snapshots yet. Snapshot\xE2\x80\xA6 keeps the project's files as they are now, in .volthistory "
                    "in its folder, so that you can come back to them or branch off from them.";
            if (modified)
                notes.emplace_back("The project has unsaved changes: the snapshot saves them first.");
            break;
        case history_state::broken:
            state = "The project's history cannot be read: " + v.problem;
            break;
        case history_state::ready:
        {
            const std::string count =
                plural(v.snapshots.size(), "snapshot", "snapshots") + (v.more ? ", the latest" : "");
            state = v.branch.empty()
                        ? std::format("At snapshot {}, on no branch ({}). New Branch\xE2\x80\xA6 puts it on one.",
                                      short_id(v.head), count)
                        : std::format("On {} ({}).", v.branch, count);
            if (!v.status_known)
            {
                notes.push_back("Cannot tell what differs from the last snapshot: " + v.problem + ".");
                warn = true;
            }
            else
            {
                if (v.changed())
                    notes.push_back("Not in a snapshot yet: " + describe_changes(v.changes) + ".");
                if (!v.problem.empty())
                    notes.push_back("Not every snapshot could be read: " + v.problem + ".");
            }
            if (modified)
                notes.emplace_back("Unsaved changes in the project.");
            warn = warn || !notes.empty();
            if (notes.empty())
                notes.emplace_back("Everything is in the last snapshot.");
            break;
        }
        }
        // What a snapshot would leave out, other than files not written
        // yet (which is how a new project's sheet and board are).
        for (const history::skipped_file &skipped : notable_skips(v.skipped))
        {
            notes.push_back(std::format("{} is left out of snapshots ({}).", skipped.path, history::describe(skipped.reason)));
            warn = true;
        }

        m_history_state->set_text(std::move(state));
        m_history_state->set_color(v.state == history_state::broken ? std::optional<math::vec4f>(t.error)
                                                                    : std::nullopt);
        std::string changes;
        for (const std::string &note : notes)
            changes += (changes.empty() ? "" : " ") + note;
        m_history_changes->set_visible(!changes.empty());
        m_history_changes->set_text(std::move(changes));
        m_history_changes->set_color(warn ? std::optional<math::vec4f>(t.warning) : std::nullopt);

        // The branch's snapshots.
        m_snapshot_list->set_rows(v.snapshots.size(), [this](std::size_t row, std::size_t column) -> std::string_view {
            const std::vector<history_entry> &snapshots = m_history.view().snapshots;
            if (row >= snapshots.size())
                return {};
            const history_entry &e = snapshots[row];
            switch (column)
            {
            case message_column:
                return e.message.empty() ? std::string_view("(no message)") : std::string_view(e.message);
            case when_column:
                return e.when;
            case id_column:
                return e.short_id;
            default:
                return {};
            }
        });
        m_snapshot_list->set_empty_text(ready                                    ? "No snapshots on this branch yet."
                                        : v.state == history_state::no_history ? "No snapshots yet."
                                                                                 : "No history.");
        int selected = -1;
        for (std::size_t i = 0; i < v.snapshots.size(); ++i)
            if (v.snapshots[i].id == m_selected_snapshot)
                selected = static_cast<int>(i);
        if (selected < 0)
            m_selected_snapshot.clear();
        m_snapshot_list->set_selected(selected);
        show_selection();
    }

    void project_manager::show_selection()
    {
        if (m_restore == nullptr)
            return;
        const history_entry *e = selected_snapshot();
        const bool can = e != nullptr && services().project() != nullptr && m_history.has_snapshots();
        m_restore->set_enabled(can);
        m_branch_from->set_enabled(can);
        if (e == nullptr)
            m_selected_text->set_text(m_history.has_snapshots() ? "Select a snapshot to restore it or branch from it."
                                                                : std::string());
        else
            m_selected_text->set_text(e->head ? e->short_id + ": the snapshot the project is at"
                                              : e->short_id + dot + e->when);
    }

    void project_manager::ask_snapshot()
    {
        window_host *h = host();
        if (h == nullptr)
            return;
        app_services &s = services();
        if (s.project() == nullptr || !m_history.can_snapshot())
        {
            h->show_message(s.project() == nullptr ? std::string("No project is open: there is nothing to snapshot")
                            : m_history.state() == history_state::not_on_disk
                                ? std::string("The project is not on disk: save it first")
                                : "The project's history cannot be read: " + m_history.view().problem,
                            std::chrono::seconds(8), log_level::warning);
            return;
        }
        window_host &host = *h;
        const history_view &v = m_history.view();
        const bool modified = s.project_modified();

        std::string about = std::format(
            "Keeps the project's files as they are on disk (the project file, its parts library, and its sheets and "
            "board once they are written) in its history, on {}.",
            v.branch.empty() ? std::string("no branch") : v.branch);
        if (v.changed())
            about += " Changed since the last snapshot: " + describe_changes(v.changes) + ".";
        if (modified)
            about += " The project has unsaved changes: they are saved first.";

        auto dialog = dialog_frame("snapshot dialog", "Take Snapshot");
        auto &note = dialog->emplace<label>(std::move(about), text_role::dim);
        note.set_wrap(true);
        auto &field = dialog->emplace<text_field>("", "What changed: Routed the power rails");
        field.set_name("snapshot message field");
        label &error = error_line(*dialog, host.screen().theme());

        auto &buttons = dialog->emplace<row>(8.0f);
        buttons.emplace<spacer>();
        auto &cancel = buttons.emplace<button>("Cancel");
        auto &take = buttons.emplace<button>("Take Snapshot", icon::save, button_kind::primary);
        take.set_name("confirm snapshot");
        take.set_enabled(false);
        field.changed.connect([&take](const std::string &text) { take.set_enabled(!trimmed(text).empty()); });

        widget &shown_dialog = open_dialog(host, std::move(dialog));
        const auto go = [this, &host, &shown_dialog, &field, &error] {
            const std::string message = trimmed(field.text());
            if (message.empty())
            {
                show_error(error, "Say what the snapshot holds: a few words are enough.");
                return;
            }
            host.screen().close_popup(shown_dialog);
            snapshot_now(message);
        };
        cancel.clicked.connect([&host, &shown_dialog] { host.screen().close_popup(shown_dialog); });
        take.clicked.connect(go);
        field.submitted.connect([go](const std::string &) { go(); });
        field.focus(focus_reason::keyboard);
    }

    bool project_manager::snapshot_now(const std::string &message)
    {
        app_services &s = services();
        if (s.project() == nullptr)
            return false;

        // A snapshot holds what is on disk: what is in memory goes there
        // first. (Saving reads the history again, with the files saved.)
        const bool saving = s.project_modified();
        if (saving)
        {
            if (auto saved = s.save_project(); !saved)
            {
                s.show_message(kind(), "No snapshot taken: the project could not be saved: " + saved.error(),
                               std::chrono::seconds(12), log_level::error);
                return false;
            }
        }
        auto taken = m_history.take_snapshot(message);
        show_history();
        if (!taken)
        {
            s.show_message(kind(), (saving ? "Saved the project, but took no snapshot: " : "No snapshot taken: ") +
                                       taken.error(),
                           std::chrono::seconds(12), log_level::error);
            return false;
        }

        const std::string branch = m_history.view().branch.empty() ? std::string("no branch")
                                                                   : m_history.view().branch;
        std::string text = taken->created
                               ? std::format("took snapshot {} on {}: {}", short_id(taken->id), branch,
                                             in_quotes(message.substr(0, message.find_first_of("\r\n"))))
                               : std::format("nothing to snapshot: the files are as in snapshot {}",
                                             short_id(taken->id));
        text = saving ? "Saved the project; " + text : upper_first(std::move(text));
        log_level level = log_level::info;
        if (const auto left_out = notable_skips(taken->skipped); !left_out.empty())
        {
            std::string paths;
            for (const history::skipped_file &skipped : left_out)
                paths += (paths.empty() ? "" : ", ") + skipped.path;
            text += " (left out: " + paths + ", outside the project's folder or not a name a snapshot can hold)";
            level = log_level::warning;
        }
        s.show_message(kind(), std::move(text), std::chrono::seconds(level == log_level::info ? 6 : 12), level);
        return true;
    }

    void project_manager::confirm_overwrite(std::string doing, std::string snapshot_message,
                                            std::function<void()> then)
    {
        app_services &s = services();
        if (host() == nullptr || s.project() == nullptr)
            return;
        const std::filesystem::path file = s.project_path();

        // First what is meant for the project and not in it yet (a part in
        // the editor, for its library): saved into it, or let go, as closing
        // the project asks. An editor on the project's library closes
        // either way: the project is about to be loaded again from disk.
        s.project_guards().confirm([this, file, doing = std::move(doing), snapshot_message = std::move(snapshot_message),
                                    then = std::move(then)] {
            app_services &s = services();
            window_host *h = host();
            if (h == nullptr)
                return;
            if (s.project() == nullptr || s.project_path() != file)
            {
                h->show_message(doing + ": not done, the project was closed meanwhile", std::chrono::seconds(8),
                                log_level::warning);
                return;
            }
            s.open_window(kind()); // raised: the editor may have asked in another window

            // Then whether anything would be lost: the project unsaved, or
            // files that differ from the last snapshot (worked out now).
            const bool modified = s.project_modified();
            auto status = m_history.status();
            if (!modified && status && status->clean())
            {
                then();
                return;
            }
            std::vector<std::string> what;
            if (modified)
                what.emplace_back("the project has changes that are not saved");
            if (!status)
                what.push_back("Volt cannot tell whether its files differ from the last snapshot (" + status.error() +
                               ")");
            else if (!status->clean())
                what.push_back(std::format("{} {} from the last snapshot", describe_changes(status->changes),
                                           status->changes.size() == 1 ? "differs" : "differ"));
            std::string text;
            for (std::size_t i = 0; i < what.size(); ++i)
                text += (i > 0 ? "; and " : "") + what[i];
            const std::string branch = m_history.view().branch;
            text = std::format("{}. {} replaces the project's files. A snapshot of them first keeps these changes in "
                               "the history{}, where you can come back to them.",
                               upper_first(std::move(text)), doing, branch.empty() ? "" : " of " + branch);

            // After the snapshot, everything is asked again rather than
            // taken for granted: the other windows went on while this
            // question was up (a part editor may have opened on the
            // project's library), and a snapshot that captured everything
            // makes the second round silent.
            std::vector<dialog_choice> choices;
            choices.push_back({"Take Snapshot and Continue", button_kind::primary,
                               [this, file, doing, snapshot_message, then] {
                                   if (services().project() == nullptr || services().project_path() != file)
                                       return;
                                   if (snapshot_now(snapshot_message))
                                       confirm_overwrite(doing, snapshot_message, then);
                               }});
            choices.push_back({"Cancel", button_kind::normal, {}});
            open_choice_dialog(*h, "Keep the changes in a snapshot first?", std::move(text), std::move(choices));
        });
    }

    void project_manager::finish_checkout(const project_history::checkout &result, const std::string &failed,
                                          const std::string &done)
    {
        app_services &s = services();
        std::string reloaded;
        bool closed = false;
        if (result.files_touched)
        {
            // What is in memory is the files as they were: the project is
            // opened again from disk, so that no window goes on showing the
            // old state, and nothing saves it over what was put back. If it
            // cannot be, it is closed rather than kept.
            const std::filesystem::path before = s.project_path();
            if (result.project_file.empty())
            {
                s.close_project();
                closed = true;
                reloaded = " The snapshot has no project file, so the project was closed.";
            }
            else if (auto opened = s.open_project(result.project_file); !opened)
            {
                s.close_project();
                closed = true;
                reloaded = " The project could not be opened again (" + opened.error() +
                           "), so it was closed rather than kept as it was; open it again from its folder.";
            }
            else if (result.project_file != before)
            {
                reloaded = " Its project file is " + shown(result.project_file.filename()) + ", which is open now.";
            }
        }
        else
        {
            show_history();
        }

        if (!result.ok())
        {
            std::string text = failed + ": " + result.error + ".";
            if (result.files_touched && !closed)
                text += " The project was loaded again from its files; what differs from the last snapshot shows "
                        "under HISTORY.";
            s.show_message(kind(), text + reloaded, std::chrono::seconds(14), log_level::error);
            return;
        }
        s.show_message(kind(), done + "." + reloaded, std::chrono::seconds(reloaded.empty() ? 8 : 12),
                       reloaded.empty() ? log_level::info : log_level::warning);
    }

    void project_manager::switch_branch(std::string name)
    {
        if (host() == nullptr || services().project() == nullptr || !m_history.has_snapshots() ||
            name == m_history.view().branch)
            return;
        confirm_overwrite(std::format("Switching to {}", name), std::format("Before switching to {}", name),
                          [this, name] {
                              const project_history::checkout result = m_history.switch_branch(name);
                              finish_checkout(result, std::format("Not switched to {}", name),
                                              std::format("Switched to {}: {}", name,
                                                          restored_summary(result.restored)));
                          });
    }

    void project_manager::restore_snapshot(std::string id)
    {
        const history_entry *e = m_history.find(id);
        if (host() == nullptr || services().project() == nullptr || e == nullptr)
            return;
        const std::string name = snapshot_name(*e);
        const std::string short_name = e->short_id;
        const std::string branch = m_history.view().branch.empty() ? std::string("no branch")
                                                                   : m_history.view().branch;
        confirm_overwrite(std::format("Restoring {}", short_name), std::format("Before restoring {}", short_name),
                          [this, id, name, branch] {
                              const project_history::checkout result = m_history.restore(id);
                              finish_checkout(result, "Not restored",
                                              std::format("Restored the files of {} ({}), still on {}: they show as "
                                                          "changes until the next snapshot records them",
                                                          name, restored_summary(result.restored), branch));
                          });
    }

    void project_manager::ask_new_branch(std::string from)
    {
        window_host *h = host();
        if (h == nullptr)
            return;
        if (services().project() == nullptr || !m_history.has_snapshots())
        {
            h->show_message("Take a snapshot first: a branch starts at one", std::chrono::seconds(8));
            return;
        }
        const history_view &v = m_history.view();
        if (from == v.head)
            from.clear();
        const history_entry *start = from.empty() ? nullptr : m_history.find(from);
        if (!from.empty() && start == nullptr)
            return;
        window_host &host = *h;

        const std::string on = v.branch.empty() ? std::string("The snapshot the project is at") : v.branch;
        const std::string about =
            start == nullptr
                ? std::format("Starts at {}, the snapshot the project is at, and becomes the current branch. The "
                              "files stay as they are, changes not in a snapshot included; {} keeps its snapshots.",
                              short_id(v.head), on)
                : std::format("Starts at {}{}, and becomes the current branch: the project's files are put back as "
                              "they were then. {} keeps its snapshots.",
                              snapshot_name(*start), start->when.empty() ? "" : " from " + start->when, on);

        auto dialog = dialog_frame("new branch dialog", "New Branch");
        auto &note = dialog->emplace<label>(about, text_role::dim);
        note.set_wrap(true);
        auto &field = dialog->emplace<text_field>("", "two-layer");
        field.set_name("branch name field");
        label &error = error_line(*dialog, host.screen().theme());

        auto &buttons = dialog->emplace<row>(8.0f);
        buttons.emplace<spacer>();
        auto &cancel = buttons.emplace<button>("Cancel");
        auto &create = buttons.emplace<button>("Create Branch", icon::plus, button_kind::primary);
        create.set_name("confirm new branch");
        create.set_enabled(false);
        field.changed.connect([this, &create, &error](const std::string &text) {
            const std::string name = trimmed(text);
            const auto checked = m_history.check_branch_name(name);
            create.set_enabled(!name.empty() && checked.has_value());
            if (!name.empty() && !checked)
                show_error(error, upper_first(checked.error()) + ".");
            else
                error.set_visible(false);
        });

        widget &shown_dialog = open_dialog(host, std::move(dialog));
        const auto go = [this, &host, &shown_dialog, &field, &error, from] {
            const std::string name = trimmed(field.text());
            if (auto checked = m_history.check_branch_name(name); !checked)
            {
                show_error(error, upper_first(checked.error()) + ".");
                return;
            }
            host.screen().close_popup(shown_dialog);
            if (from.empty())
            {
                // Nothing on disk changes: nothing to ask first.
                const project_history::checkout result = m_history.create_branch(name);
                finish_checkout(result, std::format("Branch {} not made", name),
                                std::format("Made the branch {}; the project is on it now", name));
                return;
            }
            const history_entry *e = m_history.find(from);
            const std::string start_name = e != nullptr ? snapshot_name(*e) : short_id(from);
            confirm_overwrite(std::format("Starting {} at {}", name, short_id(from)),
                              std::format("Before starting {}", name), [this, name, from, start_name] {
                                  const project_history::checkout result = m_history.create_branch(name, from);
                                  finish_checkout(result, std::format("Branch {} not made", name),
                                                  std::format("Made the branch {} at {}; the project is on it now, "
                                                              "with that snapshot's files ({})",
                                                              name, start_name, restored_summary(result.restored)));
                              });
        };
        cancel.clicked.connect([&host, &shown_dialog] { host.screen().close_popup(shown_dialog); });
        create.clicked.connect(go);
        field.submitted.connect([go](const std::string &) { go(); });
        field.focus(focus_reason::keyboard);
    }

    void project_manager::ask_rename_branch()
    {
        window_host *h = host();
        const history_view &v = m_history.view();
        if (h == nullptr || services().project() == nullptr || v.state != history_state::ready || v.branches.empty())
            return;
        window_host &host = *h;

        std::vector<std::string> names;
        int current = 0;
        for (const history_branch &b : v.branches)
        {
            if (b.current)
                current = static_cast<int>(names.size());
            names.push_back(b.name);
        }

        auto dialog = dialog_frame("rename branch dialog", "Rename Branch");
        auto &grid = dialog->emplace<property_grid>();
        grid.set_name_share(0.28f);
        auto &choice = grid.add_row("Branch", std::make_unique<dropdown>(names, current));
        choice.set_name("rename branch choice");
        auto &field = grid.add_row("New name", std::make_unique<text_field>(names[static_cast<std::size_t>(current)]));
        field.set_name("rename branch field");
        label &error = error_line(*dialog, host.screen().theme());

        auto &buttons = dialog->emplace<row>(8.0f);
        buttons.emplace<spacer>();
        auto &cancel = buttons.emplace<button>("Cancel");
        auto &rename = buttons.emplace<button>("Rename", icon::none, button_kind::primary);
        rename.set_name("confirm rename branch");
        rename.set_enabled(false);

        const auto old_name = [names, &choice] {
            const int index = choice.selected();
            return index >= 0 && static_cast<std::size_t>(index) < names.size() ? names[static_cast<std::size_t>(index)]
                                                                                 : std::string();
        };
        const auto check = [this, old_name, &field, &rename, &error] {
            const std::string name = trimmed(field.text());
            const std::string old = old_name();
            const auto checked = m_history.check_branch_name(name, old);
            rename.set_enabled(!old.empty() && name != old && checked.has_value());
            if (name != old && !name.empty() && !checked)
                show_error(error, upper_first(checked.error()) + ".");
            else
                error.set_visible(false);
        };
        choice.changed.connect([old_name, check, &field](int) {
            field.set_text(old_name());
            check();
        });
        field.changed.connect([check](const std::string &) { check(); });

        widget &shown_dialog = open_dialog(host, std::move(dialog));
        const auto go = [this, &host, &shown_dialog, &field, &error, old_name] {
            const std::string name = trimmed(field.text());
            const std::string old = old_name();
            if (name == old)
                return;
            auto renamed = m_history.rename_branch(old, name);
            if (!renamed)
            {
                show_error(error, upper_first(renamed.error()) + ".");
                return;
            }
            host.screen().close_popup(shown_dialog);
            show_history();
            services().show_message(kind(), std::format("Renamed the branch {} to {}", old, name));
        };
        cancel.clicked.connect([&host, &shown_dialog] { host.screen().close_popup(shown_dialog); });
        rename.clicked.connect(go);
        field.submitted.connect([go](const std::string &) { go(); });
        field.focus(focus_reason::keyboard);
    }

    void project_manager::ask_delete_branch()
    {
        window_host *h = host();
        if (h == nullptr || services().project() == nullptr)
            return;
        const std::vector<std::string> names = other_branches();
        if (names.empty())
        {
            h->show_message("There is no branch to delete: the current one cannot be (switch to another first)",
                            std::chrono::seconds(8));
            return;
        }
        window_host &host = *h;

        auto dialog = dialog_frame("delete branch dialog", "Delete Branch");
        auto &which = dialog->emplace<row>(8.0f);
        which.set_cross_align(align::center);
        which.emplace<label>("Branch", text_role::dim);
        auto &choice = branch_dropdown(which, names, 0);
        choice.set_name("delete branch choice");
        auto &consequence = dialog->emplace<label>("", text_role::dim);
        consequence.set_name("delete branch consequence");
        consequence.set_wrap(true);

        auto &buttons = dialog->emplace<row>(8.0f);
        buttons.emplace<spacer>();
        auto &cancel = buttons.emplace<button>("Cancel", icon::none, button_kind::primary);
        auto &remove = buttons.emplace<button>("Delete Branch", icon::minus);
        remove.set_name("confirm delete branch");

        // What deleting the branch chosen would leave unlisted: its own
        // snapshots stay on disk, but nothing in Volt leads back to them.
        const ui::theme &t = host.screen().theme();
        const auto explain = [this, names, &choice, &consequence, warning = t.warning] {
            const std::string &name = names[static_cast<std::size_t>(std::max(choice.selected(), 0))];
            const auto only = m_history.snapshots_only_on(name);
            if (!only)
            {
                consequence.set_text("Volt cannot tell whether its snapshots are on another branch too (" +
                                     only.error() + ").");
                consequence.set_color(warning);
            }
            else if (*only == 0)
            {
                consequence.set_text(std::format("Every snapshot of {} is on another branch too: deleting it loses "
                                                 "nothing.",
                                                 name));
                consequence.set_color(std::nullopt);
            }
            else
            {
                consequence.set_text(std::format(
                    "{} of {} {} on no other branch. Deleting the branch leaves {} unlisted: {} stay{} in .volthistory "
                    "on disk, but nothing in Volt leads back to {} any more.",
                    plural(*only, "snapshot", "snapshots"), name, *only == 1 ? "is" : "are", *only == 1 ? "it" : "them",
                    *only == 1 ? "it" : "they", *only == 1 ? "s" : "", *only == 1 ? "it" : "them"));
                consequence.set_color(warning);
            }
        };
        explain();
        choice.changed.connect([explain](int) { explain(); });

        widget &shown_dialog = open_dialog(host, std::move(dialog));
        cancel.clicked.connect([&host, &shown_dialog] { host.screen().close_popup(shown_dialog); });
        remove.clicked.connect([this, names, &choice, &host, &shown_dialog] {
            const std::string name = names[static_cast<std::size_t>(std::max(choice.selected(), 0))];
            host.screen().close_popup(shown_dialog);
            auto deleted = m_history.delete_branch(name);
            show_history();
            if (!deleted)
            {
                services().show_message(kind(), "The branch was not deleted: " + deleted.error(),
                                        std::chrono::seconds(10), log_level::error);
                return;
            }
            services().show_message(kind(), std::format("Deleted the branch {}", name));
        });
        cancel.focus(focus_reason::keyboard);
    }

} // namespace volt::ui
