#include <app/ui/services.hpp>

#include <app/ui/scale.hpp>
#include <app/ui/windows/content.hpp>
#include <app/ui/windows/library_clipboard.hpp>
#include <app/ui/windows/library_dialogs.hpp>
#include <image/io.hpp>
#include <library/catalog.hpp>
#include <library/package.hpp>
#include <library/project.hpp>
#include <model/io.hpp>
#include <platform/platform.hpp>

#include <math/electrical/engineering.hpp>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <iostream>

namespace volt::ui
{
    namespace
    {
        // A path as the interface shows it: its file name, UTF-8.
        std::string display_name(const std::filesystem::path &path)
        {
            const std::u8string name = path.filename().u8string();
            return std::string(name.begin(), name.end());
        }

        // Whether the path ends in `extension` (".voltproj"), in any case.
        bool has_extension(const std::filesystem::path &path, std::string_view extension)
        {
            const std::string text = library::path_to_utf8(path.extension());
            if (text.size() != extension.size())
                return false;
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
                if (lower(text[i]) != lower(extension[i]))
                    return false;
            }
            return true;
        }
    } // namespace

    app_services::app_services()
    {
        // A sheet going from saved to modified, or back, is a change of the
        // project's state: its title, Save, the question before closing.
        m_schematic.modified_changed.connect([this] {
            if (m_project && !m_sheets_busy)
                project_changed.emit();
        });
        // The connectivity rules are the project's: a change goes into it,
        // to be written by the next save.
        m_schematic.connectivity_changed.connect([this] {
            if (!m_project || m_sheets_busy)
                return;
            m_project->schematic.pins_connect_along_wires = m_schematic.connectivity().pins_inside_wires;
            mark_project_modified();
        });
    }

    app_services::~app_services()
    {
        // The loads post here: they stop before what they post to goes.
        stop_loading();
        clear_posted();
    }

    std::expected<void, std::string> app_services::create(services_setup setup)
    {
        m_setup = std::move(setup);
        m_settings.user_scale = clamp_user_scale(m_setup.user_scale);
        m_settings.output = m_setup.output;
        m_board.set_renderer(m_setup.board_renderer);
        m_schematic.apply_theme(theme());

        // Arial for everything, the stencil face for the brand mark.
        const std::filesystem::path fonts = m_setup.assets / "fonts";
        auto body = font::font::load_file(fonts / "DefaultFont.ttf");
        if (!body)
            return std::unexpected("cannot load the interface font: " + body.error());
        auto stencil = font::font::load_file(fonts / "SairaStencil.ttf");

        m_body_font = m_text.add(font::font_chain({*body}));
        m_title_font = m_body_font;
        if (stencil)
            m_title_font = m_text.add(font::font_chain({*stencil, *body}));

        // See font_text_engine::set_contrast(): the window blends coverage in
        // sRGB, which thins light text on dark; a little weight helps.
        m_text.set_contrast(0.0f, 0.25f);
        m_shared_text = std::make_unique<shared_text>(m_text);

        load_logo();

        // The parts library: scanned on the loader thread from the start,
        // so that it is there by the time anyone looks.
        m_library_directories = m_setup.library_directories;
        if (!m_library_directories.empty())
            rescan_catalog();
        return {};
    }

    // ---- settings -----------------------------------------------------------------------

    ui::theme app_services::theme() const
    {
        ui::theme theme = m_settings.theme == 1 ? ui::theme::light_theme() : ui::theme::dark_theme();
        theme.font = m_body_font;
        theme.title_font = m_title_font;
        return theme;
    }

    void app_services::set_settings(app_settings settings)
    {
        settings.theme = settings.theme == 1 ? 1 : 0;
        settings.user_scale = clamp_user_scale(settings.user_scale);
        if (settings == m_settings)
            return;

        const bool theme_changed = settings.theme != m_settings.theme;
        m_settings = settings;

        // Light text needs the weight, dark text does not.
        if (theme_changed)
        {
            m_text.set_contrast(0.0f, m_settings.theme == 1 ? 0.1f : 0.25f);
            m_schematic.apply_theme(theme());
        }
        if (m_windows != nullptr)
            m_windows->settings_changed();
        settings_changed.emit();
    }

    void app_services::set_theme(int index)
    {
        change_settings([index](app_settings &s) { s.theme = index; });
    }

    void app_services::set_user_scale(float scale)
    {
        change_settings([scale](app_settings &s) { s.user_scale = scale; });
    }

    void app_services::set_color_output(ui::color_output output)
    {
        change_settings([output](app_settings &s) { s.output = output; });
    }

    // ---- documents ------------------------------------------------------------------------

    std::string app_services::run_simulation()
    {
        // An empty network is handed over too: it clears what the last run
        // left (the render loop then runs nothing).
        schematic_document &doc = m_schematic;
        const schematic::circuit &circuit = doc.circuit();
        if (m_setup.run_simulation)
            m_setup.run_simulation(circuit.network);
        doc.record_simulation();

        // The first error, else the first warning: the circuit's (values,
        // sources), then the netlist's (connections).
        const schematic::problem *first = nullptr;
        for (const schematic::severity level : {schematic::severity::error, schematic::severity::warning})
        {
            for (const std::vector<schematic::problem> *list : {&circuit.problems, &doc.netlist().problems})
            {
                const auto found = std::ranges::find(*list, level, &schematic::problem::level);
                if (found != list->end())
                {
                    first = &*found;
                    break;
                }
            }
            if (first != nullptr)
                break;
        }

        std::string text = circuit.network.nodes == 0 ? std::string("Nothing to simulate") : circuit.summary;
        if (first != nullptr)
            text += " \xE2\x80\x94 " + first->message;
        return text;
    }

    // ---- images ---------------------------------------------------------------------------

    std::expected<texture_id, std::string> app_services::add_image(const texture_pixels &pixels)
    {
        if (!m_setup.add_image)
            return std::unexpected(std::string("images cannot be shown"));
        return m_setup.add_image(pixels);
    }

    void app_services::remove_image(texture_id id)
    {
        if (m_setup.remove_image && id.valid())
            m_setup.remove_image(id);
    }

    texture_id app_services::view_image(offscreen_view view) const noexcept
    {
        return view == offscreen_view::schematic ? m_schematic_image : m_board_image;
    }

    void app_services::load_logo()
    {
        // The SVG, drawn at the size it is shown at; the PNG, scaled, where
        // the SVG cannot be read.
        const std::filesystem::path images = m_setup.assets / "images";
        if (auto bytes = image::read_file(images / "volt.svg"))
        {
            if (auto document = image::svg::parse(std::span<const std::uint8_t>(*bytes)))
                m_logo_svg = std::move(*document);
            else
                std::cerr << "volt.svg: " << document.error() << std::endl;
        }
        if (!m_logo_svg)
        {
            if (auto png = image::load(images / "volt.png"))
                m_logo_png = prepare_texture(*png);
            else
                std::cerr << png.error() << std::endl;
        }
    }

    texture_id app_services::logo(int pixels)
    {
        pixels = std::clamp(pixels, 1, 1024);
        if (const auto found = m_logos.find(pixels); found != m_logos.end())
            return found->second;

        std::expected<texture_id, std::string> made = std::unexpected(std::string("no logo"));
        if (m_logo_svg)
        {
            // Square, in whole pixels; the drawing keeps its aspect ratio
            // inside.
            image::svg::rasterize_options options;
            options.width = static_cast<std::uint32_t>(pixels);
            options.height = options.width;
            auto raster = image::svg::rasterize(*m_logo_svg, options);
            if (raster)
                made = add_image(prepare_texture(*raster));
            else
                made = std::unexpected(std::move(raster).error());
        }
        else if (m_logo_png)
        {
            made = add_image(*m_logo_png);
        }
        if (!made)
        {
            std::cerr << "The logo: " << made.error() << std::endl;
            m_logos[pixels] = {};
            return {};
        }
        m_logos[pixels] = *made;
        return *made;
    }

    void app_services::trim_logos()
    {
        std::vector<int> used;
        for (const window_kind kind : all_window_kinds)
        {
            if (const window_host *host = window(kind))
            {
                const std::vector<int> sizes = host->logo_sizes();
                used.insert(used.end(), sizes.begin(), sizes.end());
            }
        }
        for (auto it = m_logos.begin(); it != m_logos.end();)
        {
            if (std::ranges::find(used, it->first) != used.end())
            {
                ++it;
                continue;
            }
            remove_image(it->second);
            it = m_logos.erase(it);
        }
    }

    // ---- files ----------------------------------------------------------------------------
    //
    // What happens goes to the message log (show_message()); what goes
    // wrong goes to stderr as well, and only that, so that an automated run
    // (--smoke-frames, --open) can take anything on stderr for a failure.

    void app_services::open_files(std::span<const std::string> paths, window_kind from,
                                  std::optional<math::vec2f> on_board)
    {
        // One project is open at a time.
        std::size_t projects = 0;
        for (const std::string &name : paths)
        {
            if (has_extension(std::filesystem::path(std::u8string(name.begin(), name.end())),
                              library::project_extension))
                ++projects;
        }
        if (projects > 1)
            show_message(from,
                         std::format("{} projects dropped: opening the first, one project is open at a time",
                                     projects),
                         std::chrono::seconds(8), log_level::warning);

        bool project_seen = false;
        for (const std::string &name : paths)
        {
            // The paths are UTF-8, which a std::string path is not on
            // Windows.
            const std::filesystem::path path(std::u8string(name.begin(), name.end()));
            if (has_extension(path, library::project_extension))
            {
                if (!std::exchange(project_seen, true))
                    open_project_file(path, from);
            }
            else if (has_extension(path, library::library_extension))
                open_library_file(path, from);
            else if (has_extension(path, library::package_extension))
                open_part_package(path, from);
            else if (model::format_of(path) != model::format::unknown)
                open_model(path, from, on_board);
            else if (image::format_from_extension(path) != image::file_format::unknown)
                open_image(path, from);
            else
                show_message(from,
                             "Cannot open " + display_name(path) +
                                 ": not a project, a library, a part package, a model or an image",
                             std::chrono::seconds(8), log_level::error);
        }
    }

    void app_services::open_link(const std::string &url, window_kind from)
    {
        if (!platform::is_web_url(url))
        {
            show_message(from, "Not opened: " + url + " is not a web address (http or https)",
                         std::chrono::seconds(8), log_level::warning);
            return;
        }
        if (auto opened = platform::open_url(url); !opened)
        {
            show_message(from, "Cannot open " + url + ": " + opened.error(), std::chrono::seconds(10),
                         log_level::error);
            return;
        }
        show_message(from, "Opened " + url + " in the web browser");
    }

    void app_services::start_load(std::function<void()> job)
    {
        ++m_loading;
        m_loader.push(std::move(job));
        if (m_setup.wake)
            m_setup.wake(); // the status bars show the progress
    }

    void app_services::finish_load()
    {
        if (m_loading > 0)
            --m_loading;
    }

    void app_services::stop_loading()
    {
        // A scan stops between files rather than reading the rest.
        m_scan_cancel->store(true);
        m_loader.stop();
        m_loading = 0;
        m_scanning = false;
        m_rescan_pending = false;

        // Its window is about to go, and the platform thread is held in it.
        close_file_dialog();
        m_file_dialog.reset();
    }

    void app_services::open_model(const std::filesystem::path &path, window_kind from,
                                  std::optional<math::vec2f> where)
    {
        const std::string file = display_name(path);
        show_message(from, "Loading " + file + "\xE2\x80\xA6", std::chrono::seconds(60));

        // STEP models from a part library (KiCad's) have their origin where
        // the footprint's is and z = 0 on the board, so they go as they are;
        // OBJ and STL carry no such convention and are stood on the board,
        // centred on the spot.
        const bool centre = model::format_of(path) != model::format::step;

        start_load([this, path, file, from, where, centre]() {
            auto loaded = model::load(path);
            if (!loaded)
            {
                post([this, from, error = std::move(loaded).error()] {
                    finish_load();
                    std::cerr << "Cannot open " << error << std::endl;
                    show_message(from, "Cannot open " + error, std::chrono::seconds(10), log_level::error);
                });
                return;
            }

            // The tessellated model as the renderer takes it: the slow part,
            // so it is done here too.
            auto prepared = std::make_shared<board::prepared_model>(board::prepare(loaded->model));
            auto report = std::make_shared<model::load_report>(std::move(loaded->report));
            post([this, file, from, prepared, report, where, centre] {
                finish_load();
                auto placed = m_board.place(file, *prepared, *report, where, centre);
                if (!placed)
                {
                    show_message(from, std::move(placed).error(), std::chrono::seconds(10), log_level::error);
                    return;
                }
                std::string text = std::move(*placed);
                if (!is_open(window_kind::board_editor))
                    text += " \xE2\x80\x94 on the board (Window > Board Editor)";
                show_message(from, std::move(text), std::chrono::seconds(8));
            });
        });
    }

    void app_services::open_image(const std::filesystem::path &path, window_kind from)
    {
        const std::string file = display_name(path);
        show_message(from, "Opening " + file + "\xE2\x80\xA6", std::chrono::seconds(60));
        const image::file_format format = image::format_from_extension(path);

        start_load([this, path, file, from, format]() {
            const auto started = clock::now();
            auto loaded = image::load(path);
            if (!loaded)
            {
                post([this, from, error = std::move(loaded).error()] {
                    finish_load();
                    std::cerr << "Cannot open " << error << std::endl;
                    show_message(from, "Cannot open " + error, std::chrono::seconds(10), log_level::error);
                });
                return;
            }

            const math::vec2i pixels{static_cast<int>(loaded->width()), static_cast<int>(loaded->height())};
            std::string details = std::format("{} \xC3\x97 {}  \xC2\xB7  {}{}", pixels.x(), pixels.y(),
                                              image::name(format),
                                              image::has_alpha(loaded->format()) ? ", with alpha" : "");

            // Decoded, converted and its mip levels made here; the interface
            // thread only uploads.
            const auto decoded = clock::now();
            auto prepared = std::make_shared<texture_pixels>(prepare_texture(*loaded));
            const std::chrono::duration<double> decoding = decoded - started;
            const std::chrono::duration<double> preparing = clock::now() - decoded;
            std::string timing =
                std::format("decoded in {:.2f} s, mip levels in {:.2f} s", decoding.count(), preparing.count());
            post([this, file, from, details = std::move(details), timing = std::move(timing), prepared, pixels] {
                finish_load();

                // The window it was dropped on may have closed meanwhile;
                // then there is nowhere to show it.
                window_host *host = window(from);
                if (host == nullptr)
                {
                    show_message(from, file + " was not shown: its window has closed", std::chrono::seconds(8),
                                 log_level::warning);
                    return;
                }
                auto image = add_image(*prepared);
                if (!image)
                {
                    host->show_message("Cannot show " + file + ": " + image.error(), std::chrono::seconds(10),
                                       log_level::error);
                    return;
                }
                host->show_image(*image, pixels, file, details);
                host->show_message("Opened " + file + ": " + timing);
            });
        });
    }

    // ---- the interface thread ---------------------------------------------------------------

    void app_services::post(std::function<void()> task)
    {
        {
            std::lock_guard lock(m_posted_mutex);
            m_posted.push_back(std::move(task));
        }
        if (m_setup.wake)
            m_setup.wake();
    }

    void app_services::run_posted()
    {
        std::vector<std::function<void()>> tasks;
        {
            std::lock_guard lock(m_posted_mutex);
            tasks.swap(m_posted);
        }
        for (auto &task : tasks)
            task();
    }

    void app_services::clear_posted()
    {
        std::vector<std::function<void()>> tasks;
        {
            std::lock_guard lock(m_posted_mutex);
            tasks.swap(m_posted);
        }
    }

    // ---- windows ------------------------------------------------------------------------------

    void app_services::open_window(window_kind kind)
    {
        if (m_windows != nullptr)
            m_windows->open_window(kind);
    }

    void app_services::close_window(window_kind kind)
    {
        if (m_windows != nullptr)
            m_windows->close_window(kind);
    }

    bool app_services::is_open(window_kind kind) const
    {
        return m_windows != nullptr && m_windows->is_open(kind);
    }

    window_host *app_services::window(window_kind kind)
    {
        return m_windows != nullptr ? m_windows->host(kind) : nullptr;
    }

    void app_services::exit()
    {
        // A file dialog up in some window would keep that window disabled,
        // and the question below unanswerable if it is asked there.
        close_file_dialog();

        // Not past unsaved work without asking: what the windows hold first
        // (an editor's changes, which may go into the project), then the
        // project's own changes.
        confirm_windows(0, [this] {
            confirm_discard(window_kind::project_manager, "Exit", [this] {
                if (m_windows != nullptr)
                    m_windows->exit();
            });
        });
    }

    void app_services::confirm_windows(std::size_t from, std::function<void()> then)
    {
        for (std::size_t i = from; i < all_window_kinds.size(); ++i)
        {
            // Looked up again at each step: a window may have closed while
            // the one before was asking.
            window_host *host = window(all_window_kinds[i]);
            if (host == nullptr || host->guards().empty())
                continue;
            host->guards().confirm([this, i, then] { confirm_windows(i + 1, then); });
            return;
        }
        if (then)
            then();
    }

    void app_services::show_message(window_kind where, std::string text, std::chrono::seconds duration,
                                    log_level level)
    {
        window_host *host = window(where);
        if (host == nullptr)
            host = window(window_kind::project_manager);
        if (host != nullptr)
            host->show_message(std::move(text), duration, level);
        else
            m_messages.add(where, std::move(text), level); // kept, if not shown
    }

    // ---- the project ----------------------------------------------------------------------------

    std::expected<void, std::string> app_services::new_project(const std::filesystem::path &directory,
                                                               std::string name)
    {
        if (name.empty())
            return std::unexpected(std::string("A project needs a name"));

        library::project made = library::new_project(name);
        // The file is named as new_project() named the project's other
        // files: after the project, with what a file name cannot hold
        // replaced.
        const std::filesystem::path stem = library::path_from_utf8(made.library_file).stem();
        std::filesystem::path file = directory / stem;
        file += library::project_extension;

        std::error_code error;
        if (std::filesystem::exists(file, error))
            return std::unexpected(display_name(file) + " exists already; open it instead");
        std::filesystem::create_directories(directory, error);
        if (error)
            return std::unexpected("Cannot create " + library::path_to_utf8(directory) + ": " + error.message());

        if (auto saved = library::save_project(file, made); !saved)
            return std::unexpected(std::move(saved).error());

        m_project = std::make_unique<library::project>(std::move(made));
        m_project_path = std::filesystem::absolute(file, error);
        if (error)
            m_project_path = file;
        m_project_modified = false;

        // Its sheet, new and empty, written at once like the rest.
        open_sheets(*m_project, m_project_path);
        if (auto saved = save_sheets(); !saved)
            show_message(window_kind::project_manager, "Made the project, but " + saved.error(),
                         std::chrono::seconds(10), log_level::warning);
        project_changed.emit();
        return {};
    }

    std::expected<void, std::string> app_services::open_project(const std::filesystem::path &file)
    {
        auto loaded = library::load_project(file);
        if (!loaded)
            return std::unexpected(std::move(loaded).error());

        m_project = std::make_unique<library::project>(std::move(loaded->project));
        std::error_code error;
        m_project_path = std::filesystem::absolute(file, error);
        if (error)
            m_project_path = file;
        m_project_modified = false;

        if (!loaded->report.problems.empty())
        {
            // Problems go to stderr, which automated runs watch; every one
            // to the log, the first to the status bar too.
            for (const auto &problem : loaded->report.problems)
                std::cerr << display_name(file) << ": " << problem.message << std::endl;
            for (std::size_t i = 1; i < loaded->report.problems.size(); ++i)
                m_messages.add(window_kind::project_manager,
                               display_name(file) + ": " + loaded->report.problems[i].message, log_level::warning);
            show_message(window_kind::project_manager,
                         std::format("Opened {} with {} problem(s): {}", display_name(file),
                                     loaded->report.problems.size(), loaded->report.problems.front().message),
                         std::chrono::seconds(10), log_level::warning);
        }
        open_sheets(*m_project, m_project_path);
        project_changed.emit();
        return {};
    }

    std::expected<void, std::string> app_services::save_project()
    {
        if (!m_project)
            return std::unexpected(std::string("No project is open"));

        // The sheets first: a project file is never written that lists a
        // sheet the save could not write.
        if (auto saved = save_sheets(); !saved)
        {
            project_changed.emit(); // the sheets written before it are saved
            return saved;
        }
        if (auto saved = library::save_project(m_project_path, *m_project); !saved)
            return std::unexpected(std::move(saved).error());
        m_project_modified = false;
        project_changed.emit();
        return {};
    }

    void app_services::close_project()
    {
        if (!m_project)
            return;
        m_project.reset();
        m_project_path.clear();
        m_project_modified = false;
        m_sheets_busy = true;
        m_schematic.open_scratch();
        m_sheets_busy = false;
        project_changed.emit();
    }

    void app_services::open_sheets(const library::project &p, const std::filesystem::path &file)
    {
        m_sheets_busy = true;
        const schematic::connectivity rules{.pins_inside_wires = p.schematic.pins_connect_along_wires};
        const std::vector<schematic_document::load_problem> problems =
            m_schematic.open_project_sheets(file.parent_path(), p.sheets, rules);
        m_sheets_busy = false;
        if (problems.empty())
            return;

        // As the project file's problems go: every one to stderr and the
        // log, the first to the status bar too.
        bool failed = false;
        for (const schematic_document::load_problem &problem : problems)
        {
            std::cerr << display_name(file) << ": " << problem.text << std::endl;
            failed = failed || problem.failed;
        }
        for (std::size_t i = 1; i < problems.size(); ++i)
            m_messages.add(window_kind::project_manager, problems[i].text,
                           problems[i].failed ? log_level::error : log_level::warning);
        std::string text = problems.front().text;
        if (problems.size() > 1)
            text += std::format(" (and {} more: see the messages)", problems.size() - 1);
        show_message(window_kind::project_manager, std::move(text), std::chrono::seconds(12),
                     failed ? log_level::error : log_level::warning);
    }

    std::expected<void, std::string> app_services::save_sheets()
    {
        m_sheets_busy = true;
        auto saved = m_schematic.save_project_sheets(m_project_path.parent_path());
        m_sheets_busy = false;
        if (!saved)
            return std::unexpected(std::move(saved).error());
        if (!saved->skipped.empty())
        {
            std::string names;
            for (const std::string &name : saved->skipped)
                names += (names.empty() ? "" : ", ") + name;
            const bool one = saved->skipped.size() == 1;
            show_message(window_kind::project_manager,
                         std::format("Not saved: {} {} did not load, so {} left as {}", one ? "sheet" : "sheets",
                                     names, one ? "its file is" : "their files are", one ? "it is" : "they are"),
                         std::chrono::seconds(10), log_level::warning);
        }
        return {};
    }

    void app_services::mark_project_modified()
    {
        if (!m_project)
            return;
        m_project_modified = true;
        project_changed.emit();
    }

    std::expected<std::string, std::string> app_services::add_to_project(std::string_view qualified_id)
    {
        if (!m_project)
            return std::unexpected(std::string("No project is open"));
        if (!m_catalog)
            return std::unexpected(std::string("The parts library is still loading"));
        const auto ref = m_catalog->find(qualified_id);
        if (!ref)
            return std::unexpected(std::format("{} is not in the parts library", qualified_id));

        const bool there = !project_part_of(qualified_id).empty();
        auto added = library::add_part(*m_project, *m_catalog, *ref);
        if (!added)
            return std::unexpected(std::move(added).error());
        if (!there)
            mark_project_modified();
        return added;
    }

    std::string app_services::project_part_of(std::string_view qualified_id) const
    {
        if (!m_project)
            return {};
        const std::size_t colon = qualified_id.find(':');
        if (colon == std::string_view::npos)
            return {};
        const std::string_view lib = qualified_id.substr(0, colon);
        const std::string_view id = qualified_id.substr(colon + 1);
        for (const library::part &p : m_project->parts.parts)
        {
            if (p.origin && p.origin->library == lib && p.origin->part == id)
                return p.id;
        }
        return {};
    }

    void app_services::confirm_discard(window_kind where, std::string doing, std::function<void()> then)
    {
        // What was to go into the project first: saved into it, it is one of
        // the changes asked about next.
        m_project_guards.confirm([this, where, doing = std::move(doing), then = std::move(then)] {
            ask_discard(where, doing, then);
        });
    }

    void app_services::ask_discard(window_kind where, const std::string &doing, const std::function<void()> &then)
    {
        if (!project_modified())
        {
            if (then)
                then();
            return;
        }

        window_host *host = window(where);
        if (host == nullptr)
            host = window(window_kind::project_manager);
        if (host == nullptr)
        {
            // No window left to ask in: the application is going anyway.
            if (then)
                then();
            return;
        }
        open_window(host->kind()); // raised, so the question is seen
        if (m_confirming)
            return; // asked already, and not answered yet

        m_confirming = true;
        const std::string name = m_project->name.empty() ? display_name(m_project_path) : m_project->name;
        std::vector<dialog_choice> choices;
        choices.push_back({"Save and " + doing, button_kind::primary, [this, then, where] {
                               m_confirming = false;
                               if (auto saved = save_project(); !saved)
                               {
                                   show_message(where, "Not saved: " + saved.error(), std::chrono::seconds(10),
                                                log_level::error);
                                   return;
                               }
                               if (then)
                                   then();
                           }});
        choices.push_back({doing + " Without Saving", button_kind::normal, [this, then] {
                               m_confirming = false;
                               if (then)
                                   then();
                           }});
        choices.push_back({"Cancel", button_kind::normal, [this] { m_confirming = false; }});
        const bool sheets = m_schematic.modified();
        const std::string what = sheets && m_project_modified ? "The project's sheets and parts library have"
                                 : sheets                     ? "The project's sheets have"
                                                              : "The project's parts library has";
        open_choice_dialog(*host, std::format("Save the changes to {}?", name),
                           what + " changed since the project was last saved. The changes are lost unless you "
                                  "save them.",
                           std::move(choices), [this] { m_confirming = false; });
    }

    void app_services::open_project_file(const std::filesystem::path &path, window_kind from)
    {
        confirm_discard(from, "Open", [this, path, from] {
            if (auto opened = open_project(path); !opened)
            {
                std::cerr << "Cannot open " << library::path_to_utf8(path) << ": " << opened.error() << std::endl;
                show_message(from, "Cannot open " + display_name(path) + ": " + opened.error(),
                             std::chrono::seconds(10), log_level::error);
                return;
            }
            const std::size_t parts = m_project->parts.parts.size();
            show_message(from, std::format("Opened project {} ({} part{} in its library)", m_project->name, parts,
                                           parts == 1 ? "" : "s"));
        });
    }

    void app_services::open_library_file(const std::filesystem::path &path, window_kind from)
    {
        // The catalog is built again with the file ahead of every
        // directory; the scan's end says how it went.
        const bool added = add_library_file(path, from);
        show_message(from,
                     std::format("{} {} for this session; scanning the libraries again\xE2\x80\xA6",
                                 added ? "Opening" : "Opening again", display_name(path)),
                     std::chrono::seconds(8));
    }

    void app_services::open_part_package(const std::filesystem::path &path, window_kind from)
    {
        // Into the user's own library, which is made when the first part
        // arrives. The file is small and so is the library: read, changed
        // and saved here, and the catalog scanned again to show it.
        const auto failed = [&](const std::string &why) {
            std::cerr << "Cannot import " << library::path_to_utf8(path) << ": " << why << std::endl;
            show_message(from, "Not imported: " + display_name(path) + ": " + why, std::chrono::seconds(12),
                         log_level::error);
        };
        auto loaded = library::load_package(path);
        if (!loaded)
            return failed(loaded.error());
        const std::filesystem::path target_path = user_parts_library();
        if (target_path.empty())
            return failed("there is no user library folder on this system");

        library::library target = library::new_library("my-parts", "My Parts");
        std::error_code error;
        // Not knowing is not "not there": a new library saved over the
        // user's would lose every part in it.
        const bool there = std::filesystem::exists(target_path, error);
        if (error)
            return failed(std::format("cannot tell whether {} exists ({})", display_name(target_path),
                                      error.message()));
        if (there)
        {
            auto existing = library::load_library(target_path);
            if (!existing)
                return failed(existing.error());
            target = std::move(existing->library);
        }
        const library::import_report report =
            library::import_parts(target, loaded->package, library::schema::builtin());
        if (report.changed())
        {
            std::filesystem::create_directories(target_path.parent_path(), error);
            if (auto saved = library::save_library(target_path, target); !saved)
                return failed(saved.error());
            rescan_catalog();
        }

        // One line in the status bar; what is wrong with each part goes to
        // the log, a line each.
        for (const library::load_report::problem &problem : loaded->report.problems)
            m_messages.add(from, display_name(path) + ": " + problem.message, log_level::warning);
        for (const library::imported_part &part : report.parts)
        {
            if (!part.reason.empty())
                m_messages.add(from, std::format("{}: {}: {}", part.id, library::describe(part.action), part.reason),
                               part.action == library::import_action::rejected ? log_level::error
                                                                               : log_level::warning);
            for (const library::issue &found : part.issues)
                if (found.level != library::severity::note)
                    m_messages.add(from, part.id + ": " + found.message,
                                   found.level == library::severity::error ? log_level::error : log_level::warning);
        }
        const std::string summary = report.summary();
        const bool errors = report.errors + report.rejected > 0;
        const bool warnings = report.warnings + report.skipped + loaded->report.problems.size() > 0;
        show_message(from,
                     std::format("Imported {} into {}: {}{}", display_name(path), target.display_name(),
                                 summary.substr(0, summary.find('\n')),
                                 errors || warnings ? " (see the message log)" : ""),
                     std::chrono::seconds(errors || warnings ? 14 : 8),
                     errors ? log_level::error : warnings ? log_level::warning : log_level::info);
    }

    std::filesystem::path app_services::user_parts_library()
    {
        const std::filesystem::path directory = user_library_directory();
        return directory.empty() ? directory : directory / "my-parts.voltlib";
    }

    bool app_services::add_library_file(const std::filesystem::path &file, window_kind from)
    {
        std::error_code error;
        std::filesystem::path normal = std::filesystem::absolute(file, error).lexically_normal();
        if (error)
            normal = file.lexically_normal();
        const auto there = std::ranges::find(m_library_files, normal);
        const bool added = there == m_library_files.end();
        if (!added)
            m_library_files.erase(there);
        m_library_files.insert(m_library_files.begin(), normal); // the latest wins
        m_library_files_to_report.emplace_back(normal, from);
        rescan_catalog();
        return added;
    }

    void app_services::report_opened_libraries(const library::catalog &catalog,
                                               const library::scan_report &report)
    {
        for (const auto &[path, from] : std::exchange(m_library_files_to_report, {}))
        {
            const auto entry = std::ranges::find_if(report.files, [&](const library::scan_report::file &f) {
                return f.opened && f.path == path;
            });
            if (entry == report.files.end())
            {
                // Opened while this scan ran: the one queued behind it
                // has it, and says.
                if (m_rescan_pending)
                    m_library_files_to_report.emplace_back(path, from);
                continue;
            }
            if (!entry->loaded)
            {
                show_message(from, "Not loaded: " + display_name(path) + ": " + entry->error,
                             std::chrono::seconds(12), log_level::error);
                continue;
            }
            std::string name = display_name(path);
            std::size_t parts = 0;
            for (const library::library &lib : catalog.libraries())
            {
                if (lib.path == path)
                {
                    name = lib.title.empty() ? lib.name : lib.title;
                    parts = lib.parts.size();
                }
            }
            if (entry->shadows.empty())
                show_message(from, std::format("Opened the library {} ({} part{}) for this session", name, parts,
                                               parts == 1 ? "" : "s"),
                             std::chrono::seconds(8));
            else
                show_message(from,
                             std::format("Opened the library {} ({} part{}) for this session: it shadows the library "
                                         "of the same name in {}",
                                         name, parts, parts == 1 ? "" : "s",
                                         library::path_to_utf8(entry->shadows.parent_path())),
                             std::chrono::seconds(12), log_level::warning);
        }
    }

    // ---- the parts library ----------------------------------------------------------------------

    std::filesystem::path app_services::user_library_directory()
    {
#ifdef _WIN32
        wchar_t *appdata = nullptr;
        std::size_t length = 0;
        std::filesystem::path result;
        if (_wdupenv_s(&appdata, &length, L"APPDATA") == 0 && appdata != nullptr && appdata[0] != L'\0')
            result = std::filesystem::path(appdata) / "Volt" / "libraries";
        std::free(appdata);
        return result;
#else
        if (const char *data = std::getenv("XDG_DATA_HOME"); data != nullptr && data[0] != '\0')
            return std::filesystem::path(data) / "volt" / "libraries";
        if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
            return std::filesystem::path(home) / ".local" / "share" / "volt" / "libraries";
        return {};
#endif
    }

    bool app_services::add_library_directory(const std::filesystem::path &directory)
    {
        const std::filesystem::path normal = directory.lexically_normal();
        const bool known = std::ranges::any_of(m_library_directories, [&](const std::filesystem::path &d) {
            return d.lexically_normal() == normal;
        });
        if (!known)
            m_library_directories.push_back(normal);
        rescan_catalog();
        return !known;
    }

    void app_services::rescan_catalog()
    {
        if (m_scanning)
        {
            m_rescan_pending = true;
            return;
        }
        m_scanning = true;
        m_rescan_pending = false;
        m_scan_done = 0;
        m_scan_total = 0;
        catalog_status_changed.emit();

        // Each scan its own flag: stop_loading() stops the running one.
        m_scan_cancel = std::make_shared<std::atomic<bool>>(false);
        start_load([this, directories = m_library_directories, files = m_library_files, cancel = m_scan_cancel] {
            auto report = std::make_shared<library::scan_report>();
            library::scan_options options;
            options.files = files;
            options.progress = [this, cancel](std::size_t done, std::size_t total, const std::filesystem::path &) {
                if (cancel->load())
                    return false;
                post([this, done, total] {
                    if (!m_scanning)
                        return;
                    m_scan_done = done;
                    m_scan_total = total;
                    catalog_status_changed.emit();
                });
                return true;
            };
            auto built =
                std::make_shared<const library::catalog>(library::load_catalog(directories, *report, options));
            if (cancel->load())
                return; // exiting: nothing is waiting for it
            post([this, built = std::move(built), report = std::move(report)]() mutable {
                finish_load();
                finish_scan(std::move(built), std::move(report));
            });
        });
    }

    void app_services::finish_scan(std::shared_ptr<const library::catalog> built,
                                   std::shared_ptr<const library::scan_report> report)
    {
        m_scanning = false;
        m_scan_report = report;
        if (!report->clean())
            std::cerr << "Parts library: " << report->summary() << std::endl;

        set_catalog(std::move(built));
        catalog_status_changed.emit();

        // A shadowed library is the user's choice, not a problem.
        std::size_t problems = 0;
        for (const library::scan_report::file &f : report->files)
            problems += (f.loaded || f.shadowed ? 0 : 1) + f.report.problems.size();
        std::string text = std::format("Parts library: {} parts in {} librar{} ({:.2f} s)", report->parts,
                                       report->libraries, report->libraries == 1 ? "y" : "ies", report->seconds);
        if (problems > 0)
            text += std::format(", {} problem{}: see the Library Browser", problems, problems == 1 ? "" : "s");
        show_message(window_kind::project_manager, std::move(text), std::chrono::seconds(problems > 0 ? 12 : 6),
                     problems > 0 ? log_level::warning : log_level::info);

        // What came of the files opened, last, so that it is what their
        // windows show.
        if (m_catalog)
            report_opened_libraries(*m_catalog, *report);

        if (m_rescan_pending)
            rescan_catalog();
    }

    // ---- the library windows ------------------------------------------------------------------------

    void app_services::reveal_in_library(std::string id, bool project)
    {
        m_reveal = library_reveal{std::move(id), project};
        open_window(window_kind::library_browser);
        library_reveal_requested.emit();
    }

    std::optional<app_services::library_reveal> app_services::take_library_reveal()
    {
        std::optional<library_reveal> taken = std::move(m_reveal);
        m_reveal.reset();
        return taken;
    }

    ui::clipboard &app_services::system_clipboard() noexcept
    {
        if (!m_clipboard)
            m_clipboard = make_system_clipboard();
        return *m_clipboard;
    }

} // namespace volt::ui
