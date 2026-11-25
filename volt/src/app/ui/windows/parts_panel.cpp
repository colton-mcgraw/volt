#include <app/ui/windows/parts_panel.hpp>

#include <app/ui/windows/content.hpp>
#include <app/ui/windows/library_model.hpp>
#include <app/ui/windows/library_widgets.hpp>
#include <app/ui/windows/part_match.hpp>
#include <app/ui/windows/part_search.hpp>
#include <library/project.hpp>
#include <schematic/symbol.hpp>
#include <ui/layout/box.hpp>
#include <ui/painter.hpp>
#include <ui/screen.hpp>
#include <ui/theme.hpp>
#include <ui/widgets/button.hpp>
#include <ui/widgets/label.hpp>
#include <ui/widgets/panel.hpp>
#include <ui/widgets/row_view.hpp>
#include <ui/widgets/text_field.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <functional>
#include <memory>

namespace volt::ui
{
    namespace
    {
        // As in the browser: a search slower than this waits for a pause in
        // the typing.
        constexpr double slow_search = 12.0;
        constexpr auto search_pause = std::chrono::milliseconds(90);

        constexpr std::string_view middle_dot = " \xC2\xB7 ";

        // Two lines a row: what the part is, with its package at the right;
        // who makes it and where it is from.
        class part_rows final : public row_view
        {
        public:
            struct line
            {
                std::string first;
                std::string first_right;
                std::string second;
                std::optional<library::part_status> status;
                bool marked = false; // has a status marker
            };

            std::function<std::size_t()> count;
            std::function<line(std::size_t)> row;
            std::string empty_text;

            std::size_t row_count() const override { return count ? count() : 0; }

            float row_height() const override
            {
                const ui::theme &t = theme();
                return std::round(t.font_size + t.small_font_size + 16.0f);
            }

            void changed() { rows_changed(); }

        protected:
            void paint_row(painter &p, std::size_t index, const rect &area) override
            {
                if (!row)
                    return;
                const ui::theme &t = theme();
                const line l = row(index);
                rect text = area.inset(insets{t.control_padding * 0.8f, 4.0f, t.control_padding * 0.8f, 4.0f});
                if (l.marked)
                {
                    const rect marker{text.right() - 14.0f, area.y, 14.0f, area.height};
                    paint_status_marker(p, marker, l.status);
                    text.width = std::max(0.0f, text.width - 18.0f);
                }
                const float half = text.height * 0.5f;
                const math::vec4f color = is_enabled() ? t.text : t.text_disabled;

                // The package keeps its room (up to 40% of the row); what
                // the part is takes the rest, cut short if it must be.
                float left_width = text.width;
                if (!l.first_right.empty())
                {
                    const float wanted = fonts().measure(l.first_right, t.small_style());
                    const float right_width = std::min(wanted, text.width * 0.4f);
                    left_width = std::max(0.0f, text.width - right_width - 8.0f);
                    p.text({text.right() - right_width, text.y, right_width, half + 1.0f}, l.first_right,
                           t.small_style(), t.text_dim, align::end);
                }
                p.text({text.x, text.y, left_width, half + 1.0f}, l.first, t.body_style(), color);
                p.text({text.x, text.y + half, text.width, half}, l.second, t.small_style(), t.text_dim);
            }

            void on_paint(painter &p) override
            {
                row_view::on_paint(p);
                if (row_count() != 0 || empty_text.empty())
                    return;
                const ui::theme &t = theme();
                const rect area = bounds().inset(t.padding * 1.5f);
                if (area.empty())
                    return;
                font::layout_options options;
                options.max_width = area.width;
                options.align = font::text_align::center;
                const font::text_layout lines = fonts().layout(empty_text, t.body_style(), options);
                clip_scope clip(p, area);
                p.layout(lines, t.body_style(), {area.x, area.y + t.padding}, t.text_dim);
            }
        };

        // A library part's row: its specs (or its name) and package; its
        // maker (or name) and where it is from.
        part_rows::line part_line(const library::part &p, std::string specs, std::string_view from)
        {
            part_rows::line l;
            const std::string name = p.name.empty() ? p.id : p.name;
            l.first = specs.empty() ? name : std::move(specs);
            l.first_right = p.package;
            // Who makes it and as what; without that, its name (unless the
            // first line is the name already).
            const std::string maker = maker_text(p);
            l.second = !maker.empty() ? maker : l.first == name ? std::string() : name;
            if (!from.empty())
                l.second += std::string(l.second.empty() ? "" : middle_dot) + std::string(from);
            return l;
        }

        // The panel: its widgets, and what it does with them. The panel
        // widget owns it; timers reach it through a weak pointer, since a
        // timer may outlive the panel when the panel is taken out of a tree
        // that stays.
        class parts_panel_logic final : public std::enable_shared_from_this<parts_panel_logic>
        {
        public:
            parts_panel_logic(window_host &host, parts_panel_options options)
                : m_host(host), m_services(host.services()), m_state(host.services().parts_panel(host.kind())),
                  m_options(std::move(options)),
                  m_searcher([services = &host.services()](std::function<void()> task) {
                      services->post(std::move(task));
                  })
            {
                if (!m_options.assign)
                    m_state.assigning.reset();
                if (m_state.scope == parts_scope::generic && !has_generic())
                    m_state.scope = parts_scope::library;
            }

