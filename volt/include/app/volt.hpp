#pragma once

#include <app/board/model.hpp>
#include <app/board/renderer.hpp>
#include <app/demo.hpp>
#include <app/schematic/renderer.hpp>
#include <app/simulation/compute.hpp>
#include <app/ui/images.hpp>
#include <app/ui/renderer.hpp>
#include <app/ui/services.hpp>
#include <app/ui/shared_text.hpp>
#include <app/ui/window_manager.hpp>
#include <app/ui/windows/board_editor.hpp>
#include <app/ui/windows/library_browser.hpp>
#include <app/ui/windows/project_manager.hpp>
#include <app/ui/windows/schematic_editor.hpp>
#include <gpu/context.hpp>
#include <gpu/presenter.hpp>
#include <model/io.hpp>
#include <platform/platform.hpp>
#include <ui/widgets/viewport.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// -------------------------------------------------------------------------
// volt_app
// -------------------------------------------------------------------------
//
// The application: the platform, the GPU and its renderers, the shared
// services (<app/ui/services.hpp>) and the windows (<app/ui/
// window_manager.hpp>), with the two threads that run them.
//
//   platform thread   the one that calls initialize() and run(): creates
//                     and destroys windows, pumps their events, runs what
//                     the window manager asks of it
//   render thread     started by run(); the interface thread too: input,
//                     screens, timers, and the frames
//
// The render loop, each time the thread wakes:
//
//   1. the window manager's messages (windows made or closed by the user),
//      every window's input, posted work, every screen's timers, then the
//      windows closed by any of that are taken down;
//   2. the windows that need a frame: their screen wants one, the platform
//      asked (resize, restore, display), or they show the schematic while
//      the simulation runs. A window that found nothing to draw into last
//      time (minimized) waits until the platform says it changed;
//   3. if any: one frame for all of them. Acquire each one's image (those
//      that get none are left out; one whose display held its images past
//      the presenter's timeout is tried again soon, without waiting), lay
//      each out, render the offscreen
//      views that a window being drawn shows, paint every list (again,
//      if a later window reset the shared glyph atlas), upload the glyphs
//      once, draw each window's list into its image, present all at once;
//   4. sleep until woken, or until the earliest timer of any screen.
//
// Windows that need no frame are not acquired, and keep showing what they
// showed.

namespace volt
{

    class volt_app
    {
    public:
        volt_app() = default;
        ~volt_app()
        {
            // In dependency order: files still being read post to the
            // interface, so they finish first; the interface (screens, which
            // listen to the windows' input) before the windows; everything
            // that draws lives on the device, the swapchains also in their
            // windows, windows on the platform's connection.
            m_services.stop_loading();
            m_windows.teardown();
            m_windows.shutdown();
            m_images.destroy();
            m_ui.destroy();
            m_board.destroy();
            m_schematic.destroy();
            m_simulation.destroy();
            m_presenter.destroy();
            m_gpu.destroy();
            platform::shutdown(m_platform);
        }

        // Owns the platform state, which must be shut down exactly once.
        volt_app(const volt_app &) = delete;
        volt_app &operator=(const volt_app &) = delete;

        // Lets a window leave sRGB on a monitor that is in HDR mode: an
        // scRGB or HDR10 swapchain, with colors brighter than SDR white. This
        // is what the setting starts as (--hdr); the user changes it in
        // Preferences > Display (ui::app_settings::output). Call before
        // initialize().
        //
        // Off unless asked for, because a driver offering an HDR color space
        // is no promise that it delivers one. Measured on an Intel Arc 140V,
        // Windows 11, by reading the composed desktop back: with driver
        // 101.5737 the HDR10 swapchains it offers for a window are composed
        // as if they held sRGB, so everything comes out gray and dim. Driver
        // 101.9175 gets both scRGB and HDR10 right. The sRGB swapchain is
        // right on every display, HDR ones included; the system converts it.
        void set_extended_color(bool enabled)
        {
            m_extended_color = enabled;
        }

        // The user's interface scale to start with: a multiplier on each
        // window's display scale, 1 for the size every other application
        // has on that monitor (<app/ui/scale.hpp>). The user can change it
        // in Preferences. Call before initialize().
        void set_ui_scale(float scale)
        {
            m_ui_scale = scale > 0.0f ? scale : 1.0f;
        }

