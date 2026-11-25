#pragma once

#include <app/ui/input/window_input.hpp>
#include <app/ui/services.hpp>
#include <app/ui/window.hpp>
#include <app/ui/window_book.hpp>
#include <app/ui/windows/content.hpp>
#include <events/token.hpp>
#include <gpu/swapchain.hpp>
#include <platform/platform.hpp>
#include <ui/draw_list.hpp>
#include <ui/screen.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace volt::gpu
{
    class context;
} // namespace volt::gpu

// -------------------------------------------------------------------------
// window manager
// -------------------------------------------------------------------------
//
// The application's top-level windows: making them, handing each between
// the two threads that need it, and taking them down again, in an order
// that never leaves one thread holding something the other has destroyed.
//
// Two threads (see volt_app):
//
//   * the platform thread creates and destroys windows, and pumps
//     platform::poll_events(), which runs the windows' callbacks;
//   * the render thread, which is also the interface thread: it owns every
//     screen and its widgets, the window contents, app_services, and the
//     frames; it drains each window's queued input (window_input).
//
// The interface thread decides (opening, raising, closing, exiting); the
// platform thread does what only it may (window calls). They talk through
// two mailboxes, each a mutex-guarded queue that its owner empties when
// woken:
//
//   to the platform thread   create, focus, retire   + platform::wake()
//   to the interface thread  created, failed,         + the render thread's
//                            close_requested            wake
//
// Each window is an app_window, shared between them by shared_ptr: the
// platform thread's list holds it from creation until retired, the
// interface thread's book from the request until it closes, the messages
// in between. window_book (<app/ui/window_book.hpp>) holds the rules for
// what a request means; the serial it gives each window lets either side
// recognize a message about a window that has gone since.
//
// Opening
//   1. Interface: open_window(kind). The book says create: a new
//      app_window goes to the platform thread (create). Asked again while
//      it is on its way, nothing happens; asked while it is open, the
//      platform thread is asked to focus() it (raise, restore, show).
//   2. Platform: create the window (where and as big as that kind was when
//      last closed, else the content's default size at the user's scale,
//      in the middle of the project manager's monitor, <app/ui/
//      window_geometry.hpp>; either way inside the monitor's work area,
//      and no smaller than the content's minimum), its
//      swapchain (surface only, <gpu/swapchain.hpp> "Adding a window", step
//      1), turn on its input's queue and listen on its immediate bus (size,
//      display, close button), attach the input, show it. Then hand it over
//      (created). On failure everything made is destroyed and the
//      interface hears why (failed).
//   3. Interface, between frames: if the book still wants it, build its
//      screen (at the window's content scale times the user's), its host
//      and its content's widgets, and connect the screen to the queued
//      input. From the next frame on the render loop draws into it. If the
//      book no longer wants it (closed meanwhile, or exiting), it goes
//      straight to retire: its swapchain was never acquired.
//
// Closing (the close button, File > Close Window, or the interface)
//   1. The close button is heard on the platform thread (immediate bus),
//      which only passes it on (close_requested, with the serial): the
//      interface decides. A second request for the same window finds it
//      closed in the book and is dropped; so is one about an older window
//      of the same kind.
//   2. Interface: close_window() asks the window's close guards (work in
//      it that closing would lose, window_host::guards()), which may answer
//      later or keep it open; once they let it go, it marks it in the book
//      and sets it aside; nothing is destroyed yet, since the request may come from inside one
//      of the window's own handlers. collect(), between frames, takes it
//      down: popups closed (their `closed` callbacks run, giving textures
//      back), the content detached (it forgets its widgets), the screen's
//      listeners removed from the queued bus, the host and the screen
//      destroyed. So the screen and its listeners die on the interface
//      thread, before the input and the window they listen to. Then the
//      swapchain is released (gpu "Removing a window", step 2: outside any
//      frame) and the window goes to the platform thread (retire).
//   3. Platform: remember where the window was, remove the immediate
//      listeners, detach the input, destroy the swapchain (the surface),
//      then the window; drop it from the list. The last shared_ptr goes
//      with it, wherever that is: by then every part is already taken down.
//
// Work posted to a screen that has gone: a screen's timers and posted
// functions die with it. Work that may outlive a window (a file being
// read) is posted to app_services::post(), which runs whatever windows are
// open, and looks the window up again (app_services::window()) when it
// gets there.
//
// Closing the project manager, by any of the above, exits instead.
//
// Exiting (File > Exit, the project manager closed, --smoke-frames, a
// render error)
//   1. Any thread: volt_app::request_close() sets the flag and wakes both
//      threads. The book stops opening windows.
//   2. Interface: the render loop leaves between frames and teardown()
//      takes down every window's interface as above, opening ones included,
//      releases the swapchains, and destroys the contents. Then the thread
//      ends.
//   3. Platform: the event loop ends, joins the render thread, and
//      shutdown() destroys every window it has, whatever stage it reached:
//      create tasks not yet run are dropped, windows made but never
//      reported are destroyed with the rest. No frames run any more, so the
//      swapchains need no hand-over (<gpu/swapchain.hpp>).
//
// The first window is made before the render thread exists, by the thread
// that will become the platform thread (create_first(), finish_first()):
// the GPU device is chosen for the first window's surface, so the window
// comes first, the device second, the swapchain third.