            ~parts_panel_logic()
            {
                if (m_connections.size() != 3)
                    return;
                m_services.catalog_changed.disconnect(m_connections[0]);
                m_services.catalog_status_changed.disconnect(m_connections[1]);
                m_services.project_changed.disconnect(m_connections[2]);
            }

            parts_panel_logic(const parts_panel_logic &) = delete;
            parts_panel_logic &operator=(const parts_panel_logic &) = delete;

            void build(panel &root);
            bool handle_key(const key_event &e);
            bool navigate(key k);

            void focus_search();
            bool begin_assign(part_assignment what);
            void end_assign();
            bool assigning() const noexcept { return m_state.assigning.has_value(); }

        private:
            void catalog_changed();
            void project_changed();
            void set_scope(parts_scope scope);
            void search_edited(const std::string &text);
            void apply_search();
            // Searches now if the hits shown are of an earlier text.
            void catch_up();
            void refresh();
            void collect_candidates();
            void select(int row);
            void show_selection();
            void show_assign_banner();
            void add_selected();
            void place_selected();
            void assign_selected();
            // What Enter and a double-click do: assign in assign mode;
            // place when placing is on, else add (or, of a project part,
            // show it in the browser).
            void activate();
            void open_in_browser();
            std::size_t rows() const;
            part_rows::line row_line(std::size_t index);
            const library::part *selected_part() const;
            const project_row *selected_project_row() const;
            const generic_entry *selected_generic() const;
            const part_candidate *selected_candidate() const;
            static std::string candidate_key(const part_candidate &c, const library::catalog *catalog);
            bool placing() const noexcept { return static_cast<bool>(m_options.place); }
            bool has_generic() const noexcept
            {
                return static_cast<bool>(m_options.place_symbol) || static_cast<bool>(m_options.place_power);
            }
            parts_scope scope() const noexcept { return m_state.scope; }
            // Whether the Generic scope lists it: what the host can place.
            bool offered(const generic_entry &g) const noexcept
            {
                return g.what == generic_entry::kind::power ? static_cast<bool>(m_options.place_power)
                                                            : static_cast<bool>(m_options.place_symbol);
            }

            window_host &m_host;
            app_services &m_services;
            parts_panel_state &m_state;
            parts_panel_options m_options;
            std::vector<std::size_t> m_connections;

            part_list m_parts;
            std::vector<project_row> m_project_rows;  // all of them
            std::vector<std::size_t> m_project_shown; // those the search lets through
            std::vector<std::size_t> m_generic_shown; // into generic_entries()
            // Assign mode: every candidate, and those the search lets
            // through (copies: a candidate points into the catalog or the
            // project, and is made again whenever either changes).
            std::vector<part_candidate> m_candidates;
            std::vector<part_candidate> m_candidates_shown;
            screen::timer_id m_timer = 0;

            // A large catalog is searched on a thread of its own, as the
            // browser does (part_search): typing never waits for it. Its
            // results come back only while this object lives.
            part_search m_searcher;

            search_field *m_search = nullptr;
            row *m_scopes = nullptr;
            button *m_all = nullptr;
            button *m_project = nullptr;
            button *m_generic = nullptr;
            label *m_count = nullptr;
            panel *m_banner = nullptr;
            label *m_banner_text = nullptr;
            label *m_banner_count = nullptr;
            part_rows *m_list = nullptr;
            label *m_name = nullptr;
            label *m_line = nullptr;
            label *m_line2 = nullptr;
            button *m_add = nullptr;
            button *m_place = nullptr;
            button *m_assign = nullptr;
            button *m_open = nullptr;
            label *m_hint = nullptr;
        };

        class parts_panel_widget final : public panel
        {
        public:
            parts_panel_widget(window_host &host, parts_panel_options options)
                : panel("", surface::panel, 6.0f, insets::uniform(8.0f)),
                  m_logic(std::make_shared<parts_panel_logic>(host, std::move(options)))
            {
                set_name("parts panel");
                layout().min_size = {220.0f, 0.0f};
                m_logic->build(*this);
            }

            parts_panel_logic &logic() noexcept { return *m_logic; }
            const parts_panel_logic &logic() const noexcept { return *m_logic; }

        protected:
            bool on_key(const key_event &e) override { return m_logic->handle_key(e); }

        private:
            std::shared_ptr<parts_panel_logic> m_logic;
        };

