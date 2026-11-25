#include <gpu/target.hpp>

#include <gpu/buffer.hpp>
#include <gpu/context.hpp>

#include <cassert>

namespace volt::gpu
{
    namespace
    {
        bool is_depth_attachment(VkPhysicalDevice device, VkFormat format)
        {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(device, format, &properties);
            return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        }

        VkImageMemoryBarrier2 barrier_for(VkImage image, VkImageAspectFlags aspect)
        {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = {aspect, 0, 1, 0, 1};
            return barrier;
        }
    } // namespace

    std::expected<void, std::string> target::create(context &context, bool depth)
    {
        if (m_context != nullptr)
            return std::unexpected("the target is already created");

        if (depth)
        {
            // Vulkan guarantees one of the two, not which.
            if (is_depth_attachment(context.physical_device(), VK_FORMAT_D32_SFLOAT))
                m_depth_format = VK_FORMAT_D32_SFLOAT;
            else if (is_depth_attachment(context.physical_device(), VK_FORMAT_X8_D24_UNORM_PACK32))
                m_depth_format = VK_FORMAT_X8_D24_UNORM_PACK32;
            else
                return std::unexpected("the GPU has no usable depth format");
        }

        m_context = &context;
        return {};
    }

    void target::destroy()
    {
        if (m_context == nullptr)
            return;

        destroy_images();
        m_context = nullptr;
        m_depth_format = VK_FORMAT_UNDEFINED;
    }

    void target::destroy_images()
    {
        const VkDevice device = m_context->device();

        for (image *image : {&m_color, &m_depth})
        {
            if (image->view != VK_NULL_HANDLE)
                vkDestroyImageView(device, image->view, nullptr);
            if (image->handle != VK_NULL_HANDLE)
                vkDestroyImage(device, image->handle, nullptr);
            if (image->memory != VK_NULL_HANDLE)
                vkFreeMemory(device, image->memory, nullptr);
            *image = {};
        }

        m_extent = {};
        m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    std::expected<void, std::string> target::create_image(image &image, VkFormat format, VkImageUsageFlags usage,
                                                          VkImageAspectFlags aspect)
    {
        const VkDevice device = m_context->device();

        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {m_extent.width, m_extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (const VkResult result = vkCreateImage(device, &info, nullptr, &image.handle); result != VK_SUCCESS)
            return failure("vkCreateImage", result);

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, image.handle, &requirements);

        const auto type = find_memory_type(m_context->physical_device(), requirements.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!type)
            return std::unexpected("the GPU has no memory type fit for a render target");

        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = *type;
        if (const VkResult result = vkAllocateMemory(device, &allocation, nullptr, &image.memory);
            result != VK_SUCCESS)
            return failure("vkAllocateMemory", result);

        if (const VkResult result = vkBindImageMemory(device, image.handle, image.memory, 0); result != VK_SUCCESS)
            return failure("vkBindImageMemory", result);

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = image.handle;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange = {aspect, 0, 1, 0, 1};
        if (const VkResult result = vkCreateImageView(device, &view_info, nullptr, &image.view);
            result != VK_SUCCESS)
            return failure("vkCreateImageView", result);

        return {};
    }

    std::expected<void, std::string> target::resize(VkExtent2D extent)
    {
        assert(m_context != nullptr && "resize on a target that was not created");

        if (extent.width == m_extent.width && extent.height == m_extent.height)
            return {};
        if (extent.width == 0 || extent.height == 0)
            return std::unexpected("a render target cannot be empty");

        if (m_color.handle != VK_NULL_HANDLE)
        {
            m_context->graphics().wait_idle();
            destroy_images();
        }

        m_extent = extent;

        auto created = create_image(m_color, color_format,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
        if (created && m_depth_format != VK_FORMAT_UNDEFINED)
        {
            created = create_image(m_depth, m_depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                   VK_IMAGE_ASPECT_DEPTH_BIT);
        }

        if (!created)
            destroy_images();
        return created;
    }

    void target::begin(VkCommandBuffer commands, const math::vec4f &clear)
    {
        assert(m_color.handle != VK_NULL_HANDLE && "begin on a target without a size");

        // Both images are cleared, so their old contents can be dropped:
        // that is what coming from the undefined layout means. What has to
        // be waited for is whoever used them last: the interface sampling
        // the color image, the previous pass testing against the depth.
        VkImageMemoryBarrier2 barriers[2];
        std::uint32_t barrier_count = 0;

        VkImageMemoryBarrier2 &color = barriers[barrier_count++];
        color = barrier_for(m_color.handle, VK_IMAGE_ASPECT_COLOR_BIT);
        color.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        color.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        color.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        color.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        color.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        if (m_depth.handle != VK_NULL_HANDLE)
        {
            const VkPipelineStageFlags2 tests =
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

            VkImageMemoryBarrier2 &depth = barriers[barrier_count++];
            depth = barrier_for(m_depth.handle, VK_IMAGE_ASPECT_DEPTH_BIT);
            depth.srcStageMask = tests;
            depth.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            depth.dstStageMask = tests;
            depth.dstAccessMask =
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            depth.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            depth.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        }

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = barrier_count;
        dependency.pImageMemoryBarriers = barriers;
        vkCmdPipelineBarrier2(commands, &dependency);

        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = m_color.view;
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color = {{clear[0], clear[1], clear[2], clear[3]}};

        // Nobody reads the depth buffer once the pass is over.
        VkRenderingAttachmentInfo depth_attachment{};
        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment.imageView = m_depth.view;
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_attachment.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, m_extent};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color_attachment;
        rendering.pDepthAttachment = m_depth.handle != VK_NULL_HANDLE ? &depth_attachment : nullptr;
        vkCmdBeginRendering(commands, &rendering);

        const VkViewport viewport{
            0.0f, 0.0f, static_cast<float>(m_extent.width), static_cast<float>(m_extent.height), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, m_extent};
        vkCmdSetViewport(commands, 0, 1, &viewport);
        vkCmdSetScissor(commands, 0, 1, &scissor);

        m_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    void target::end(VkCommandBuffer commands)
    {
        assert(m_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && "end without begin");

        vkCmdEndRendering(commands);

        VkImageMemoryBarrier2 barrier = barrier_for(m_color.handle, VK_IMAGE_ASPECT_COLOR_BIT);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commands, &dependency);

        m_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

} // namespace volt::gpu