        // Where the runtime assets are: the build stages them in an assets
        // directory next to the executable. Call before initialize().
        void set_asset_root(std::filesystem::path root)
        {
            m_assets = std::move(root);
        }

        // Files to open once the project manager is up (--open), UTF-8. They
        // go through its input as a drop of those files would, so they take
        // exactly the path a drop takes. Call before initialize().
        void add_open_path(std::string path)
        {
            m_open.push_back(std::move(path));
        }

        // A directory of parts libraries (--library-dir), scanned before the
        // user's own (app_services::user_library_directory()) and the
        // shipped ones (assets/libraries); a library found in an earlier one
        // wins over one of the same name in a later one. May be given more
        // than once. Call before initialize().
        void add_library_directory(std::filesystem::path directory)
        {
            m_library_directories.push_back(std::move(directory));
        }

        // Windows to open at start besides the project manager (--windows),
        // through the same path as opening them from the interface. Call
        // before initialize().
        void add_start_window(ui::window_kind kind)
        {
            m_start_windows.push_back(kind);
        }

        // For automated smoke runs (--smoke-frames): once this many frames
        // have been presented, the application closes as if asked to, and
        // run() returns without an error. Until then every frame asks for
        // the next, of every open window, so the count is reached even with
        // nothing to redraw, and every window is drawn all the while. 0, the
        // default, runs until closed. Call before initialize().
        void set_smoke_frames(std::uint64_t frames)
        {
            m_smoke_frames = frames;
        }

        std::expected<void, std::string> initialize()
        {
            auto platform_state = platform::initialize();
            if (!platform_state)
                return std::unexpected(std::move(platform_state).error());

            m_platform = *platform_state;

            m_services.set_windows(&m_windows);
            m_windows.create({.platform = m_platform,
                              .gpu = &m_gpu,
                              .services = &m_services,
                              .wake = [this] { wake(); },
                              .exit = [this] { request_close(); }});

            // What the windows share, and one content per kind of window,
            // kept until exit; before the first window, whose size its
            // content says. None of it needs the device yet: images are made
            // when windows are built (on this thread until run() starts the
            // render thread, on that one after).
            ui::services_setup setup;
            setup.assets = m_assets;
            setup.board_renderer = &m_board;
            setup.add_image = [this](const ui::texture_pixels &pixels) { return m_images.add(pixels); };
            setup.remove_image = [this](ui::texture_id id) { m_images.release(id); };
            setup.run_simulation = [this](const simulation::network &network) { run_simulation(network); };
            setup.wake = [this] { wake(); };
            setup.user_scale = m_ui_scale;
            setup.output = m_extended_color ? ui::color_output::hdr : ui::color_output::sdr;
            setup.library_directories = m_library_directories;
            if (const std::filesystem::path user = ui::app_services::user_library_directory(); !user.empty())
                setup.library_directories.push_back(user);
            setup.library_directories.push_back(m_assets / "libraries");
            if (auto created = m_services.create(std::move(setup)); !created)
                return created;

            // The board is a stand-in, see <app/demo.hpp>; its parts follow
            // once there is a device to put their meshes on. The schematic
            // starts on its scratch sheet (<app/ui/schematic_document.hpp>).
            m_services.board().scene = demo::board();

            m_windows.set_contents({std::make_unique<ui::project_manager>(m_services),
                                    std::make_unique<ui::schematic_editor>(m_services),
                                    std::make_unique<ui::board_editor>(m_services),
                                    std::make_unique<ui::library_browser>(m_services)});

            // The project manager's window first: the device is chosen for
            // presenting to its surface.
            auto first = m_windows.create_first(ui::window_kind::project_manager);
            if (!first)
                return std::unexpected(std::move(first).error());
            if (auto created = m_gpu.create({}, **first); !created)
                return created;

            if (auto created = m_presenter.create(m_gpu); !created)
                return created;
            if (auto created = m_simulation.create(m_gpu); !created)
                return created;
            if (auto created = m_schematic.create(m_gpu); !created)
                return created;
            if (auto created = m_board.create(m_gpu); !created)
                return created;
            if (auto created = m_ui.create(m_gpu); !created)
                return created;

            // The interface's images; the views' targets are two of them.
            m_images.create(m_gpu, m_ui);
            m_schematic_image = m_images.reserve();
            m_board_image = m_images.reserve();
            m_services.set_view_images(m_schematic_image, m_board_image);

            // The scratch sheet simulated as the Run command would, so that
            // its wires show their potentials from the first frame.
            place_demo_parts();
            (void)m_services.run_simulation();

            // The project manager's swapchain, input, and its hand-over to
            // the interface, which builds its screen at its first wake-up.
            if (auto finished = m_windows.finish_first(); !finished)
                return finished;

            // --open: as if the files had been dropped on the project
            // manager.
            if (!m_open.empty())
                m_windows.inject_drop(ui::window_kind::project_manager, m_open);

            // --windows: asked for as the interface asks (until run() starts
            // the render thread, this thread is the interface thread).
            for (ui::window_kind kind : m_start_windows)
                m_windows.open_window(kind);
            return {};
        }