        void parts_panel_logic::build(panel &root)
        {
            const ui::theme t = m_services.theme();

            // The search, and how many it found.
            auto &top = root.emplace<row>(t.spacing);
            top.set_cross_align(align::center);
            auto &search = top.emplace<search_field>(m_state.search, "Search parts\xE2\x80\xA6");
            search.layout().flex = 1.0f;
            search.set_name("parts panel search");
            search.set_tooltip("Every word must match. A value is a value (\"1k\", \"100n\", \"4k7\"); a package, "
                               "tolerance or rating narrows it (\"0603\", \"1%\", \"50V\"); anything else is "
                               "looked for in the name, MPN and description. Escape clears.");
            search.changed.connect([this](const std::string &text) { search_edited(text); });
            search.navigate = [this](key k) { return navigate(k); };
            search.submitted.connect([this](const std::string &) {
                if (m_list == nullptr)
                    return;
                catch_up();
                if (m_list->selected() >= 0)
                    activate();
                else if (rows() > 0)
                {
                    m_list->set_selected(0);
                    select(0);
                }
            });
            m_search = &search;
            m_count = &top.emplace<label>("", text_role::dim);
            m_count->set_size(text_size::small);
            m_count->set_align(align::end);

            auto &scopes = root.emplace<row>(4.0f);
            scopes.set_cross_align(align::center);
            m_all = &scopes.emplace<button>("Library");
            m_all->set_name("parts panel all");
            m_all->set_toggle(true);
            m_all->set_tooltip("Every part in the parts library");
            m_all->clicked.connect([this] { set_scope(parts_scope::library); });
            m_project = &scopes.emplace<button>("Project");
            m_project->set_name("parts panel project");
            m_project->set_toggle(true);
            m_project->set_tooltip("Only the parts already in the open project");
            m_project->clicked.connect([this] { set_scope(parts_scope::project); });
            if (has_generic())
            {
                m_generic = &scopes.emplace<button>("Generic");
                m_generic->set_name("parts panel generic");
                m_generic->set_toggle(true);
                m_generic->set_tooltip("Placeholder parts (a resistor, a capacitor, ...) to choose a real part for "
                                       "later, and power ports");
                m_generic->clicked.connect([this] { set_scope(parts_scope::generic); });
            }
            m_scopes = &scopes;

            // Assign mode: what is being assigned, and the way out.
            auto &banner = root.emplace<panel>("", surface::raised, 4.0f, insets::symmetric(8.0f, 6.0f));
            banner.set_name("parts panel assign banner");
            banner.set_border(true);
            m_banner_text = &banner.emplace<label>("");
            m_banner_text->set_wrap(true, 3);
            auto &banner_row = banner.emplace<row>(t.spacing);
            banner_row.set_cross_align(align::center);
            m_banner_count = &banner_row.emplace<label>("", text_role::dim);
            m_banner_count->set_size(text_size::small);
            m_banner_count->layout().flex = 1.0f;
            auto &cancel = banner_row.emplace<button>("Cancel", icon::close);
            cancel.set_name("parts panel assign cancel");
            cancel.set_tooltip("Stop assigning a part (Escape)");
            cancel.clicked.connect([this] { end_assign(); });
            m_banner = &banner;

            auto &list = root.emplace<part_rows>();
            list.set_name("parts panel list");
            list.layout().flex = 1.0f;
            list.layout().min_size = {0.0f, 120.0f};
            list.count = [this] { return rows(); };
            list.row = [this](std::size_t index) { return row_line(index); };
            list.selection_changed.connect([this](int index) { select(index); });
            list.activated.connect([this](int) { activate(); });
            m_list = &list;

            // The selection: its name; maker, MPN and package; every
            // parameter.
            auto &strip = root.emplace<panel>("", surface::field, 2.0f, insets::symmetric(8.0f, 6.0f));
            strip.set_border(true);
            m_name = &strip.emplace<label>("");
            m_name->set_wrap(true, 2);
            m_line = &strip.emplace<label>("", text_role::dim);
            m_line->set_size(text_size::small);
            m_line->set_wrap(true, 2);
            m_line2 = &strip.emplace<label>("", text_role::dim);
            m_line2->set_size(text_size::small);
            m_line2->set_wrap(true, 4);

            auto &actions = root.emplace<row>(t.spacing);
            if (m_options.assign)
            {
                m_assign = &actions.emplace<button>("Assign", icon::check, button_kind::primary);
                m_assign->set_name("parts panel assign");
                m_assign->clicked.connect([this] { assign_selected(); });
            }
            if (placing() || has_generic())
            {
                m_place = &actions.emplace<button>("Place", icon::component, button_kind::primary);
                m_place->set_name("parts panel place");
                m_place->clicked.connect([this] { place_selected(); });
            }
            m_add = &actions.emplace<button>(placing() ? "Add" : "Add to Project", icon::plus,
                                             placing() ? button_kind::normal : button_kind::primary);
            m_add->set_name("parts panel add");
            m_add->set_tooltip("Copy the part into the open project's library (Enter)");
            m_add->clicked.connect([this] { add_selected(); });
            actions.emplace<spacer>();
            m_open = &actions.emplace<button>("Browse", icon::open);
            m_open->set_name("parts panel open");
            m_open->set_tooltip("Show this part in the Library Browser");
            m_open->clicked.connect([this] { open_in_browser(); });
            m_hint = &root.emplace<label>("", text_role::dim);
            m_hint->set_size(text_size::small);
            m_hint->set_wrap(true, 3);

            m_connections = {m_services.catalog_changed.connect([this] { catalog_changed(); }),
                             m_services.catalog_status_changed.connect([this] { refresh(); }),
                             m_services.project_changed.connect([this] { project_changed(); })};

            part_filter f;
            f.text = assigning() ? std::string() : m_state.search;
            m_parts.set_filter(std::move(f));
            m_parts.set_catalog(m_services.catalog());
            collect_candidates();
            refresh();
        }

        bool parts_panel_logic::handle_key(const key_event &e)
        {
            if (e.action == action::release || (e.mods & held_modifiers) != modifiers::none || m_list == nullptr)
                return false;
            if (e.key != key::escape || m_search == nullptr)
                return false;
            if (!m_search->text().empty())
            {
                m_search->set_text("");
                search_edited("");
                return true;
            }
            if (assigning())
            {
                end_assign();
                return true;
            }
            return false;
        }

