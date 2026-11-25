#include <gpu/texture.hpp>

#include <gpu/context.hpp>

#include <cassert>
#include <cstring>
#include <limits>
#include <vector>

namespace volt::gpu
{
    namespace
    {
        constexpr std::uint64_t forever = std::numeric_limits<std::uint64_t>::max();

        VkFormat vulkan_format(texture_format format) noexcept
        {
            switch (format)
            {
            case texture_format::r8_unorm:
                return VK_FORMAT_R8_UNORM;
            case texture_format::rgba8_srgb:
                return VK_FORMAT_R8G8B8A8_SRGB;
            case texture_format::rgba8_unorm:
                return VK_FORMAT_R8G8B8A8_UNORM;
            }
            return VK_FORMAT_UNDEFINED;
        }

        VkImageMemoryBarrier2 barrier_for(VkImage image, std::uint32_t levels)
        {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
            return barrier;
        }

        void submit_barrier(VkCommandBuffer commands, const VkImageMemoryBarrier2 &barrier)
        {
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(commands, &dependency);
        }
    } // namespace

    std::size_t bytes_per_pixel(texture_format format) noexcept
    {
        return format == texture_format::r8_unorm ? 1u : 4u;
    }

    std::expected<void, std::string> texture::create_image(context &context, std::uint32_t width,
                                                           std::uint32_t height, texture_format format,
                                                           std::uint32_t levels)
    {
        if (m_context != nullptr)
            return std::unexpected("the texture is already created");
        if (width == 0 || height == 0)
            return std::unexpected("a texture cannot be empty");

        m_context = &context;
        m_width = width;
        m_height = height;
        m_levels = levels;
        m_format = format;
        const VkDevice device = context.device();

        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = vulkan_format(format);
        info.extent = {width, height, 1};
        info.mipLevels = levels;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (const VkResult result = vkCreateImage(device, &info, nullptr, &m_image); result != VK_SUCCESS)
            return failure("vkCreateImage", result);

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, m_image, &requirements);

        const auto type = find_memory_type(context.physical_device(), requirements.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!type)
            return std::unexpected("the GPU has no memory type fit for a texture");

        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = *type;
        if (const VkResult result = vkAllocateMemory(device, &allocation, nullptr, &m_memory); result != VK_SUCCESS)
            return failure("vkAllocateMemory", result);

