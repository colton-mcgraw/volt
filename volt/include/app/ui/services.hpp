#pragma once

#include <app/simulation/network.hpp>
#include <app/ui/close_guards.hpp>
#include <app/ui/documents.hpp>
#include <app/ui/file_dialogs.hpp>
#include <app/ui/images.hpp>
#include <app/ui/message_log.hpp>
#include <app/ui/settings.hpp>
#include <app/ui/shared_text.hpp>
#include <app/ui/window_book.hpp>
#include <app/ui/windows/parts_panel.hpp>
#include <app/ui/worker.hpp>
#include <image/svg.hpp>
#include <ui/clipboard.hpp>
#include <ui/draw_list.hpp>
#include <ui/signal.hpp>
#include <ui/text.hpp>
#include <ui/theme.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace volt::board
{
    class renderer;
} // namespace volt::board

// The parts library (<library/catalog.hpp>, <library/project.hpp>, the
// volt_library module): only declared here, so that the windows that do
// not use it do not compile it.
namespace volt::library
{
    class catalog;
    struct project;
    struct scan_report;
} // namespace volt::library

// -------------------------------------------------------------------------
// app_services
// -------------------------------------------------------------------------
//
// What every window's content shares: the application's state and the
// services that reach outside a window. One object, owned by volt_app, as
// long as the application; every window content gets it (window_host::
// services()). It holds
//
//   * settings: the theme and the user's interface scale, both application
//     wide: setting either changes every open window at once;
//   * the text engine all screens share (<app/ui/shared_text.hpp>);
//   * the documents (<app/ui/documents.hpp>): the schematic and the board;
//   * images: textures for the interface (add_image(), remove_image()), the
//     offscreen views' targets (view_image()) and the logo at any size;
//   * files: open_files() reads models and images on a worker thread and
//     puts them where they belong;
//   * work for the interface thread from any thread (post());
//   * windows: opening, closing and raising the four kinds of window, and
//     showing a message in one (through the window manager);
//   * a slot for the parts library's catalog.
//
// Threading. Everything here is the interface thread's (the render thread,
// see <ui/screen.hpp>), except post(), which any thread may call. Until
// the render thread starts, the thread that creates the application is the
// interface thread.
//
// How to... (for the windows' code; see also <app/ui/windows/content.hpp>)
//
//   ask for another window      services.open_window(window_kind::board_editor)
//                               (raises it if it is open already)
//   show a status message       host.show_message("text") in your own window;
//                               services.show_message(kind, "text") in another
//   get an image texture        services.add_image(prepare_texture(image)),
//                               give it back with remove_image(); a logo of a
//                               given pixel size: services.logo(pixels)
//   do something on the        services.post([this] { ... }) from any thread;
//   interface thread later      not screen::post(), whose screen may be gone
//                               by then (its window closed)
//   react to settings           connect to settings_changed, or override
//                               window_content::scale_changed()/theme_changed()

namespace volt::ui
{

    class window_host;

    // The offscreen views: scenes rendered into a target of their own,
    // which the interface shows in a viewport (<ui/widgets/viewport.hpp>).
    // Each is rendered only in frames that draw a window showing it
    // (window_content::view()).
    enum class offscreen_view : std::uint8_t
    {
        schematic,
        board,
    };

    // What the services ask of the window manager, which implements it.
    // Interface thread.
    class window_control
    {
    public:
        virtual ~window_control() = default;

        // Opens a window of the kind, or raises it if it is open.
        virtual void open_window(window_kind kind) = 0;

        // Closes it, if open, once its close guards (window_host::guards())
        // let it go. Takes effect between frames, not during the call, so a
        // window may close itself from its own menu. Closing the project
        // manager exits.
        virtual void close_window(window_kind kind) = 0;

        virtual bool is_open(window_kind kind) const = 0;

        // The open window's host, or null.
        virtual window_host *host(window_kind kind) = 0;

        // Closes every window and ends the application.
        virtual void exit() = 0;

        // The settings changed: apply them to every window, and draw each
        // again.
        virtual void settings_changed() = 0;
    };

    // What app_services is given to work with. Set once, by volt_app.
    struct services_setup
    {
        // The runtime assets: fonts/, images/, models/.
        std::filesystem::path assets;

        // Where the board's meshes are made. Interface thread.
        board::renderer *board_renderer = nullptr;