        // Runs until the application closes. Reports what stopped the
        // renderer, if that is why.
        std::expected<void, std::string> run()
        {
            // Events and frames each get a thread, so that neither waits for
            // the other: poll_events() sleeps until the OS has something, and
            // Windows keeps the platform thread inside its own loop for as
            // long as a window is being dragged or resized.
            std::thread render_thread([this] { render(); });

            // The render thread wakes this loop (platform::wake()) when it
            // has asked for a window to be made, raised or destroyed.
            while (!should_close() && platform::poll_events(m_platform))
                m_windows.run_platform_tasks();

            request_close(); // tell the other threads the loop has ended
            render_thread.join();

            // The interface is gone; the windows go now, on this thread.
            m_windows.shutdown();

            if (!m_render_error.empty())
                return std::unexpected(std::move(m_render_error));
            return {};
        }

        // Asks the application to exit. Callable from any thread.
        void request_close()
        {
            m_should_close.store(true);
            wake();                     // the render thread may be asleep
            platform::wake(m_platform); // and the platform thread too
        }

        // Callable from any thread.
        bool should_close() const
        {
            return m_should_close.load();
        }

    private:
        static VkExtent2D extent_of(const math::vec2i &size)
        {
            return {static_cast<std::uint32_t>(size.x() > 0 ? size.x() : 0),
                    static_cast<std::uint32_t>(size.y() > 0 ? size.y() : 0)};
        }

        gpu::display display_of(const ui::display_info &info) const
        {
            // The user's choice (Preferences > Display), read for every
            // frame: the swapchain follows it as it follows the display.
            const bool wanted = m_services.color_output() == ui::color_output::hdr;
            return {.hdr = wanted && info.hdr, .white = info.sdr_white};
        }

        static std::string_view encoding_name(gpu::color_encoding encoding)
        {
            switch (encoding)
            {
            case gpu::color_encoding::scrgb:
                return "scRGB";
            case gpu::color_encoding::hdr10:
                return "HDR10";
            case gpu::color_encoding::srgb:
                break;
            }
            return "sRGB";
        }

        // Rouses the render thread: input arrived, a screen wants a frame, a
        // window changed, work was posted. Callable from any thread.
        void wake()
        {
            {
                std::lock_guard lock(m_wake_mutex);
                m_wake = true;
            }
            m_wake_cv.notify_one();
        }

        // The demo board's parts that have models (<app/demo.hpp>), loaded
        // here, before the render thread starts: they are small. A part that
        // does not load is reported and left out.
        void place_demo_parts()
        {
            board::scene &scene = m_services.board().scene;
            for (const demo::board_part &part : demo::board_parts())
            {
                const std::filesystem::path path = m_assets / "models" / part.file;
                auto loaded = model::load(path);
                if (!loaded)
                {
                    std::cerr << "Demo board: " << loaded.error() << std::endl;
                    continue;
                }
                if (!loaded->report.clean())
                    std::cerr << "Demo board, " << part.file << ": " << loaded->report.summary() << std::endl;

                auto meshes = board::upload(m_board, board::prepare(loaded->model));
                if (!meshes)
                {
                    std::cerr << "Demo board, " << part.file << ": " << meshes.error() << std::endl;
                    continue;
                }
                for (const math::mat4f &placement : part.placements)
                    board::add_model(scene, *meshes, placement);
                m_demo_models.push_back(std::move(*meshes));
            }
        }