namespace volt::ui
{

    // ---------------------------------------------------------------------
    // app_window
    // ---------------------------------------------------------------------
    //
    // One top-level window and everything that goes with it. Its members
    // belong to the threads their groups say; the hand-overs are the
    // manager's mailboxes, which synchronize.
    struct app_window
    {
        app_window(window_kind kind, std::uint64_t serial) : kind(kind), serial(serial) {}

        app_window(const app_window &) = delete;
        app_window &operator=(const app_window &) = delete;

        const window_kind kind;
        const std::uint64_t serial;

        // ---- platform thread --------------------------------------------------------

        ui::window window;

        // After the window, so that it goes first and takes its callbacks
        // out of the window; the listeners after it, so that they go before
        // the bus they are on.
        ui::window_input input;
        std::vector<events::scoped_token> platform_tokens;

        // ---- handed over (<gpu/swapchain.hpp>) ------------------------------------
        //
        // Made on the platform thread, the render thread's from `created`
        // until it releases it before `retire`, then the platform thread's
        // again to destroy.
        gpu::swapchain swapchain;

        // ---- written on the platform thread, read on the render thread ------------

        // The content area in pixels, as last reported: what the swapchain
        // is sized by.
        std::atomic<math::vec2i> size{};

        // What the window's monitor does with color: what the swapchain's
        // format and encoding follow.
        std::atomic<ui::display_info> display{};

        // A frame was asked for from outside the interface: the window was
        // resized, restored, or its display changed. The first one is on
        // the house.
        std::atomic<bool> redraw{true};

        // The content scale when the window was made. Written before the
        // hand-over; later changes come through the queued input.
        float initial_scale = 1.0f;

        // ---- render (interface) thread ---------------------------------------------

        std::unique_ptr<ui::screen> screen;
        std::unique_ptr<window_host> host;
        window_content *content = nullptr;

        // The screen's listeners on input.queued(). Before the input in
        // destruction order (declared after it).
        std::vector<events::scoped_token> ui_tokens;

        ui::draw_list list;

        // The last frame found nothing to draw into (minimized): skip it
        // until something says it changed (redraw).
        bool skipped = false;

        // The last frame's acquire timed out: the window's display holds
        // its images (gpu::acquisition::busy). It is tried again, without
        // waiting, in every frame and at least every so often, until it
        // gives one back.
        bool busy = false;
    };

    class window_manager final : public window_control
    {
    public:
        struct setup
        {
            platform::state platform;
            gpu::context *gpu = nullptr;
            app_services *services = nullptr;

            // Rouses the render thread. Any thread; cheap.
            std::function<void()> wake;

            // Ends the application (volt_app::request_close()). Any thread.
            std::function<void()> exit;
        };

        window_manager() = default;
        ~window_manager();

        window_manager(const window_manager &) = delete;
        window_manager &operator=(const window_manager &) = delete;

        // Before anything else. The context need not be created yet.
        void create(setup setup);

        // The content of each kind of window, owned from here on. Before
        // the first window opens.
        void set_contents(std::array<std::unique_ptr<window_content>, window_kind_count> contents);

        // ---- platform thread ----------------------------------------------------

        // The first window, before the render thread starts: the OS window
        // (for the GPU device to be chosen for), then the rest of it once
        // the device exists. finish_first() hands it to the interface.
        std::expected<platform::native_window *, std::string> create_first(window_kind kind);
        std::expected<void, std::string> finish_first();