        // Makes an image the interface can draw (image_textures::add()),
        // and drops one again. Interface thread, between frames.
        std::function<std::expected<texture_id, std::string>(const texture_pixels &)> add_image;
        std::function<void(texture_id)> remove_image;

        // Replaces the simulation's network and runs it.
        std::function<void(const simulation::network &)> run_simulation;

        // Rouses the interface thread. Any thread; must be cheap.
        std::function<void()> wake;

        // The user's interface scale to start with (--ui-scale).
        float user_scale = 1.0f;

        // What windows send to their displays to start with (--hdr).
        color_output output = color_output::sdr;

        // The parts library's directories, in order of priority (see
        // app_services::library_directories()). create() starts scanning
        // them; none, no scan (a catalog may still be set).
        std::vector<std::filesystem::path> library_directories;
    };

    class app_services
    {
    public:
        app_services();
        ~app_services();

        app_services(const app_services &) = delete;
        app_services &operator=(const app_services &) = delete;

        // Loads the fonts and the logo from setup.assets.
        std::expected<void, std::string> create(services_setup setup);

        // The window manager. Set once, before any window opens.
        void set_windows(window_control *windows) noexcept { m_windows = windows; }

        // ---- text and settings ----------------------------------------------------

        // The text engine every screen is made with.
        shared_text &text() noexcept { return *m_shared_text; }

        // Its glyph atlas, which the interface renderer mirrors once per
        // frame.
        font::atlas &text_atlas() noexcept { return m_text.atlas(); }

        // The theme every window uses: the chosen one, with the
        // application's fonts.
        ui::theme theme() const;

        // Everything the user chooses for the whole application
        // (<app/ui/settings.hpp>).
        const app_settings &settings() const noexcept { return m_settings; }

        // Replaces the settings: what is out of range is brought into it,
        // and if anything differs every window is brought up to date and
        // settings_changed fires. The one way a setting changes.
        void set_settings(app_settings settings);

        // Changes some of them: `change` is given a copy to edit.
        template <typename F>
        void change_settings(F &&change)
        {
            app_settings next = m_settings;
            change(next);
            set_settings(next);
        }

        // 0 dark, 1 light.
        int theme_index() const noexcept { return m_settings.theme; }
        void set_theme(int index);

        // The user's multiplier on the display's scale (<app/ui/scale.hpp>).
        float user_scale() const noexcept { return m_settings.user_scale; }
        void set_user_scale(float scale);

        // What windows send to their displays. The render loop reads it
        // for every frame, so a change shows in the next one.
        ui::color_output color_output() const noexcept { return m_settings.output; }
        void set_color_output(ui::color_output output);

        // After a change has been applied to every window: for whatever
        // shows the settings (an open Preferences).
        signal<> settings_changed;

        // ---- documents ---------------------------------------------------------------

        schematic_document &schematic() noexcept { return m_schematic; }
        board_document &board() noexcept { return m_board; }

        // Runs the simulation on the active sheet as it is: its circuit
        // (schematic_document::circuit()) goes to the simulation, and the
        // document records what was run (record_simulation()), so that the
        // wires show the potentials until the sheet changes. Returns what to
        // tell the user: the circuit's summary and the first error or
        // warning, after a dash ("9 nodes, 8 resistors, 1 source - R3: ..."),
        // or "Nothing to simulate".
        std::string run_simulation();

        // ---- images ------------------------------------------------------------------

        std::expected<texture_id, std::string> add_image(const texture_pixels &pixels);
        void remove_image(texture_id id);

        // What a viewport showing the view shows: the view's target,
        // registered with the interface renderer under this id.
        texture_id view_image(offscreen_view view) const noexcept;

        // Set once by the application, when the images exist; before any
        // window is built.
        void set_view_images(texture_id schematic, texture_id board) noexcept
        {
            m_schematic_image = schematic;
            m_board_image = board;
        }

        // The Volt logo as a texture `pixels` square. Made the first time a
        // size is asked for and kept while a window shows it at that size:
        // a window asks again when its scale changes (window_host::
        // track_logo() does), and trim_logos() then lets the sizes go that
        // no open window shows any more. Invalid if there is no logo.
        texture_id logo(int pixels);

        // Releases the logo textures of the sizes no open window tracks
        // (window_host::logo_sizes()). The window manager calls it when a
        // window closes, the hosts when their scale changes. An id given
        // out for a size released names nothing from then on.
        void trim_logos();

        // ---- files -------------------------------------------------------------------

