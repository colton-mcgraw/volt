#include <app/ui/windows/content.hpp>

#include <app/ui/scale.hpp>
#include <app/ui/windows/image_viewer.hpp>
#include <ui/widgets/image_view.hpp>

#include <algorithm>
#include <format>

namespace volt::ui
{

    // ---- window_host ----------------------------------------------------------------

    window_host::window_host(window_kind kind, ui::screen &screen, app_services &services)
        : m_kind(kind), m_screen(screen), m_services(services), m_status(screen)
    {
    }

    window_host::~window_host() = default;

    void window_host::show_message(std::string text, std::chrono::seconds duration, log_level level)
    {
        m_services.messages().add(m_kind, text, level);
        m_status.show_message(std::move(text), duration);
    }

    void window_host::show_image(texture_id image, math::vec2i pixels, std::string title, std::string details)
    {
        open_image_viewer(*this, image, pixels, std::move(title), std::move(details));
    }

    void window_host::track_logo(image_view &view, float logical)
    {
        forget_logo(view);
        m_logos.push_back({&view, logical});
        view.set_image(m_services.logo(scaled_pixels(logical, scale())));
    }

    void window_host::forget_logo(const image_view &view) noexcept
    {
        std::erase_if(m_logos, [&](const tracked_logo &t) { return t.view == &view; });
    }

    void window_host::close()
    {
        m_services.close_window(m_kind);
    }

    void window_host::apply_scale(float content_scale)
    {
        m_content_scale = content_scale;
        m_screen.set_scale(effective_scale(content_scale, m_services.user_scale()));

        // The logos' textures are made for a pixel size. The old ones are
        // released only once no window shows them (trim_logos()), and a
        // released id names nothing at once (image_textures::release()),
        // so nothing draws a freed texture in between.
        for (const tracked_logo &t : m_logos)
            t.view->set_image(m_services.logo(scaled_pixels(t.logical, scale())));

        // The sizes no window shows any more go; this one's new ones are
        // in use already.
        m_services.trim_logos();
        scale_changed.emit();
    }

    std::vector<int> window_host::logo_sizes() const
    {
        std::vector<int> sizes;
        for (const tracked_logo &t : m_logos)
            sizes.push_back(scaled_pixels(t.logical, scale()));
        return sizes;
    }

    void window_host::set_frame_info(const math::vec2i &pixels, std::string_view encoding)
    {
        m_status.set_frame_info(std::format("{} \xC3\x97 {}  \xC2\xB7  {}  \xC2\xB7  Scale {}", pixels.x(), pixels.y(),
                                            encoding, describe_scale(m_content_scale, m_services.user_scale())));
        if (m_encoding != encoding)
        {
            m_encoding = std::string(encoding);
            display_changed.emit();
        }
    }

    void window_host::set_display_hdr(bool hdr)
    {
        if (std::exchange(m_display_hdr, hdr) != hdr)
            display_changed.emit();
    }

    // ---- window_content -----------------------------------------------------------------

    void window_content::drop(std::span<const std::string> paths, math::vec2i /*position*/)
    {
        m_services.open_files(paths, kind());
    }

} // namespace volt::ui
