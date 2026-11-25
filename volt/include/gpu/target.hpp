#pragma once

#include <gpu/vulkan.hpp>
#include <math/vector.hpp>

#include <expected>
#include <string>

namespace volt::gpu
{

    class context;

    // ---------------------------------------------------------------------
    // target
    // ---------------------------------------------------------------------
    //
    // An image a view renders into and the interface then shows: the
    // schematic and the board each have one. Its colors are in the working
    // space of <gpu/color.hpp>, in 16 bit float so that values brighter than
    // SDR white survive until the interface encodes them for the display.
    //
    // A pass is everything between begin() and end(). Outside of one the
    // image is ready to be sampled, and stays so for as long as nobody draws
    // into it again: a view that has not changed does not have to be redrawn
    // for the interface to show it.
    //
    // Everything here belongs to the render thread.
    class target
    {
    public:
        static constexpr VkFormat color_format = VK_FORMAT_R16G16B16A16_SFLOAT;

        target() = default;
        ~target() { destroy(); }

        target(const target &) = delete;
        target &operator=(const target &) = delete;

        // `depth` adds a depth buffer, for views that are 3D. There are no
        // images yet: resize() makes them.
        std::expected<void, std::string> create(context &context, bool depth);
        void destroy();

        // Makes the target `extent` pixels big; nothing happens if it
        // already is. Otherwise the contents are lost and view() changes.
        // Waits for the frames in flight, which may still be showing the
        // old image.
        std::expected<void, std::string> resize(VkExtent2D extent);

        VkExtent2D extent() const noexcept { return m_extent; }

        // What pipelines drawing into the target are built for. The depth
        // format depends on the GPU and is VK_FORMAT_UNDEFINED without a
        // depth buffer.
        VkFormat depth_format() const noexcept { return m_depth_format; }

        // The color image, for sampling. Null until the first resize(), and
        // not to be sampled before the first pass has ended.
        VkImageView view() const noexcept { return m_color.view; }
        bool is_drawn() const noexcept { return m_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }

        // Starts the pass: the image is cleared to `clear` (and the depth
        // buffer to the far plane), and viewport and scissor are set to all
        // of it.
        void begin(VkCommandBuffer commands, const math::vec4f &clear);
        void end(VkCommandBuffer commands);

    private:
        struct image
        {
            VkImage handle = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
        };

        std::expected<void, std::string> create_image(image &image, VkFormat format, VkImageUsageFlags usage,
                                                      VkImageAspectFlags aspect);
        void destroy_images();

        context *m_context = nullptr;
        VkFormat m_depth_format = VK_FORMAT_UNDEFINED;
        VkExtent2D m_extent{};

        image m_color;
        image m_depth;

        // The layout the commands recorded so far leave the color image in.
        VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

} // namespace volt::gpu