        // Opens files dropped on window `from` (or named on the command
        // line), UTF-8 paths. A project (.voltproj) replaces the open one,
        // asking first about unsaved changes (confirm_discard()); of several,
        // the first. A library (.voltlib) joins the catalog for the session
        // (see add_library_file()). Models (.step, .stp, .obj, .stl) go on the
        // board: at `on_board` (millimeters on the board's top) when they
        // were dropped on the board view, else in a free spot. Images (.png,
        // .jpg, .jpeg, .bmp, .svg) open in `from`'s image viewer. Reading,
        // tessellating and decoding run on a worker thread, a file at a time
        // (<app/ui/worker.hpp>); messages go to `from`'s status bar, or the
        // project manager's if `from` has closed meanwhile.
        void open_files(std::span<const std::string> paths, window_kind from,
                        std::optional<math::vec2f> on_board = std::nullopt);

        // Opens a web address (a part's datasheet) in the system's
        // browser (platform::open_url()), saying in window `from` that it
        // did, or why not. Only http and https; anything else is refused
        // without starting anything.
        void open_link(const std::string &url, window_kind from);

        // Files being read.
        std::size_t loading() const noexcept { return m_loading; }

        // Drops the files not started and waits for the one being read. The
        // application calls it before it takes apart what their results
        // would be posted to. An open file dialog is closed too (its answer
        // is dropped).
        void stop_loading();

        // ---- file dialogs -------------------------------------------------------------
        //
        // The system's file dialogs (<platform/file_dialog.hpp>), modal to
        // a window of the application, one at a time. Each kind of file
        // has a purpose (<app/ui/file_dialogs.hpp>), which says what the
        // dialog lists, and the folder last chosen in for a purpose is
        // where its next dialog starts, for the session. The answer comes
        // on the interface thread, later: the window may have closed
        // meanwhile, so look it up again (window_content::host()) before
        // touching its widgets.
        //
        // From a window, for one file (a "Browse..." button):
        //
        //     services().choose_file(kind(), file_purpose::model,
        //                            [this](const std::filesystem::path &file) {
        //                                if (host() != nullptr)
        //                                    m_model_field->set_text(platform::path_to_utf8(file));
        //                            });

        // Asks for one file (or folder) with `purpose`'s dialog, modal to
        // window `from`; `then` gets the path chosen, and does not run when
        // the user cancels. When there is no dialog (or one is open
        // already, or it failed), `from`'s status bar says so instead.
        void choose_file(window_kind from, file_purpose purpose,
                         std::function<void(const std::filesystem::path &)> then);

        // The general form: `dialog` as given (start from
        // file_dialog_for(purpose); an empty directory gets the purpose's
        // folder), and `then` with the whole result: the paths, none when
        // cancelled, or the error, whose `unavailable` says there is no
        // dialog on this system so that the caller can fall back.
        void show_file_dialog(window_kind from, file_purpose purpose, platform::file_dialog dialog,
                              std::function<void(platform::file_dialog_result)> then);

        // File > Open in window `from`: everything Volt opens, several
        // files at once, which then open as if dropped on that window
        // (open_files()). Where there is no dialog, `unavailable` runs if
        // given (the project manager asks for a typed path); otherwise the
        // window says to drop the files instead.
        void open_with_dialog(window_kind from, std::function<void()> unavailable = {});

        // A file dialog is up, and in which window.
        std::optional<window_kind> file_dialog_window() const noexcept { return m_file_dialog; }

        // Where `purpose`'s next dialog starts: the folder last chosen in
        // for it, else one that fits (the open project's), else empty (the
        // system's choice).
        std::filesystem::path file_folder(file_purpose purpose) const;

        // ---- the interface thread ---------------------------------------------------

        // From any thread: runs `task` on the interface thread at its next
        // wake-up, whatever windows are open then.
        void post(std::function<void()> task);

        // Interface thread, each wake-up: runs what has been posted.
        void run_posted();

        // Forgets what has been posted and not run.
        void clear_posted();

        // ---- windows -----------------------------------------------------------------

        void open_window(window_kind kind);
        void close_window(window_kind kind);
        bool is_open(window_kind kind) const;

        // The open window's host, or null.
        window_host *window(window_kind kind);

        // Ends the application, as File > Exit does: once every open
        // window's close guards (window_host::guards()) and then
        // confirm_discard() have let it go.
        void exit();

