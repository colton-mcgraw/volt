#pragma once

#include <app/ui/close_guards.hpp>
#include <app/ui/services.hpp>
#include <app/ui/window_book.hpp>
#include <app/ui/windows/status_bar.hpp>
#include <ui/screen.hpp>
#include <ui/signal.hpp>
#include <ui/widgets/menu.hpp>

#include <math/vector.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// -------------------------------------------------------------------------
// window content
// -------------------------------------------------------------------------
//
// What a top-level window shows, apart from what every window has. The
// application has four kinds of window (<app/ui/window_book.hpp>), and one
// window_content object per kind, made at start and kept until exit, open
// or not:
//
//   project_manager    <app/ui/windows/project_manager.hpp>
//   schematic_editor   <app/ui/windows/schematic_editor.hpp>
//   board_editor       <app/ui/windows/board_editor.hpp>
//   library_browser    <app/ui/windows/library_browser.hpp>
//
// Because the object outlives its windows, a window opened again comes back
// as the user left it: whatever the content keeps in its members (cameras,
// splitter positions, a filter) survives; only the widgets are made anew.
// What all windows share lives in app_services (<app/ui/services.hpp>).
//
// The frame. The window manager builds every window the same way, around
// what build() returns (window_layout):
//
//   menu bar     File (the content's file_items, then Preferences, Close
//                Window, Exit), the content's own menus, Window, Help
//   body         the content's widget tree, filling the window
//   status bar   the content's own segments at the left, then the message,
//                the progress bar, the size, display and scale
//
// Lifecycle, all on the interface thread (the render thread):
//
//   build(host)       a window of this kind has opened: register commands
//                     on host.screen(), connect to shared signals, return
//                     the layout. `host` stays valid until detach().
//   update(now)       every wake-up, after the window's input was handled
//   before_frame()    the window is about to be laid out and painted:
//                     bring status texts up to date
//   view(v)           which viewport shows offscreen view `v`, if any; the
//                     render loop renders the view at that viewport's size
//                     in frames that draw this window
//   drop(paths, at)   files were dropped on the window
//   scale_changed()   the window's scale changed (monitor, or Preferences);
//   theme_changed()   the theme changed; screen and widgets are restyled
//                     already, this is for what the content draws itself
//   detach()          the window is closing: forget every widget pointer,
//                     disconnect from shared signals, keep what should
//                     survive. The widgets and the screen go right after.
//
// Extension points (for the code of one window):
//
//   a command         host.screen().add_command("board.flip", [this] {...})
//                     in build(), and a shortcut with host.screen()
//                     .shortcuts().bind("Ctrl+F", "board.flip"). Ids are
//                     per screen, so each window has its own; the ones
//                     every window has are reserved (menus.hpp lists them).
//   a menu            layout.menus.push_back({"Place", {command_item(...)}})
//                     (helpers in <app/ui/windows/menus.hpp>); File items go
//                     in layout.file_items.
//   a dock panel      the body is the content's own tree: split it with a
//                     ui::splitter (set_fixed(keep::first, width) for a side
//                     panel), keep the splitter's position() in detach() and
//                     give it back to set_fixed() in the next build().
//   a status segment  layout.status = [this](status_bar &bar) {
//                         m_label = &bar.add_segment("", 120.0f); };
//   another window    host.services().open_window(window_kind::board_editor)
//   a message         host.show_message("Saved")
//   an image texture  host.services().add_image(prepare_texture(image)),
//                     remove_image() when done; host.show_image() shows one
//                     in a viewer and frees it when the viewer closes
//   the logo          host.track_logo(view, logical_size) keeps an
//                     image_view's logo sharp at this window's scale
//   a timer           host.screen().schedule_after(...): dies with the
//                     window. Work from other threads: services().post().
//   unsaved work      host.guards().add(...) while there is some: closing
//                     the window or exiting asks it first, and waits for
//                     its answer (Save, Discard, Keep Editing).

namespace volt::ui
{

    class image_view;
    class menu_bar;
    class viewport;

    // What a content puts in its window, see the frame above.
    struct window_layout
    {
        struct menu
        {
            std::string title;
            std::vector<menu_item> items;
        };

        // At the top of the File menu, above Preferences, Close Window and
        // Exit.
        std::vector<menu_item> file_items;

        // The window's own menus, between File and Window, in order.
        std::vector<menu> menus;

        // The window's widgets, between the menu bar and the status bar.
        std::unique_ptr<widget> body;

        // Adds the window's own segments at the left of the status bar.
        std::function<void(status_bar &)> status;
    };

    // ---------------------------------------------------------------------
    // window_host
    // ---------------------------------------------------------------------
    //
    // One open window as its content sees it: its screen, the shared
    // services, and what every window can do (messages, the image viewer,
    // the logo, closing). Made by the window manager when the window opens
    // and destroyed when it closes, after the content's detach(). Interface
    // thread.
    class window_host
    {
    public:
        window_host(window_kind kind, ui::screen &screen, app_services &services);
        ~window_host();

        window_host(const window_host &) = delete;
        window_host &operator=(const window_host &) = delete;

        window_kind kind() const noexcept { return m_kind; }
        ui::screen &screen() const noexcept { return m_screen; }
        app_services &services() const noexcept { return m_services; }

        // ---- scale -------------------------------------------------------------------

        // The window's scale (pixels per logical unit), and the two numbers
        // it is made of (<app/ui/scale.hpp>).
        float scale() const noexcept { return m_screen.scale(); }
        float content_scale() const noexcept { return m_content_scale; }
        float user_scale() const noexcept { return m_services.user_scale(); }