        // Render thread, between frames (from the interface's handlers).
        // A network of no nodes only clears the last one: there is nothing
        // to run.
        void run_simulation(const simulation::network &network)
        {
            if (auto loaded = m_simulation.load(network); !loaded)
            {
                std::cerr << "Simulation: " << loaded.error() << std::endl;
                return;
            }
            if (network.nodes != 0)
                m_simulation.run(8);
        }

        // Body of the render thread: the loop, then the interface's end,
        // which belongs on this thread whatever ended the loop.
        void render()
        {
            render_loop();
            m_services.stop_loading();
            m_services.clear_posted();
            m_windows.teardown();
        }

        void render_loop()
        {
            while (true)
            {
                {
                    std::unique_lock lock(m_wake_mutex);
                    const auto ready = [this] { return m_wake || should_close(); };
                    std::optional<ui::time_point> deadline = m_windows.next_wakeup();
                    if (m_retry_busy && (!deadline || *m_retry_busy < *deadline))
                        deadline = m_retry_busy;
                    if (deadline)
                        m_wake_cv.wait_until(lock, *deadline, ready);
                    else
                        m_wake_cv.wait(lock, ready);
                    m_wake = false;
                    m_retry_busy.reset();
                }
                if (should_close())
                    return;

                // 1. Windows made and closed, input, posted work, timers;
                // then what all that closed goes, between frames.
                m_windows.process();
                m_windows.drain();
                m_services.run_posted();
                m_windows.update(ui::clock::now());
                m_windows.collect();
                if (should_close())
                    return;

                // 2. Who needs a frame.
                const bool simulating = m_simulation.is_busy();
                std::vector<ui::app_window *> wanted;
                for (const std::shared_ptr<ui::app_window> &w : m_windows.windows())
                {
                    const bool asked = w->redraw.exchange(false);
                    if (asked)
                        w->skipped = false;
                    const bool wants = w->screen->needs_redraw() ||
                                       (simulating && w->content->view(ui::offscreen_view::schematic) != nullptr);
                    if (asked || w->busy || (!w->skipped && wants))
                        wanted.push_back(w.get());
                }
                if (wanted.empty())
                    continue;

                // 3. One frame for all of them.
                auto frame = m_presenter.begin_frame();
                if (!frame)
                    return fail_render(std::move(frame).error());

                // Ended even if drawing failed: the presenter has begun a
                // frame and wants it submitted.
                auto drawn = draw(*frame, wanted);

                auto presented = m_presenter.end_frame();
                if (!presented)
                    return fail_render(std::move(presented).error());
                if (!drawn)
                    return fail_render(std::move(drawn).error());

                // --smoke-frames: count the frames that reached a window,
                // and keep every window's coming, since nothing else may ask.
                if (m_smoke_frames != 0 && *presented != 0)
                {
                    if (++m_smoke_presented >= m_smoke_frames)
                    {
                        request_close();
                        return;
                    }
                    for (const std::shared_ptr<ui::app_window> &w : m_windows.windows())
                        w->redraw.store(true);
                    wake();
                }

                // The simulation moves along with the frames of the windows
                // that show it.
                if (m_simulation.is_busy())
                    wake();

                // A window whose display held its images is tried again in
                // a while, if nothing else draws a frame before.
                for (const std::shared_ptr<ui::app_window> &w : m_windows.windows())
                {
                    if (w->busy)
                        m_retry_busy = ui::clock::now() + std::chrono::milliseconds(100);
                }
            }
        }

        // A window being drawn this frame, and the image it draws into.
        struct drawn_window
        {
            ui::app_window *window = nullptr;
            gpu::window_image image;
        };