        // A message in the status bar of window `where`, or of the project
        // manager if `where` is not open, kept in messages() at `level`:
        // what reports a failure says so, so that the log can tell it
        // apart (warning: done, but not all of it, or not as asked; error:
        // not done).
        void show_message(window_kind where, std::string text,
                          std::chrono::seconds duration = std::chrono::seconds(4),
                          log_level level = log_level::info);

        // Every message shown in any window, kept (<app/ui/message_log.hpp>).
        message_log &messages() noexcept { return m_messages; }

        // ---- the parts library ------------------------------------------------------
        //
        // The catalog (<library/catalog.hpp>) of every library under the
        // library directories, built on the loader thread (the one files are
        // read on) and handed over finished: create() starts the first scan,
        // rescan_catalog() another, and each one's catalog replaces the one
        // shown when it is done. Windows keep part_refs only with the
        // catalog they came from, and find parts again in a new one by
        // qualified id ("volt-resistors:p-7k3f9x2m4qha"). Null until the
        // first scan is done.

        const std::shared_ptr<const library::catalog> &catalog() const noexcept { return m_catalog; }
        void set_catalog(std::shared_ptr<const library::catalog> catalog)
        {
            m_catalog = std::move(catalog);
            catalog_changed.emit();
        }
        signal<> catalog_changed;

        // The directories scanned, in order of priority: where two hold a
        // library of the same name, the first one's is used (and the other
        // reported). --library-dir ones, then the user's, then the shipped.
        const std::vector<std::filesystem::path> &library_directories() const noexcept
        {
            return m_library_directories;
        }

        // Adds a directory for this session, last, and rescans. False when
        // it is there already (it is rescanned anyway).
        bool add_library_directory(const std::filesystem::path &directory);

        // A library file the user opened (a .voltlib dropped on a window):
        // for this session it is loaded ahead of every directory, so it
        // wins over a library of the same name there, the shipped one
        // included, which it shadows (library::scan_options::files). The
        // file opened last wins over those opened before. Rescans; when the
        // scan is done, window `from` says what came of it: loaded, and
        // what it shadows, or why not. False when it was open already.
        bool add_library_file(const std::filesystem::path &file, window_kind from);
        const std::vector<std::filesystem::path> &library_files() const noexcept { return m_library_files; }

        // Scans the library directories again; the new catalog replaces the
        // shown one when it is done. A scan asked for while one runs follows
        // it.
        void rescan_catalog();

        // A scan is running, and how far it is: files done of total (0 of 0
        // while it is still looking for them).
        bool catalog_loading() const noexcept { return m_scanning; }
        std::size_t scan_done() const noexcept { return m_scan_done; }
        std::size_t scan_total() const noexcept { return m_scan_total; }

        // What the last finished scan found: files, problems, time. Null
        // before the first one is done.
        const std::shared_ptr<const library::scan_report> &catalog_report() const noexcept { return m_scan_report; }

        // A scan started, moved on, or finished (its report is there). The
        // catalog itself comes with catalog_changed.
        signal<> catalog_status_changed;

        // Where the user's own libraries go: %APPDATA%\Volt\libraries on
        // Windows, $XDG_DATA_HOME/volt/libraries (~/.local/share/volt/
        // libraries) elsewhere. Nothing creates it: it is scanned when it
        // exists. Empty when there is no such location.
        static std::filesystem::path user_library_directory();

        // The library that parts handed to the application go into (a
        // .voltpart file dropped on a window): "my-parts.voltlib" in the
        // user's library directory, made when the first part arrives.
        // Empty when there is no such directory.
        static std::filesystem::path user_parts_library();

        // Where parts' 3D models are (a part's model is relative to it).
        std::filesystem::path models_directory() const { return m_setup.assets / "models"; }

        // The runtime assets directory. The libraries below it ship with
        // Volt and are not changed in place (is_shipped_library(),
        // <app/ui/windows/part_editor_model.hpp>).
        const std::filesystem::path &assets_directory() const noexcept { return m_setup.assets; }

        // ---- the library windows ------------------------------------------------------

        // Opens (or raises) the Library Browser showing a part: a catalog
        // part by qualified id, or with `project` a part of the project
        // library by its id there.
        void reveal_in_library(std::string id, bool project = false);

        // The part the browser was asked to show and has not shown yet:
        // the browser takes it when it builds or hears library_reveal.
        struct library_reveal
        {
            std::string id;
            bool project = false;
        };
        std::optional<library_reveal> take_library_reveal();
        signal<> library_reveal_requested;

