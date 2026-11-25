#include <app/ui/windows/part_editor.hpp>

#include <app/ui/file_dialogs.hpp>
#include <app/ui/services.hpp>
#include <app/ui/windows/content.hpp>
#include <app/ui/windows/library_dialogs.hpp>
#include <app/ui/windows/part_editor_model.hpp>
#include <library/edit.hpp>
#include <library/project.hpp>
#include <ui/layout/box.hpp>
#include <ui/screen.hpp>
#include <ui/theme.hpp>
#include <ui/widgets/button.hpp>
#include <ui/widgets/dropdown.hpp>
#include <ui/widgets/label.hpp>
#include <ui/widgets/panel.hpp>
#include <ui/widgets/property_grid.hpp>
#include <ui/widgets/scroll_area.hpp>
#include <ui/widgets/text_field.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace volt::ui
{
    namespace
    {
        // The dialog's widest, logical units; narrower on a small window.
        constexpr float editor_width = 960.0f;
        constexpr float checks_width = 290.0f;

        // Where the part is saved.
        enum class destination : std::uint8_t
        {
            user_library,    // My Parts: app_services::user_parts_library()
            project_library, // the open project's library, in memory
            library_file,    // the library file the part is in
        };

        // The file a part goes into, as it is on disk now: read again for
        // every save, so that what another save (or another program) put
        // there is kept. My Parts may not exist yet. A file that lost parts
        // when it was read is not saved over: they would be lost for good.
        std::expected<library::library, std::string> load_target_file(const std::filesystem::path &path,
                                                                       bool may_be_new)
        {
            std::error_code error;
            if (may_be_new)
            {
                // Not knowing is not "not there": a new library saved over
                // the user's would lose every part in it.
                const bool there = std::filesystem::exists(path, error);
                if (error)
                    return std::unexpected(std::format("Cannot tell whether {} exists ({}).",
                                                       library::path_to_utf8(path.filename()), error.message()));
                if (!there)
                    return library::new_library("my-parts", "My Parts");
            }
            auto loaded = library::load_library(path);
            if (!loaded)
                return std::unexpected(std::move(loaded.error()));
            if (const std::size_t lost = loaded->report.parts_skipped; lost > 0)
                return std::unexpected(std::format("{} has {} part{} that could not be read, and saving it would "
                                                   "lose {}. Fix the file first (Library > Scan Report says what "
                                                   "is wrong).",
                                                   library::path_to_utf8(path.filename()), lost, lost == 1 ? "" : "s",
                                                   lost == 1 ? "it" : "them"));
            return std::move(loaded->library);
        }

        // The 3D model files the board reads (see app_services::open_files).
        bool is_model_file(const std::filesystem::path &path)
        {
            std::string extension = library::path_to_utf8(path.extension());
            for (char &c : extension)
                c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            return extension == ".step" || extension == ".stp" || extension == ".obj" || extension == ".stl";
        }

        // Each level named as well as coloured, so that the two tell apart
        // without the colours too.
        std::string_view severity_word(library::severity level) noexcept
        {
            switch (level)
            {
            case library::severity::error:
                return "ERROR";
            case library::severity::warning:
                return "WARNING";
            case library::severity::note:
                return "NOTE";
            }
            return "NOTE";
        }

        math::vec4f severity_color(const ui::theme &t, library::severity level) noexcept
        {
            switch (level)
            {
            case library::severity::error:
                return t.error;
            case library::severity::warning:
                return t.warning;
            case library::severity::note:
                return t.text_dim;
            }
            return t.text_dim;
        }

        std::string part_display_name(const library::part &p)
        {
            if (!p.name.empty())
                return p.name;
            if (!p.id.empty())
                return p.id;
            return "the new part";
        }

        // "Resistance (Ω)": a completion as a button shows it.
        std::string completion_text(const parameter_completion &c)
        {
            std::string text = c.name;
            if (!c.unit.empty())
                text += " (" + c.unit + ")";
            return text;
        }

        // A text field that says when it loses the focus. What is typed
        // into a field is one step of the part's history until then;
        // typed into again later, it is another (part_editor_model::seal()).
        class edit_field final : public text_field
        {
        public:
            using text_field::text_field;

            signal<> focus_lost;

        protected:
            void on_focus_changed(bool focused, focus_reason reason) override
            {
                text_field::on_focus_changed(focused, reason);
                if (!focused)
                    focus_lost.emit();
            }
        };

        // Shows `text` unless the field shows it already: setting it moves
        // the caret to the end, which the field being typed in must not
        // have happen for nothing.
        void show_text(text_field &field, const std::string &text)
        {
            if (field.text() != text)
                field.set_text(text);
        }

        // ---------------------------------------------------------------------
        // the dialog
        // ---------------------------------------------------------------------
        //
        // The popup's content, which owns the model: the editor lives as
        // long as its popup. Its widgets are made once (build()), apart from
        // the parameter rows, which are made again when rows come, go or
        // move, and the checks and hints, which are made again after every
        // change (refresh()). A text field the user is typing in is never
        // made again, so it keeps the focus and the caret.
        //
        // What a button inside a part that is made again does runs later
        // (later()), after the click has been handled: the button is then
        // no longer in the middle of its own event when it goes.
        //
        // Undo and redo are the model's, one history for the whole part:
        // Ctrl+Z, Ctrl+Y and Ctrl+Shift+Z, and the buttons in the header.
        // The keys are taken here (on_key) rather than bound as the
        // window's shortcuts, which the screen does not run behind a modal
        // dialog; a text field has no undo of its own and lets them through
        // to here, so what Ctrl+Z takes back in a field being typed in is
        // the model's step, all that was typed there. Afterwards every
        // widget shows the model again (show_model()).
        class part_editor_frame final : public panel
        {
        public:
            explicit part_editor_frame(window_host &host)
                : panel("", surface::raised, 10.0f, insets::uniform(16.0f)), m_host(host), m_services(host.services())
            {
                set_border(true);
                set_name("part editor");
            }

            ~part_editor_frame() override
            {
                *m_alive = false;
                // Normally done when the popup closed (unwatch()); the host
                // may be gone by now, the services are not.
                m_services.project_guards().remove(m_project_guard);
                if (m_project_connection != 0)
                    m_services.project_changed.disconnect(m_project_connection);
            }

            part_editor_model &model() noexcept { return m_model; }

            // For a part of a library file, edited in place.
            void set_file(std::filesystem::path file) { m_file = std::move(file); }

            // Where the part goes; reads the target library for the id's
            // checks.
            void choose_destination(destination where);

            void build();
            void focus_first();
            void attach_model(const std::filesystem::path &file);

            // Asks first when there are changes.
            void request_close();

            // Once shown: the window's close guard (its window closing, the
            // application exiting), the project's (the project closing or
            // replaced, when the part goes into it), and the project's
            // changes followed. Undone by unwatch(), when the popup closes.
            void watch();
            void unwatch();

        protected:
            bool on_key(const key_event &e) override
            {
                if (e.action == action::release)
                    return false;
                const modifiers held = e.mods & held_modifiers;

                // The screen closes dismissable popups on Escape; this one
                // is not (a stray click must not lose the edits), so it
                // closes itself, asking first.
                if (e.key == key::escape && held == modifiers::none)
                {
                    request_close();
                    return true;
                }

                // Taken whether or not there is something to undo: nothing
                // behind the dialog is to have them.
                if (e.key == key::z && held == modifiers::control)
                {
                    undo();
                    return true;
                }
                if ((e.key == key::y && held == modifiers::control) ||
                    (e.key == key::z && held == (modifiers::control | modifiers::shift)))
                {
                    redo();
                    return true;
                }
                return false;
            }

        private:
            struct parameter_row
            {
                text_field *name = nullptr;
                text_field *value = nullptr;
                text_field *unit = nullptr;
                widget *notes = nullptr;
                label *note = nullptr;
                row *hints = nullptr;
            };

            void later(std::function<void()> action);
            void close();
            // True when the part was saved (and the editor closed).
            bool save();

            // Closes the editor, asking first when there are changes: Save,
            // Discard, or Keep Editing; then `answer` (if any) hears whether
            // it went. Asked again while the question is up, the one answer
            // goes to every caller.
            void confirm_leave(close_guards::answer answer);
            void answer_waiting(bool gone);
            void project_changed();

            void undo();
            void redo();

            void build_fields(column &into);
            void build_parameters_header(column &into);
            void rebuild_parameters();
            // What is typed into the field is a step of the history that
            // ends when it loses the focus.
            void end_step_on_leaving(edit_field &field);
            // Every widget shows the model as it is: after undo and redo,
            // which change what the widgets did not.
            void show_model();
            void refresh();
            void refresh_parameter(std::size_t index);
            void refresh_checks();
            void add_check(library::severity level, std::string heading, std::string message,
                           std::function<void()> fix = {}, std::string fix_text = {});
            void show_status(std::string text, bool error);

            std::string title() const;
            text_field &field(part_field f) { return *m_fields[static_cast<std::size_t>(f)]; }

            window_host &m_host;
            app_services &m_services;
            part_editor_model m_model;
            std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);

            // See watch(). Ids, 0 for none.
            std::size_t m_window_guard = 0;
            std::size_t m_project_guard = 0;
            std::size_t m_project_connection = 0;
            // The Save / Discard / Keep Editing question is up, and who
            // waits for its answer.
            bool m_asking = false;
            std::vector<close_guards::answer> m_waiting;

            destination m_destination = destination::user_library;
            std::filesystem::path m_file; // for library_file
            // For project_library: the project the part was meant for, which
            // another one opened meanwhile is not. Compared, never followed.
            const library::project *m_project = nullptr;
            bool m_project_lost = false;
            std::string m_target_title;
            std::string m_where_text;
            // Why nothing can be saved where the part goes (no project, a
            // file that does not read); empty when it can.
            std::string m_target_problem;

            // Widgets, owned by the tree below this frame.
            label *m_title = nullptr;
            button *m_undo = nullptr;
            button *m_redo = nullptr;
            label *m_where = nullptr;
            std::array<text_field *, part_field_count> m_fields{};
            dropdown *m_device = nullptr;
            std::vector<device_choice> m_device_choices;
            button *m_device_parameters = nullptr;
            label *m_device_note = nullptr;
            dropdown *m_known = nullptr;
            std::vector<parameter_completion> m_known_parameters;
            column *m_parameter_list = nullptr;
            label *m_no_parameters = nullptr;
            std::vector<parameter_row> m_rows;
            label *m_checks_summary = nullptr;
            column *m_checks = nullptr;
            label *m_footer = nullptr;
            button *m_save = nullptr;
        };

        void part_editor_frame::later(std::function<void()> action)
        {
            if (owner() == nullptr)
                return;
            owner()->post([alive = m_alive, action = std::move(action)] {
                if (*alive && action)
                    action();
            });
        }

        void part_editor_frame::close()
        {
            if (owner() != nullptr)
                owner()->close_popup(*this);
        }

        void part_editor_frame::choose_destination(destination where)
        {
            m_destination = where;
            m_target_problem.clear();
            switch (where)
            {
            case destination::user_library: {
                const std::filesystem::path path = app_services::user_parts_library();
                m_target_title = "My Parts";
                if (path.empty())
                {
                    m_target_problem = "There is no folder for your own libraries on this system, so the part "
                                       "cannot go into My Parts.";
                    m_where_text = m_target_problem;
                    break;
                }
                // Read now to say whether it can be saved there; read again
                // when it is.
                auto target = load_target_file(path, true);
                if (!target)
                    m_target_problem = target.error();
                else
                    m_target_title = std::string(target->display_name());
                std::error_code error;
                m_where_text = std::format("Saved to {}, your own library: {}{}", m_target_title,
                                           library::path_to_utf8(path),
                                           std::filesystem::exists(path, error) ? "" : " (made when you save)");
                break;
            }
            case destination::project_library: {
                const library::project *p = m_services.project();
                m_project = p;
                m_project_lost = false;
                if (p == nullptr)
                {
                    m_target_title = "the project";
                    m_target_problem = "No project is open: open one, or save the part to My Parts.";
                    m_where_text = m_target_problem;
                    break;
                }
                m_target_title = p->name.empty() ? std::string("the project") : p->name;
                m_where_text = std::format("Saved to {}\xE2\x80\x99s library ({}), written with the project (Ctrl+S)",
                                           m_target_title, p->library_file);
                break;
            }
            case destination::library_file: {
                auto target = load_target_file(m_file, false);
                if (!target)
                {
                    m_target_title = library::path_to_utf8(m_file.filename());
                    m_target_problem = target.error();
                }
                else
                {
                    m_target_title = std::string(target->display_name());
                }
                m_where_text = std::format("Saved in place in {}: {}", m_target_title, library::path_to_utf8(m_file));
                break;
            }
            }
        }

        std::string part_editor_frame::title() const
        {
            const std::string name = part_display_name(m_model.part());
            switch (m_model.mode())
            {
            case editor_mode::create:
                return m_model.part().name.empty() ? std::string("New Part") : "New Part: " + name;
            case editor_mode::duplicate:
                return "Duplicate Part: " + name;
            case editor_mode::edit:
                return "Edit Part: " + name;
            }
            return "Part";
        }

        // ---- building ---------------------------------------------------------------------------------------------

        void part_editor_frame::build()
        {
            const ui::theme t = m_services.theme();

            // ---- header: what, and where it goes
            auto &header = emplace<row>(10.0f);
            header.set_cross_align(align::center);
            m_title = &header.emplace<label>("");
            m_title->set_name("part editor title");
            m_title->set_size(text_size::title);
            m_title->layout().width = 0.0f;
            m_title->layout().flex = 1.0f;
            // The dialog's toolbar: the window's menus and shortcuts are not
            // reached behind it. The step each would take is in its tooltip
            // (refresh()).
            m_undo = &header.emplace<button>("", icon::undo, button_kind::flat);
            m_undo->set_name("part editor undo");
            m_undo->clicked.connect([this] { undo(); });
            m_redo = &header.emplace<button>("", icon::redo, button_kind::flat);
            m_redo->set_name("part editor redo");
            m_redo->clicked.connect([this] { redo(); });
            if (m_model.mode() != editor_mode::edit && m_services.project() != nullptr)
            {
                header.emplace<label>("Save to", text_role::dim);
                auto &to = header.emplace<dropdown>(std::vector<std::string>{"My Parts", "Project library"},
                                                    m_destination == destination::project_library ? 1 : 0);
                to.set_name("part editor destination");
                to.set_tooltip("Where the part goes: your own library, which every project can use, or the open "
                               "project's library");
                to.layout().width = 190.0f;
                to.changed.connect([this](int index) {
                    later([this, index] {
                        choose_destination(index == 1 ? destination::project_library : destination::user_library);
                        refresh();
                    });
                });
            }
            m_where = &emplace<label>("", text_role::dim);
            m_where->set_size(text_size::small);

            // ---- body: the part at the left, the checks at the right
            auto &body = emplace<row>(12.0f);
            body.layout().flex = 1.0f;

            auto &scroll = body.emplace<scroll_area>(true, false);
            scroll.set_name("part editor scroll");
            scroll.layout().width = 0.0f;
            scroll.layout().flex = 1.0f;
            auto &content = scroll.set_content(std::make_unique<column>(t.spacing * 1.5f, insets{0.0f, 0.0f, 12.0f, 4.0f}));
            build_fields(content);
            content.emplace<separator>();
            build_parameters_header(content);
            m_no_parameters = &content.emplace<label>("No parameters yet. Add the values a datasheet gives "
                                                      "(resistance, voltage rating\xE2\x80\xA6), or choose a device "
                                                      "and add the parameters it needs.",
                                                      text_role::dim);
            m_no_parameters->set_wrap(true, 3);
            m_parameter_list = &content.emplace<column>(6.0f);
            m_parameter_list->set_name("part editor parameters");

            auto checks = std::make_unique<panel>("CHECKS", surface::panel, 6.0f, insets::uniform(8.0f));
            checks->set_border(true);
            checks->layout().width = std::min(checks_width, std::max(200.0f, m_host.screen().size().x() * 0.3f));
            m_checks_summary = &checks->emplace<label>("", text_role::dim);
            m_checks_summary->set_name("part editor checks summary");
            m_checks_summary->set_wrap(true, 3);
            auto &checks_scroll = checks->emplace<scroll_area>(true, false);
            checks_scroll.layout().flex = 1.0f;
            m_checks = &checks_scroll.set_content(std::make_unique<column>(10.0f, insets{0.0f, 2.0f, 10.0f, 2.0f}));
            m_checks->set_name("part editor checks");
            body.add(std::move(checks));

            // ---- footer
            emplace<separator>();
            auto &footer = emplace<row>(8.0f);
            footer.set_cross_align(align::center);
            m_footer = &footer.emplace<label>("", text_role::dim);
            m_footer->set_name("part editor footer");
            m_footer->layout().width = 0.0f;
            m_footer->layout().flex = 1.0f;
            auto &cancel = footer.emplace<button>("Cancel");
            cancel.set_name("part editor cancel");
            cancel.set_tooltip("Close without saving (Escape); asks first when something changed");
            cancel.clicked.connect([this] { later([this] { request_close(); }); });
            m_save = &footer.emplace<button>("Save", icon::save, button_kind::primary);
            m_save->set_name("part editor save");
            m_save->clicked.connect([this] { later([this] { save(); }); });

            rebuild_parameters();
            refresh();
        }

        void part_editor_frame::build_fields(column &into)
        {
            auto &grid = into.emplace<property_grid>();
            grid.set_name("part editor fields");
            grid.set_name_share(0.24f);

            const auto add_field = [&](part_field f, std::string tooltip = {}) -> text_field & {
                auto edit = std::make_unique<edit_field>(m_model.field(f), std::string(field_placeholder(f)));
                edit->set_name(std::string("part ") + std::string(field_title(f)));
                if (!tooltip.empty())
                    edit->set_tooltip(std::move(tooltip));
                edit_field &made = *edit;
                m_fields[static_cast<std::size_t>(f)] = &made;
                made.changed.connect([this, f](const std::string &text) {
                    m_model.set_field(f, text);
                    refresh();
                });
                end_step_on_leaving(made);
                grid.add_row(std::string(field_title(f)), std::move(edit));
                return made;
            };

            grid.add_section("Part");
            add_field(part_field::name, "What the part is called in lists: \xE2\x80\x9C"
                                        "10k\xE2\x84\xA6 1% 0603\xE2\x80\x9D");

            // The id: the part's identity, shown (and copyable) but not
            // typed. A new part or a duplicate is given one when saved.
            {
                auto edit = std::make_unique<text_field>(m_model.field(part_field::id),
                                                         std::string(field_placeholder(part_field::id)));
                edit->set_name("part ID");
                edit->set_read_only(true);
                edit->set_tooltip("The part's identity in its library, given by Volt when the part is first saved. "
                                  "It never changes, so that projects that use the part keep finding it whatever "
                                  "else about it is corrected.");
                m_fields[static_cast<std::size_t>(part_field::id)] = edit.get();
                grid.add_row(std::string(field_title(part_field::id)), std::move(edit));
            }
            add_field(part_field::category, "A path of categories from the most general, separated by \xE2\x80\x9C/\xE2\x80\x9D");
            add_field(part_field::description);
            add_field(part_field::manufacturer);
            add_field(part_field::mpn, "The manufacturer part number");
            add_field(part_field::value, "The value printed beside the symbol: \xE2\x80\x9C"
                                         "10k\xE2\x80\x9D, \xE2\x80\x9C"
                                         "100n\xE2\x80\x9D");
            add_field(part_field::package);
            add_field(part_field::prefix, "The reference designator's letters: R, C, U");

            grid.add_section("Simulation");
            {
                auto line = std::make_unique<column>(4.0f);
                auto &choice = line->emplace<row>(6.0f);
                choice.set_cross_align(align::center);
                m_device_choices = m_model.device_choices();
                std::vector<std::string> names;
                for (const device_choice &c : m_device_choices)
                    names.push_back(c.name);
                m_device = &choice.emplace<dropdown>(std::move(names), static_cast<int>(m_model.device_index()));
                m_device->set_name("part Device");
                m_device->set_tooltip("What the part is to a simulation, which decides the parameters it reads");
                m_device->layout().width = 0.0f;
                m_device->layout().flex = 1.0f;
                m_device->changed.connect([this](int index) {
                    if (index < 0 || static_cast<std::size_t>(index) >= m_device_choices.size())
                        return;
                    m_model.set_field(part_field::device, m_device_choices[static_cast<std::size_t>(index)].id);
                    refresh();
                });
                m_device_parameters = &choice.emplace<button>("Add Its Parameters", icon::plus);
                m_device_parameters->set_name("part editor device parameters");
                m_device_parameters->clicked.connect([this] {
                    later([this] {
                        const bool required = !m_model.missing_parameters(true).empty();
                        const std::size_t before = m_model.parameter_count();
                        if (m_model.add_device_parameters(required) == 0)
                            return;
                        rebuild_parameters();
                        refresh();
                        if (before < m_rows.size())
                            m_rows[before].value->focus(focus_reason::keyboard);
                    });
                });
                m_device_note = &line->emplace<label>("", text_role::dim);
                m_device_note->set_size(text_size::small);
                m_device_note->set_wrap(true, 3);
                grid.add_row("Device", std::move(line));
            }

            grid.add_section("Drawing and documents");
            add_field(part_field::symbol, "The schematic symbol's name");
            add_field(part_field::footprint, "\xE2\x80\x9C<footprint library>:<footprint>\xE2\x80\x9D, as KiCad names them");
            {
                auto line = std::make_unique<row>(6.0f);
                line->set_cross_align(align::center);
                auto &edit = line->emplace<edit_field>(m_model.field(part_field::model),
                                                       std::string(field_placeholder(part_field::model)));
                edit.set_name("part 3D model");
                edit.set_tooltip("A file in the models folder (\xE2\x80\x9Cparts/R_0603.step\xE2\x80\x9D), the name "
                                 "of a file the library carries, or the full path of a file to put into the "
                                 "library. Dropping a .step, .stp, .obj or .stl file on the window does the last.");
                edit.layout().width = 0.0f;
                edit.layout().flex = 1.0f;
                m_fields[static_cast<std::size_t>(part_field::model)] = &edit;
                edit.changed.connect([this](const std::string &text) {
                    m_model.set_field(part_field::model, text);
                    refresh();
                });
                end_step_on_leaving(edit);

                // FILE DIALOG: this is where Browse… asks the platform's
                // native open dialog (app_services::choose_file) for a
                // .step, .stp, .obj or .stl file, and hands the path it
                // returns to attach_model(path).
                auto &browse = line->emplace<button>("Browse\xE2\x80\xA6", icon::open);
                browse.set_name("part editor browse model");
                browse.set_tooltip("Choose a 3D model file (.step, .stp, .obj, .stl) to put into the library with "
                                   "the part");
                // The answer comes later, and only if a file was chosen;
                // the editor may be gone by then.
                browse.clicked.connect([this] {
                    m_services.choose_file(m_host.kind(), file_purpose::model,
                                           [this, alive = m_alive](const std::filesystem::path &file) {
                                               if (*alive)
                                                   attach_model(file);
                                           });
                });
                grid.add_row(std::string(field_title(part_field::model)), std::move(line));
            }
            add_field(part_field::datasheet, "The datasheet's web address");
            add_field(part_field::keywords, "Words a search should find the part by, separated by commas");
        }

        void part_editor_frame::build_parameters_header(column &into)
        {
            auto &header = into.emplace<row>(6.0f);
            header.set_cross_align(align::center);
            auto &title = header.emplace<label>("PARAMETERS", text_role::dim);
            title.set_size(text_size::small);
            title.layout().width = 0.0f;
            title.layout().flex = 1.0f;

            m_known_parameters = m_model.known_parameters();
            std::vector<std::string> names{"Add a known parameter\xE2\x80\xA6"};
            for (const parameter_completion &c : m_known_parameters)
                names.push_back(completion_text(c));
            m_known = &header.emplace<dropdown>(std::move(names), 0);
            m_known->set_name("part editor known parameters");
            m_known->set_tooltip("Add a parameter Volt knows, with its unit");
            m_known->layout().width = 230.0f;
            m_known->changed.connect([this](int index) {
                if (index <= 0 || static_cast<std::size_t>(index) > m_known_parameters.size())
                    return;
                const std::string name = m_known_parameters[static_cast<std::size_t>(index) - 1].name;
                later([this, name] {
                    if (m_known != nullptr)
                        m_known->set_selected(0);
                    const std::size_t at = m_model.add_known_parameter(name);
                    rebuild_parameters();
                    refresh();
                    if (at < m_rows.size())
                        m_rows[at].value->focus(focus_reason::keyboard);
                });
            });

            auto &add = header.emplace<button>("Add", icon::plus);
            add.set_name("part editor add parameter");
            add.set_tooltip("Add an empty parameter row");
            add.clicked.connect([this] {
                later([this] {
                    const std::size_t at = m_model.add_parameter();
                    rebuild_parameters();
                    refresh();
                    if (at < m_rows.size())
                        m_rows[at].name->focus(focus_reason::keyboard);
                });
            });
        }

        void part_editor_frame::rebuild_parameters()
        {
            if (m_parameter_list == nullptr)
                return;
            m_parameter_list->clear_children();
            m_rows.clear();

            const std::size_t count = m_model.parameter_count();
            for (std::size_t i = 0; i < count; ++i)
            {
                const library::parameter &p = m_model.parameter(i);
                parameter_row r;
                auto &entry = m_parameter_list->emplace<column>(3.0f);

                auto &fields = entry.emplace<row>(4.0f);
                fields.set_cross_align(align::center);
                auto &name = fields.emplace<edit_field>(p.name, "Name");
                r.name = &name;
                r.name->set_name(std::format("parameter {} name", i + 1));
                r.name->layout().width = 0.0f;
                r.name->layout().flex = 2.0f;
                r.name->changed.connect([this, i](const std::string &text) {
                    m_model.set_parameter_name(i, text);
                    refresh();
                });
                end_step_on_leaving(name);
                auto &value = fields.emplace<edit_field>(p.value, "Value");
                r.value = &value;
                r.value->set_name(std::format("parameter {} value", i + 1));
                r.value->set_tooltip("In engineering notation for a quantity: 4.7k, 4k7, 100n, 1e-7");
                r.value->layout().width = 0.0f;
                r.value->layout().flex = 1.4f;
                r.value->changed.connect([this, i](const std::string &text) {
                    m_model.set_parameter_value(i, text);
                    refresh();
                });
                end_step_on_leaving(value);
                auto &unit = fields.emplace<edit_field>(p.unit, "Unit");
                r.unit = &unit;
                r.unit->set_name(std::format("parameter {} unit", i + 1));
                r.unit->layout().width = 70.0f;
                r.unit->changed.connect([this, i](const std::string &text) {
                    m_model.set_parameter_unit(i, text);
                    refresh();
                });
                end_step_on_leaving(unit);

                auto &up = fields.emplace<button>("", icon::chevron_up, button_kind::flat);
                up.set_name(std::format("parameter {} up", i + 1));
                up.set_tooltip("Move up");
                up.set_enabled(i > 0);
                up.clicked.connect([this, i] {
                    later([this, i] {
                        if (i == 0 || !m_model.move_parameter(i, i - 1))
                            return;
                        rebuild_parameters();
                        refresh();
                    });
                });
                auto &down = fields.emplace<button>("", icon::chevron_down, button_kind::flat);
                down.set_name(std::format("parameter {} down", i + 1));
                down.set_tooltip("Move down");
                down.set_enabled(i + 1 < count);
                down.clicked.connect([this, i] {
                    later([this, i] {
                        if (!m_model.move_parameter(i, i + 1))
                            return;
                        rebuild_parameters();
                        refresh();
                    });
                });
                auto &remove = fields.emplace<button>("", icon::close, button_kind::flat);
                remove.set_name(std::format("parameter {} remove", i + 1));
                remove.set_tooltip("Remove this parameter");
                remove.clicked.connect([this, i] {
                    later([this, i] {
                        m_model.remove_parameter(i);
                        rebuild_parameters();
                        refresh();
                    });
                });

                // Under the row: what the schema says of it, and the names
                // it probably means, a click away.
                auto &notes = entry.emplace<column>(3.0f, insets{4.0f, 0.0f, 0.0f, 2.0f});
                r.notes = &notes;
                r.note = &notes.emplace<label>("", text_role::dim);
                r.note->set_size(text_size::small);
                r.note->set_wrap(true, 3);
                r.hints = &notes.emplace<row>(4.0f);
                m_rows.push_back(r);
            }
        }

        void part_editor_frame::end_step_on_leaving(edit_field &field)
        {
            // The frame may be going while its fields are (the focus goes
            // with them).
            field.focus_lost.connect([this, alive = m_alive] {
                if (*alive)
                    m_model.seal();
            });
        }

        // ---- undo and redo ----------------------------------------------------------------------------------------

        void part_editor_frame::undo()
        {
            if (m_model.undo())
                show_model();
        }

        void part_editor_frame::redo()
        {
            if (m_model.redo())
                show_model();
        }

        // ---- keeping up with the model ------------------------------------------------------------------------

        void part_editor_frame::show_model()
        {
            if (m_title == nullptr)
                return;
            for (std::size_t i = 0; i < part_field_count; ++i)
            {
                if (m_fields[i] != nullptr)
                    show_text(*m_fields[i], m_model.field(static_cast<part_field>(i)));
            }

            // The rows: their text where as many are left, else made again,
            // the focus put back in the same field of the same row (or the
            // last), if a row had it.
            if (m_rows.size() == m_model.parameter_count())
            {
                for (std::size_t i = 0; i < m_rows.size(); ++i)
                {
                    const library::parameter &p = m_model.parameter(i);
                    show_text(*m_rows[i].name, p.name);
                    show_text(*m_rows[i].value, p.value);
                    show_text(*m_rows[i].unit, p.unit);
                }
            }
            else
            {
                // 0 name, 1 value, 2 unit.
                const auto cell = [](const parameter_row &r, int column) -> text_field & {
                    return column == 0 ? *r.name : column == 1 ? *r.value : *r.unit;
                };
                std::optional<std::size_t> focused_row;
                int focused_column = 0;
                for (std::size_t i = 0; i < m_rows.size(); ++i)
                {
                    for (int column = 0; column < 3; ++column)
                    {
                        if (cell(m_rows[i], column).has_focus())
                        {
                            focused_row = i;
                            focused_column = column;
                        }
                    }
                }
                rebuild_parameters();
                if (focused_row && !m_rows.empty())
                    cell(m_rows[std::min(*focused_row, m_rows.size() - 1)], focused_column)
                        .focus(focus_reason::programmatic);
            }
            refresh();
        }

        void part_editor_frame::refresh()
        {
            if (m_title == nullptr)
                return;
            m_title->set_text(title() + (m_model.modified() ? "  \xE2\x80\xA2" : ""));
            m_where->set_text(m_where_text);

            // Undo and redo, and what they would do.
            m_undo->set_enabled(m_model.can_undo());
            m_undo->set_tooltip(m_model.can_undo() ? std::format("Undo {} (Ctrl+Z)", m_model.undo_label())
                                                   : std::string("Nothing to undo"));
            m_redo->set_enabled(m_model.can_redo());
            m_redo->set_tooltip(m_model.can_redo() ? std::format("Redo {} (Ctrl+Y)", m_model.redo_label())
                                                   : std::string("Nothing to redo"));

            // The device: its choice, and what it still needs.
            const std::vector<device_choice> choices = m_model.device_choices();
            if (choices != m_device_choices)
            {
                m_device_choices = choices;
                std::vector<std::string> names;
                for (const device_choice &c : choices)
                    names.push_back(c.name);
                m_device->set_items(std::move(names));
            }
            m_device->set_selected(static_cast<int>(m_model.device_index()));
            const auto required = m_model.missing_parameters(true);
            const auto all = m_model.missing_parameters(false);
            const auto names_of = [](const std::vector<library::device_parameter> &list, bool required_ones) {
                std::string text;
                for (const library::device_parameter &q : list)
                {
                    if (q.required != required_ones)
                        continue;
                    if (!text.empty())
                        text += ", ";
                    text += q.name;
                }
                return text;
            };
            std::string device_note;
            if (!required.empty())
                device_note = std::format("Needs {}.", names_of(required, true));
            if (const std::string optional = names_of(all, false); !optional.empty())
                device_note += std::format("{}May also have {}.", device_note.empty() ? "" : " ", optional);
            m_device_note->set_text(std::move(device_note));
            m_device_note->set_visible(!m_device_note->text().empty());
            m_device_parameters->set_enabled(!all.empty());
            m_device_parameters->set_text(!required.empty() ? "Add Required" : "Add Its Parameters");
            m_device_parameters->set_tooltip(!required.empty()
                                                 ? "Add the parameters this device needs, with their units"
                                             : all.empty() ? "The part has every parameter its device has"
                                                           : "Add the parameters this device may have, with their "
                                                             "units");

            // The parameters' notes.
            m_no_parameters->set_visible(m_model.parameter_count() == 0);
            for (std::size_t i = 0; i < m_rows.size(); ++i)
                refresh_parameter(i);

            refresh_checks();

            // Save, and why not.
            const std::vector<std::string> problems = m_model.save_problems();
            const bool can_save = problems.empty() && m_target_problem.empty();
            m_save->set_enabled(can_save);
            m_save->set_text(m_model.mode() == editor_mode::edit ? "Save" : "Save to " + m_target_title);
            std::string footer;
            bool error = false;
            if (!m_target_problem.empty())
            {
                footer = m_target_problem;
                error = true;
            }
            else if (!problems.empty())
            {
                footer = "Cannot save: " + problems.front();
                error = true;
            }
            else if (!m_model.modified() && m_model.mode() == editor_mode::edit)
                footer = "No changes.";
            else if (m_destination == destination::project_library)
                footer = "Saves to the project\xE2\x80\x99s library; Save Project (Ctrl+S) writes it.";
            else
                footer = "Saving writes the library file and scans the libraries again.";
            m_save->set_tooltip(can_save ? std::string("Save the part and close the editor") : footer);
            show_status(std::move(footer), error);
        }

        void part_editor_frame::refresh_parameter(std::size_t index)
        {
            parameter_row &r = m_rows[index];
            const ui::theme t = m_services.theme();
            const std::vector<library::issue> issues = m_model.parameter_issues(index);
            const std::vector<parameter_completion> hints = m_model.parameter_hints(index);
            const library::parameter &p = m_model.parameter(index);

            std::string note;
            std::optional<math::vec4f> color;
            if (!issues.empty())
            {
                const library::issue *worst = &issues.front();
                for (const library::issue &i : issues)
                    if (i.level > worst->level)
                        worst = &i;
                note = std::format("{}: {}", severity_word(worst->level), worst->message);
                color = severity_color(t, worst->level);
            }
            else if (!hints.empty())
            {
                note = hints.size() == 1 && !hints.front().alias.empty()
                           ? std::format("Volt calls \xE2\x80\x9C{}\xE2\x80\x9D {}:", hints.front().alias,
                                         hints.front().name)
                           : std::string("Known parameters:");
            }
            else if (p.name.empty() && (!p.value.empty() || !p.unit.empty()))
            {
                note = "Give this parameter a name.";
                color = t.error;
            }
            r.note->set_text(std::move(note));
            r.note->set_color(color);
            r.note->set_visible(!r.note->text().empty());

            r.hints->clear_children();
            for (const parameter_completion &c : hints)
            {
                auto &use = r.hints->emplace<button>(completion_text(c), icon::none, button_kind::flat);
                use.set_name(std::format("parameter {} use {}", index + 1, c.name));
                use.set_tooltip(c.alias.empty() ? std::format("Name it \xE2\x80\x9C{}\xE2\x80\x9D", c.name)
                                                : std::format("\xE2\x80\x9C{}\xE2\x80\x9D is Volt's name for \xE2\x80\x9C{}\xE2\x80\x9D",
                                                              c.name, c.alias));
                use.clicked.connect([this, index, c] {
                    later([this, index, c] {
                        if (index >= m_rows.size())
                            return;
                        m_model.apply_completion(index, c);
                        m_rows[index].name->set_text(m_model.parameter(index).name);
                        m_rows[index].unit->set_text(m_model.parameter(index).unit);
                        refresh();
                        m_rows[index].value->focus(focus_reason::keyboard);
                    });
                });
            }
            r.hints->set_visible(!hints.empty());
            r.notes->set_visible(r.note->visible() || r.hints->visible());
        }

        void part_editor_frame::add_check(library::severity level, std::string heading, std::string message,
                                          std::function<void()> fix, std::string fix_text)
        {
            const ui::theme t = m_services.theme();
            auto &entry = m_checks->emplace<column>(2.0f);
            auto &head = entry.emplace<label>(std::move(heading));
            head.set_size(text_size::small);
            head.set_color(severity_color(t, level));
            auto &text = entry.emplace<label>(std::move(message));
            text.set_wrap(true, 8);
            if (fix)
            {
                auto &b = entry.emplace<button>(std::move(fix_text), icon::none, button_kind::flat);
                b.clicked.connect([this, fix = std::move(fix)] { later(fix); });
            }
        }

        void part_editor_frame::refresh_checks()
        {
            m_checks->clear_children();

            // What stops a save first: those are what has to change.
            const std::vector<std::string> problems = m_model.save_problems();
            if (!m_target_problem.empty())
                add_check(library::severity::error, "CANNOT SAVE", m_target_problem);
            for (const std::string &problem : problems)
                add_check(library::severity::error, "CANNOT SAVE", problem);

            const std::vector<library::issue> &issues = m_model.issues();
            std::size_t counts[3] = {0, 0, 0};
            for (const library::issue &i : issues)
            {
                ++counts[static_cast<std::size_t>(i.level)];
                std::string heading(severity_word(i.level));
                if (!i.parameter.empty())
                    heading += "  \xC2\xB7  " + i.parameter;

                std::function<void()> fix;
                std::string fix_text;
                if (i.code == library::issue_code::missing_parameter)
                {
                    fix_text = std::format("Add \xE2\x80\x9C{}\xE2\x80\x9D", i.parameter);
                    fix = [this, name = i.parameter] {
                        const std::size_t at = m_model.add_known_parameter(name);
                        rebuild_parameters();
                        refresh();
                        if (at < m_rows.size())
                            m_rows[at].value->focus(focus_reason::keyboard);
                    };
                }
                else if (i.code == library::issue_code::misspelled_parameter && !i.suggestion.empty())
                {
                    fix_text = std::format("Rename to \xE2\x80\x9C{}\xE2\x80\x9D", i.suggestion);
                    fix = [this, from = i.parameter, to = i.suggestion] {
                        for (std::size_t at = 0; at < m_model.parameter_count() && at < m_rows.size(); ++at)
                        {
                            if (m_model.parameter(at).name != from)
                                continue;
                            const library::parameter_definition *d = library::schema::builtin().find(to);
                            m_model.apply_completion(at, {to, d != nullptr ? d->unit : std::string(), {}});
                            m_rows[at].name->set_text(m_model.parameter(at).name);
                            m_rows[at].unit->set_text(m_model.parameter(at).unit);
                            break;
                        }
                        refresh();
                    };
                }
                else if (i.code == library::issue_code::unknown_device && !i.suggestion.empty())
                {
                    fix_text = std::format("Use \xE2\x80\x9C{}\xE2\x80\x9D", i.suggestion);
                    fix = [this, to = i.suggestion] {
                        const library::device_definition *d = library::schema::builtin().find_device(to);
                        m_model.set_field(part_field::device, d != nullptr ? d->id : to);
                        refresh();
                    };
                }
                add_check(i.level, std::move(heading), i.message, std::move(fix), std::move(fix_text));
            }

            const std::size_t errors = counts[static_cast<std::size_t>(library::severity::error)];
            const std::size_t warnings = counts[static_cast<std::size_t>(library::severity::warning)];
            const std::size_t notes = counts[static_cast<std::size_t>(library::severity::note)];
            std::string summary;
            if (issues.empty())
                summary = "Nothing a simulation would stumble on.";
            else
                summary = std::format("{} error{}, {} warning{}, {} note{}. Errors do not stop a save; they stop a "
                                      "simulation of the part.",
                                      errors, errors == 1 ? "" : "s", warnings, warnings == 1 ? "" : "s", notes,
                                      notes == 1 ? "" : "s");
            m_checks_summary->set_text(std::move(summary));
            const ui::theme t = m_services.theme();
            m_checks_summary->set_color(errors > 0     ? std::optional<math::vec4f>(t.error)
                                        : warnings > 0 ? std::optional<math::vec4f>(t.warning)
                                                       : std::nullopt);
        }

        void part_editor_frame::show_status(std::string text, bool error)
        {
            if (m_footer == nullptr)
                return;
            m_footer->set_tooltip(text);
            m_footer->set_text(std::move(text));
            if (error)
                m_footer->set_color(m_services.theme().error);
            else
                m_footer->set_color(std::nullopt);
        }

        void part_editor_frame::focus_first()
        {
            text_field &first = field(part_field::name);
            first.focus(focus_reason::keyboard);
            first.select_all();
        }

        // ---- the model file ---------------------------------------------------------------------------------------

        void part_editor_frame::attach_model(const std::filesystem::path &file)
        {
            if (auto attached = m_model.attach_model(file); !attached)
            {
                refresh();
                show_status("Not used as the 3D model: " + attached.error(), true);
                return;
            }
            field(part_field::model).set_text(m_model.field(part_field::model));
            refresh();
            show_status(std::format("{} goes into the library with the part when it is saved.",
                                    m_model.field(part_field::model)),
                        false);
        }

        // ---- saving and closing -----------------------------------------------------------------------------------

        bool part_editor_frame::save()
        {
            refresh();
            if (!m_target_problem.empty())
            {
                show_status(m_target_problem, true);
                return false;
            }
            if (const std::vector<std::string> problems = m_model.save_problems(); !problems.empty())
            {
                show_status("Cannot save: " + problems.front(), true);
                return false;
            }

            // Committed to a copy first: if writing the file fails, the
            // editor is as it was, and Save can be tried again.
            part_editor_model attempt = m_model;
            const std::string name = part_display_name(m_model.part());
            app_services &services = m_services;
            window_host &host = m_host;

            if (m_destination == destination::project_library)
            {
                library::project *p = services.project();
                if (p == nullptr || p != m_project)
                {
                    show_status("Not saved: the project has been closed. Save the part to My Parts instead.", true);
                    return false;
                }
                auto id = attempt.commit(p->parts);
                if (!id)
                {
                    show_status("Not saved: " + id.error(), true);
                    return false;
                }
                m_model = std::move(attempt);
                close();
                services.mark_project_modified();
                services.reveal_in_library(*id, true);
                host.show_message(std::format("Saved {} to the project\xE2\x80\x99s library as {} (not written yet: "
                                              "Ctrl+S)",
                                              name, *id),
                                  std::chrono::seconds(8));
                return true;
            }

            const std::filesystem::path path =
                m_destination == destination::user_library ? app_services::user_parts_library() : m_file;
            if (path.empty())
            {
                show_status("Not saved: there is no folder for your own libraries on this system.", true);
                return false;
            }
            auto target = load_target_file(path, m_destination == destination::user_library);
            if (!target)
            {
                show_status("Not saved: " + target.error(), true);
                return false;
            }
            auto id = attempt.commit(*target);
            if (!id)
            {
                show_status("Not saved: " + id.error(), true);
                return false;
            }
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            if (auto saved = library::save_library(path, *target); !saved)
            {
                show_status("Not saved: " + saved.error(), true);
                host.show_message("Part not saved: " + saved.error(), std::chrono::seconds(10), log_level::error);
                return false;
            }
            m_model = std::move(attempt);
            const std::string qualified = target->name + ":" + *id;
            const std::string library_name(target->display_name());
            close();
            services.rescan_catalog();
            services.reveal_in_library(qualified);
            host.show_message(std::format("Saved {} to {} as {}; scanning the libraries again\xE2\x80\xA6", name,
                                          library_name, *id),
                              std::chrono::seconds(8));
            return true;
        }

        void part_editor_frame::request_close()
        {
            confirm_leave({});
        }

        void part_editor_frame::answer_waiting(bool gone)
        {
            m_asking = false;
            // Taken first: an answer may close the window, the project or
            // the application, and this editor may be gone after.
            std::vector<close_guards::answer> waiting = std::exchange(m_waiting, {});
            for (close_guards::answer &answer : waiting)
                answer(gone);
        }

        void part_editor_frame::confirm_leave(close_guards::answer answer)
        {
            if (!m_model.modified())
            {
                close();
                if (answer)
                    answer(true);
                return;
            }
            if (answer)
                m_waiting.push_back(std::move(answer));
            // Raised, so that the question is seen: it may be asked for
            // from another window (Exit), or by the project closing.
            m_services.open_window(m_host.kind());
            if (m_asking)
                return; // asked already: its answer goes to every one waiting
            m_asking = true;

            const std::string name = part_display_name(m_model.part());
            const std::vector<std::string> problems = m_model.save_problems();
            const bool can_save = problems.empty() && m_target_problem.empty();

            std::vector<dialog_choice> choices;
            std::string text;
            const auto guarded = [alive = m_alive](std::function<void()> action) {
                return [alive, action = std::move(action)] {
                    if (*alive)
                        action();
                };
            };
            if (can_save)
            {
                text = std::format("The changes are lost unless you save them. Saving writes them to {}.",
                                   m_target_title);
                // Saving may fail (a file that cannot be written): then the
                // editor stays, saying why, and nothing waiting goes on.
                choices.push_back({"Save", button_kind::primary, guarded([this] { answer_waiting(save()); })});
            }
            else
            {
                text = "It cannot be saved as it is: " +
                       (m_target_problem.empty() ? problems.front() : m_target_problem) +
                       " Discarding loses the changes.";
            }
            choices.push_back({"Discard Changes", button_kind::normal, guarded([this] {
                                   close();
                                   answer_waiting(true);
                               })});
            choices.push_back({"Keep Editing", can_save ? button_kind::normal : button_kind::primary,
                               guarded([this] { answer_waiting(false); })});
            open_choice_dialog(m_host,
                               m_model.mode() == editor_mode::edit
                                   ? std::format("Save the changes to {}?", name)
                                   : std::format("Save {}?", name),
                               std::move(text), std::move(choices), guarded([this] { answer_waiting(false); }));
        }

        void part_editor_frame::watch()
        {
            // Its window closing, or the application exiting: an editor
            // with nothing to lose lets them go (and goes with the window).
            m_window_guard = m_host.guards().add([this](close_guards::answer answer) {
                if (!m_model.modified())
                {
                    answer(true);
                    return;
                }
                confirm_leave(std::move(answer));
            });

            // The project closing, or another opened in its place: what was
            // to go into its library is saved there first or let go, and the
            // editor closes either way, its library gone.
            m_project_guard = m_services.project_guards().add([this](close_guards::answer answer) {
                if (m_destination != destination::project_library || m_services.project() == nullptr ||
                    m_services.project() != m_project)
                {
                    answer(true);
                    return;
                }
                confirm_leave(std::move(answer));
            });
            m_project_connection = m_services.project_changed.connect([this] { project_changed(); });
        }

        void part_editor_frame::unwatch()
        {
            m_host.guards().remove(std::exchange(m_window_guard, 0));
            m_services.project_guards().remove(std::exchange(m_project_guard, 0));
            if (m_project_connection != 0)
                m_services.project_changed.disconnect(std::exchange(m_project_connection, 0));
        }

        void part_editor_frame::project_changed()
        {
            if (m_destination != destination::project_library || m_title == nullptr)
                return;

            // Closed, or another opened in its place, past the project guard
            // (which closes the editor first): the part is not to go into
            // whatever project is open now, and from then on never will
            // (another may come at the same address).
            if (m_project_lost || (m_project != nullptr && m_services.project() != m_project))
            {
                m_project = nullptr;
                m_project_lost = true;
                m_target_problem = m_model.mode() == editor_mode::edit
                                       ? "The project this part belongs to has been closed: it cannot be saved."
                                       : "The project the part was to go into has been closed: save it to My Parts "
                                         "instead.";
                m_where_text = m_target_problem;
                refresh();
                return;
            }

            // Saved elsewhere (another file name), or renamed: what the
            // editor says of it, as it is now.
            choose_destination(destination::project_library);
            refresh();
        }

        // ---- opening ------------------------------------------------------------------------------------------------

        part_editor_frame *find_editor(window_host &host)
        {
            ui::screen &s = host.screen();
            for (std::size_t i = 0; i < s.popup_count(); ++i)
                if (auto *frame = dynamic_cast<part_editor_frame *>(s.popup(i)); frame != nullptr)
                    return frame;
            return nullptr;
        }

        void show_editor(window_host &host, std::unique_ptr<part_editor_frame> frame)
        {
            ui::screen &s = host.screen();
            const math::vec2f size = s.size();
            frame->layout().width = std::max(420.0f, std::min(editor_width, size.x() - 32.0f));
            frame->layout().height = std::max(360.0f, size.y() - 48.0f);
            frame->build();

            part_editor_frame *editor = frame.get();
            popup_options options;
            options.placement = placement::centered;
            options.modal = true;
            options.dismissable = false;
            // However it closes (Save, Discard, its window going): it
            // guards nothing from then on.
            options.closed = [editor] { editor->unwatch(); };
            part_editor_frame &shown = s.open(std::move(frame), std::move(options));
            shown.watch();
            shown.focus_first();
        }
    } // namespace

    void open_new_part(window_host &host, bool project)
    {
        if (find_editor(host) != nullptr)
            return;
        auto frame = std::make_unique<part_editor_frame>(host);
        frame->model().start_new();
        frame->choose_destination(project && host.services().project() != nullptr ? destination::project_library
                                                                                   : destination::user_library);
        show_editor(host, std::move(frame));
    }

    void open_edit_part(window_host &host, const library::library &lib, const library::part &part)
    {
        if (find_editor(host) != nullptr)
            return;
        auto frame = std::make_unique<part_editor_frame>(host);
        frame->model().start_edit(part);
        frame->set_file(lib.path);
        frame->choose_destination(destination::library_file);
        show_editor(host, std::move(frame));
    }

    void open_duplicate_part(window_host &host, const library::library &lib, const library::part &part)
    {
        if (find_editor(host) != nullptr)
            return;
        auto frame = std::make_unique<part_editor_frame>(host);
        frame->model().start_duplicate(part, lib);
        frame->choose_destination(destination::user_library);
        show_editor(host, std::move(frame));
    }

    void open_edit_project_part(window_host &host, std::string_view id)
    {
        if (find_editor(host) != nullptr)
            return;
        const library::project *p = host.services().project();
        const library::part *part = p != nullptr ? p->parts.find(id) : nullptr;
        if (part == nullptr)
        {
            host.show_message(std::format("The project has no part {}", id), std::chrono::seconds(6),
                              log_level::warning);
            return;
        }
        auto frame = std::make_unique<part_editor_frame>(host);
        frame->model().start_edit(*part);
        frame->choose_destination(destination::project_library);
        show_editor(host, std::move(frame));
    }

    bool part_editor_open(window_host &host)
    {
        return find_editor(host) != nullptr;
    }

    bool part_editor_drop(window_host &host, std::span<const std::string> paths)
    {
        part_editor_frame *editor = find_editor(host);
        if (editor == nullptr)
            return false;
        for (const std::string &text : paths)
        {
            const std::filesystem::path path = library::path_from_utf8(text);
            if (is_model_file(path))
            {
                editor->attach_model(path);
                return true;
            }
        }
        host.show_message("Only a 3D model (.step, .stp, .obj, .stl) can be dropped on the part editor",
                          std::chrono::seconds(6), log_level::warning);
        return true;
    }

    void confirm_delete_part(window_host &host, const library::library &lib, const library::part &part)
    {
        const std::filesystem::path path = lib.path;
        const std::string id = part.id;
        const std::string name = part.name.empty() ? part.id : part.name;
        const std::string library_name(lib.display_name());
        app_services &services = host.services();
        window_host *where = &host;

        const auto remove = [&services, where, path, id, name, library_name] {
            auto target = load_target_file(path, false);
            if (!target)
            {
                where->show_message("Not deleted: " + target.error(), std::chrono::seconds(10), log_level::error);
                return;
            }
            // Its model file goes too, unless another part names it; files
            // that nothing named before are none of this deletion's business.
            const library::part *there = target->find(id);
            const std::string model = there != nullptr ? there->model : std::string();
            if (!library::remove_part(*target, id))
            {
                where->show_message(std::format("Not deleted: {} has no part {} any more", library_name, id),
                                    std::chrono::seconds(8), log_level::warning);
                return;
            }
            remove_file_if_unused(*target, model);
            if (auto saved = library::save_library(path, *target); !saved)
            {
                where->show_message("Not deleted: " + saved.error(), std::chrono::seconds(10), log_level::error);
                return;
            }
            services.rescan_catalog();
            where->show_message(std::format("Deleted {} from {}", name, library_name), std::chrono::seconds(6));
        };

        open_choice_dialog(host, std::format("Delete {} from {}?", name, library_name),
                           std::format("The part is removed from {}. This cannot be undone. Projects that copied it "
                                       "keep their copy.",
                                       library::path_to_utf8(path)),
                           {{"Delete", button_kind::normal, remove}, {"Cancel", button_kind::primary, {}}});
    }

} // namespace volt::ui
