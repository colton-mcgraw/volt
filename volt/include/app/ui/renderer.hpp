#pragma once

#include <app/ui/glyph_cache.hpp>
#include <gpu/buffer.hpp>
#include <gpu/presenter.hpp>
#include <gpu/texture.hpp>
#include <math/vector.hpp>
#include <ui/draw_list.hpp>

#include <font/atlas.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace volt::gpu
{
    class context;
} // namespace volt::gpu

namespace volt::ui
{

    // ---------------------------------------------------------------------
    // renderer
    // ---------------------------------------------------------------------
    //
    // Draws the interface: turns a draw list (<ui/draw_list.hpp>) into
    // commands. Its pass is the one that fills a window: the schematic
    // and board views render into targets of their own first, and the
    // interface then shows those as images in its viewports.
    //
    // Being the pass that writes the window's image also makes it the one
    // place that encodes for the display (window_image::output,
    // <gpu/color.hpp>).
    //
    // A frame goes to every window at once (<gpu/presenter.hpp>), so the
    // renderer is called like this, per frame:
    //
    //     upload_glyphs(frame, atlas)                       once
    //     draw(frame, window A's image, A's list, ...)      once per window
    //     draw(frame, window B's image, B's list, ...)
    //
    // Each draw() is a pass of its own into its own image, with its own
    // format, size and encoding. What a draw needs to outlive its recording
    // (the instances, the descriptor sets) piles up over the frame's draws
    // and is let go of together when the slot comes round again, so one
    // draw never overwrites another's. The images set with set_image() are
    // the same for every draw: two windows may show the same view.
    //
    // Every item of the list is an instance of one pipeline, whose shader
    // draws all the shapes (assets/shaders/ui_quad.*). The list is drawn in
    // order, in batches of neighbouring items that share a clip rectangle
    // (the scissor), a glyph page and an image; an item that uses no texture
    // never breaks a batch.
    //
    // Textures: the list names them by texture_id. Glyph pages are the pages
    // of the text engine's atlas, which upload_glyphs() mirrors into
    // textures. Images are whatever was last set_image()d under the id's
    // index: an image's gpu::texture (<app/ui/images.hpp>), or a view's
    // render target. An item whose texture has nothing set, or is not
    // ready, is skipped.
    //
    // An image's colours are premultiplied by its alpha, in linear light:
    // sampled through an sRGB format they come out as linear colour times
    // alpha, which is the one form in which filtering between a pixel and
    // its transparent neighbours (and between mip levels) does not drag in
    // the colour of what cannot be seen. image::premultiply() makes the
    // pixels so; a view's target is opaque, alpha 1, where both forms are
    // the same. The shader divides the alpha back out after filtering.
    //
    // create() and destroy() belong on the thread that owns the context;
    // the rest on the render thread.
    class renderer
    {
    public:
        // Batches with a texture of their own, per frame, over all of the
        // frame's draws; past this the rest of what is drawn in the frame is
        // left out. Room for a few windows of a busy interface each.
        static constexpr std::uint32_t max_sets = 2048;

        renderer() = default;
        ~renderer() { destroy(); }

        renderer(const renderer &) = delete;
        renderer &operator=(const renderer &) = delete;

        std::expected<void, std::string> create(gpu::context &context);
        void destroy();

        // What texture_id::image(index) stands for from now on; null to
        // forget it. The view must be ready to be sampled, in
        // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, whenever a list that
        // uses it is drawn, and stay alive until the frame is through. One
        // table for every window.
        void set_image(std::uint32_t index, VkImageView view);

        // Brings the glyph textures up to date with `atlas`. Once per frame,
        // before the frame's first draw().
        std::expected<void, std::string> upload_glyphs(const gpu::frame &frame, font::atlas &atlas);

        // Records an interface pass into `frame`: clears `target` (a window's
        // image, presenter::acquire()) to `background` (working space) and
        // draws `list` over it, encoded for target.output. Any number of
        // times per frame, each into another image.
        std::expected<void, std::string> draw(const gpu::frame &frame, const gpu::window_image &target,
                                              const draw_list &list, const math::vec3f &background);

    private:
        // A window's format follows its display (<gpu/swapchain.hpp>), and a
        // pipeline is built for one format. Windows on different monitors
        // may need different ones in the same frame.
        struct pipeline_entry
        {
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkPipeline pipeline = VK_NULL_HANDLE;
        };

        std::expected<VkPipeline, std::string> pipeline_for(VkFormat format);
        VkImageView resolve(texture_id id) const noexcept;

        gpu::context *m_context = nullptr;

        VkSampler m_sampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_set_layout = VK_NULL_HANDLE;
        VkPipelineLayout m_layout = VK_NULL_HANDLE;
        std::vector<pipeline_entry> m_pipelines;

        // Bound where an item needs no texture: one white pixel.
        gpu::texture m_white;

        glyph_cache m_glyphs;
        std::vector<VkImageView> m_images;

        // Per frame in flight: the instances, and the descriptor sets naming
        // the textures. Both are made anew every frame, and added to by each
        // of its draws (gpu::stream::append()).
        gpu::stream m_instances;
        std::array<VkDescriptorPool, gpu::presenter::frames_in_flight> m_pools{};

        // frame::number of the frame the last draw() was in, and how many of
        // its slot's sets the frame's draws have taken.
        std::uint64_t m_frame = 0;
        std::uint32_t m_sets = 0;
    };

} // namespace volt::ui