        // What a parts panel keeps while its window is closed, one per kind
        // of window that docks one (<app/ui/windows/parts_panel.hpp>).
        parts_panel_state &parts_panel(window_kind kind) noexcept { return m_parts_panels[index_of(kind)]; }

        // The system's clipboard where the platform has one the
        // application can reach (Windows), else one in memory. The window
        // manager gives it to every window's screen, so that text copied in
        // any window pastes in any other and in other applications.
        ui::clipboard &system_clipboard() noexcept;

        // ---- the project ---------------------------------------------------------------
        //
        // At most one project is open (<library/project.hpp>): its .voltproj
        // file, its project library (the parts the design uses, copied out
        // of the catalog), and its sheets, which open in the schematic
        // document (schematic(), <app/ui/schematic_document.hpp>) as the
        // project opens and are written as it saves. With none open, the
        // schematic shows its scratch sheet. Interface thread. The functions
        // that can fail return what to tell the user; what they find on the
        // way (a sheet that did not load, one not saved because of that)
        // goes to show_message() and the message log.

        // The open project, or null. Changing it through this pointer (its
        // parts) is allowed; call mark_project_modified() after.
        library::project *project() noexcept { return m_project.get(); }
        const library::project *project() const noexcept { return m_project.get(); }

        // The open project's .voltproj file; empty when none is open.
        const std::filesystem::path &project_path() const noexcept { return m_project_path; }

        // The project was opened, closed or saved, or its library changed
        // (added, removed, updated parts; modified or not), or its sheets
        // went from saved to modified or back (not at every edit).
        signal<> project_changed;

        // Makes `directory`/`name`.voltproj (and its project library and
        // its sheet, empty), saves them at once and opens the project,
        // closing the open one. Fails if the project file exists already.
        // The directory is created if needed. A sheet file of the name
        // there already is opened as the project's sheet, not replaced.
        std::expected<void, std::string> new_project(const std::filesystem::path &directory, std::string name);

        // Opens a .voltproj file, closing the open project, and its sheets
        // (schematic_document::open_project_sheets()): a sheet file that is
        // missing is a new empty sheet; one that cannot be read opens empty
        // and is never saved over. Problems that do not stop it opening (a
        // missing project library, a sheet that did not load) are reported
        // through show_message.
        std::expected<void, std::string> open_project(const std::filesystem::path &file);

        // Writes the open project: the sheets that changed or have no file
        // yet (not those that did not load: that is reported), then its
        // library and the project file.
        std::expected<void, std::string> save_project();

        // Writes the open project to `file` (".voltproj" added if it has no
        // extension) and goes on with it there. Its project library is
        // written beside it, named after it ("Blinky 2.voltlib"). Its sheets
        // are written beside it too, every one, under the paths the project
        // lists them by, now relative to the new folder; a sheet file named
        // after the project ("Blinky.voltsch") is named after the new one
        // ("Blinky 2.voltsch"), and one that would land on the file it came
        // from (Save As in the same folder) gets the new project's name in
        // front ("Blinky 2 - power.voltsch"), so that the project saved from
        // keeps its files. A sheet that did not load stays where it is,
        // referred to from the new folder. The board, which is not written
        // yet, is referred to where it is. Refused when the library or a
        // sheet file exists and the project file does not (they would
        // belong to something else). The same file as now is a plain
        // save_project().
        std::expected<void, std::string> save_project_as(const std::filesystem::path &file);

        // Closes the project, unsaved changes and all (see
        // confirm_discard() for asking first). The schematic goes back to
        // its scratch sheet.
        void close_project();

        // Runs `then` once the open project's unsaved changes are dealt
        // with: at once when there are none; otherwise after asking, in a
        // dialog in window `where` (raised), whether to save them first:
        // Save (then runs if saving worked), Don't Save (then runs), Cancel
        // (it does not). `doing` names what waits, for the dialog's
        // buttons and text: "Exit", "Open Project", "Close Project". Whoever
        // closes, replaces or exits past the project goes through here;
        // exit() does. The project guards are asked before that.
        void confirm_discard(window_kind where, std::string doing, std::function<void()> then);

        // What holds work meant for the open project (an editor saving into
        // its library, <app/ui/windows/part_editor.hpp>): asked first by
        // confirm_discard(), so that it can be saved into the project, or
        // let go, before the project is closed or replaced; any can keep
        // the project open (<app/ui/close_guards.hpp>).
        close_guards &project_guards() noexcept { return m_project_guards; }

