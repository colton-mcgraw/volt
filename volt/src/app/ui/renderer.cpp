#include <app/ui/renderer.hpp>

#include <gpu/context.hpp>
#include <gpu/pipeline.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace volt::ui
{
    namespace
    {
        constexpr std::uint32_t quad_vertex[] =
#include <shaders/ui_quad.vert.inc>
            ;
        constexpr std::uint32_t quad_fragment[] =
#include <shaders/ui_quad.frag.inc>
            ;

        // An item as the vertex shader gets it (ui_quad.vert).
        struct instance
        {
            float rect[4];
            float color[4];
            float border_color[4];
            float uv[4];
            float params[4]; // radius, border, extra, kind
        };

        constexpr VkVertexInputAttributeDescription attributes[] = {
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(instance, rect)},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(instance, color)},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(instance, border_color)},
            {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(instance, uv)},
            {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(instance, params)},
        };

        // ui.glsl
        struct pass_constants
        {
            float viewport[2];
            std::int32_t encoding;
            float white;
        };

        constexpr VkShaderStageFlags pass_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        instance instance_of(const draw_item &item)
        {
            instance result{};
            if (item.kind == shape_kind::line)
            {
                result.rect[0] = item.from.x();
                result.rect[1] = item.from.y();
                result.rect[2] = item.to.x();
                result.rect[3] = item.to.y();
            }
            else
            {
                result.rect[0] = item.bounds.x;
                result.rect[1] = item.bounds.y;
                result.rect[2] = item.bounds.width;
                result.rect[3] = item.bounds.height;
            }
            for (std::size_t i = 0; i < 4; ++i)
            {
                result.color[i] = item.color[i];
                result.border_color[i] = item.border_color[i];
            }
            result.uv[0] = item.uv_min.x();
            result.uv[1] = item.uv_min.y();
            result.uv[2] = item.uv_max.x();
            result.uv[3] = item.uv_max.y();
            result.params[0] = item.radius;
            result.params[1] = item.border;
            result.params[2] = item.extra;
            result.params[3] = static_cast<float>(item.kind);
            return result;
        }

        // A run of items drawn with one scissor and one set of textures.
        struct batch
        {
            std::uint32_t first = 0;
            std::uint32_t count = 0;
            VkRect2D scissor{};
            VkImageView glyphs = VK_NULL_HANDLE; // null: the default
            VkImageView image = VK_NULL_HANDLE;
        };

        VkRect2D scissor_of(const rect &clip, VkExtent2D extent)
        {
            const float width = static_cast<float>(extent.width);
            const float height = static_cast<float>(extent.height);
            const float left = std::clamp(std::floor(clip.x), 0.0f, width);
            const float top = std::clamp(std::floor(clip.y), 0.0f, height);
            const float right = std::clamp(std::ceil(clip.right()), left, width);
            const float bottom = std::clamp(std::ceil(clip.bottom()), top, height);
            return {{static_cast<std::int32_t>(left), static_cast<std::int32_t>(top)},
                    {static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top)}};
        }

        bool same(const VkRect2D &a, const VkRect2D &b)
        {
            return a.offset.x == b.offset.x && a.offset.y == b.offset.y && a.extent.width == b.extent.width &&
                   a.extent.height == b.extent.height;
        }
    } // namespace

    std::expected<void, std::string> renderer::create(gpu::context &context)
    {
        if (m_context != nullptr)
            return std::unexpected("the interface renderer is already created");

        m_context = &context;
        const VkDevice device = context.device();

        const auto fail = [this](std::unexpected<std::string> error) {
            destroy();
            return error;
        };

        // Glyphs are drawn at their own size on whole pixels, views at their
        // own size, so filtering only matters for an image shown scaled.
        // Trilinear: an image drawn smaller than it is reads from the levels
        // around the size it is drawn at, if it has them, instead of
        // aliasing; glyph pages and views have one level and are unaffected.
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.minLod = 0.0f;
        sampler_info.maxLod = VK_LOD_CLAMP_NONE;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (const VkResult result = vkCreateSampler(device, &sampler_info, nullptr, &m_sampler); result != VK_SUCCESS)
            return fail(gpu::failure("vkCreateSampler", result));

        const VkSampler samplers[] = {m_sampler, m_sampler};
        VkDescriptorSetLayoutBinding bindings[2]{};
        for (std::uint32_t i = 0; i < 2; ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[i].pImmutableSamplers = &samplers[i];
        }

        VkDescriptorSetLayoutCreateInfo set_info{};
        set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_info.bindingCount = 2;
        set_info.pBindings = bindings;
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

        for (VkDescriptorPool &pool : m_pools)
        {
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * max_sets};

            VkDescriptorPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool_info.maxSets = max_sets;
            pool_info.poolSizeCount = 1;
            pool_info.pPoolSizes = &size;
            if (const VkResult result = vkCreateDescriptorPool(device, &pool_info, nullptr, &pool);
                result != VK_SUCCESS)
                return fail(gpu::failure("vkCreateDescriptorPool", result));
        }

        const std::byte white[] = {std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
        if (auto created = m_white.create(context, 1, 1, gpu::texture_format::rgba8_unorm, white); !created)
            return fail(std::unexpected(std::move(created).error()));

        m_glyphs.create(context);
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
        m_glyphs.destroy();
        m_white.destroy();
        m_images.clear();

        for (VkDescriptorPool &pool : m_pools)
        {
            if (pool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(device, pool, nullptr);
            pool = VK_NULL_HANDLE;
        }

        for (const pipeline_entry &entry : m_pipelines)
            vkDestroyPipeline(device, entry.pipeline, nullptr);
        m_pipelines.clear();

        if (m_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device, m_layout, nullptr);
        if (m_set_layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, m_set_layout, nullptr);
        if (m_sampler != VK_NULL_HANDLE)
            vkDestroySampler(device, m_sampler, nullptr);

        m_context = nullptr;
        m_layout = VK_NULL_HANDLE;
        m_set_layout = VK_NULL_HANDLE;
        m_sampler = VK_NULL_HANDLE;
        m_frame = 0;
        m_sets = 0;
    }

    void renderer::set_image(std::uint32_t index, VkImageView view)
    {
        if (index >= m_images.size())
            m_images.resize(index + 1, VK_NULL_HANDLE);
        m_images[index] = view;
    }

    std::expected<void, std::string> renderer::upload_glyphs(const gpu::frame &frame, font::atlas &atlas)
    {
        return m_glyphs.upload(frame, atlas);
    }

    VkImageView renderer::resolve(texture_id id) const noexcept
    {
        switch (id.kind)
        {
        case texture_kind::glyphs:
            return m_glyphs.view(id.index);
        case texture_kind::image:
            return id.index < m_images.size() ? m_images[id.index] : VK_NULL_HANDLE;
        case texture_kind::none:
            break;
        }
        return VK_NULL_HANDLE;
    }

    std::expected<VkPipeline, std::string> renderer::pipeline_for(VkFormat format)
    {
        for (const pipeline_entry &entry : m_pipelines)
        {
            if (entry.format == format)
                return entry.pipeline;
        }

        // The first frame in this format: the display changed mode. There
        // are three formats at most, so they are simply all kept.
        gpu::graphics_pipeline_description description;
        description.vertex = quad_vertex;
        description.fragment = quad_fragment;
        description.layout = m_layout;
        description.color_format = format;
        description.blending = gpu::blending::premultiplied;
        description.instance_stride = sizeof(instance);
        description.attributes = attributes;

        const auto created = gpu::create_graphics_pipeline(*m_context, description);
        if (!created)
            return std::unexpected(created.error());

        m_pipelines.push_back({format, *created});
        return *created;
    }

    std::expected<void, std::string> renderer::draw(const gpu::frame &frame, const gpu::window_image &target,
                                                    const draw_list &list, const math::vec3f &background)
    {
        const VkDevice device = m_context->device();
        const VkCommandBuffer commands = frame.commands;
        const std::span<const draw_item> items = list.items();
        const std::span<const rect> clips = list.clips();

        const auto pipeline = pipeline_for(target.format);
        if (!pipeline)
            return std::unexpected(pipeline.error());

        // The frame's first draw. The sets of the frame that had this slot
        // last are no longer used; this frame's draws share the pool from
        // here on, each taking sets after the last one's.
        const VkDescriptorPool pool = m_pools[frame.slot];
        if (m_frame != frame.number)
        {
            vkResetDescriptorPool(device, pool, 0);
            m_frame = frame.number;
            m_sets = 0;
        }

        // ---- what the pass needs, before it starts -----------------------
        //
        // The instances, and the batches: a new one wherever the scissor
        // changes, or an item needs a texture other than the one its batch
        // has bound. Items whose texture is missing are left out.

        std::vector<instance> instances;
        std::vector<batch> batches;
        instances.reserve(items.size());

        for (const draw_item &item : items)
        {
            VkImageView needed = VK_NULL_HANDLE;
            if (item.kind == shape_kind::glyph || item.kind == shape_kind::image)
            {
                needed = resolve(item.texture);
                if (needed == VK_NULL_HANDLE)
                    continue;
            }

            const VkRect2D scissor = scissor_of(clips[item.clip], target.extent);
            if (scissor.extent.width == 0 || scissor.extent.height == 0)
                continue;

            batch *current = batches.empty() ? nullptr : &batches.back();
            bool fits = current != nullptr && same(current->scissor, scissor);
            if (fits && item.kind == shape_kind::glyph && current->glyphs != VK_NULL_HANDLE)
                fits = current->glyphs == needed;
            if (fits && item.kind == shape_kind::image && current->image != VK_NULL_HANDLE)
                fits = current->image == needed;

            if (!fits)
            {
                batches.push_back({static_cast<std::uint32_t>(instances.size()), 0, scissor, VK_NULL_HANDLE,
                                   VK_NULL_HANDLE});
                current = &batches.back();
            }

            if (item.kind == shape_kind::glyph)
                current->glyphs = needed;
            else if (item.kind == shape_kind::image)
                current->image = needed;

            instances.push_back(instance_of(item));
            ++current->count;
        }

        // After the instances of the frame's earlier draws, which their
        // passes still read.
        gpu::stream_range instance_range;
        if (!instances.empty())
        {
            const auto written = m_instances.append(frame, std::span<const instance>(instances));
            if (!written)
                return std::unexpected(written.error());
            instance_range = *written;
        }

        // ---- the pass ----------------------------------------------------

        // A clear color skips the shader: encode it here.
        const math::vec3f clear = gpu::encode(background, target.output);

        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = target.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{clear[0], clear[1], clear[2], 1.0f}};

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, target.extent};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;

        vkCmdBeginRendering(commands, &rendering);

        if (!batches.empty())
        {
            const float width = static_cast<float>(target.extent.width);
            const float height = static_cast<float>(target.extent.height);

            const VkViewport viewport{0.0f, 0.0f, width, height, 0.0f, 1.0f};
            vkCmdSetViewport(commands, 0, 1, &viewport);

            const pass_constants constants{
                {width, height}, static_cast<std::int32_t>(target.output.encoding), target.output.white};
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline);
            vkCmdPushConstants(commands, m_layout, pass_stages, 0, sizeof(constants), &constants);

            vkCmdBindVertexBuffers(commands, 0, 1, &instance_range.buffer, &instance_range.offset);

            // Neighbouring batches often bind the same pair of textures (a
            // clip changed, not a texture), so the last set is reused.
            // Within this pass only: a set bound in another window's pass
            // is not bound here.
            VkImageView bound_glyphs = VK_NULL_HANDLE;
            VkImageView bound_image = VK_NULL_HANDLE;
            bool bound = false;

            for (const batch &b : batches)
            {
                const VkImageView glyphs = b.glyphs != VK_NULL_HANDLE ? b.glyphs : m_white.view();
                const VkImageView image = b.image != VK_NULL_HANDLE ? b.image : m_white.view();

                if (!bound || glyphs != bound_glyphs || image != bound_image)
                {
                    if (m_sets == max_sets)
                        break;

                    VkDescriptorSet set = VK_NULL_HANDLE;
                    VkDescriptorSetAllocateInfo allocation{};
                    allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                    allocation.descriptorPool = pool;
                    allocation.descriptorSetCount = 1;
                    allocation.pSetLayouts = &m_set_layout;
                    if (vkAllocateDescriptorSets(device, &allocation, &set) != VK_SUCCESS)
                        break;
                    ++m_sets;

                    VkDescriptorImageInfo infos[2]{};
                    infos[0].imageView = glyphs;
                    infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    infos[1].imageView = image;
                    infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                    VkWriteDescriptorSet writes[2]{};
                    for (std::uint32_t i = 0; i < 2; ++i)
                    {
                        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        writes[i].dstSet = set;
                        writes[i].dstBinding = i;
                        writes[i].descriptorCount = 1;
                        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        writes[i].pImageInfo = &infos[i];
                    }
                    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
                    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &set, 0,
                                            nullptr);

                    bound = true;
                    bound_glyphs = glyphs;
                    bound_image = image;
                }

                vkCmdSetScissor(commands, 0, 1, &b.scissor);
                vkCmdDraw(commands, 4, b.count, 0, b.first);
            }
        }

        vkCmdEndRendering(commands);
        return {};
    }

} // namespace volt::ui
