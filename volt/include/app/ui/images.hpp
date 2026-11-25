#pragma once

#include <app/ui/image_registry.hpp>
#include <gpu/presenter.hpp>
#include <gpu/retire.hpp>
#include <gpu/texture.hpp>
#include <image/image.hpp>
#include <math/vector.hpp>
#include <ui/draw_list.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace volt::gpu
{
    class context;
} // namespace volt::gpu

namespace volt::ui
{

    class renderer;

    // An image made ready to become a texture: the pixels of every mip
    // level, rgba8, sRGB encoded, premultiplied by alpha in linear light
    // (what <app/ui/renderer.hpp> samples). Plain CPU work, so it is made
    // on whatever thread decoded the image.
    struct texture_pixels
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<image::image> levels; // levels[0] is width x height
    };

    // Any thread. `source` in any format and alpha mode; images that are
    // shown smaller than they are get their mip levels, by image::mip_chain
    // (linear light, weighted by alpha, on straight alpha), premultiplied
    // only afterwards. `mipmaps` false keeps one level: for an image drawn
    // at its own size.
    texture_pixels prepare_texture(const image::image &source, bool mipmaps = true);

    // ---------------------------------------------------------------------
    // image_textures
    // ---------------------------------------------------------------------
    //
    // The interface's images on the GPU, under the ids the interface draws
    // them by: one gpu::texture each, an index from an image_registry, and
    // the renderer told where it is. The views' render targets take indices
    // from the same registry (reserve()), but stay their renderers' own.
    //
    // add() creates the texture with its pixels and waits for the upload
    // (gpu::texture::create()), so it runs between frames. release() makes
    // the id name nothing at once (the renderer skips items that use it)
    // and retires the texture with the last frame begun, which may still
    // sample it; collect(), at the start of each frame, frees it once that
    // frame is through. So images come and go without stalling the frames.
    //
    // Render thread, apart from create() and destroy(), which belong to the
    // thread that owns the context.
    class image_textures
    {
    public:
        image_textures() = default;
        ~image_textures() { destroy(); }

        image_textures(const image_textures &) = delete;
        image_textures &operator=(const image_textures &) = delete;

        // Both must outlive this.
        void create(gpu::context &context, renderer &renderer);

        // Waits for the device, frees every texture, forgets every id.
        void destroy();

        // An index for a view the caller keeps set with renderer::set_image().
        texture_id reserve();

        std::expected<texture_id, std::string> add(const texture_pixels &pixels);

        // Gives back an id from add() or reserve(). Ignores ids it does not
        // know.
        void release(texture_id id);

        void collect(const gpu::frame &frame);

        // The size of the image under `id`, or zero.
        math::vec2i size_of(texture_id id) const noexcept;

        std::size_t count() const noexcept { return m_registry.size(); }

    private:
        gpu::context *m_context = nullptr;
        renderer *m_renderer = nullptr;

        image_registry m_registry;

        // By index; null for reserved indices and free ones.
        std::vector<std::unique_ptr<gpu::texture>> m_textures;

        gpu::retire_queue m_retired;
        std::uint64_t m_last_frame = 0;
    };

} // namespace volt::ui
