#include <app/ui/glyph_cache.hpp>

#include <span>

namespace volt::ui
{

    std::expected<void, std::string> glyph_cache::upload(const gpu::frame &frame, font::atlas &atlas)
    {
        for (std::size_t index = 0; index < atlas.page_count(); ++index)
        {
            const font::atlas_page page = atlas.page(index);
            if (page.width <= 0 || page.height <= 0)
                continue;

            const auto width = static_cast<std::uint32_t>(page.width);
            const auto height = static_cast<std::uint32_t>(page.height);

            if (index >= m_pages.size())
                m_pages.push_back(std::make_unique<gpu::texture>());

            gpu::texture &texture = *m_pages[index];
            if (!texture.is_created() || texture.width() != width || texture.height() != height)
            {
                texture.destroy();
                if (auto created = texture.create(*m_context, width, height, gpu::texture_format::r8_unorm);
                    !created)
                    return created;
            }

            const font::pixel_rect dirty = atlas.take_dirty(index);
            if (dirty.empty())
                continue;

            // The dirty rectangle's rows, straight out of the page: the
            // stride is the page's width.
            const std::size_t offset =
                static_cast<std::size_t>(dirty.y) * width + static_cast<std::size_t>(dirty.x);
            const auto bytes = std::as_bytes(page.pixels.subspan(offset));
            const gpu::texture_region region{static_cast<std::uint32_t>(dirty.x), static_cast<std::uint32_t>(dirty.y),
                                             static_cast<std::uint32_t>(dirty.width),
                                             static_cast<std::uint32_t>(dirty.height)};
            if (auto updated = texture.update(frame, region, bytes, width); !updated)
                return updated;
        }
        return {};
    }

    VkImageView glyph_cache::view(std::size_t page) const noexcept
    {
        if (page >= m_pages.size() || !m_pages[page]->is_ready())
            return VK_NULL_HANDLE;
        return m_pages[page]->view();
    }

} // namespace volt::ui
