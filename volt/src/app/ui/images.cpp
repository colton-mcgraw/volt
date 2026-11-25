#include <app/ui/images.hpp>

#include <app/ui/renderer.hpp>
#include <gpu/context.hpp>

#include <cstddef>
#include <span>

namespace volt::ui
{

    texture_pixels prepare_texture(const image::image &source, bool mipmaps)
    {
        texture_pixels result;
        if (source.empty())
            return result;

        // Straight alpha for the filtering, which weights colour by alpha
        // itself; premultiplied for the GPU, level by level.
        image::image base = image::to_rgba8_srgb(source);
        if (base.premultiplied())
            image::unpremultiply(base);

        result.width = base.width();
        result.height = base.height();
        result.levels = mipmaps ? image::mip_chain(base) : std::vector<image::image>{std::move(base)};
        for (image::image &level : result.levels)
            image::premultiply(level);
        return result;
    }

    void image_textures::create(gpu::context &context, renderer &renderer)
    {
        m_context = &context;
        m_renderer = &renderer;
    }

    void image_textures::destroy()
    {
        if (m_context == nullptr)
            return;

        m_context->graphics().wait_idle();
        m_retired.flush();
        for (std::size_t i = 0; i < m_textures.size(); ++i)
        {
            if (m_textures[i] == nullptr)
                continue;
            m_renderer->set_image(static_cast<std::uint32_t>(i), VK_NULL_HANDLE);
            m_textures[i]->destroy_unused(); // the device is idle
        }
        m_textures.clear();
        m_registry = {};
        m_context = nullptr;
        m_renderer = nullptr;
        m_last_frame = 0;
    }

    texture_id image_textures::reserve()
    {
        const texture_id id = m_registry.allocate();
        if (id.index >= m_textures.size())
            m_textures.resize(id.index + 1);
        return id;
    }

    std::expected<texture_id, std::string> image_textures::add(const texture_pixels &pixels)
    {
        if (m_context == nullptr)
            return std::unexpected("the image textures are not created");
        if (pixels.levels.empty() || pixels.width == 0 || pixels.height == 0)
            return std::unexpected("the image is empty");

        std::vector<std::span<const std::byte>> levels;
        for (const image::image &level : pixels.levels)
            levels.push_back(std::as_bytes(level.pixels()));

        auto texture = std::make_unique<gpu::texture>();
        if (auto created = texture->create(*m_context, pixels.width, pixels.height, gpu::texture_format::rgba8_srgb,
                                           levels);
            !created)
            return std::unexpected(std::move(created).error());

        const texture_id id = reserve();
        m_renderer->set_image(id.index, texture->view());
        m_textures[id.index] = std::move(texture);
        return id;
    }

    void image_textures::release(texture_id id)
    {
        if (!m_registry.contains(id))
            return;

        m_renderer->set_image(id.index, VK_NULL_HANDLE);
        if (std::unique_ptr<gpu::texture> texture = std::move(m_textures[id.index]))
        {
            m_retired.retire(m_last_frame, [texture = std::move(texture)]() mutable { texture->destroy_unused(); });
        }
        m_registry.release(id);
    }

    void image_textures::collect(const gpu::frame &frame)
    {
        m_retired.collect(frame);
        m_last_frame = frame.number;
    }

    math::vec2i image_textures::size_of(texture_id id) const noexcept
    {
        if (!m_registry.contains(id) || m_textures[id.index] == nullptr)
            return {0, 0};
        const gpu::texture &texture = *m_textures[id.index];
        return {static_cast<int>(texture.width()), static_cast<int>(texture.height())};
    }

} // namespace volt::ui
