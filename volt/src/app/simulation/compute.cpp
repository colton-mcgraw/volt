#include <app/simulation/compute.hpp>

#include <gpu/context.hpp>
#include <gpu/pipeline.hpp>

#include <cstring>

namespace volt::simulation
{
    namespace
    {
        constexpr std::uint32_t relax_code[] =
#include <shaders/simulation_relax.comp.inc>
            ;

        // The shader's local_size_x.
        constexpr std::uint32_t group_size = 64;

        // simulation_relax.comp
        struct sweep_constants
        {
            std::uint32_t source;
            std::uint32_t destination;
            std::uint32_t count;
        };

        // Makes what the commands so far wrote to the buffers visible to
        // the sweeps that follow.
        void wait_for_writes(VkCommandBuffer commands, VkPipelineStageFlags2 stage, VkAccessFlags2 access)
        {
            VkMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            barrier.srcStageMask = stage;
            barrier.srcAccessMask = access;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(commands, &dependency);
        }
    } // namespace

    std::expected<void, std::string> compute::create(gpu::context &context)
    {
        if (m_context != nullptr)
            return std::unexpected("the simulation is already created");

        m_context = &context;
        const VkDevice device = context.device();

        const auto fail = [this](std::unexpected<std::string> error) {
            destroy();
            return error;
        };

        // Nodes, edges, potentials.
        VkDescriptorSetLayoutBinding bindings[3]{};
        for (std::uint32_t i = 0; i < 3; ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        VkDescriptorSetLayoutCreateInfo set_info{};
        set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_info.bindingCount = 3;
        set_info.pBindings = bindings;
        if (const VkResult result = vkCreateDescriptorSetLayout(device, &set_info, nullptr, &m_set_layout);
            result != VK_SUCCESS)
            return fail(gpu::failure("vkCreateDescriptorSetLayout", result));

        VkPushConstantRange constants{};
        constants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        constants.size = sizeof(sweep_constants);

        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &m_set_layout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &constants;
        if (const VkResult result = vkCreatePipelineLayout(device, &layout_info, nullptr, &m_layout);
            result != VK_SUCCESS)
            return fail(gpu::failure("vkCreatePipelineLayout", result));

        const auto pipeline = gpu::create_compute_pipeline(context, relax_code, m_layout);
        if (!pipeline)
            return fail(std::unexpected(pipeline.error()));
        m_pipeline = *pipeline;

        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
        VkDescriptorPoolCreateInfo descriptor_info{};
        descriptor_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        descriptor_info.maxSets = 1;
        descriptor_info.poolSizeCount = 1;
        descriptor_info.pPoolSizes = &size;
        if (const VkResult result = vkCreateDescriptorPool(device, &descriptor_info, nullptr, &m_descriptors);
            result != VK_SUCCESS)
            return fail(gpu::failure("vkCreateDescriptorPool", result));

        VkDescriptorSetAllocateInfo set_allocation{};
        set_allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        set_allocation.descriptorPool = m_descriptors;
        set_allocation.descriptorSetCount = 1;
        set_allocation.pSetLayouts = &m_set_layout;
        if (const VkResult result = vkAllocateDescriptorSets(device, &set_allocation, &m_set); result != VK_SUCCESS)
            return fail(gpu::failure("vkAllocateDescriptorSets", result));

        // Of the compute queue's family: command buffers only run on the
        // family they were allocated for.
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = context.compute().family();
        if (const VkResult result = vkCreateCommandPool(device, &pool_info, nullptr, &m_pool); result != VK_SUCCESS)
            return fail(gpu::failure("vkCreateCommandPool", result));

        VkCommandBufferAllocateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        buffer_info.commandPool = m_pool;
        buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        buffer_info.commandBufferCount = 1;
        if (const VkResult result = vkAllocateCommandBuffers(device, &buffer_info, &m_commands);
            result != VK_SUCCESS)
            return fail(gpu::failure("vkAllocateCommandBuffers", result));

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (const VkResult result = vkCreateFence(device, &fence_info, nullptr, &m_done); result != VK_SUCCESS)
            return fail(gpu::failure("vkCreateFence", result));

        return {};
    }

    void compute::destroy()
    {
        if (m_context == nullptr)
            return;

        const VkDevice device = m_context->device();
        destroy_network();

        if (m_done != VK_NULL_HANDLE)
            vkDestroyFence(device, m_done, nullptr);
        if (m_pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(device, m_pool, nullptr); // takes its command buffer along
        if (m_descriptors != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device, m_descriptors, nullptr); // and this its set
        if (m_pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(device, m_pipeline, nullptr);
        if (m_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device, m_layout, nullptr);
        if (m_set_layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, m_set_layout, nullptr);

        m_context = nullptr;
        m_done = VK_NULL_HANDLE;
        m_pool = VK_NULL_HANDLE;
        m_commands = VK_NULL_HANDLE;
        m_descriptors = VK_NULL_HANDLE;
        m_set = VK_NULL_HANDLE;
        m_pipeline = VK_NULL_HANDLE;
        m_layout = VK_NULL_HANDLE;
        m_set_layout = VK_NULL_HANDLE;
        m_remaining = 0;
    }

    void compute::destroy_network()
    {
        // A step may be running, and frames may be drawing from the state.
        m_context->compute().wait_idle();
        m_context->graphics().wait_idle();

        m_nodes.destroy();
        m_edges.destroy();
        m_state.destroy();

        m_count = 0;
        m_pending = false;
        m_front.reset();
    }

    std::expected<void, std::string> compute::load(const network &network)
    {
        destroy_network();
        if (network.nodes == 0)
            return {};

        const auto fail = [this](std::unexpected<std::string> error) {
            destroy_network();
            return error;
        };

        packed_network packed = pack(network);

        // A buffer cannot be empty, and a network may well have no branches.
        if (packed.edges.empty())
            packed.edges.emplace_back();

        // The network is written once, by the CPU; the potentials only ever
        // by the GPU, and read on both queues.
        gpu::buffer_description description;
        description.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        description.mapped = true;

        description.size = packed.nodes.size() * sizeof(packed_node);
        if (auto created = m_nodes.create(*m_context, description); !created)
            return fail(std::unexpected(std::move(created).error()));
        std::memcpy(m_nodes.data(), packed.nodes.data(), description.size);

        description.size = packed.edges.size() * sizeof(packed_edge);
        if (auto created = m_edges.create(*m_context, description); !created)
            return fail(std::unexpected(std::move(created).error()));
        std::memcpy(m_edges.data(), packed.edges.data(), description.size);

        description.size = VkDeviceSize{network.nodes} * (result_sets + scratch_sets) * sizeof(float);
        description.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        description.mapped = false;
        description.shared = true;
        if (auto created = m_state.create(*m_context, description); !created)
            return fail(std::unexpected(std::move(created).error()));

        const VkDescriptorBufferInfo buffers[3] = {
            {m_nodes.handle(), 0, VK_WHOLE_SIZE},
            {m_edges.handle(), 0, VK_WHOLE_SIZE},
            {m_state.handle(), 0, VK_WHOLE_SIZE},
        };

        VkWriteDescriptorSet writes[3]{};
        for (std::uint32_t i = 0; i < 3; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = m_set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buffers[i];
        }
        vkUpdateDescriptorSets(m_context->device(), 3, writes, 0, nullptr);

        m_count = network.nodes;
        return {};
    }

    std::optional<compute::values> compute::latest() const noexcept
    {
        if (!m_front)
            return std::nullopt;
        return values{m_state.handle(), *m_front * m_count, m_count};
    }

    std::expected<void, std::string> compute::advance()
    {
        if (m_count == 0)
            return {};

        if (m_pending)
        {
            const VkResult status = vkGetFenceStatus(m_context->device(), m_done);
            if (status == VK_NOT_READY)
                return {}; // still running; look again next frame
            if (status != VK_SUCCESS)
                return gpu::failure("vkGetFenceStatus", status);

            m_pending = false;
            m_front = m_writing;
        }

        if (m_remaining == 0)
            return {};

        --m_remaining;
        return start_step();
    }

    std::expected<void, std::string> compute::start_step()
    {
        const VkDevice device = m_context->device();

        // The first step of a network has nothing to continue from.
        const bool first = !m_front;
        m_writing = first ? 0 : (*m_front + 1) % result_sets;

        vkResetCommandPool(device, m_pool, 0);

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (const VkResult result = vkBeginCommandBuffer(m_commands, &begin_info); result != VK_SUCCESS)
            return gpu::failure("vkBeginCommandBuffer", result);

        if (first)
        {
            vkCmdFillBuffer(m_commands, m_state.handle(), 0, VK_WHOLE_SIZE, 0);
            wait_for_writes(m_commands, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        }

        vkCmdBindPipeline(m_commands, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(m_commands, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &m_set, 0, nullptr);

        // From the latest result, back and forth between the two scratch
        // sets, and with the last sweep into the next result.
        const auto scratch = [](std::uint32_t sweep) { return result_sets + sweep % scratch_sets; };
        const std::uint32_t groups = (m_count + group_size - 1) / group_size;

        for (std::uint32_t sweep = 0; sweep < sweeps_per_step; ++sweep)
        {
            // A first step reads zeros, wherever it reads them.
            const std::uint32_t source = sweep != 0 ? scratch(sweep - 1) : first ? result_sets - 1 : *m_front;
            const std::uint32_t destination = sweep != sweeps_per_step - 1 ? scratch(sweep) : m_writing;

            const sweep_constants constants{source * m_count, destination * m_count, m_count};
            vkCmdPushConstants(m_commands, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
            vkCmdDispatch(m_commands, groups, 1, 1);

            wait_for_writes(m_commands, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }

        if (const VkResult result = vkEndCommandBuffer(m_commands); result != VK_SUCCESS)
            return gpu::failure("vkEndCommandBuffer", result);

        VkCommandBufferSubmitInfo commands{};
        commands.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        commands.commandBuffer = m_commands;

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &commands;

        vkResetFences(device, 1, &m_done);
        if (const VkResult result = m_context->compute().submit(submit, m_done); result != VK_SUCCESS)
            return gpu::failure("vkQueueSubmit2", result);

        m_pending = true;
        return {};
    }

} // namespace volt::simulation
