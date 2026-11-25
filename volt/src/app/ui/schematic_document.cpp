#include <app/ui/schematic_document.hpp>

#include <library/project.hpp>
#include <schematic/demo.hpp>
#include <schematic/io.hpp>

#include <algorithm>
#include <format>
#include <system_error>

namespace volt::ui
{
    namespace
    {
        // A colour part of the way from `from` to `to`, alpha from `from`.
        math::vec4f toward(const math::vec4f &from, const math::vec4f &to, float amount)
        {
            math::vec4f out = from;
            for (std::size_t i = 0; i < 3; ++i)
                out[i] = from[i] + (to[i] - from[i]) * amount;
            return out;
        }

        math::vec4f with_alpha(math::vec4f color, float alpha)
        {
            color[3] = alpha;
            return color;
        }

        std::vector<schematic_document::new_sheet> scratch_sheets()
        {
            std::vector<schematic_document::new_sheet> sheets;
            sheets.push_back({std::string(schematic_document::scratch_name), {}, schematic::demo_ladder(), true});
            return sheets;
        }

        // How a sheet is named in a message: its name, and its file.
        std::string described(const library::sheet &entry)
        {
            if (entry.name.empty())
                return entry.file;
            return std::format("{} ({})", entry.name, entry.file);
        }
    } // namespace

    schematic_document::schematic_document()
    {
        // The dark theme's canvas until apply_theme() brings the chosen one.
        scene.view.zoom = 36.0f;
        scene.background = {0.0060f, 0.0070f, 0.0100f};
        scene.grid_color = {1.0f, 1.0f, 1.0f, 0.08f};
        scene.grid = grid;
        open_scratch();
    }

    schematic_document::~schematic_document() = default;

    // ---- the view ---------------------------------------------------------------------

    void schematic_document::apply_grid()
    {
        scene.grid = show_grid ? grid : 0.0f;
        mark_dirty();
    }

    void schematic_document::apply_wire_width()
    {
        m_style.wire_width = wire_width;
        m_style.junction_diameter = wire_width * 3.0f;
        style_changed();
    }

    void schematic_document::apply_theme(const ui::theme &theme)
    {
        scene.background = {theme.canvas[0], theme.canvas[1], theme.canvas[2]};
        scene.grid_color = theme.canvas_grid;

        m_style.wire = theme.canvas_wire;
        m_style.symbol = theme.canvas_symbol;
        m_style.text = theme.canvas_symbol;
        m_style.power_label = theme.canvas_symbol;
        // Pin text and notes recede towards the sheet.
        m_style.pin_text = toward(theme.canvas_symbol, theme.canvas, 0.4f);
        m_style.note = toward(theme.canvas_symbol, theme.canvas, 0.3f);
        m_style.unconnected = with_alpha(theme.error, 1.0f);
        m_style.selection = with_alpha(theme.accent, 1.0f);
        m_style.ghost = with_alpha(theme.accent, 0.5f);
        m_style.rubber_band = with_alpha(theme.accent, 0.9f);
        m_style.marker = with_alpha(theme.accent, 1.0f);
        style_changed();
    }

    void schematic_document::style_changed()
    {
        m_scene_stale = true;
        mark_dirty();
    }

    bool schematic_document::update_scene()
    {
        const sheet_key key = active_key();
        const bool current = simulation_current();
        if (!m_scene_stale && m_built && *m_built == key && m_built_current == current)
            return false;

        schematic::draw_options options;
        options.style = m_style;
        options.selection = m_selection;
        options.wire_nodes = current ? &m_simulated->wire_nodes : nullptr;
        options.rules = m_connectivity;

        scene.segments.clear();
        schematic::append_sheet(scene.segments, active().sheet(), options);
        scene.segments.insert(scene.segments.end(), m_overlay.begin(), m_overlay.end());

        m_built = key;
        m_built_current = current;
        m_scene_stale = false;
        return true;
    }

    // ---- the sheets --------------------------------------------------------------------

    schematic_document::sheet_key schematic_document::active_key() const noexcept
    {
        return {m_generation, m_active, active().revision()};
    }

    bool schematic_document::set_active(std::size_t i)
    {
        if (i >= m_sheets.size() || i == m_active)
            return false;
        m_active = i;
        m_selection.clear();
        m_overlay.clear();
        m_scene_stale = true;
        mark_dirty();
        sheets_changed.emit();
        return true;
    }