        bool parts_panel_logic::navigate(key k)
        {
            if (m_list == nullptr)
                return false;
            catch_up();
            const int count = static_cast<int>(rows());
            if (count == 0)
                return false;
            const int at = m_list->selected();
            int next = at;
            if (k == key::down)
                next = std::min(count - 1, at + 1);
            else if (k == key::up)
                next = std::max(0, at - 1);
            else if (k == key::page_down)
                next = std::min(count - 1, std::max(0, at) + 5);
            else if (k == key::page_up)
                next = std::max(0, at - 5);
            m_list->set_selected(next);
            select(next);
            return true;
        }

        std::size_t parts_panel_logic::rows() const
        {
            if (assigning())
                return m_candidates_shown.size();
            switch (scope())
            {
            case parts_scope::project:
                return m_project_shown.size();
            case parts_scope::generic:
                return m_generic_shown.size();
            case parts_scope::library:
                break;
            }
            return m_parts.size();
        }

        part_rows::line parts_panel_logic::row_line(std::size_t index)
        {
            if (assigning())
            {
                const part_candidate &c = m_candidates_shown[index];
                std::string from;
                if (c.in_project())
                    from = "in the project";
                else if (c.ref && m_parts.catalog() != nullptr)
                    from = m_parts.catalog()->library_of(*c.ref).name;
                part_rows::line l = part_line(*c.part, spec_summary(*c.part), from);
                if (c.other_symbol)
                    l.second += std::string(middle_dot) + "symbol " + c.part->symbol;
                return l;
            }
            switch (scope())
            {
            case parts_scope::project: {
                const project_row &r = m_project_rows[m_project_shown[index]];
                const library::project *project = m_services.project();
                const library::part *p = project != nullptr ? project->parts.find(r.id) : nullptr;
                part_rows::line l;
                if (p != nullptr)
                    l = part_line(*p, spec_summary(*p), r.status_text);
                else
                {
                    l.first = r.name;
                    l.second = r.status_text;
                }
                l.status = r.status;
                l.marked = true;
                return l;
            }
            case parts_scope::generic: {
                const generic_entry &g = generic_entries()[m_generic_shown[index]];
                part_rows::line l;
                l.first = std::string(g.name);
                if (g.what == generic_entry::kind::power)
                    l.second = schematic::is_ground_name(g.name) ? "Power port \xC2\xB7 ground" : "Power port";
                else
                {
                    l.first_right = std::string(g.value);
                    l.second = g.symbol == "V_DC" || g.symbol == "I_DC"
                                   ? "Simulation source \xC2\xB7 symbol " + std::string(g.symbol)
                                   : "Placeholder \xC2\xB7 symbol " + std::string(g.symbol);
                }
                return l;
            }
            case parts_scope::library:
                break;
            }
            return part_line(m_parts.part(index), std::string(m_parts.specs(index)),
                             m_parts.cell(index, part_column::library));
        }

        void parts_panel_logic::catalog_changed()
        {
            m_parts.set_catalog(m_services.catalog());
            collect_candidates();
            refresh();
        }

        void parts_panel_logic::project_changed()
        {
            collect_candidates();
            refresh();
        }

        void parts_panel_logic::set_scope(parts_scope s)
        {
            if (s == parts_scope::generic && !has_generic())
                s = parts_scope::library;
            const bool library_before = m_state.scope == parts_scope::library;
            m_state.scope = s;
            // The library's hits were left behind while another scope was
            // searched: catch up with the search text.
            if (s == parts_scope::library && !library_before && m_parts.filter().text != m_state.search)
            {
                apply_search();
                return;
            }
            refresh();
        }

        void parts_panel_logic::search_edited(const std::string &text)
        {
            m_state.search = text;
            ui::screen &s = m_host.screen();
            if (m_timer != 0)
            {
                s.cancel(m_timer);
                m_timer = 0;
            }
            // The small scopes are searched where they are.
            if (assigning() || scope() != parts_scope::library)
            {
                refresh();
                return;
            }
            if (part_search::wanted(m_parts))
            {
                part_filter f = m_parts.filter();
                f.text = m_state.search;
                m_searcher.start(m_parts.shared_catalog(), std::move(f),
                                 [this](const std::shared_ptr<const library::catalog> &catalog, part_filter filter,
                                        std::vector<library::part_ref> ranked, double ms) {
                                     if (m_parts.set_results(catalog, std::move(filter), std::move(ranked), ms))
                                         refresh();
                                 });
                return;
            }
            const double last = m_parts.search_milliseconds() + m_parts.sort_milliseconds();
            if (last < slow_search)
            {
                apply_search();
                return;
            }
            std::weak_ptr<parts_panel_logic> weak = weak_from_this();
            m_timer = s.schedule_after(search_pause, [weak] {
                if (auto self = weak.lock())
                {
                    self->m_timer = 0;
                    self->apply_search();
                }
            });
        }

        void parts_panel_logic::catch_up()
        {
            // The library's hits are still those of an earlier text (the
            // search waits for a pause in the typing, or runs in the
            // background): a key that acts on them searches now, so that
            // Down picks the first hit of what was typed, not of the text
            // before.
            if (assigning() || scope() != parts_scope::library || m_parts.filter().text == m_state.search)
                return;
            if (m_timer != 0)
            {
                m_host.screen().cancel(m_timer);
                m_timer = 0;
            }
            apply_search();
        }

