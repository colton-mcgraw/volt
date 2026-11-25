#include <app/ui/windows/image_viewer.hpp>

#include <ui/layout/box.hpp>
#include <ui/widgets/button.hpp>
#include <ui/widgets/image_view.hpp>
#include <ui/widgets/label.hpp>
#include <ui/widgets/panel.hpp>

#include <algorithm>

namespace volt::ui
{

    void open_image_viewer(window_host &host, texture_id image, math::vec2i pixels, std::string title,
                           std::string details)
    {
        ui::screen &screen = host.screen();

        // Most of the window, the image fitted into it on a checkerboard.
        const math::vec2f room = screen.size();
        auto dialog = std::make_unique<panel>("", surface::raised, 10.0f, insets::uniform(14.0f));
        dialog->set_name("image viewer");
        dialog->set_border(true);
        dialog->layout().width = std::clamp(room.x() * 0.8f, std::min(320.0f, room.x()), 1400.0f);
        dialog->layout().height = std::clamp(room.y() * 0.8f, std::min(240.0f, room.y()), 1000.0f);

        auto &heading = dialog->emplace<row>(10.0f);
        heading.set_cross_align(align::center);
        auto &name = heading.emplace<label>(std::move(title));
        name.layout().flex = 1.0f;
        heading.emplace<label>(std::move(details), text_role::dim);

        auto &view = dialog->emplace<image_view>(image, math::vec2f{64.0f, 64.0f});
        view.set_name("viewed image");
        view.set_fit(pixels.cast<float>());
        view.set_checkerboard(true);
        view.layout().flex = 1.0f;

        auto &buttons = dialog->emplace<row>(8.0f);
        buttons.emplace<spacer>();
        auto &close = buttons.emplace<button>("Close", icon::none, button_kind::primary);
        close.set_name("close image");

        // The texture goes with the dialog, however it is closed; the
        // services outlive every window.
        popup_options options;
        options.placement = placement::centered;
        options.modal = true;
        options.closed = [&services = host.services(), image] { services.remove_image(image); };
        widget &shown = screen.open_popup(std::move(dialog), std::move(options));
        close.clicked.connect([&screen, &shown] { screen.close_popup(shown); });
        close.focus(focus_reason::keyboard);
    }

} // namespace volt::ui
