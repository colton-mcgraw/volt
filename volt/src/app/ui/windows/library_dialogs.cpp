#include <app/ui/windows/library_dialogs.hpp>

#include <app/ui/windows/content.hpp>
#include <ui/layout/box.hpp>
#include <ui/screen.hpp>
#include <ui/theme.hpp>
#include <ui/widgets/label.hpp>
#include <ui/widgets/list_view.hpp>
#include <ui/widgets/panel.hpp>

#include <algorithm>
#include <memory>

namespace volt::ui
{
    namespace
    {
        // A dialog's width, logical units; narrower on a small window.
        constexpr float dialog_width = 440.0f;
        constexpr float report_width = 640.0f;

        // The dialog's frame, which answers Escape: the screen closes
        // dismissable popups on Escape, but these are not dismissable (a
        // press outside must not lose the question), so they close
        // themselves.
        class dialog_frame final : public panel
        {
        public:
            dialog_frame() : panel("", surface::raised, 12.0f, insets::uniform(18.0f))
            {
                set_border(true);
            }

            std::function<void()> escape;

        protected:
            bool on_key(const key_event &e) override
            {
                if (e.key == key::escape && e.action != action::release && (e.mods & held_modifiers) == modifiers::none &&
                    escape)
                {
                    escape();
                    return true;
                }
                return false;
            }
        };

        float width_for(const window_host &host, float wanted)
        {
            return std::max(240.0f, std::min(wanted, host.screen().size().x() - 32.0f));
        }
    } // namespace

    void open_choice_dialog(window_host &host, std::string title, std::string text,
                            std::vector<dialog_choice> choices, std::function<void()> cancelled)
    {
        auto dialog = std::make_unique<dialog_frame>();
        dialog->set_name("choice dialog");
        dialog->layout().width = width_for(host, dialog_width);

        auto &heading = dialog->emplace<label>(std::move(title));
        heading.set_wrap(true, 2);
        if (!text.empty())
        {
            auto &body = dialog->emplace<label>(std::move(text), text_role::dim);
            body.set_wrap(true);
        }

        // Whether a button closed it, so that closing it any other way
        // counts as cancelling.
        auto chosen = std::make_shared<bool>(false);

        auto &buttons = dialog->emplace<row>(8.0f);
        buttons.emplace<spacer>();
        std::vector<button *> made;
        for (dialog_choice &choice : choices)
        {
            auto &b = buttons.emplace<button>(choice.text, icon::none, choice.kind);
            b.set_name("dialog " + choice.text);
            made.push_back(&b);
        }

        dialog_frame &frame = *dialog;
        popup_options options;
        options.placement = placement::centered;
        options.modal = true;
        options.dismissable = false;
        options.closed = [chosen, cancelled = std::move(cancelled)] {
            if (!*chosen && cancelled)
                cancelled();
        };
        widget &shown = host.screen().open_popup(std::move(dialog), std::move(options));
        ui::screen &s = host.screen();
        frame.escape = [&s, &shown] { s.close_popup(shown); };

        for (std::size_t i = 0; i < made.size(); ++i)
        {
            made[i]->clicked.connect([&s, &shown, chosen, action = std::move(choices[i].action)] {
                *chosen = true;
                const std::function<void()> run = action; // the button goes with the dialog
                s.close_popup(shown);
                if (run)
                    run();
            });
        }

        button *initial = made.empty() ? nullptr : made.back();
        for (std::size_t i = 0; i < made.size(); ++i)
        {
            if (choices[i].kind == button_kind::primary)
            {
                initial = made[i];
                break;
            }
        }
        if (initial != nullptr)
            initial->focus(focus_reason::keyboard);
        else
            frame.focus();
    }

    void open_report_dialog(window_host &host, std::string title, std::string summary,
                            std::vector<std::string> lines)
    {
        auto dialog = std::make_unique<dialog_frame>();
        dialog->set_name("report dialog");
        dialog->layout().width = width_for(host, report_width);

        dialog->emplace<label>(std::move(title));
        if (!summary.empty())
        {
            auto &text = dialog->emplace<label>(std::move(summary), text_role::dim);
            text.set_wrap(true);
        }
        auto &list = dialog->emplace<list_view>(std::move(lines));
        list.set_name("report lines");
        list.layout().height = std::max(120.0f, std::min(320.0f, host.screen().size().y() - 220.0f));

        auto &buttons = dialog->emplace<row>(8.0f);
        buttons.emplace<spacer>();
        auto &close = buttons.emplace<button>("Close", icon::none, button_kind::primary);
        close.set_name("report close");

        dialog_frame &frame = *dialog;
        popup_options options;
        options.placement = placement::centered;
        options.modal = true;
        widget &shown = host.screen().open_popup(std::move(dialog), std::move(options));
        ui::screen &s = host.screen();
        frame.escape = [&s, &shown] { s.close_popup(shown); };
        close.clicked.connect([&s, &shown] { s.close_popup(shown); });
        close.focus(focus_reason::keyboard);
    }

} // namespace volt::ui