        void parts_panel_logic::apply_search()
        {
            // A search still running in the background is overtaken.
            m_searcher.cancel();
            part_filter f = m_parts.filter();
            f.text = m_state.search;
            m_parts.set_filter(std::move(f));
            refresh();
        }

        void parts_panel_logic::collect_candidates()
        {
            m_candidates.clear();
            if (!m_state.assigning)
                return;
            placeholder what;
            what.device = m_state.assigning->device;
            what.value = m_state.assigning->value;
            what.symbol = m_state.assigning->symbol;
            m_candidates = match_parts(what, m_services.catalog().get(), m_services.project());
        }

        std::string parts_panel_logic::candidate_key(const part_candidate &c, const library::catalog *catalog)
        {
            if (c.in_project())
                return "project:" + c.project_id;
            return c.ref && catalog != nullptr ? catalog->qualified_id(*c.ref) : std::string();
        }

        void parts_panel_logic::refresh()
        {
            if (m_list == nullptr)
                return;
            const library::project *p = m_services.project();
            const library::catalog *catalog = m_services.catalog().get();
            m_project_rows = p != nullptr ? project_rows(*p, catalog) : std::vector<project_row>{};
            m_project_shown.clear();
            m_generic_shown.clear();
            m_candidates_shown.clear();
            if (assigning())
                m_candidates_shown = filter_candidates(m_candidates, catalog, m_state.search);
            else if (scope() == parts_scope::project && p != nullptr)
                m_project_shown = project_matches(*p, m_project_rows, catalog, m_state.search);
            else if (scope() == parts_scope::generic)
            {
                const auto entries = generic_entries();
                for (std::size_t i = 0; i < entries.size(); ++i)
                {
                    if (offered(entries[i]) && matches(entries[i], m_state.search))
                        m_generic_shown.push_back(i);
                }
            }

            m_all->set_checked(scope() == parts_scope::library);
            m_project->set_checked(scope() == parts_scope::project);
            if (m_generic != nullptr)
                m_generic->set_checked(scope() == parts_scope::generic);
            m_scopes->set_visible(!assigning());
            show_assign_banner();

            // What an empty list says.
            if (assigning())
            {
                if (m_candidates.empty())
                    m_list->empty_text = catalog == nullptr && m_services.catalog_loading()
                                             ? "Loading the parts library\xE2\x80\xA6"
                                             : "No part in the library fits: none has this device, value and "
                                               "pins. Make one in the Part Editor, or change the value.";
                else
                    m_list->empty_text = "No fitting part matches the search.";
            }
            else if (scope() == parts_scope::project)
            {
                if (p == nullptr)
                    m_list->empty_text = "No project is open. Open or create one in the Project Manager.";
                else if (m_project_rows.empty())
                    m_list->empty_text = "The project has no parts yet. Find them under Library.";
                else
                    m_list->empty_text = "No project part matches the search.";
            }
            else if (scope() == parts_scope::generic)
                m_list->empty_text = "Nothing generic matches the search.";
            else if (m_parts.catalog() == nullptr)
                m_list->empty_text = m_services.catalog_loading() ? "Loading the parts library\xE2\x80\xA6"
                                                                  : "The parts library is not loaded.";
            else if (m_parts.total() == 0)
                m_list->empty_text = "No parts libraries were found.";
            else
                m_list->empty_text = "No parts match the search.";
            m_list->changed();

            // The selection, found again by its key.
            int selected = -1;
            if (assigning())
            {
                for (std::size_t i = 0; i < m_candidates_shown.size(); ++i)
                {
                    if (candidate_key(m_candidates_shown[i], catalog) == m_state.selected_candidate)
                        selected = static_cast<int>(i);
                }
                // The best candidate is chosen to start with.
                if (selected < 0 && !m_candidates_shown.empty())
                {
                    selected = 0;
                    m_state.selected_candidate = candidate_key(m_candidates_shown[0], catalog);
                }
            }
            else if (scope() == parts_scope::project)
            {
                for (std::size_t i = 0; i < m_project_shown.size(); ++i)
                {
                    if (m_project_rows[m_project_shown[i]].id == m_state.selected_project)
                        selected = static_cast<int>(i);
                }
            }
            else if (scope() == parts_scope::generic)
            {
                for (std::size_t i = 0; i < m_generic_shown.size(); ++i)
                {
                    if (generic_entries()[m_generic_shown[i]].name == m_state.selected_generic)
                        selected = static_cast<int>(i);
                }
            }
            else if (const auto found = m_parts.find(m_state.selected))
            {
                selected = static_cast<int>(*found);
            }
            m_list->set_selected(selected);
            if (selected < 0)
                m_list->scroll_to(0.0f);
            else
                m_list->ensure_visible(selected);

            const std::size_t shown = rows();
            std::size_t of = m_parts.total();
            if (assigning())
                of = m_candidates.size();
            else if (scope() == parts_scope::project)
                of = m_project_rows.size();
            else if (scope() == parts_scope::generic)
                of = static_cast<std::size_t>(std::ranges::count_if(
                    generic_entries(), [this](const generic_entry &g) { return offered(g); }));
            const std::string counted = shown == of ? grouped(of) + (of == 1 ? " part" : " parts")
                                                    : std::format("{} of {}", grouped(shown), grouped(of));
            m_count->set_text(counted);
            if (m_banner_count != nullptr)
                m_banner_count->set_text(of == 0 ? std::string() : counted + (of == 1 ? " fits" : " fit"));
            show_selection();
        }