        // ---- what every window can do --------------------------------------------------

        // Shows `text` in the status bar for `duration`, replacing what was
        // there, and keeps it in app_services::messages() at `level`.
        void show_message(std::string text, std::chrono::seconds duration = std::chrono::seconds(4),
                          log_level level = log_level::info);

        // Opens `image` (an id from add_image(), `pixels` big) in this
        // window's image viewer, a modal dialog; the texture is given back
        // when it closes.
        void show_image(texture_id image, math::vec2i pixels, std::string title, std::string details);

        // Keeps `view` showing the logo, `logical` units square, rasterized
        // for this window's scale whenever that changes. Call forget_logo()
        // before the view goes, unless it goes with the window.
        void track_logo(image_view &view, float logical);
        void forget_logo(const image_view &view) noexcept;

        // Closes this window (between frames; safe from its own handlers),
        // once its close guards have let it go.
        void close();

        // What in the window holds work that closing it would lose (an
        // editor in a popup with changes): asked before the window closes,
        // whichever way (its close button, File > Close Window, Ctrl+W), and
        // before the application exits; any of them can keep it open
        // (<app/ui/close_guards.hpp>). Add a guard while there is something
        // to lose, and remove it before it goes.
        close_guards &guards() noexcept { return m_guards; }

        // ---- for the window manager ----------------------------------------------

        window_status &status() noexcept { return m_status; }

        // The window's content scale changed, or the user's: the screen
        // gets content * user and the logos follow; then scale_changed.
        void apply_scale(float content_scale);

        // The window's scale was applied anew (it moved to another monitor,
        // or a scale changed): for what shows it while open, such as the
        // Preferences dialog. Interface thread.
        signal<> scale_changed;

        // The pixel sizes of the logos this window shows now, for
        // app_services::trim_logos().
        std::vector<int> logo_sizes() const;

        // What the status bar says about the window's frames.
        void set_frame_info(const math::vec2i &pixels, std::string_view encoding);

        // Whether the window's monitor is in HDR mode, and what the window
        // sends it ("sRGB", "scRGB", "HDR10"), as of the last frame drawn;
        // display_changed when either became something else. For what
        // shows them while open: the Preferences dialog.
        void set_display_hdr(bool hdr);
        bool display_hdr() const noexcept { return m_display_hdr; }
        const std::string &encoding() const noexcept { return m_encoding; }
        signal<> display_changed;

        // The menu bar, for F10 (view.menu).
        void set_menu_bar(menu_bar *bar) noexcept { m_menu_bar = bar; }
        menu_bar *menus() const noexcept { return m_menu_bar; }

        // The window's OS handles, for the system's dialogs to be modal to
        // (app_services::show_file_dialog()). Set when the host is made;
        // the window outlives the host. Only handed to the platform calls
        // that may be made from this thread (<platform/file_dialog.hpp>).
        void set_native(platform::native_window *native) noexcept { m_native = native; }
        platform::native_window *native() const noexcept { return m_native; }

    private:
        struct tracked_logo
        {
            image_view *view = nullptr;
            float logical = 0.0f;
        };

        window_kind m_kind;
        ui::screen &m_screen;
        app_services &m_services;
        window_status m_status;
        std::vector<tracked_logo> m_logos;
        close_guards m_guards;
        menu_bar *m_menu_bar = nullptr;
        platform::native_window *m_native = nullptr;
        float m_content_scale = 1.0f;
        bool m_display_hdr = false;
        std::string m_encoding;
    };

    // ---------------------------------------------------------------------
    // window_content
    // ---------------------------------------------------------------------
    //
    // The base of the four windows' contents; see the top of this file.
    class window_content
    {
    public:
        explicit window_content(app_services &services) : m_services(services) {}
        virtual ~window_content() = default;

        window_content(const window_content &) = delete;
        window_content &operator=(const window_content &) = delete;

        virtual window_kind kind() const noexcept = 0;

        // How big the window opens the first time, logical units (later it
        // opens where and as big as it was when last closed), in the middle
        // of its monitor. Keep it well inside a laptop's screen: 3840 x
        // 2400 at 250% is 1536 x 960 less the taskbar. A window that does
        // not fit is made smaller to fit the work area.
        virtual math::vec2i default_size() const noexcept { return {1200, 720}; }

        // The smallest the user may make the window, logical units: below
        // it the frame no longer fits (the menu bar runs out of room, the
        // status bar cuts off its frame information, tool strips clip their
        // last tools). Both sizes are the interface's: at a user scale
        // above 1 the window is made that much bigger (with_user_scale(),
        // <app/ui/window_geometry.hpp>), and the work area has the last
        // word where it is smaller.
        virtual math::vec2i min_size() const noexcept { return {720, 440}; }

        // ---- with a window -------------------------------------------------------

        virtual window_layout build(window_host &host) = 0;
        virtual void detach() {}

        virtual void update(time_point /*now*/) {}
        virtual void before_frame() {}

        virtual viewport *view(offscreen_view /*view*/) noexcept { return nullptr; }

        // Files dropped at `position` (the window's pixels). By default they
        // go to app_services::open_files() for this window.
        virtual void drop(std::span<const std::string> paths, math::vec2i position);

        virtual void scale_changed(float /*scale*/) {}
        virtual void theme_changed() {}

        // ---- for the window manager ----------------------------------------------

        // The host while a window is open, else null.
        window_host *host() const noexcept { return m_host; }
        void set_host(window_host *host) noexcept { m_host = host; }

    protected:
        app_services &services() const noexcept { return m_services; }

    private:
        app_services &m_services;
        window_host *m_host = nullptr;
    };

} // namespace volt::ui
