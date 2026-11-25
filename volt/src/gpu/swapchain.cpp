#include <gpu/swapchain.hpp>

#include <gpu/context.hpp>
#include <gpu/surface.hpp>

#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>

namespace volt::gpu
{
    namespace
    {
        // Window systems that let the swapchain decide the size of a window
        // (Wayland) report this as the surface's current extent.
        constexpr std::uint32_t undefined_extent = std::numeric_limits<std::uint32_t>::max();

        // The brightest an HDR10 frame is declared to get, in nits. A promise
        // to the display rather than a limit: nothing clamps to it.
        constexpr float hdr10_peak = 1000.0f;

        struct chosen_format
        {
            VkSurfaceFormatKHR format{};
            color_encoding encoding = color_encoding::srgb;
        };

        // The best format the surface offers for a display in the given mode.
        // The HDR color spaces are only on offer where the instance has
        // VK_EXT_swapchain_colorspace (the context enables it when it can)
        // and the window system has something to back them.
        chosen_format choose_format(VkPhysicalDevice device, VkSurfaceKHR surface, bool hdr)
        {
            std::uint32_t count = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(count);
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, formats.data());
            formats.resize(count);

            struct option
            {
                VkFormat format;
                VkColorSpaceKHR color_space;
                color_encoding encoding;
                bool needs_hdr;
            };

            // In order of preference. scRGB before HDR10: it is the working
            // space scaled by a constant, and what the Windows compositor
            // itself works in.
            constexpr option options[] = {
                {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, color_encoding::scrgb, true},
                {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT, color_encoding::hdr10, true},
                {VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT, color_encoding::hdr10, true},
                {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, color_encoding::srgb, false},
                {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, color_encoding::srgb, false},
            };

            for (const option &option : options)
            {
                if (option.needs_hdr && !hdr)
                    continue;

                for (const VkSurfaceFormatKHR &format : formats)
                {
                    if (format.format == option.format && format.colorSpace == option.color_space)
                        return {format, option.encoding};
                }
            }

            // Something unusual. Take any SDR format rather than none.
            for (const VkSurfaceFormatKHR &format : formats)
            {
                if (format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                    return {format, color_encoding::srgb};
            }
            return {};
        }
    } // namespace

    std::expected<void, std::string> swapchain::create(context &context, const platform::native_window &window)
    {
        if (m_context != nullptr)
            return std::unexpected("the swapchain is already created");
        if (!context.is_created())
            return std::unexpected("the swapchain needs a GPU context");

        m_context = &context;

        auto surface = create_surface(context.instance(), window);
        if (!surface)
        {
            m_context = nullptr;
            return std::unexpected(std::move(surface).error());
        }
        m_surface = *surface;

        // The context picked its graphics queue for being able to present to
        // this window system. Whether it can present to this very surface is
        // a separate question Vulkan wants asked.
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(context.physical_device(), context.graphics().family(), m_surface,
                                             &supported);
        if (supported != VK_TRUE)
        {
            destroy();
            return std::unexpected("the GPU cannot present to this window");
        }

        return {};
    }