        void parts_panel_logic::show_assign_banner()
        {
            if (m_banner == nullptr)
                return;
            m_banner->set_visible(assigning());
            if (assigning())
                m_banner_text->set_text(m_state.assigning->title);
        }

        void parts_panel_logic::select(int row)
        {
            const library::catalog *catalog = m_parts.catalog();
            if (row < 0 || static_cast<std::size_t>(row) >= rows())
            {
                // Deselected: nothing is chosen.
                if (assigning())
                    m_state.selected_candidate.clear();
                else if (scope() == parts_scope::project)
                    m_state.selected_project.clear();
                else if (scope() == parts_scope::generic)
                    m_state.selected_generic.clear();
                else
                    m_state.selected.clear();
                show_selection();
                return;
            }
            const auto at = static_cast<std::size_t>(row);
            if (assigning())
                m_state.selected_candidate = candidate_key(m_candidates_shown[at], catalog);
            else if (scope() == parts_scope::project)
                m_state.selected_project = m_project_rows[m_project_shown[at]].id;
            else if (scope() == parts_scope::generic)
                m_state.selected_generic = std::string(generic_entries()[m_generic_shown[at]].name);
            else
                m_state.selected = m_parts.qualified_id(at);
            show_selection();
        }

        const library::part *parts_panel_logic::selected_part() const
        {
            const library::catalog *c = m_parts.catalog();
            if (c == nullptr || m_state.selected.empty())
                return nullptr;
            const auto ref = c->find(m_state.selected);
            return ref ? &c->get(*ref) : nullptr;
        }

        const project_row *parts_panel_logic::selected_project_row() const
        {
            for (const project_row &r : m_project_rows)
            {
                if (r.id == m_state.selected_project)
                    return &r;
            }
            return nullptr;
        }

        const generic_entry *parts_panel_logic::selected_generic() const
        {
            for (const generic_entry &g : generic_entries())
            {
                if (g.name == m_state.selected_generic)
                    return &g;
            }
            return nullptr;
        }

        const part_candidate *parts_panel_logic::selected_candidate() const
        {
            for (const part_candidate &c : m_candidates_shown)
            {
                if (candidate_key(c, m_parts.catalog()) == m_state.selected_candidate)
                    return &c;
            }
            return nullptr;
        }

