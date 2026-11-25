#include <gpu/pipeline.hpp>

#include <gpu/context.hpp>

namespace volt::gpu
{
    namespace
    {
        // Only needed while a pipeline is being created from it.
        class shader_module
        {
        public:
            shader_module(VkDevice device, shader_code code) : m_device(device)
            {
                VkShaderModuleCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                info.codeSize = code.size_bytes();
                info.pCode = code.data();
                m_result = vkCreateShaderModule(device, &info, nullptr, &m_handle);
            }

            ~shader_module()
            {
                if (m_handle != VK_NULL_HANDLE)
                    vkDestroyShaderModule(m_device, m_handle, nullptr);
            }

            shader_module(const shader_module &) = delete;
            shader_module &operator=(const shader_module &) = delete;

            VkResult result() const noexcept { return m_result; }

            VkPipelineShaderStageCreateInfo stage(VkShaderStageFlagBits stage) const noexcept
            {
                VkPipelineShaderStageCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                info.stage = stage;
                info.module = m_handle;
                info.pName = "main";
                return info;
            }

        private:
            VkDevice m_device;
            VkShaderModule m_handle = VK_NULL_HANDLE;
            VkResult m_result;
        };
    } // namespace

    std::expected<VkPipeline, std::string> create_graphics_pipeline(context &context,
                                                                    const graphics_pipeline_description &description)
    {
        const VkDevice device = context.device();

        const shader_module vertex(device, description.vertex);
        const shader_module fragment(device, description.fragment);
        if (vertex.result() != VK_SUCCESS)
            return failure("vkCreateShaderModule", vertex.result());
        if (fragment.result() != VK_SUCCESS)
            return failure("vkCreateShaderModule", fragment.result());

        const VkPipelineShaderStageCreateInfo stages[] = {
            vertex.stage(VK_SHADER_STAGE_VERTEX_BIT),
            fragment.stage(VK_SHADER_STAGE_FRAGMENT_BIT),
        };

        VkVertexInputBindingDescription bindings[2]{};
        bindings[0].binding = 0;
        bindings[0].stride = description.instance_stride;
        bindings[0].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        bindings[1].binding = 1;
        bindings[1].stride = description.vertex_stride;
        bindings[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        if (!description.attributes.empty())
        {
            vertex_input.vertexBindingDescriptionCount = description.vertex_stride != 0 ? 2u : 1u;
            vertex_input.pVertexBindingDescriptions = bindings;
            vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(description.attributes.size());
            vertex_input.pVertexAttributeDescriptions = description.attributes.data();
        }

        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = description.topology;

        // The values come from vkCmdSetViewport and vkCmdSetScissor.
        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;

        // No culling: the shapes are flat quads, or closed boxes and meshes
        // that the depth buffer sorts out, and not culling spares every
        // shader from agreeing with every projection on which way is front.
        // It also keeps the inside of a model whose file left a face out,
        // or turned one the wrong way, from showing through.
        VkPipelineRasterizationStateCreateInfo rasterization{};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        const bool depth = description.depth_format != VK_FORMAT_UNDEFINED;
        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable = depth ? VK_TRUE : VK_FALSE;
        depth_stencil.depthWriteEnable = depth && description.depth_write ? VK_TRUE : VK_FALSE;
        depth_stencil.depthCompareOp = description.depth_compare;

        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                    VK_COLOR_COMPONENT_A_BIT;
        if (description.blending == blending::premultiplied)
        {
            attachment.blendEnable = VK_TRUE;
            attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            attachment.colorBlendOp = VK_BLEND_OP_ADD;
            attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        }

        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &attachment;

        // Dynamic rendering: the formats stand in for a render pass object.
        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &description.color_format;
        rendering.depthAttachmentFormat = description.depth_format;

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &rasterization;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth_stencil;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = description.layout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (const VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
            result != VK_SUCCESS)
            return failure("vkCreateGraphicsPipelines", result);
        return pipeline;
    }

    std::expected<VkPipeline, std::string> create_compute_pipeline(context &context, shader_code code,
                                                                   VkPipelineLayout layout)
    {
        const VkDevice device = context.device();

        const shader_module shader(device, code);
        if (shader.result() != VK_SUCCESS)
            return failure("vkCreateShaderModule", shader.result());

        VkComputePipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.stage = shader.stage(VK_SHADER_STAGE_COMPUTE_BIT);
        info.layout = layout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
            result != VK_SUCCESS)
            return failure("vkCreateComputePipelines", result);
        return pipeline;
    }

} // namespace volt::gpu
