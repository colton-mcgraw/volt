#include <app/schematic/renderer.hpp>

#include <gpu/context.hpp>
#include <gpu/pipeline.hpp>

#include <cstddef>
#include <cstring>

namespace volt::schematic
{
    namespace
    {
        constexpr std::uint32_t grid_vertex[] =
#include <shaders/schematic_grid.vert.inc>
            ;
        constexpr std::uint32_t grid_fragment[] =
#include <shaders/schematic_grid.frag.inc>
            ;
        constexpr std::uint32_t segment_vertex[] =
#include <shaders/schematic_segment.vert.inc>
            ;
        constexpr std::uint32_t segment_fragment[] =
#include <shaders/schematic_segment.frag.inc>
            ;

        // A segment as the vertex shader gets it (schematic_segment.vert).
        struct instance
        {
            float ends[4];
            float color[4];
            float width;
            std::int32_t node;
        };

        constexpr VkVertexInputAttributeDescription attributes[] = {
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(instance, ends)},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(instance, color)},
            {2, 0, VK_FORMAT_R32_SFLOAT, offsetof(instance, width)},
            {3, 0, VK_FORMAT_R32_SINT, offsetof(instance, node)},
        };

        // schematic.glsl. The padding is the shader's: a vec4 starts at a
        // multiple of 16 bytes.
        struct pass_constants
        {
            float viewport[2];
            float center[2];
            float zoom;
            float grid;
            std::int32_t values;
            float low;
            float high;
            float padding[3];
            float grid_color[4];
        };
        static_assert(offsetof(pass_constants, grid_color) == 48);

        constexpr VkShaderStageFlags pass_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    } // namespace

    std::expected<void, std::string> renderer::create(gpu::context &context)
    {
        if (m_context != nullptr)
            return std::unexpected("the schematic renderer is already created");

        m_context = &context;
        const VkDevice device = context.device();

        const auto fail = [this](std::unexpected<std::string> error) {
            destroy();
            return error;
        };

        if (auto created = m_target.create(context, false); !created)
            return fail(std::unexpected(std::move(created).error()));

        // The simulation's values, read where the segments get their color.
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo set_info{};
        set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_info.bindingCount = 1;
        set_info.pBindings = &binding;
        if (const VkResult result = vkCreateDescriptorSetLayout(device, &set_info, nullptr, &m_set_layout);
            result != VK_SUCCESS)
            return fail(gpu::failure("vkCreateDescriptorSetLayout", result));

        VkPushConstantRange constants{};
        constants.stageFlags = pass_stages;
        constants.size = sizeof(pass_constants);

        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &m_set_layout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &constants;
        if (const VkResult result = vkCreatePipelineLayout(device, &layout_info, nullptr, &m_layout);
            result != VK_SUCCESS)
            return fail(gpu::failure("vkCreatePipelineLayout", result));

        gpu::graphics_pipeline_description description;
        description.layout = m_layout;
        description.color_format = gpu::target::color_format;
        description.blending = gpu::blending::premultiplied;

        description.vertex = grid_vertex;
        description.fragment = grid_fragment;
        description.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        const auto grid = gpu::create_graphics_pipeline(context, description);
        if (!grid)
            return fail(std::unexpected(grid.error()));
        m_grid = *grid;

        description.vertex = segment_vertex;
        description.fragment = segment_fragment;
        description.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        description.instance_stride = sizeof(instance);
        description.attributes = attributes;
        const auto segments = gpu::create_graphics_pipeline(context, description);
        if (!segments)
            return fail(std::unexpected(segments.error()));
        m_segments = *segments;

        gpu::buffer_description no_values;
        no_values.size = sizeof(float);
        no_values.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        no_values.mapped = true;
        if (auto created = m_no_values.create(context, no_values); !created)
            return fail(std::unexpected(std::move(created).error()));
        std::memset(m_no_values.data(), 0, sizeof(float));

        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                        static_cast<std::uint32_t>(m_sets.size())};
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = static_cast<std::uint32_t>(m_sets.size());
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &size;
        if (const VkResult result = vkCreateDescriptorPool(device, &pool_info, nullptr, &m_descriptors);
            result != VK_SUCCESS)
            return fail(gpu::failure("vkCreateDescriptorPool", result));

        for (VkDescriptorSet &set : m_sets)
        {
            VkDescriptorSetAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocation.descriptorPool = m_descriptors;
            allocation.descriptorSetCount = 1;
            allocation.pSetLayouts = &m_set_layout;
            if (const VkResult result = vkAllocateDescriptorSets(device, &allocation, &set); result != VK_SUCCESS)
                return fail(gpu::failure("vkAllocateDescriptorSets", result));
        }

        m_instances.create(context, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        return {};
    }

    void renderer::destroy()
    {
        if (m_context == nullptr)
            return;

        const VkDevice device = m_context->device();
        m_context->graphics().wait_idle();

        m_instances.destroy();
        m_no_values.destroy();
        m_target.destroy();

        if (m_descriptors != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device, m_descriptors, nullptr); // takes its sets along
        if (m_segments != VK_NULL_HANDLE)
            vkDestroyPipeline(device, m_segments, nullptr);
        if (m_grid != VK_NULL_HANDLE)
            vkDestroyPipeline(device, m_grid, nullptr);
        if (m_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device, m_layout, nullptr);
        if (m_set_layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, m_set_layout, nullptr);

        m_context = nullptr;
        m_descriptors = VK_NULL_HANDLE;
        m_sets = {};
        m_segments = VK_NULL_HANDLE;
        m_grid = VK_NULL_HANDLE;
        m_layout = VK_NULL_HANDLE;
        m_set_layout = VK_NULL_HANDLE;
    }

    std::expected<void, std::string> renderer::draw(const gpu::frame &frame, VkExtent2D size, const scene &scene,
                                                    const values *values)
    {
        const VkCommandBuffer commands = frame.commands;

        if (auto resized = m_target.resize(size); !resized)
            return resized;

        // ---- what the pass needs, before it starts -----------------------

        VkBuffer instance_buffer = VK_NULL_HANDLE;
        if (!scene.segments.empty())
        {
            std::vector<instance> instances;
            instances.reserve(scene.segments.size());
            for (const segment &segment : scene.segments)
            {
                instances.push_back({{segment.a.x(), segment.a.y(), segment.b.x(), segment.b.y()},
                                     {segment.color[0], segment.color[1], segment.color[2], segment.color[3]},
                                     segment.width,
                                     segment.node});
            }

            const auto written = m_instances.write(frame.slot, std::span<const instance>(instances));
            if (!written)
                return std::unexpected(written.error());
            instance_buffer = *written;
        }

        // The set of the frame that had this slot last is no longer used.
        const VkDescriptorSet set = m_sets[frame.slot];
        const VkDescriptorBufferInfo buffer_info{values != nullptr ? values->buffer : m_no_values.handle(), 0,
                                                 VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(m_context->device(), 1, &write, 0, nullptr);

        pass_constants constants{};
        constants.viewport[0] = static_cast<float>(size.width);
        constants.viewport[1] = static_cast<float>(size.height);
        constants.center[0] = scene.view.center.x();
        constants.center[1] = scene.view.center.y();
        constants.zoom = scene.view.zoom;
        constants.grid = scene.grid;
        constants.values = values != nullptr ? static_cast<std::int32_t>(values->first) : -1;
        constants.low = values != nullptr ? values->low : 0.0f;
        constants.high = values != nullptr ? values->high : 1.0f;
        for (std::size_t i = 0; i < 4; ++i)
            constants.grid_color[i] = scene.grid_color[i];

        // ---- the pass ----------------------------------------------------

        m_target.begin(commands, {scene.background[0], scene.background[1], scene.background[2], 1.0f});

        vkCmdPushConstants(commands, m_layout, pass_stages, 0, sizeof(constants), &constants);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &set, 0, nullptr);

        if (scene.grid > 0.0f)
        {
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, m_grid);
            vkCmdDraw(commands, 3, 1, 0, 0);
        }

        if (instance_buffer != VK_NULL_HANDLE)
        {
            const VkDeviceSize offset = 0;
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, m_segments);
            vkCmdBindVertexBuffers(commands, 0, 1, &instance_buffer, &offset);
            vkCmdDraw(commands, 4, static_cast<std::uint32_t>(scene.segments.size()), 0, 0);
        }

        m_target.end(commands);
        return {};
    }

} // namespace volt::schematic