        // Records one frame into the windows of `wanted` that have an image
        // to draw into. Render thread. False when none had one (all
        // minimized): the presenter then submits the frame empty, and the
        // simulation waits for a frame that draws.
        std::expected<bool, std::string> draw(const gpu::frame &frame, const std::vector<ui::app_window *> &wanted)
        {
            using failed = std::unexpected<std::string>;

            // What the frames before the last ones left behind can go.
            m_images.collect(frame);
            m_board.collect(frame);

            // The windows this frame draws into. Any may be left out:
            // minimized, still changing size, or its display holding its
            // images (busy: then it is not waited for twice in a row, so
            // that one stuck display does not slow every window down).
            std::vector<drawn_window> shown;
            for (ui::app_window *w : wanted)
            {
                const std::uint64_t timeout = w->busy ? 0 : gpu::presenter::acquire_timeout_ns;
                auto acquired = m_presenter.acquire(w->swapchain, extent_of(w->size.load()),
                                                    display_of(w->display.load()), timeout);
                if (!acquired)
                    return failed(std::move(acquired).error());
                w->busy = acquired->busy;
                w->skipped = !acquired->image && !acquired->busy;
                if (acquired->image)
                    shown.push_back({w, *acquired->image});
            }
            if (shown.empty())
                return false;

            if (auto advanced = m_simulation.advance(); !advanced)
                return failed(std::move(advanced).error());

            const auto potentials = m_simulation.latest();
            ui::schematic_document &schematic = m_services.schematic();
            schematic.simulation_busy = m_simulation.is_busy();
            schematic.simulation_results = potentials.has_value();
            const bool loading = m_services.loading() > 0;

            // Each window at its image's size; status texts, then layout
            // (the viewports' sizes are the targets' sizes).
            for (drawn_window &d : shown)
            {
                ui::app_window &w = *d.window;
                const math::vec2i size{static_cast<int>(d.image.extent.width), static_cast<int>(d.image.extent.height)};
                w.screen->set_size(size);
                w.host->set_frame_info(size, encoding_name(d.image.output.encoding));
                w.host->set_display_hdr(w.display.load().hdr);
                const bool shows_simulation = w.content->view(ui::offscreen_view::schematic) != nullptr;
                w.host->status().set_busy(loading || (schematic.simulation_busy && shows_simulation));
                w.content->before_frame();
                w.screen->layout();
            }

            // The offscreen views, each where a window drawn this frame
            // shows it; not at all when none does.
            if (ui::viewport *port = find_view(shown, ui::offscreen_view::schematic))
            {
                // The potentials only on the sheet, and the revision of it,
                // that they were worked out for.
                const bool show = schematic.show_potentials && schematic.simulation_current();
                if (auto drawn = draw_schematic(frame, *port, show ? potentials : std::nullopt); !drawn)
                    return failed(std::move(drawn).error());
            }
            if (ui::viewport *port = find_view(shown, ui::offscreen_view::board))
            {
                if (auto drawn = draw_board(frame, *port); !drawn)
                    return failed(std::move(drawn).error());
            }

            // Every list, consistent with the one atlas (<app/ui/
            // shared_text.hpp>), then the atlas uploaded once.
            const bool consistent = ui::paint_consistently(m_services.text(), shown.size(), [&](std::size_t i) {
                shown[i].window->screen->paint(shown[i].window->list);
            });
            if (!consistent && !m_reported_atlas)
            {
                std::cerr << "The glyph atlas cannot hold one frame of every window; some text shows wrong"
                          << std::endl;
                m_reported_atlas = true;
            }
            if (auto uploaded = m_ui.upload_glyphs(frame, m_services.text_atlas()); !uploaded)
                return failed(std::move(uploaded).error());

            // One pass per window.
            for (drawn_window &d : shown)
            {
                const math::vec4f &theme = d.window->screen->theme().background;
                const math::vec3f background{theme[0], theme[1], theme[2]};
                if (auto drawn = m_ui.draw(frame, d.image, d.window->list, background); !drawn)
                    return failed(std::move(drawn).error());
            }
            return true;
        }

        // The viewport showing `view` in one of the windows being drawn.
        static ui::viewport *find_view(const std::vector<drawn_window> &shown, ui::offscreen_view view)
        {
            for (const drawn_window &d : shown)
            {
                if (ui::viewport *port = d.window->content->view(view); port != nullptr)
                    return port;
            }
            return nullptr;
        }

