#include <gpu/presenter.hpp>

#include <gpu/context.hpp>

#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>

namespace volt::gpu
{
    namespace
    {
        constexpr std::uint64_t forever = std::numeric_limits<std::uint64_t>::max();

        bool same(VkExtent2D a, VkExtent2D b) noexcept
        {
            return a.width == b.width && a.height == b.height;
        }

        // Moves a swapchain image between the layout it is drawn in and the
        // one it is displayed in.
        void transition(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to)
        {
            const bool to_present = to == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            // The same stage the submission waits for the image in, so the
            // transition cannot run before the image has been acquired.
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.srcAccessMask = to_present ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_NONE;
            barrier.dstStageMask =
                to_present ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.dstAccessMask = to_present ? VK_ACCESS_2_NONE : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.oldLayout = from;
            barrier.newLayout = to;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(commands, &dependency);
        }
    } // namespace

    std::expected<void, std::string> presenter::create(context &context)
    {
        if (m_context != nullptr)
            return std::unexpected("the presenter is already created");
        if (!context.is_created())
            return std::unexpected("the presenter needs a GPU context");

        m_context = &context;
        const VkDevice device = context.device();

        const auto fail = [this](std::unexpected<std::string> error) {
            destroy();
            return error;
        };

        for (slot &slot : m_slots)
        {
            // Each frame has a pool of its own, so that starting a frame can
            // drop everything the slot recorded last time with one reset.
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pool_info.queueFamilyIndex = context.graphics().family();
            if (const VkResult result = vkCreateCommandPool(device, &pool_info, nullptr, &slot.pool);
                result != VK_SUCCESS)
                return fail(failure("vkCreateCommandPool", result));

            VkCommandBufferAllocateInfo buffer_info{};
            buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            buffer_info.commandPool = slot.pool;
            buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            buffer_info.commandBufferCount = 1;
            if (const VkResult result = vkAllocateCommandBuffers(device, &buffer_info, &slot.commands);
                result != VK_SUCCESS)
                return fail(failure("vkAllocateCommandBuffers", result));

            // Signaled, as if a frame had already finished: the first frame
            // in each slot waits on it like every other.
            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (const VkResult result = vkCreateFence(device, &fence_info, nullptr, &slot.done); result != VK_SUCCESS)
                return fail(failure("vkCreateFence", result));
        }

        return {};
    }

    void presenter::destroy()
    {
        if (m_context == nullptr)
            return;

        const VkDevice device = m_context->device();
        m_context->graphics().wait_idle();

        for (gpu::swapchain *acquired : m_acquired)
            acquired->m_pending = false;
        m_acquired.clear();

        for (slot &slot : m_slots)
        {
            if (slot.done != VK_NULL_HANDLE)
                vkDestroyFence(device, slot.done, nullptr);
            if (slot.pool != VK_NULL_HANDLE)
                vkDestroyCommandPool(device, slot.pool, nullptr); // takes its command buffer along
            slot = {};
        }

        m_context = nullptr;
        m_slot = 0;
        m_recording = false;
    }

    std::expected<frame, std::string> presenter::begin_frame()
    {
        assert(m_context != nullptr && "begin_frame on a presenter that was not created");
        assert(!m_recording && "begin_frame without end_frame");

        const VkDevice device = m_context->device();
        slot &slot = m_slots[m_slot];

        // The frame that used this slot last has to be through the GPU before
        // its command buffer can be recorded again.
        if (const VkResult result = vkWaitForFences(device, 1, &slot.done, VK_TRUE, forever); result != VK_SUCCESS)
            return failure("vkWaitForFences", result);

        // The fence itself is reset only in end_frame(), right before the
        // submission that signals it again: a frame that failed on the way
        // must not leave this slot blocked forever.
        vkResetCommandPool(device, slot.pool, 0);

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (const VkResult result = vkBeginCommandBuffer(slot.commands, &begin_info); result != VK_SUCCESS)
            return failure("vkBeginCommandBuffer", result);

        m_recording = true;
        m_acquired.clear();

        frame result;
        result.commands = slot.commands;
        result.slot = m_slot;
        result.number = ++m_frames;
        return result;
    }

