#include <app/ui/windows/preferences.hpp>

#include <app/ui/scale.hpp>
#include <app/ui/settings.hpp>
#include <ui/layout/box.hpp>
#include <ui/widgets/button.hpp>
#include <ui/widgets/dropdown.hpp>
#include <ui/widgets/label.hpp>
#include <ui/widgets/panel.hpp>
#include <ui/widgets/property_grid.hpp>
#include <ui/widgets/tab_view.hpp>

#include <format>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace volt::ui
{
    namespace
    {
        // The dialog remembers the page it was on, for the session.
        int last_page = 0;

        // What a page is built with. A page's controls change the settings
        // through services().change_settings(), and show them through
        // show(): the function given runs once now and again whenever what
        // it shows may have changed (a setting, here or in another window's
        // Preferences; this window's scale or display).
        class page_context
        {
        public:
            explicit page_context(window_host &host) : m_host(host) {}

            window_host &host() const noexcept { return m_host; }
            app_services &services() const noexcept { return m_host.services(); }
            const app_settings &settings() const noexcept { return m_host.services().settings(); }

            void show(std::function<void()> refresh)
            {
                refresh();
                m_refresh.push_back(std::move(refresh));
            }

            void refresh() const
            {
                for (const std::function<void()> &f : m_refresh)
                    f();
            }

        private:
            window_host &m_host;
            std::vector<std::function<void()>> m_refresh;
        };

        // A page: a column with a grid of settings, and notes under it.
        std::unique_ptr<panel> make_page(const char *name)
        {
            auto page = std::make_unique<panel>("", surface::raised, 10.0f, insets{0.0f, 12.0f, 0.0f, 0.0f});
            page->set_name(name);
            return page;
        }

        label &add_note(panel &page, const char *name)
        {
            auto &note = page.emplace<label>("", text_role::dim);
            note.set_name(name);
            note.set_wrap(true);
            return note;
        }

        // ---- Appearance ------------------------------------------------------------

        std::unique_ptr<widget> appearance_page(page_context &context)
        {
            auto page = make_page("appearance page");
            auto &grid = page->emplace<property_grid>();
            grid.set_name("appearance grid");

            auto &theme = grid.add_row("Theme", std::make_unique<dropdown>(std::vector<std::string>{"Dark", "Light"},
                                                                           context.settings().theme));
            theme.set_name("theme");
            theme.changed.connect([&context](int index) {
                context.services().change_settings([index](app_settings &s) { s.theme = index; });
            });

            std::vector<std::string> names;
            for (float s : user_scales)
                names.push_back(scale_text(s));
            auto &scale = grid.add_row("Interface scale", std::make_unique<dropdown>(std::move(names), 0));
            scale.set_name("scale");
            scale.set_tooltip("Multiplies the size the system gives this display");
            scale.changed.connect([&context](int index) {
                const float chosen = user_scales[static_cast<std::size_t>(index)];
                context.services().change_settings([chosen](app_settings &s) { s.user_scale = chosen; });
            });

            // What the user scale comes to on the window's monitor.
            label &note = add_note(*page, "scale note");

            context.show([&context, &theme, &scale, &note] {
                const window_host &host = context.host();
                theme.set_selected(context.settings().theme);
                scale.set_selected(static_cast<int>(nearest_user_scale(host.user_scale())));
                note.set_text(std::format("{} on this display: {} of the system's size, which is {} here.",
                                          scale_text(effective_scale(host.content_scale(), host.user_scale())),
                                          scale_text(clamp_user_scale(host.user_scale())),
                                          scale_text(host.content_scale())));
            });
            return page;
        }

        // ---- Display ---------------------------------------------------------------

        // What the choice comes to in this window, on its monitor.
        std::string output_note(const window_host &host, color_output chosen)
        {
            const std::string &encoding = host.encoding();
            const std::string sends = encoding.empty() ? std::string() : std::format(" This window sends {}.", encoding);
            if (!host.display_hdr())
            {
                return chosen == color_output::hdr
                           ? "This display is not in HDR mode, so windows on it stay SDR." + sends
                           : "This display is not in HDR mode." + sends;
            }
            return chosen == color_output::hdr
                       ? "This display is in HDR mode." + sends
                       : "This display is in HDR mode; the system converts SDR windows for it." + sends;
        }

        std::unique_ptr<widget> display_page(page_context &context)
        {
            auto page = make_page("display page");
            auto &grid = page->emplace<property_grid>();
            grid.set_name("display grid");

            auto &output = grid.add_row(
                "Color output", std::make_unique<dropdown>(std::vector<std::string>{"SDR", "HDR, where the display is"},
                                                           0));
            output.set_name("color output");
            output.set_tooltip("HDR lets windows on a display in HDR mode show colors brighter than SDR white");
            output.changed.connect([&context](int index) {
                const color_output chosen = index == 1 ? color_output::hdr : color_output::sdr;
                context.services().change_settings([chosen](app_settings &s) { s.output = chosen; });
            });

            label &note = add_note(*page, "output note");

            // Why it is a choice, and not simply on: see
            // volt_app::set_extended_color().
            label &caution = add_note(
                *page, "output caution");
            caution.set_text("SDR is right on every display. If colors turn gray and dim with HDR, the graphics "
                             "driver does not handle HDR windows properly: switch back to SDR, or update the driver.");
            caution.set_size(text_size::small);

            context.show([&context, &output, &note] {
                const color_output chosen = context.settings().output;
                output.set_selected(chosen == color_output::hdr ? 1 : 0);
                note.set_text(output_note(context.host(), chosen));
            });
            return page;
        }

        // The pages, in the order of their tabs. A new group of settings is
        // a function like the ones above and a line here.
        struct preferences_page
        {
            const char *title;
            std::unique_ptr<widget> (*build)(page_context &);
        };

        constexpr preferences_page pages[] = {
            {"Appearance", appearance_page},
            {"Display", display_page},
        };
    } // namespace

    void open_preferences(window_host &host)
    {
        app_services &services = host.services();

        auto dialog = std::make_unique<panel>("", surface::raised, 12.0f, insets::uniform(18.0f));
        dialog->set_name("preferences dialog");
        dialog->set_border(true);
        dialog->layout().width = 480.0f;

        auto &heading = dialog->emplace<label>("Preferences");
        heading.set_size(text_size::title);

        // The pages keep references into the context, which lives as long
        // as the dialog: the closed callback holds the last owner.
        auto context = std::make_shared<page_context>(host);

        auto &tabs = dialog->emplace<tab_view>();
        tabs.set_name("preferences tabs");
        // Every page gets the height of the tallest, so the dialog does not
        // change size from one tab to the next.
        tabs.layout().min_size = {0.0f, 210.0f};
        for (const preferences_page &page : pages)
            tabs.add_tab(page.title, page.build(*context));
        tabs.select(last_page < static_cast<int>(tabs.tab_count()) ? last_page : 0);
        tabs.changed.connect([](int index) { last_page = index; });

        auto &hint = dialog->emplace<label>("Preferences apply to every window.", text_role::dim);
        hint.set_size(text_size::small);

        auto &buttons = dialog->emplace<row>(8.0f);
        buttons.emplace<spacer>();
        auto &close = buttons.emplace<button>("Close", icon::none, button_kind::primary);
        close.set_name("preferences close");

        // A change made here, or in another window's Preferences, shows
        // here; so does what is about this window's monitor, when the
        // window moves to another or the monitor changes mode while the
        // dialog is open. The host outlives its popups.
        const auto refresh = [context] { context->refresh(); };
        const std::size_t changed = services.settings_changed.connect(refresh);
        const std::size_t scaled = host.scale_changed.connect(refresh);
        const std::size_t display = host.display_changed.connect(refresh);

        popup_options options;
        options.placement = placement::centered;
        options.modal = true;
        options.closed = [&services, &host, context, changed, scaled, display] {
            services.settings_changed.disconnect(changed);
            host.scale_changed.disconnect(scaled);
            host.display_changed.disconnect(display);
        };
        widget &shown = host.screen().open_popup(std::move(dialog), std::move(options));
        close.clicked.connect([&host, &shown] { host.screen().close_popup(shown); });
        close.focus(focus_reason::keyboard);
    }

} // namespace volt::ui