        if (const VkResult result = vkBindImageMemory(device, m_image, m_memory, 0); result != VK_SUCCESS)
            return failure("vkBindImageMemory", result);

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = m_image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = info.format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};

        // A single channel reads as (r, 0, 0, 1) unless told otherwise; the
        // shaders only ever look at r, but a mask that reads the same in
        // every channel is the least surprising thing to sample.
        if (format == texture_format::r8_unorm)
        {
            view_info.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                                    VK_COMPONENT_SWIZZLE_R};
        }
        if (const VkResult result = vkCreateImageView(device, &view_info, nullptr, &m_view); result != VK_SUCCESS)
            return failure("vkCreateImageView", result);

        m_staging.create(context, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return {};
    }

    std::expected<void, std::string> texture::create(context &context, std::uint32_t width, std::uint32_t height,
                                                     texture_format format)
    {
        auto created = create_image(context, width, height, format, 1);
        if (!created && m_context == &context)
            destroy();
        return created;
    }

    std::expected<void, std::string> texture::create(context &context, std::uint32_t width, std::uint32_t height,
                                                     texture_format format, std::span<const std::byte> pixels)
    {
        const std::span<const std::byte> levels[] = {pixels};
        return create(context, width, height, format, levels);
    }

    std::expected<void, std::string> texture::create(context &context, std::uint32_t width, std::uint32_t height,
                                                     texture_format format,
                                                     std::span<const std::span<const std::byte>> levels)
    {
        if (m_context != nullptr)
            return std::unexpected("the texture is already created");
        if (levels.empty())
            return std::unexpected("a texture needs at least one level of pixels");

        // Where each level goes in the staging buffer. Offsets are kept to
        // multiples of 16, more than any copy from a buffer asks for.
        std::vector<level_copy> copies;
        VkDeviceSize size = 0;
        std::uint32_t level_width = width;
        std::uint32_t level_height = height;
        for (const std::span<const std::byte> &level : levels)
        {
            const std::size_t bytes = std::size_t{level_width} * level_height * bytes_per_pixel(format);
            if (level.size() < bytes)
                return std::unexpected("the texture's pixels are fewer than its size needs");
            copies.push_back({size, level_width, level_height});
            size = (size + bytes + 15) & ~VkDeviceSize{15};

            if (level_width == 1 && level_height == 1 && copies.size() < levels.size())
                return std::unexpected("a texture has more levels than halving its size allows");
            level_width = level_width > 1 ? level_width / 2 : 1;
            level_height = level_height > 1 ? level_height / 2 : 1;
        }

        if (width == 0 || height == 0)
            return std::unexpected("a texture cannot be empty");
        if (auto created = create_image(context, width, height, format, static_cast<std::uint32_t>(levels.size()));
            !created)
        {
            if (m_context == &context)
                destroy();
            return created;
        }

        const VkDevice device = context.device();
        const auto fail = [this](std::unexpected<std::string> error) {
            destroy();
            return error;
        };

        // A staging buffer and a command buffer of its own, gone once the
        // copy is through.
        buffer staging;
        buffer_description description;
        description.size = size;
        description.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        description.mapped = true;
        if (auto made = staging.create(context, description); !made)
            return fail(std::unexpected(std::move(made).error()));
        for (std::size_t i = 0; i < copies.size(); ++i)
        {
            const std::size_t bytes = std::size_t{copies[i].width} * copies[i].height * bytes_per_pixel(format);
            std::memcpy(staging.data() + copies[i].offset, levels[i].data(), bytes);
        }

        VkCommandPool pool = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        const auto release = [&] {
            if (fence != VK_NULL_HANDLE)
                vkDestroyFence(device, fence, nullptr);
            if (pool != VK_NULL_HANDLE)
                vkDestroyCommandPool(device, pool, nullptr);
        };

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = context.graphics().family();
        if (const VkResult result = vkCreateCommandPool(device, &pool_info, nullptr, &pool); result != VK_SUCCESS)
            return fail(failure("vkCreateCommandPool", result));

        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        if (const VkResult result = vkAllocateCommandBuffers(device, &allocation, &commands); result != VK_SUCCESS)
        {
            release();
            return fail(failure("vkAllocateCommandBuffers", result));
        }

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (const VkResult result = vkCreateFence(device, &fence_info, nullptr, &fence); result != VK_SUCCESS)
        {
            release();
            return fail(failure("vkCreateFence", result));
        }

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commands, &begin);
        record_upload(commands, staging.handle(), {0, 0, width, height}, copies);
        if (const VkResult result = vkEndCommandBuffer(commands); result != VK_SUCCESS)
        {
            release();
            return fail(failure("vkEndCommandBuffer", result));
        }

        VkCommandBufferSubmitInfo command_info{};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        command_info.commandBuffer = commands;

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &command_info;

        VkResult result = context.graphics().submit(submit, fence);
        if (result == VK_SUCCESS)
            result = vkWaitForFences(device, 1, &fence, VK_TRUE, forever);

        release();
        if (result != VK_SUCCESS)
            return fail(failure("uploading a texture", result));
        return {};
    }

    void texture::destroy()
    {
        if (m_context == nullptr)
            return;

        // Frames in flight may still be sampling it.
        m_context->graphics().wait_idle();
        destroy_unused();
    }

    void texture::destroy_unused()
    {
        if (m_context == nullptr)
            return;

        const VkDevice device = m_context->device();

        m_staging.destroy();
        if (m_view != VK_NULL_HANDLE)
            vkDestroyImageView(device, m_view, nullptr);
        if (m_image != VK_NULL_HANDLE)
            vkDestroyImage(device, m_image, nullptr);
        if (m_memory != VK_NULL_HANDLE)
            vkFreeMemory(device, m_memory, nullptr);

        m_context = nullptr;
        m_view = VK_NULL_HANDLE;
        m_image = VK_NULL_HANDLE;
        m_memory = VK_NULL_HANDLE;
        m_width = 0;
        m_height = 0;
        m_levels = 1;
        m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    void texture::record_upload(VkCommandBuffer commands, VkBuffer staging, const texture_region &region,
                                std::span<const level_copy> levels)
    {
        const bool fresh = m_layout == VK_IMAGE_LAYOUT_UNDEFINED;

        // Whoever sampled the image before (an earlier frame's interface
        // pass) has to be done with it before the copy writes. A fresh image
        // has nothing to keep, so its contents may be dropped.
        VkImageMemoryBarrier2 to_copy = barrier_for(m_image, m_levels);
        to_copy.srcStageMask = fresh ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        to_copy.srcAccessMask = VK_ACCESS_2_NONE;
        to_copy.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
        to_copy.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_copy.oldLayout = fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        submit_barrier(commands, to_copy);

        const bool whole = !levels.empty() || (region.x == 0 && region.y == 0 && region.width == m_width &&
                                               region.height == m_height);
        if (fresh && !whole)
        {
            // What the copy does not cover would otherwise be garbage.
            const VkClearColorValue zero{};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(commands, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);

            VkImageMemoryBarrier2 after_clear = barrier_for(m_image, m_levels);
            after_clear.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            after_clear.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            after_clear.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            after_clear.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            after_clear.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            after_clear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            submit_barrier(commands, after_clear);
        }

        std::vector<VkBufferImageCopy> copies;
        if (levels.empty())
        {
            VkBufferImageCopy &copy = copies.emplace_back();
            copy.bufferOffset = 0;
            copy.bufferRowLength = 0; // tightly packed
            copy.bufferImageHeight = 0;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageOffset = {static_cast<std::int32_t>(region.x), static_cast<std::int32_t>(region.y), 0};
            copy.imageExtent = {region.width, region.height, 1};
        }
        for (std::size_t level = 0; level < levels.size(); ++level)
        {
            VkBufferImageCopy &copy = copies.emplace_back();
            copy.bufferOffset = levels[level].offset;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<std::uint32_t>(level), 0, 1};
            copy.imageExtent = {levels[level].width, levels[level].height, 1};
        }
        vkCmdCopyBufferToImage(commands, staging, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(copies.size()), copies.data());

        VkImageMemoryBarrier2 to_read = barrier_for(m_image, m_levels);
        to_read.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_read.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        submit_barrier(commands, to_read);

        m_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    std::expected<void, std::string> texture::update(const frame &frame, const texture_region &region,
                                                     std::span<const std::byte> pixels, std::size_t stride)
    {
        assert(m_context != nullptr && "update on a texture that was not created");

        if (m_levels != 1)
            return std::unexpected("only a texture of one level can be updated");

        if (region.width == 0 || region.height == 0)
            return std::unexpected("a texture update cannot be empty");
        if (region.x + region.width > m_width || region.y + region.height > m_height)
            return std::unexpected("a texture update reaches outside the texture");

        const std::size_t row = std::size_t{region.width} * bytes_per_pixel(m_format);
        if (stride < row || pixels.size() < stride * (region.height - 1) + row)
            return std::unexpected("a texture update has fewer pixels than its region");

        // The region's rows, packed, into the slot's staging memory.
        std::vector<std::byte> packed(row * region.height);
        for (std::uint32_t y = 0; y < region.height; ++y)
            std::memcpy(packed.data() + row * y, pixels.data() + stride * y, row);

        const auto staging = m_staging.write(frame.slot, std::span<const std::byte>(packed));
        if (!staging)
            return std::unexpected(staging.error());

        record_upload(frame.commands, *staging, region);
        return {};
    }

} // namespace volt::gpu