    bool schematic_document::modified() const noexcept
    {
        for (const std::unique_ptr<open_sheet> &s : m_sheets)
        {
            if (s->document.modified())
                return true;
        }
        return false;
    }

    void schematic_document::open_scratch()
    {
        m_connectivity = {};
        replace(scratch_sheets(), true);
    }

    void schematic_document::set_sheets(std::vector<new_sheet> sheets)
    {
        if (sheets.empty())
        {
            // The scratch sheet, read by the rules as they are.
            replace(scratch_sheets(), true);
            return;
        }
        replace(std::move(sheets), false);
    }

    void schematic_document::set_connectivity(const schematic::connectivity &rules)
    {
        if (rules == m_connectivity)
            return;
        m_connectivity = rules;
        m_derived.reset();
        m_scene_stale = true;
        mark_dirty();
        connectivity_changed.emit();
    }

    void schematic_document::replace(std::vector<new_sheet> sheets, bool scratch)
    {
        // The sheets going stay alive until everyone has heard of the new
        // ones (sheets_changed): whoever edits through a pointer to one (the
        // editor's tools, with a drag's transaction open in it) lets go of
        // it then, not after it is gone.
        std::vector<std::unique_ptr<open_sheet>> retired = std::move(m_sheets);
        m_sheets.clear();
        for (new_sheet &s : sheets)
        {
            auto opened = std::make_unique<open_sheet>(open_sheet{std::move(s.name), std::move(s.file),
                                                                  schematic::document(std::move(s.sheet)), s.loaded});
            // The pointer stays valid: the sheet is never moved out of its
            // allocation, and its callback goes with it.
            const open_sheet *self = opened.get();
            opened->document.on_change([this, self] { sheet_changed(self); });
            m_sheets.push_back(std::move(opened));
        }
        m_active = 0;
        m_scratch = scratch;
        ++m_generation;
        m_selection.clear();
        m_overlay.clear();
        m_derived.reset();
        m_scene_stale = true;

        const bool was_modified = std::exchange(m_was_modified, modified());
        mark_dirty();
        sheets_changed.emit();
        if (was_modified != m_was_modified)
            modified_changed.emit();

        // They go now; their callbacks with them.
        for (std::unique_ptr<open_sheet> &old : retired)
            old->document.on_change(nullptr);
    }

    void schematic_document::sheet_changed(const open_sheet *changed_sheet)
    {
        // A sheet that is no longer open (being replaced) changes nothing
        // here.
        const auto open = [&](const std::unique_ptr<open_sheet> &s) { return s.get() == changed_sheet; };
        if (std::ranges::none_of(m_sheets, open))
            return;
        if (changed_sheet == m_sheets[m_active].get())
            mark_dirty();
        const bool now = modified();
        if (now != m_was_modified)
        {
            m_was_modified = now;
            modified_changed.emit();
        }
    }

    // ---- a project's sheets ------------------------------------------------------------

    std::vector<schematic_document::load_problem> schematic_document::open_project_sheets(
        const std::filesystem::path &folder, std::span<const library::sheet> sheets,
        const schematic::connectivity &rules)
    {
        std::vector<load_problem> problems;
        std::vector<new_sheet> opened;
        for (const library::sheet &entry : sheets)
        {
            new_sheet s{entry.name, entry.file, {}, true};
            if (s.name.empty())
                s.name = library::path_to_utf8(library::path_from_utf8(entry.file).stem());
            if (entry.file.empty())
            {
                // Nowhere to read it from, and nowhere to save it to.
                s.loaded = false;
                problems.push_back({std::format("Sheet {} has no file in the project: it opens empty, and is not "
                                                "saved",
                                                s.name),
                                    true});
                opened.push_back(std::move(s));
                continue;
            }

            const std::filesystem::path path = library::resolve_path(folder, entry.file);
            std::error_code error;
            const bool exists = std::filesystem::exists(path, error);
            if (!exists && !error)
            {
                // Not written yet (a new project's): a new sheet, which the
                // next save writes.
                opened.push_back(std::move(s));
                continue;
            }

            auto loaded = schematic::load_sheet(path);
            if (!loaded)
            {
                s.loaded = false;
                problems.push_back({std::format("Sheet {} did not load: {}. It opens empty, and saving leaves its "
                                                "file as it is",
                                                described(entry), loaded.error()),
                                    true});
                opened.push_back(std::move(s));
                continue;
            }

            if (!loaded->report.clean())
            {
                std::string text = std::format("Sheet {} loaded with problems: {}", described(entry),
                                               loaded->report.summary());
                if (loaded->report.records_skipped > 0)
                    text += " (what was left out is lost if the sheet is saved)";
                problems.push_back({std::move(text), false});
            }
            s.sheet = std::move(loaded->sheet);
            opened.push_back(std::move(s));
        }
        // Before the sheets open, so that whoever hears of them reads them
        // by the project's rules.
        m_connectivity = rules;
        set_sheets(std::move(opened));
        return problems;
    }