        // The schematic is drawn again when its scene or camera changed, its
        // viewport changed size, or the simulation has new values for it (or
        // has none to show any more); otherwise its target still holds the
        // last drawing.
        std::expected<void, std::string> draw_schematic(const gpu::frame &frame, const ui::viewport &port,
                                                        const std::optional<simulation::compute::values> &potentials)
        {
            ui::schematic_document &doc = m_services.schematic();
            const math::vec2i pixels = port.pixel_size();
            if (pixels.x() < 1 || pixels.y() < 1)
                return {};
            // The segments are built from the active sheet when it, or how
            // it is drawn, changed since they last were.
            const bool rebuilt = doc.update_scene();
            const bool dirty = doc.take_dirty() || rebuilt;

            const VkExtent2D extent{static_cast<std::uint32_t>(pixels.x()), static_cast<std::uint32_t>(pixels.y())};
            const gpu::target &target = m_schematic.target();
            const bool new_values = potentials.has_value() != m_had_values ||
                                    (potentials && (potentials->buffer != m_values_buffer ||
                                                    potentials->first != m_values_first));
            const bool resized = target.extent().width != extent.width || target.extent().height != extent.height;

            if (dirty || new_values || resized || !target.is_drawn())
            {
                // Wires take the color of their potential once the
                // simulation has one to show, on the ramp of the potentials
                // its circuit holds.
                schematic::values values;
                if (potentials)
                    values = {potentials->buffer, potentials->first, doc.simulation_low(), doc.simulation_high()};
                if (auto drawn = m_schematic.draw(frame, extent, doc.scene, potentials ? &values : nullptr); !drawn)
                    return drawn;

                m_had_values = potentials.has_value();
                m_values_buffer = potentials ? potentials->buffer : VK_NULL_HANDLE;
                m_values_first = potentials ? potentials->first : 0;
            }
            m_ui.set_image(m_schematic_image.index, target.is_drawn() ? target.view() : VK_NULL_HANDLE);
            return {};
        }

        std::expected<void, std::string> draw_board(const gpu::frame &frame, const ui::viewport &port)
        {
            ui::board_document &doc = m_services.board();
            const math::vec2i pixels = port.pixel_size();
            if (pixels.x() < 1 || pixels.y() < 1)
                return {};
            const bool dirty = doc.take_dirty();

            const VkExtent2D extent{static_cast<std::uint32_t>(pixels.x()), static_cast<std::uint32_t>(pixels.y())};
            const gpu::target &target = m_board.target();
            const bool resized = target.extent().width != extent.width || target.extent().height != extent.height;

            if (dirty || resized || !target.is_drawn())
            {
                if (auto drawn = m_board.draw(frame, extent, doc.scene); !drawn)
                    return drawn;
            }
            m_ui.set_image(m_board_image.index, target.is_drawn() ? target.view() : VK_NULL_HANDLE);
            return {};
        }

        void fail_render(std::string error)
        {
            m_render_error = std::move(error);
            request_close();
        }

        // Written once by initialize() and read-only afterwards, so threads can
        // read it without synchronization.
        platform::state m_platform;

        // The GPU device. Shared by all threads once initialize() is done.
        gpu::context m_gpu;

        // Created and destroyed on the platform thread; in between, while
        // run() is going, they belong to the render thread.
        gpu::presenter m_presenter;
        simulation::compute m_simulation;
        schematic::renderer m_schematic;
        board::renderer m_board;
        ui::renderer m_ui;

        // The interface's images, and the ids of the views' targets among
        // them.
        ui::image_textures m_images;
        ui::texture_id m_schematic_image;
        ui::texture_id m_board_image;

        // The demo board's parts. Their meshes are the board renderer's.
        std::vector<board::model_meshes> m_demo_models;

        // What every window shares, then the windows, which use it. The
        // destructor takes them down explicitly, windows first.
        ui::app_services m_services;
        ui::window_manager m_windows;

        // What the schematic's target was last drawn with. Render thread.
        bool m_had_values = false;
        VkBuffer m_values_buffer = VK_NULL_HANDLE;
        std::uint32_t m_values_first = 0;
        bool m_reported_atlas = false;

        // See the setters above. Fixed once initialize() has run.
        bool m_extended_color = false;
        float m_ui_scale = 1.0f;
        std::filesystem::path m_assets = "assets";
        std::vector<std::string> m_open;
        std::vector<std::filesystem::path> m_library_directories;
        std::vector<ui::window_kind> m_start_windows;
        std::uint64_t m_smoke_frames = 0;

        // Frames presented so far, for --smoke-frames. Render thread.
        std::uint64_t m_smoke_presented = 0;

        // When to try a busy window again (app_window::busy). Render thread.
        std::optional<ui::time_point> m_retry_busy;

        // Wakes the render thread: set under the mutex, waited for on the
        // condition variable.
        std::mutex m_wake_mutex;
        std::condition_variable m_wake_cv;
        bool m_wake = true;

        // Set by the render thread before it exits, read by run() after the
        // join.
        std::string m_render_error;

        // The one piece of state several threads write, hence the atomic.
        std::atomic<bool> m_should_close{false};
    };

} // namespace volt