        // Feeds `paths` to an open window's input as a drop of those files
        // (--open), so they take exactly the path a drop takes.
        void inject_drop(window_kind kind, std::span<const std::string> paths);

        // After every platform::poll_events(): what the interface asked for.
        void run_platform_tasks();

        // Once the render thread has ended: destroys every window.
        void shutdown();

        // ---- render (interface) thread ---------------------------------------------

        // At each wake-up, in this order: the platform thread's messages
        // (windows made, failed, closed by the user); every window's input;
        // timers and posted work; then collect() what any of those closed.
        void process();
        void drain();
        void update(time_point now);
        void collect();

        // The earliest time any screen has something to do.
        std::optional<time_point> next_wakeup() const;

        // The windows the render loop draws, in the order they opened.
        std::span<const std::shared_ptr<app_window>> windows() const noexcept { return m_live; }

        // At exit, after the render loop: takes down every window's
        // interface and releases its swapchain, then destroys the contents.
        void teardown();

        // ---- window_control ----------------------------------------------------------

        void open_window(window_kind kind) override;
        void close_window(window_kind kind) override;
        bool is_open(window_kind kind) const override;
        window_host *host(window_kind kind) override;
        void exit() override;
        void settings_changed() override;

    private:
        // To the platform thread.
        struct platform_task
        {
            enum class kind : std::uint8_t
            {
                create,
                focus,
                retire,
                configure, // a new title and minimum size
            };
            kind what = kind::create;
            std::shared_ptr<app_window> window{};
            math::vec2i default_size{}; // create: logical units
            math::vec2i min_size{};     // create, configure: logical units
            std::string title{};        // create, configure
        };

        // To the interface thread.
        struct interface_message
        {
            enum class kind : std::uint8_t
            {
                created,
                failed,
                close_requested,
            };
            kind what = kind::created;
            std::shared_ptr<app_window> window{}; // created, failed
            ui::window_kind about = ui::window_kind::project_manager; // close_requested
            std::uint64_t serial = 0;                                 // close_requested
            std::string error{};                  // failed
        };

        // Where a kind of window was when it last closed, to open it there
        // again. Platform thread.
        struct geometry
        {
            math::vec2i position{};
            math::vec2i size{}; // logical units
        };

        // ---- platform thread
        std::expected<void, std::string> make_window(app_window &w, const platform_task &task);
        std::expected<void, std::string> connect_window(const std::shared_ptr<app_window> &w);
        void retire(app_window &w);
        void update_display(app_window &w, bool refresh);
        void to_interface(interface_message message);

        // ---- render thread
        void to_platform(platform_task task);
        void on_created(const std::shared_ptr<app_window> &w);
        void on_close_requested(window_kind kind, std::uint64_t serial);
        void build_interface(app_window &w);
        void take_down(app_window &w);
        void set_aside(window_kind kind);

        // What a window of `kind` is made with, or changed to: its title
        // (the project and whether it is saved), its default and minimum
        // size at the user's scale.
        platform_task describe(platform_task::kind what, window_kind kind, std::shared_ptr<app_window> w) const;

        // Every window, opening or open: its title, and its minimum after a
        // change of the user's scale.
        void reconfigure();

        setup m_setup;
        std::array<std::unique_ptr<window_content>, window_kind_count> m_contents;

        // ---- platform thread
        std::vector<std::shared_ptr<app_window>> m_platform_windows;
        std::array<std::optional<geometry>, window_kind_count> m_geometry;
        std::shared_ptr<app_window> m_first;

        // ---- render (interface) thread
        window_book m_book;
        std::array<std::shared_ptr<app_window>, window_kind_count> m_slots; // opening or open, by kind
        std::vector<std::shared_ptr<app_window>> m_live;                    // open, in the order they opened
        std::vector<std::shared_ptr<app_window>> m_closing;                 // closed, not yet taken down
        bool m_torn_down = false;
        std::size_t m_project_connection = 0; // app_services::project_changed

        // ---- the mailboxes
        std::mutex m_platform_mutex;
        std::vector<platform_task> m_platform_tasks;
        std::mutex m_interface_mutex;
        std::vector<interface_message> m_interface_messages;
    };

} // namespace volt::ui