    std::expected<schematic_document::save_report, std::string> schematic_document::save_project_sheets(
        const std::filesystem::path &folder)
    {
        save_report report;
        if (m_scratch)
            return report;

        for (const std::unique_ptr<open_sheet> &s : m_sheets)
        {
            if (!s->loaded)
            {
                report.skipped.push_back(s->name);
                continue;
            }

            const std::filesystem::path path = library::resolve_path(folder, s->file);
            std::error_code error;
            const bool exists = std::filesystem::exists(path, error);
            if (!s->document.modified() && exists)
                continue;

            if (const std::filesystem::path parent = path.parent_path(); !parent.empty())
                std::filesystem::create_directories(parent, error);
            if (auto saved = schematic::save_sheet(path, s->document.sheet()); !saved)
                return std::unexpected(std::format("Cannot save sheet {}: {}", s->name, saved.error()));
            s->document.mark_saved();
            ++report.written;
        }
        return report;
    }

    std::expected<void, std::string> schematic_document::write_sheet(std::size_t i,
                                                                     const std::filesystem::path &path) const
    {
        std::error_code error;
        if (const std::filesystem::path parent = path.parent_path(); !parent.empty())
            std::filesystem::create_directories(parent, error);
        if (auto saved = schematic::save_sheet(path, m_sheets[i]->document.sheet()); !saved)
            return std::unexpected(std::format("Cannot save sheet {}: {}", m_sheets[i]->name, saved.error()));
        return {};
    }

    void schematic_document::sheet_saved_as(std::size_t i, std::string file)
    {
        m_sheets[i]->file = std::move(file);
        m_sheets[i]->document.mark_saved();
    }

    // ---- derived from the active sheet ------------------------------------------------------

    const schematic_document::derived &schematic_document::derive() const
    {
        const sheet_key key = active_key();
        if (!m_derived || m_derived->key != key)
        {
            const schematic::sheet &s = active().sheet();
            schematic::netlist nets = schematic::build_netlist(s, m_connectivity);
            schematic::circuit c = schematic::build_circuit(s, nets);
            m_derived = derived{key, std::move(nets), std::move(c)};
        }
        return *m_derived;
    }

    const schematic::netlist &schematic_document::netlist() const
    {
        return derive().nets;
    }

    const schematic::circuit &schematic_document::circuit() const
    {
        return derive().circuit;
    }

    bool schematic_document::sheet_bounds(schematic::point &min, schematic::point &max) const
    {
        return schematic::sheet_bounds(active().sheet(), min, max, m_style);
    }

    // ---- selection and overlay ------------------------------------------------------------

    void schematic_document::set_selection(std::span<const schematic::item_ref> items)
    {
        if (std::ranges::equal(items, m_selection))
            return;
        m_selection.assign(items.begin(), items.end());
        m_scene_stale = true;
        mark_dirty();
    }

    void schematic_document::set_overlay(std::vector<schematic::segment> segments)
    {
        if (segments.empty() && m_overlay.empty())
            return;
        m_overlay = std::move(segments);
        m_scene_stale = true;
        mark_dirty();
    }

    // ---- the simulation -----------------------------------------------------------------------

    void schematic_document::record_simulation()
    {
        const schematic::circuit &c = circuit();
        m_simulated = simulated{active_key(), c.wire_nodes, c.low, c.high};
        mark_dirty();
    }

    schematic_document::run_state schematic_document::simulation_state() const noexcept
    {
        if (!m_simulated || m_simulated->key.generation != m_generation)
            return run_state::not_run;
        if (m_simulated->key.index != m_active)
            return run_state::other_sheet;
        return m_simulated->key.revision == active().revision() ? run_state::current : run_state::edited;
    }

    float schematic_document::simulation_low() const noexcept
    {
        return m_simulated ? m_simulated->low : 0.0f;
    }

    float schematic_document::simulation_high() const noexcept
    {
        const float low = simulation_low();
        const float high = m_simulated ? m_simulated->high : 0.0f;
        return high > low ? high : low + 1.0f;
    }

} // namespace volt::ui
