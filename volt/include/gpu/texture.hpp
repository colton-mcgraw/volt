#pragma once

#include <gpu/buffer.hpp>
#include <gpu/presenter.hpp>
#include <gpu/vulkan.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace volt::gpu
{

    class context;

    // How a texture's pixels are stored, and so how the bytes handed to it
    // are read. Rows run top to bottom, pixels left to right.
    enum class texture_format
    {
        // One byte per pixel, read as 0 to 1: glyph coverage, masks. Linear,
        // not a colour, so never sRGB.
        r8_unorm,

        // Four bytes per pixel, R G B A, alpha not multiplied in. The colour
        // channels are sRGB encoded, which is how image files store them;
        // sampling decodes them into the working space (<gpu/color.hpp>).
        rgba8_srgb,

        // The same bytes taken as linear values: data, normal maps, or
        // colours someone already decoded.
        rgba8_unorm,
    };

    std::size_t bytes_per_pixel(texture_format format) noexcept;

    // A rectangle of a texture, in pixels.
    struct texture_region
    {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    // ---------------------------------------------------------------------
    // texture
    // ---------------------------------------------------------------------
    //
    // A 2D image made from pixels the CPU has, to be sampled by shaders:
    // glyph pages, icons, decoded image files. No array layers. It knows
    // nothing about where the pixels came from; whoever decodes a file hands
    // over width, height, format and bytes.
    //
    // There are two ways to fill it:
    //
    //   * create() with pixels uploads them right away and waits for the
    //     copy, on the graphics queue. For images that are made once and
    //     then only drawn (icons). Not while the calling thread is recording
    //     a frame: it submits work of its own. Given a chain of levels it
    //     makes a mipmapped texture, so that an image drawn smaller than it
    //     is can be sampled from a level near the size it is drawn at,
    //     rather than alias. The levels are the caller's to make (with
    //     image::mip_chain, which filters in linear light); the GPU does not
    //     blit them, which for an sRGB format it would do on encoded values.
    //
    //   * update() records the copy into a frame's command buffer, for
    //     images that change while the application runs (a glyph atlas
    //     growing). The copy runs before whatever the frame records after
    //     it, so the frame's passes see the new pixels; call it before the
    //     pass that samples the texture begins, never inside a pass. The
    //     pixels are copied into staging memory of the frame's slot first,
    //     which the next frame with that slot reuses, by when the presenter
    //     has waited for this one; so the caller's bytes are free again as
    //     soon as update() returns. One update() per texture per frame,
    //     like gpu::stream::write.
    //
    // Frames in flight: an update() does not wait for the previous frame,
    // which may still be sampling the image. Both frames go to the graphics
    // queue, and the barrier before the copy orders it after every fragment
    // shader read submitted earlier, so the old frame sees the old pixels
    // and the new one the new pixels. There is one image, not one per frame.
    //
    // A texture that was created without pixels is cleared to zero by its
    // first update(), so the part that update does not cover reads as zero
    // rather than garbage. update() is for textures of one level only.
    //
    // destroy() waits for the graphics queue to go idle before freeing
    // anything, since frames in flight may still sample the image. That is
    // a stall, so textures are meant to live long: an atlas keeps its pages,
    // an icon set its icons. An owner whose textures come and go (images
    // opened and closed) retires them through a gpu::retire_queue instead,
    // which calls destroy_unused() once no frame in flight can sample them.
    //
    // create() and destroy() belong on the thread that owns the context, or
    // on the render thread between frames; update() on the render thread.
    class texture
    {
    public:
        texture() = default;
        ~texture() { destroy(); }

        texture(const texture &) = delete;
        texture &operator=(const texture &) = delete;

        // An image of that size and format whose contents are undefined
        // until the first update().
        std::expected<void, std::string> create(context &context, std::uint32_t width, std::uint32_t height,
                                                texture_format format);

        // The same, filled with `pixels`: width * height pixels, tightly
        // packed. Waits for the upload.
        std::expected<void, std::string> create(context &context, std::uint32_t width, std::uint32_t height,
                                                texture_format format, std::span<const std::byte> pixels);

        // A mipmapped texture filled with `levels`: levels[0] is the image
        // at width * height, each next one half the size of the one before
        // in each direction (rounded down, at least 1), tightly packed.
        // Fewer levels than down to 1 x 1 are fine. Waits for the upload.
        std::expected<void, std::string> create(context &context, std::uint32_t width, std::uint32_t height,
                                                texture_format format,
                                                std::span<const std::span<const std::byte>> levels);

        void destroy();

        // Frees at once, without waiting for the queue. Only for an owner
        // that knows no frame in flight samples the texture any more: one
        // that retired it through a gpu::retire_queue.
        void destroy_unused();

        bool is_created() const noexcept { return m_image != VK_NULL_HANDLE; }

        // Records the upload of `region` into `frame`. `pixels` holds the
        // region's rows, each `stride` bytes apart (at least region.width *
        // bytes_per_pixel()), starting with its top row. The region must lie
        // inside the texture and must not be empty. The texture must have
        // one level.
        std::expected<void, std::string> update(const frame &frame, const texture_region &region,
                                                std::span<const std::byte> pixels, std::size_t stride);

        // For sampling, in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL once
        // is_ready(). Null until created.
        VkImageView view() const noexcept { return m_view; }

        // Has been filled (by create() with pixels, or an update() recorded
        // in a frame that came before the use) and may be sampled.
        bool is_ready() const noexcept { return m_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }

        std::uint32_t width() const noexcept { return m_width; }
        std::uint32_t height() const noexcept { return m_height; }
        texture_format format() const noexcept { return m_format; }
        std::uint32_t levels() const noexcept { return m_levels; }

    private:
        std::expected<void, std::string> create_image(context &context, std::uint32_t width, std::uint32_t height,
                                                      texture_format format, std::uint32_t levels);

        // A level's place in a staging buffer that holds them all, and its
        // size.
        struct level_copy
        {
            VkDeviceSize offset = 0;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
        };

        // The barriers and copies of one upload from `staging`: either
        // `region` of level 0 from offset 0, rows tightly packed, or (with
        // `levels` non-empty) every level, whole.
        void record_upload(VkCommandBuffer commands, VkBuffer staging, const texture_region &region,
                           std::span<const level_copy> levels = {});

        context *m_context = nullptr;

        VkImage m_image = VK_NULL_HANDLE;
        VkDeviceMemory m_memory = VK_NULL_HANDLE;
        VkImageView m_view = VK_NULL_HANDLE;

        std::uint32_t m_width = 0;
        std::uint32_t m_height = 0;
        std::uint32_t m_levels = 1;
        texture_format m_format = texture_format::r8_unorm;

        // The layout the commands recorded so far leave the image in.
        VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;

        // Per frame in flight: what update() copies from.
        stream m_staging;
    };

} // namespace volt::gpu
