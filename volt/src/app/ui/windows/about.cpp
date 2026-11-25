#include <app/ui/windows/about.hpp>

#include <ui/layout/box.hpp>
#include <ui/widgets/button.hpp>
#include <ui/widgets/image_view.hpp>
#include <ui/widgets/label.hpp>
#include <ui/widgets/panel.hpp>

namespace volt::ui
{
    namespace
    {
        // The logo's size in the dialog, logical units.
        constexpr float about_logo = 32.0f;
    } // namespace

    void open_about(window_host &host)
    {
        auto dialog = std::make_unique<panel>("", surface::raised, 10.0f, insets::uniform(18.0f));
        dialog->set_name("about dialog");
        dialog->set_border(true);
        dialog->layout().width = 380.0f;

        auto &heading = dialog->emplace<row>(10.0f);
        heading.set_cross_align(align::center);
        auto &logo = heading.emplace<image_view>(texture_id{}, math::vec2f{about_logo, about_logo});
        host.track_logo(logo, about_logo);
        auto &name = heading.emplace<label>("VOLT 0.3");
        name.set_size(text_size::title);

        auto &text = dialog->emplace<label>(
            "Schematic capture, board layout and circuit simulation, written from scratch: its own math, "
            "platform layer, Vulkan renderer, fonts and interface.",
            text_role::dim);
        text.set_wrap(true);

        auto &buttons = dialog->emplace<row>(8.0f);
        buttons.emplace<spacer>();
        auto &ok = buttons.emplace<button>("OK", icon::none, button_kind::primary);
        ok.set_name("about ok");

        popup_options options;
        options.placement = placement::centered;
        options.modal = true;
        options.closed = [&host, &logo] { host.forget_logo(logo); };
        widget &shown = host.screen().open_popup(std::move(dialog), std::move(options));
        ok.clicked.connect([&host, &shown] { host.screen().close_popup(shown); });
        ok.focus(focus_reason::keyboard);
    }

} // namespace volt::ui