        void parts_panel_logic::show_selection()
        {
            const library::project *project = m_services.project();
            std::string name;
            std::string line;
            std::string line2;
            std::string hint;
            bool can_add = false;
            bool can_open = false;
            bool can_place = false;
            std::string place_tip = "Choose a part to place";

            // A library part's strip: maker, MPN and package; its
            // parameters, or else its description.
            const auto describe_part = [&](const library::part &p) {
                name = p.name.empty() ? p.id : p.name;
                line = maker_text(p);
                if (!p.package.empty())
                    line += (line.empty() ? "" : std::string(middle_dot)) + p.package;
                line2 = parameter_summary(p);
                if (line2.empty())
                    line2 = p.description.empty() ? p.category : p.description;
            };

            if (assigning())
            {
                const part_candidate *c = selected_candidate();
                if (c != nullptr)
                {
                    describe_part(*c->part);
                    if (c->other_symbol)
                        line2 = "Drawn with symbol " + c->part->symbol + ", whose pins match. " + line2;
                    can_open = true;
                }
                if (project == nullptr)
                    hint = "Open a project to assign parts: an assigned part is kept in the project's library.";
            }
            else if (scope() == parts_scope::project)
            {
                const project_row *r = selected_project_row();
                const library::part *part = r != nullptr && project != nullptr ? project->parts.find(r->id) : nullptr;
                if (part != nullptr)
                {
                    describe_part(*part);
                    line = r->status_text + (r->source.empty() ? "" : std::string(middle_dot) + r->source);
                    if (const std::string maker = maker_text(*part); !maker.empty())
                        line = maker + std::string(middle_dot) + line;
                    can_open = true;
                    can_place = true;
                    place_tip = "Place the part on the sheet (Enter, or double-click)";
                }
            }
            else if (scope() == parts_scope::generic)
            {
                if (const generic_entry *g = selected_generic())
                {
                    name = std::string(g->name);
                    can_place = true;
                    if (g->what == generic_entry::kind::power)
                    {
                        line = "Power port";
                        line2 = "Names the net " + std::string(g->name) +
                                ", the same net wherever a port of that name is, on every sheet.";
                        place_tip = "Place the power port (Enter, or double-click)";
                    }
                    else if (g->symbol == "V_DC" || g->symbol == "I_DC")
                    {
                        line = "Symbol " + std::string(g->symbol) + (g->value.empty() ? "" : ", " + std::string(g->value));
                        line2 = "A source for the simulation: no part is bought for it.";
                        place_tip = "Place the source (Enter, or double-click)";
                    }
                    else
                    {
                        line = "Symbol " + std::string(g->symbol) +
                               (g->value.empty() ? "" : std::string(middle_dot) + "starts as " + std::string(g->value));
                        line2 = "A placeholder: no manufacturer part yet. Assign one later from the parts that fit "
                                "its value (Assign Part\xE2\x80\xA6 on the sheet).";
                        place_tip = "Place a placeholder (Enter, or double-click)";
                    }
                }
            }
            else
            {
                const library::part *p = selected_part();
                if (p != nullptr)
                {
                    describe_part(*p);
                    if (const auto ref = m_parts.catalog()->find(m_state.selected))
                        line += (line.empty() ? std::string() : std::string(middle_dot)) +
                                m_parts.catalog()->library_of(*ref).name;
                    can_open = true;
                    can_place = true;
                    place_tip = project != nullptr ? "Add the part to the project if need be, then place it on the "
                                                     "sheet (Enter, or double-click)"
                                                   : "Place the part on the sheet (Enter, or double-click)";
                    const std::string there = m_services.project_part_of(m_state.selected);
                    if (project == nullptr)
                        hint = "Open or create a project to add parts to it.";
                    else if (!there.empty())
                        hint = "Already in the project as " + there + ".";
                    else
                        can_add = true;
                    m_add->set_text(!there.empty() ? "In Project" : placing() ? "Add" : "Add to Project");
                    m_add->set_icon(there.empty() ? icon::plus : icon::check);
                }
            }

            const bool library_scope = !assigning() && scope() == parts_scope::library;
            m_add->set_visible(library_scope);
            m_open->set_visible(!(scope() == parts_scope::generic && !assigning()));
            if (m_place != nullptr)
            {
                // A generic entry only where it can be placed; a library
                // part only for an editor that places them.
                const bool shown = !assigning() && (scope() == parts_scope::generic ? has_generic() : placing());
                m_place->set_visible(shown);
                m_place->set_enabled(can_place);
                m_place->set_tooltip(place_tip);
            }
            if (m_assign != nullptr)
            {
                const bool can_assign = assigning() && selected_candidate() != nullptr && project != nullptr;
                m_assign->set_visible(assigning());
                m_assign->set_enabled(can_assign);
                m_assign->set_tooltip(can_assign ? std::string("Use this part for the placeholders (Enter, or "
                                                               "double-click)")
                                      : project == nullptr ? std::string("Open a project to assign parts")
                                                           : std::string("Choose a part"));
            }

            const bool nothing = name.empty();
            m_name->set_text(nothing ? std::string(assigning() ? "No part chosen" : "No part selected") : name);
            m_name->set_role(nothing ? text_role::dim : text_role::normal);
            m_line->set_text(std::move(line));
            m_line->set_visible(!nothing && !m_line->text().empty());
            m_line2->set_text(std::move(line2));
            m_line2->set_visible(!nothing && !m_line2->text().empty());
            m_add->set_enabled(can_add);
            m_add->set_tooltip(!can_add   ? hint
                               : placing() ? std::string("Copy the part into the open project's library")
                                           : std::string("Copy the part into the open project's library (Enter)"));
            m_open->set_enabled(can_open);
            m_hint->set_text(std::move(hint));
            m_hint->set_visible(!m_hint->text().empty());
        }

        void parts_panel_logic::add_selected()
        {
            if (assigning() || scope() != parts_scope::library)
                return;
            const library::part *p = selected_part();
            if (p == nullptr)
                return;
            if (m_services.project() == nullptr)
            {
                m_host.show_message("No project is open: create or open one in the Project Manager first",
                                    std::chrono::seconds(6), log_level::warning);
                return;
            }
            if (const std::string there = m_services.project_part_of(m_state.selected); !there.empty())
            {
                m_host.show_message("Already in the project as " + there);
                return;
            }
            auto added = m_services.add_to_project(m_state.selected);
            if (!added)
            {
                m_host.show_message("Not added: " + added.error(), std::chrono::seconds(8), log_level::error);
                return;
            }
            m_host.show_message(std::format("Added {} to the project as {} (not saved yet)",
                                            p->name.empty() ? p->id : p->name, *added));
        }

        void parts_panel_logic::activate()
        {
            if (assigning())
                assign_selected();
            else if (scope() == parts_scope::generic || placing())
                place_selected();
            else if (scope() == parts_scope::project)
                open_in_browser();
            else
                add_selected();
        }

        void parts_panel_logic::place_selected()
        {
            if (assigning())
                return;
            if (scope() == parts_scope::generic)
            {
                const generic_entry *g = selected_generic();
                if (g == nullptr)
                    return;
                if (g->what == generic_entry::kind::power)
                {
                    if (m_options.place_power)
                        m_options.place_power(std::string(g->name));
                }
                else if (m_options.place_symbol)
                    m_options.place_symbol(std::string(g->symbol), std::string(g->value));
                return;
            }
            if (!placing())
                return;
            if (scope() == parts_scope::project)
            {
                const library::project *project = m_services.project();
                const library::part *part =
                    project != nullptr && selected_project_row() != nullptr
                        ? project->parts.find(m_state.selected_project)
                        : nullptr;
                if (part != nullptr)
                    m_options.place(*part, part->id);
                return;
            }

            const library::part *p = selected_part();
            if (p == nullptr)
                return;
            if (m_services.project() == nullptr)
            {
                // The scratch sheet: a part of no library.
                m_options.place(*p, {});
                return;
            }

            // Into the project's library first (once), and placed as that
            // copy.
            const bool there = !m_services.project_part_of(m_state.selected).empty();
            auto added = m_services.add_to_project(m_state.selected);
            if (!added)
            {
                m_host.show_message("Not placed: " + added.error(), std::chrono::seconds(8), log_level::error);
                return;
            }
            const library::part *copy = m_services.project()->parts.find(*added);
            if (copy == nullptr)
                return;
            if (!there)
                m_host.show_message(std::format("Added {} to the project as {} (not saved yet)",
                                                p->name.empty() ? p->id : p->name, *added));
            // The project changed: a copy, so that refreshing the panel on
            // the way cannot take it from under the editor.
            const library::part placed = *copy;
            m_options.place(placed, placed.id);
        }