        // Changes since it was opened or saved: to the project or its
        // library (mark_project_modified()), or to any of its sheets (each
        // sheet's history knows, so undoing back to what was saved is
        // unmodified again).
        bool project_modified() const noexcept
        {
            return m_project != nullptr && (m_project_modified || m_schematic.modified());
        }
        void mark_project_modified();

        // Copies the catalog part `qualified_id` ("lib:part") into the
        // project library and returns its id there. Adding a part that is
        // there already returns its id and changes nothing
        // (project_part_of() tells the two apart beforehand).
        std::expected<std::string, std::string> add_to_project(std::string_view qualified_id);

        // The project library's id of the copy of catalog part
        // `qualified_id`, or empty when the project has none (or no project
        // is open).
        std::string project_part_of(std::string_view qualified_id) const;

    private:
        void load_logo();
        void start_load(std::function<void()> job);
        void finish_load();
        void open_model(const std::filesystem::path &path, window_kind from, std::optional<math::vec2f> where);
        void open_image(const std::filesystem::path &path, window_kind from);
        void open_project_file(const std::filesystem::path &path, window_kind from);
        void open_library_file(const std::filesystem::path &path, window_kind from);
        void open_part_package(const std::filesystem::path &path, window_kind from);
        void finish_scan(std::shared_ptr<const library::catalog> built,
                         std::shared_ptr<const library::scan_report> report);

        services_setup m_setup;
        window_control *m_windows = nullptr;
        texture_id m_schematic_image;
        texture_id m_board_image;

        // The text engine and the wrapper every screen talks to.
        font_text_engine m_text;
        std::unique_ptr<shared_text> m_shared_text;
        font_id m_body_font = 0;
        font_id m_title_font = 0;

        app_settings m_settings;

        schematic_document m_schematic;
        board_document m_board;

        // The logo: the SVG (or the PNG, without it), and the sizes made so
        // far, by pixel size.
        std::optional<image::svg::document> m_logo_svg;
        std::optional<texture_pixels> m_logo_png;
        std::map<int, texture_id> m_logos;

        // Files being read, and how many are not done.
        worker m_loader;
        std::size_t m_loading = 0;

        std::mutex m_posted_mutex;
        std::vector<std::function<void()>> m_posted;

        message_log m_messages;

        std::shared_ptr<const library::catalog> m_catalog;

        std::unique_ptr<library::project> m_project;
        std::filesystem::path m_project_path;
        bool m_project_modified = false;
        // The sheets are being opened or saved, which emits project_changed
        // once at the end rather than as each sheet's state changes.
        bool m_sheets_busy = false;
        void open_sheets(const library::project &p, const std::filesystem::path &file);
        std::expected<void, std::string> save_sheets();
        bool m_confirming = false; // a confirm_discard() dialog is open
        close_guards m_project_guards;
        void ask_discard(window_kind where, const std::string &doing, const std::function<void()> &then);

        // Asks the close guards of the open windows, from all_window_kinds[from]
        // on, a window at a time; `then` when all have let go.
        void confirm_windows(std::size_t from, std::function<void()> then);

        // The catalog's scans. The flag stops a running scan between files
        // (stop_loading()); the counts are the interface thread's copy of
        // its progress.
        std::vector<std::filesystem::path> m_library_directories;
        std::vector<std::filesystem::path> m_library_files; // opened, latest first

        // Opened files whose outcome the next finished scan tells, and
        // where.
        std::vector<std::pair<std::filesystem::path, window_kind>> m_library_files_to_report;
        void report_opened_libraries(const library::catalog &catalog, const library::scan_report &report);

        std::shared_ptr<const library::scan_report> m_scan_report;
        std::shared_ptr<std::atomic<bool>> m_scan_cancel = std::make_shared<std::atomic<bool>>(false);
        bool m_scanning = false;
        bool m_rescan_pending = false;
        std::size_t m_scan_done = 0;
        std::size_t m_scan_total = 0;

        std::optional<library_reveal> m_reveal;
        std::array<parts_panel_state, window_kind_count> m_parts_panels{};
        std::unique_ptr<ui::clipboard> m_clipboard;

        // The file dialogs: the window one is up in, and the folder last
        // chosen in for each purpose (empty: none yet).
        std::optional<window_kind> m_file_dialog;
        std::array<std::filesystem::path, file_purpose_count> m_file_folders{};
        void close_file_dialog();
        void report_file_dialog(window_kind from, const platform::file_dialog_error &error);
    };

} // namespace volt::ui