    std::expected<acquisition, std::string> presenter::acquire(swapchain &swapchain, VkExtent2D size,
                                                              const display &display, std::uint64_t timeout_ns)
    {
        assert(m_recording && "acquire outside a frame");
        assert(swapchain.m_context == m_context && "acquire of a swapchain that was not created");
        if (swapchain.m_pending)
            return std::unexpected("the swapchain was acquired twice in one frame");

        const VkDevice device = m_context->device();

        // A window that is being resized can outrun the swapchain a few times
        // in a row. Past that, leave it out of the frame; the resize asks for
        // another.
        bool acquired = false;
        for (int attempt = 0; attempt < 4 && !acquired; ++attempt)
        {
            if (swapchain.m_stale || !same(size, swapchain.m_size) || display.hdr != swapchain.m_hdr)
            {
                if (auto rebuilt = swapchain.rebuild(size, display, frames_in_flight); !rebuilt)
                    return std::unexpected(std::move(rebuilt).error());
            }

            if (swapchain.m_extent.width == 0)
                return acquisition{};

            const VkResult result = vkAcquireNextImageKHR(device, swapchain.m_swapchain, timeout_ns,
                                                          swapchain.m_acquired[m_slot], VK_NULL_HANDLE,
                                                          &swapchain.m_image);
            if (result == VK_ERROR_OUT_OF_DATE_KHR)
            {
                swapchain.m_stale = true;
                continue;
            }

            // No image in time: nothing was signalled, the semaphore stays
            // free for the next try.
            if (result == VK_TIMEOUT || result == VK_NOT_READY)
                return acquisition{.image = std::nullopt, .busy = true};

            // Suboptimal still gives a usable image: draw this frame, rebuild
            // before the next.
            if (result == VK_SUBOPTIMAL_KHR)
                swapchain.m_stale = true;
            else if (result != VK_SUCCESS)
                return failure("vkAcquireNextImageKHR", result);

            acquired = true;
        }

        if (!acquired)
            return acquisition{};

        swapchain.m_pending = true;
        swapchain.m_last_frame = m_frames;
        m_acquired.push_back(&swapchain);

        const slot &slot = m_slots[m_slot];
        const gpu::swapchain::target &target = swapchain.m_targets[swapchain.m_image];
        transition(slot.commands, target.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        window_image result;
        result.image = target.image;
        result.view = target.view;
        result.format = swapchain.m_format.format;
        result.extent = swapchain.m_extent;

        // The white level needs no new swapchain; it is only a number the
        // encoding uses.
        swapchain.m_output.white =
            swapchain.m_output.encoding == color_encoding::srgb ? scrgb_white : display.white;
        result.output = swapchain.m_output;
        return acquisition{.image = result, .busy = false};
    }

    std::expected<std::size_t, std::string> presenter::end_frame()
    {
        assert(m_recording && "end_frame without begin_frame");
        m_recording = false;

        const VkDevice device = m_context->device();
        const std::size_t index = m_slot;
        slot &slot = m_slots[index];
        m_slot = (m_slot + 1) % frames_in_flight;

        // Whatever happens below, the swapchains are no longer held by a
        // frame being recorded.
        std::vector<gpu::swapchain *> acquired = std::move(m_acquired);
        m_acquired.clear();
        for (gpu::swapchain *swapchain : acquired)
            swapchain->m_pending = false;

        std::vector<VkSemaphoreSubmitInfo> waits;
        std::vector<VkSemaphoreSubmitInfo> signals;
        std::vector<VkSemaphore> rendered;
        std::vector<VkSwapchainKHR> handles;
        std::vector<std::uint32_t> indices;
        for (gpu::swapchain *swapchain : acquired)
        {
            const gpu::swapchain::target &target = swapchain->m_targets[swapchain->m_image];
            transition(slot.commands, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

            VkSemaphoreSubmitInfo wait{};
            wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            wait.semaphore = swapchain->m_acquired[index];
            wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            waits.push_back(wait);

            VkSemaphoreSubmitInfo signal{};
            signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            signal.semaphore = target.rendered;
            signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            signals.push_back(signal);

            rendered.push_back(target.rendered);
            handles.push_back(swapchain->m_swapchain);
            indices.push_back(swapchain->m_image);
        }

        if (const VkResult result = vkEndCommandBuffer(slot.commands); result != VK_SUCCESS)
            return failure("vkEndCommandBuffer", result);

        VkCommandBufferSubmitInfo commands{};
        commands.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        commands.commandBuffer = slot.commands;

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = static_cast<std::uint32_t>(waits.size());
        submit.pWaitSemaphoreInfos = waits.data();
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &commands;
        submit.signalSemaphoreInfoCount = static_cast<std::uint32_t>(signals.size());
        submit.pSignalSemaphoreInfos = signals.data();

        // Only now that the submission that signals it again is certain to
        // be made.
        vkResetFences(device, 1, &slot.done);
        if (const VkResult result = m_context->graphics().submit(submit, slot.done); result != VK_SUCCESS)
            return failure("vkQueueSubmit2", result);

        if (acquired.empty())
            return 0;

        std::vector<VkResult> results(acquired.size(), VK_SUCCESS);

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = static_cast<std::uint32_t>(rendered.size());
        present.pWaitSemaphores = rendered.data();
        present.swapchainCount = static_cast<std::uint32_t>(handles.size());
        present.pSwapchains = handles.data();
        present.pImageIndices = indices.data();
        present.pResults = results.data();

        // Each swapchain gets a result of its own. Out of date or suboptimal
        // means the image was queued (or the window changed under it, in
        // which case the next frame replaces it anyway): only that
        // swapchain is rebuilt, at its next acquire.
        const VkResult overall = m_context->graphics().present(present);
        for (std::size_t i = 0; i < acquired.size(); ++i)
        {
            if (results[i] == VK_ERROR_OUT_OF_DATE_KHR || results[i] == VK_SUBOPTIMAL_KHR)
                acquired[i]->m_stale = true;
            else if (results[i] != VK_SUCCESS)
                return failure("vkQueuePresentKHR", results[i]);
        }

        // A failure that is no swapchain's own (the device lost).
        if (overall != VK_SUCCESS && overall != VK_SUBOPTIMAL_KHR && overall != VK_ERROR_OUT_OF_DATE_KHR)
            return failure("vkQueuePresentKHR", overall);

        return acquired.size();
    }

} // namespace volt::gpu