        void parts_panel_logic::assign_selected()
        {
            if (!assigning() || !m_options.assign)
                return;
            const part_candidate *c = selected_candidate();
            if (c == nullptr)
                return;
            if (m_services.project() == nullptr)
            {
                m_host.show_message("Open a project to assign parts: an assigned part is kept in the project's "
                                    "library",
                                    std::chrono::seconds(6), log_level::warning);
                return;
            }
            // What the candidate is, taken now: adding a part changes the
            // project, which makes the candidates again.
            std::string project_id = c->project_id;
            const std::string qualified = c->in_project() ? std::string() : candidate_key(*c, m_parts.catalog());
            const std::string name = c->part->name.empty() ? c->part->id : c->part->name;
            if (project_id.empty())
            {
                const bool there = !m_services.project_part_of(qualified).empty();
                auto added = m_services.add_to_project(qualified);
                if (!added)
                {
                    m_host.show_message("Not assigned: " + added.error(), std::chrono::seconds(8), log_level::error);
                    return;
                }
                project_id = *added;
                if (!there)
                    m_host.show_message(
                        std::format("Added {} to the project as {} (not saved yet)", name, project_id));
            }
            const library::project *project = m_services.project();
            const library::part *chosen = project != nullptr ? project->parts.find(project_id) : nullptr;
            if (chosen == nullptr)
                return;
            const library::part part = *chosen;
            const part_assignment what = *m_state.assigning;
            end_assign();
            m_options.assign(what, part, project_id);
        }

        void parts_panel_logic::open_in_browser()
        {
            if (assigning())
            {
                if (const part_candidate *c = selected_candidate())
                {
                    if (c->in_project())
                        m_services.reveal_in_library(c->project_id, true);
                    else
                        m_services.reveal_in_library(candidate_key(*c, m_parts.catalog()), false);
                }
                return;
            }
            if (scope() == parts_scope::project)
            {
                if (selected_project_row() != nullptr)
                    m_services.reveal_in_library(m_state.selected_project, true);
            }
            else if (scope() == parts_scope::library && selected_part() != nullptr)
            {
                m_services.reveal_in_library(m_state.selected, false);
            }
        }

        void parts_panel_logic::focus_search()
        {
            // In the next update, not now: a key that asks for this (the
            // schematic editor's A) has its character still to come, which
            // the field would take as typing.
            std::weak_ptr<parts_panel_logic> weak = weak_from_this();
            m_host.screen().post([weak] {
                auto self = weak.lock();
                if (!self || self->m_search == nullptr)
                    return;
                self->m_search->focus();
                self->m_search->select_all();
            });
        }

        bool parts_panel_logic::begin_assign(part_assignment what)
        {
            if (!m_options.assign)
                return false;
            if (!assigning())
                m_state.search_before = m_state.search;
            m_state.assigning = std::move(what);
            m_state.selected_candidate.clear();
            m_state.search.clear();
            if (m_timer != 0)
            {
                m_host.screen().cancel(m_timer);
                m_timer = 0;
            }
            m_searcher.cancel();
            if (m_search != nullptr)
                m_search->set_text("");
            collect_candidates();
            refresh();
            return true;
        }

        void parts_panel_logic::end_assign()
        {
            if (!assigning())
                return;
            m_state.assigning.reset();
            m_state.selected_candidate.clear();
            m_candidates.clear();
            m_candidates_shown.clear();
            m_state.search = std::move(m_state.search_before);
            m_state.search_before.clear();
            if (m_search != nullptr)
                m_search->set_text(m_state.search);
            if (scope() == parts_scope::library && m_parts.filter().text != m_state.search)
                apply_search();
            else
                refresh();
        }

        parts_panel_widget *panel_of(widget &w)
        {
            return dynamic_cast<parts_panel_widget *>(&w);
        }
    } // namespace

    std::unique_ptr<widget> make_parts_panel(window_host &host, parts_panel_options options)
    {
        return std::make_unique<parts_panel_widget>(host, std::move(options));
    }

    void focus_parts_search(widget &panel)
    {
        if (parts_panel_widget *p = panel_of(panel))
            p->logic().focus_search();
    }

    bool begin_part_assignment(widget &panel, part_assignment what)
    {
        parts_panel_widget *p = panel_of(panel);
        return p != nullptr && p->logic().begin_assign(std::move(what));
    }

    void end_part_assignment(widget &panel)
    {
        if (parts_panel_widget *p = panel_of(panel))
            p->logic().end_assign();
    }

    bool assigning_part(const widget &panel)
    {
        const auto *p = dynamic_cast<const parts_panel_widget *>(&panel);
        return p != nullptr && p->logic().assigning();
    }

} // namespace volt::ui