    void swapchain::release()
    {
        if (m_context == nullptr)
            return;
        assert(!m_pending && "release of a swapchain acquired in the frame being recorded");

        const VkDevice device = m_context->device();
        if (m_swapchain != VK_NULL_HANDLE || !m_acquired.empty())
        {
            // The last frames that drew into the images may still be on the
            // GPU, and their presents wait on the images' semaphores.
            m_context->graphics().wait_idle();
        }

        destroy_targets();
        if (m_swapchain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(device, m_swapchain, nullptr);
        for (const VkSemaphore semaphore : m_acquired)
            vkDestroySemaphore(device, semaphore, nullptr);
        m_acquired.clear();

        m_swapchain = VK_NULL_HANDLE;
        m_format = {};
        m_output = {};
        m_extent = {};
        m_size = {};
        m_hdr = false;
        m_stale = true;
        m_image = 0;
    }

    void swapchain::destroy()
    {
        if (m_context == nullptr)
            return;

        release();
        if (m_surface != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(m_context->instance(), m_surface, nullptr);

        m_context = nullptr;
        m_surface = VK_NULL_HANDLE;
        m_last_frame = 0;
    }

    void swapchain::destroy_targets()
    {
        const VkDevice device = m_context->device();

        // The images themselves belong to the swapchain.
        for (const target &target : m_targets)
        {
            if (target.view != VK_NULL_HANDLE)
                vkDestroyImageView(device, target.view, nullptr);
            if (target.rendered != VK_NULL_HANDLE)
                vkDestroySemaphore(device, target.rendered, nullptr);
        }
        m_targets.clear();
    }

    // Replaces the swapchain with one that fits the surface as it is now.
    // Leaves m_extent at zero, and the swapchain stale, when the surface
    // currently has no area to draw into.
    std::expected<void, std::string> swapchain::rebuild(VkExtent2D size, const display &display,
                                                        std::size_t frame_slots)
    {
        const VkDevice device = m_context->device();

        // The frames in flight still draw into the old images. (The frame
        // being recorded has not been submitted, so this does not wait for
        // it; it has not touched this swapchain's images yet either.)
        m_context->graphics().wait_idle();

        m_size = size;
        m_hdr = display.hdr;
        m_extent = {};
        m_stale = true;

        // The frames' semaphores outlive the swapchains: made once, here,
        // the first time there is to be a swapchain.
        while (m_acquired.size() < frame_slots)
        {
            VkSemaphoreCreateInfo semaphore_info{};
            semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VkSemaphore semaphore = VK_NULL_HANDLE;
            if (const VkResult result = vkCreateSemaphore(device, &semaphore_info, nullptr, &semaphore);
                result != VK_SUCCESS)
                return failure("vkCreateSemaphore", result);
            m_acquired.push_back(semaphore);
        }

        VkSurfaceCapabilitiesKHR capabilities{};
        if (const VkResult result =
                vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_context->physical_device(), m_surface, &capabilities);
            result != VK_SUCCESS)
            return failure("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);

        VkExtent2D extent = capabilities.currentExtent;
        if (extent.width == undefined_extent)
        {
            extent.width = std::clamp(size.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            extent.height =
                std::clamp(size.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }

        // Minimized. The old swapchain stays until there is something to
        // replace it with.
        if (extent.width == 0 || extent.height == 0)
            return {};

        // One more than the minimum, so that acquiring an image does not have
        // to wait for the display to release one.
        std::uint32_t image_count = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount != 0)
            image_count = std::min(image_count, capabilities.maxImageCount);

        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if ((capabilities.supportedCompositeAlpha & alpha) == 0)
            alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;

        // Chosen anew every time: what the surface offers changes with the
        // display's mode.
        const chosen_format chosen = choose_format(m_context->physical_device(), m_surface, display.hdr);
        if (chosen.format.format == VK_FORMAT_UNDEFINED)
            return std::unexpected("the window surface offers no usable image format");
        m_format = chosen.format;
        m_output.encoding = chosen.encoding;

        const VkSwapchainKHR old = m_swapchain;

        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = m_surface;
        info.minImageCount = image_count;
        info.imageFormat = m_format.format;
        info.imageColorSpace = m_format.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = alpha;
        // Vsync. The one mode every implementation has, and the right one for
        // an editor: no tearing, and no frames drawn that are never shown.
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        info.oldSwapchain = old;

        m_swapchain = VK_NULL_HANDLE;
        const VkResult created = vkCreateSwapchainKHR(device, &info, nullptr, &m_swapchain);

        // The old swapchain is retired by the call above whether or not it
        // succeeded.
        destroy_targets();
        if (old != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(device, old, nullptr);

        if (created != VK_SUCCESS)
        {
            m_swapchain = VK_NULL_HANDLE;
            return failure("vkCreateSwapchainKHR", created);
        }

        // HDR10 images carry absolute luminance; this tells the display what
        // range to expect of them, so it can fit that to what it can show.
        if (m_output.encoding == color_encoding::hdr10 && m_context->has_hdr_metadata())
        {
            // An extension function, so not exported by the loader library.
            const auto set_metadata =
                reinterpret_cast<PFN_vkSetHdrMetadataEXT>(vkGetDeviceProcAddr(device, "vkSetHdrMetadataEXT"));
            if (set_metadata != nullptr)
            {
                VkHdrMetadataEXT metadata{};
                metadata.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT;
                metadata.displayPrimaryRed = {0.708f, 0.292f}; // BT.2020
                metadata.displayPrimaryGreen = {0.170f, 0.797f};
                metadata.displayPrimaryBlue = {0.131f, 0.046f};
                metadata.whitePoint = {0.3127f, 0.3290f}; // D65
                metadata.maxLuminance = hdr10_peak;
                metadata.minLuminance = 0.001f;
                metadata.maxContentLightLevel = hdr10_peak;
                metadata.maxFrameAverageLightLevel = hdr10_peak / 4.0f;
                set_metadata(device, 1, &m_swapchain, &metadata);
            }
        }

        std::uint32_t count = 0;
        vkGetSwapchainImagesKHR(device, m_swapchain, &count, nullptr);
        std::vector<VkImage> images(count);
        if (const VkResult result = vkGetSwapchainImagesKHR(device, m_swapchain, &count, images.data());
            result != VK_SUCCESS)
            return failure("vkGetSwapchainImagesKHR", result);

        m_targets.resize(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            target &target = m_targets[i];
            target.image = images[i];

            VkImageViewCreateInfo view_info{};
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = target.image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = m_format.format;
            view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (const VkResult result = vkCreateImageView(device, &view_info, nullptr, &target.view);
                result != VK_SUCCESS)
                return failure("vkCreateImageView", result);

            VkSemaphoreCreateInfo semaphore_info{};
            semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (const VkResult result = vkCreateSemaphore(device, &semaphore_info, nullptr, &target.rendered);
                result != VK_SUCCESS)
                return failure("vkCreateSemaphore", result);
        }

        m_extent = extent;
        m_stale = false;
        return {};
    }

} // namespace volt::gpu
