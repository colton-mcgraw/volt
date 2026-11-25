#include <app/board/renderer.hpp>

#include <gpu/context.hpp>
#include <gpu/pipeline.hpp>
#include <math/matrix.hpp>
#include <math/transform.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>

namespace volt::board
{
    namespace
    {
        constexpr std::uint32_t box_vertex[] =
#include <shaders/board_box.vert.inc>
            ;
        constexpr std::uint32_t box_fragment[] =
#include <shaders/board_box.frag.inc>
            ;
        constexpr std::uint32_t mesh_vertex[] =
#include <shaders/board_mesh.vert.inc>
            ;
        constexpr std::uint32_t mesh_fragment[] =
#include <shaders/board_mesh.frag.inc>
            ;

        // A box or a mesh instance as the vertex shaders get it
        // (board_box.vert, board_mesh.vert). Matrices are stored the way GLSL
        // reads them: column after column. For a mesh, color[3] is -1 where
        // the transform mirrors, which turns its triangles clockwise, else
        // 1; for a box it is 1, or 2 for a mark, drawn unlit.
        struct instance
        {
            float model[16];
            float color[4];
        };

        constexpr VkVertexInputAttributeDescription box_attributes[] = {
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(instance, model) + 0 * sizeof(float)},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(instance, model) + 4 * sizeof(float)},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(instance, model) + 8 * sizeof(float)},
            {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(instance, model) + 12 * sizeof(float)},
            {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(instance, color)},
        };

        // The same per instance, and the mesh's vertices from binding 1.
        constexpr VkVertexInputAttributeDescription mesh_attributes[] = {
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(instance, model) + 0 * sizeof(float)},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(instance, model) + 4 * sizeof(float)},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(instance, model) + 8 * sizeof(float)},
            {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(instance, model) + 12 * sizeof(float)},
            {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(instance, color)},
            {5, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(vertex, position)},
            {6, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(vertex, normal)},
        };

        // board.glsl
        struct pass_constants
        {
            float view_projection[16];
            float light[4];
            float eye[4];
        };

        constexpr VkShaderStageFlags pass_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        // The math library addresses elements as (row, column) whatever it
        // stores; a shader wants the columns one after the other.
        void store_columns(const math::mat4f &matrix, float *out)
        {
            for (std::size_t column = 0; column < 4; ++column)
            {
                for (std::size_t row = 0; row < 4; ++row)
                    out[column * 4 + row] = matrix(row, column);
            }
        }

        instance instance_of(const math::mat4f &model, const math::vec3f &color, float handedness)
        {
            instance result{};
            store_columns(model, result.model);
            result.color[0] = color[0];
            result.color[1] = color[1];
            result.color[2] = color[2];
            result.color[3] = handedness;
            return result;
        }

        // The sign of the determinant of the transform's linear part.
        float handedness(const math::mat4f &m)
        {
            const float det = m(0, 0) * (m(1, 1) * m(2, 2) - m(1, 2) * m(2, 1)) -
                              m(0, 1) * (m(1, 0) * m(2, 2) - m(1, 2) * m(2, 0)) +
                              m(0, 2) * (m(1, 0) * m(2, 1) - m(1, 1) * m(2, 0));
            return det < 0.0f ? -1.0f : 1.0f;
        }
    } // namespace

    std::expected<void, std::string> renderer::create(gpu::context &context)
    {
        if (m_context != nullptr)
            return std::unexpected("the board renderer is already created");

        m_context = &context;
        const VkDevice device = context.device();

        const auto fail = [this](std::unexpected<std::string> error) {
            destroy();
            return error;
        };

        if (auto created = m_target.create(context, true); !created)
            return fail(std::unexpected(std::move(created).error()));

        VkPushConstantRange constants{};
        constants.stageFlags = pass_stages;
        constants.size = sizeof(pass_constants);

        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &constants;
        if (const VkResult result = vkCreatePipelineLayout(device, &layout_info, nullptr, &m_layout);
            result != VK_SUCCESS)
            return fail(gpu::failure("vkCreatePipelineLayout", result));

        gpu::graphics_pipeline_description description;
        description.vertex = box_vertex;
        description.fragment = box_fragment;
        description.layout = m_layout;
        description.color_format = gpu::target::color_format;
        description.depth_format = m_target.depth_format();
        description.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        description.instance_stride = sizeof(instance);
        description.attributes = box_attributes;
        const auto boxes = gpu::create_graphics_pipeline(context, description);
        if (!boxes)
            return fail(std::unexpected(boxes.error()));
        m_boxes = *boxes;

        description.vertex = mesh_vertex;
        description.fragment = mesh_fragment;
        description.vertex_stride = sizeof(vertex);
        description.attributes = mesh_attributes;
        const auto meshes = gpu::create_graphics_pipeline(context, description);
        if (!meshes)
            return fail(std::unexpected(meshes.error()));
        m_meshes = *meshes;

        // The marks where something hides them: only there (GREATER: behind
        // what the depth buffer holds), blended, leaving the depth alone.
        description.vertex = box_vertex;
        description.fragment = box_fragment;
        description.vertex_stride = 0;
        description.attributes = box_attributes;
        description.depth_compare = VK_COMPARE_OP_GREATER;
        description.depth_write = false;
        description.blending = gpu::blending::premultiplied;
        const auto behind = gpu::create_graphics_pipeline(context, description);
        if (!behind)
            return fail(std::unexpected(behind.error()));
        m_marks_behind = *behind;

        m_instances.create(context, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        return {};
    }

    void renderer::destroy()
    {
        if (m_context == nullptr)
            return;

        const VkDevice device = m_context->device();
        m_context->graphics().wait_idle();

        // Nothing is in flight any more: what was retired can go, and so
        // can every mesh still held.
        m_retired.flush();
        m_slots.clear();
        m_free.clear();
        m_pending.clear();

        m_instances.destroy();
        m_target.destroy();

        if (m_marks_behind != VK_NULL_HANDLE)
            vkDestroyPipeline(device, m_marks_behind, nullptr);
        if (m_meshes != VK_NULL_HANDLE)
            vkDestroyPipeline(device, m_meshes, nullptr);
        if (m_boxes != VK_NULL_HANDLE)
            vkDestroyPipeline(device, m_boxes, nullptr);
        if (m_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device, m_layout, nullptr);

        m_context = nullptr;
        m_meshes = VK_NULL_HANDLE;
        m_marks_behind = VK_NULL_HANDLE;
        m_boxes = VK_NULL_HANDLE;
        m_layout = VK_NULL_HANDLE;
        m_last_frame = 0;
    }

    // ---- meshes ---------------------------------------------------------------

    renderer::mesh *renderer::find(mesh_handle handle) noexcept
    {
        if (!handle.valid() || handle.index >= m_slots.size())
            return nullptr;
        mesh &m = m_slots[handle.index];
        return m.live && m.generation == handle.generation ? &m : nullptr;
    }

    const renderer::mesh *renderer::find(mesh_handle handle) const noexcept
    {
        return const_cast<renderer *>(this)->find(handle);
    }

    bool renderer::contains(mesh_handle mesh) const noexcept
    {
        return find(mesh) != nullptr;
    }

    std::uint32_t renderer::triangle_count(mesh_handle mesh) const noexcept
    {
        const renderer::mesh *m = find(mesh);
        return m != nullptr ? m->index_count / 3 : 0;
    }

    std::size_t renderer::mesh_count() const noexcept
    {
        return m_slots.size() - m_free.size();
    }

    std::expected<mesh_handle, std::string> renderer::create_mesh(std::span<const vertex> vertices,
                                                                  std::span<const std::uint32_t> indices)
    {
        if (m_context == nullptr)
            return std::unexpected("the board renderer is not created");
        if (vertices.empty() || indices.empty())
            return std::unexpected("a mesh cannot be empty");
        if (indices.size() % 3 != 0)
            return std::unexpected("a mesh's indices must come in threes");
        if (indices.size() > std::numeric_limits<std::uint32_t>::max())
            return std::unexpected("a mesh has too many indices to draw");

        // An index past the end would have the GPU read outside the buffer.
        const std::size_t count = vertices.size();
        if (std::ranges::any_of(indices, [count](std::uint32_t index) { return index >= count; }))
            return std::unexpected("a mesh's index points past its vertices");

        const VkDeviceSize vertex_bytes = vertices.size_bytes();
        const VkDeviceSize index_bytes = indices.size_bytes();

        auto made_vertices = std::make_unique<gpu::buffer>();
        auto made_indices = std::make_unique<gpu::buffer>();
        auto staging = std::make_unique<gpu::buffer>();

        gpu::buffer_description description;
        description.size = vertex_bytes;
        description.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (auto created = made_vertices->create(*m_context, description); !created)
            return std::unexpected(std::move(created).error());

        description.size = index_bytes;
        description.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (auto created = made_indices->create(*m_context, description); !created)
            return std::unexpected(std::move(created).error());

        // The indices follow the vertices, at an offset any copy accepts.
        const VkDeviceSize index_offset = (vertex_bytes + 15) & ~VkDeviceSize{15};
        description.size = index_offset + index_bytes;
        description.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        description.mapped = true;
        if (auto created = staging->create(*m_context, description); !created)
            return std::unexpected(std::move(created).error());
        std::memcpy(staging->data(), vertices.data(), vertex_bytes);
        std::memcpy(staging->data() + index_offset, indices.data(), index_bytes);

        std::uint32_t slot = 0;
        if (!m_free.empty())
        {
            slot = m_free.back();
            m_free.pop_back();
        }
        else
        {
            slot = static_cast<std::uint32_t>(m_slots.size());
            m_slots.emplace_back();
        }

        mesh &m = m_slots[slot];
        ++m.generation;
        if (m.generation == 0) // wrapped: 0 means no mesh
            m.generation = 1;
        m.live = true;
        m.vertices = std::move(made_vertices);
        m.indices = std::move(made_indices);
        m.index_count = static_cast<std::uint32_t>(indices.size());
        m.staging = std::move(staging);
        m.vertex_bytes = vertex_bytes;
        m.index_bytes = index_bytes;
        m_pending.push_back(slot);

        return mesh_handle{slot, m.generation};
    }

    void renderer::release_mesh(mesh_handle handle)
    {
        mesh *m = find(handle);
        if (m == nullptr)
            return;

        // The last frame that drew the board may still be reading the
        // buffers. One whose copy was never recorded was never read at all.
        const bool uploaded = m->staging == nullptr;
        m_retired.retire(uploaded ? m_last_frame : 0,
                         [vertices = std::move(m->vertices), indices = std::move(m->indices),
                          staging = std::move(m->staging)]() mutable {
                             vertices.reset();
                             indices.reset();
                             staging.reset();
                         });

        m->live = false;
        m->index_count = 0;
        std::erase(m_pending, handle.index);
        m_free.push_back(handle.index);
    }

    void renderer::collect(const gpu::frame &frame)
    {
        if (frame.number <= m_last_frame && m_last_frame != 0)
            return; // this frame has been seen already
        m_retired.collect(frame);
        m_last_frame = frame.number;
    }

    void renderer::record_uploads(const gpu::frame &frame)
    {
        if (m_pending.empty())
            return;

        for (const std::uint32_t slot : m_pending)
        {
            mesh &m = m_slots[slot];
            const VkDeviceSize index_offset = (m.vertex_bytes + 15) & ~VkDeviceSize{15};

            const VkBufferCopy vertex_copy{0, 0, m.vertex_bytes};
            vkCmdCopyBuffer(frame.commands, m.staging->handle(), m.vertices->handle(), 1, &vertex_copy);
            const VkBufferCopy index_copy{index_offset, 0, m.index_bytes};
            vkCmdCopyBuffer(frame.commands, m.staging->handle(), m.indices->handle(), 1, &index_copy);

            // The copy reads the staging buffer while this frame runs.
            m_retired.retire(frame.number, [staging = std::move(m.staging)]() mutable { staging.reset(); });
        }
        m_pending.clear();

        // Every copy above before any vertex or index is read, in this
        // frame's pass or any later frame's.
        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(frame.commands, &dependency);
    }

    // ---- drawing ----------------------------------------------------------------

    std::expected<void, std::string> renderer::draw(const gpu::frame &frame, VkExtent2D size, const scene &scene)
    {
        const VkCommandBuffer commands = frame.commands;

        collect(frame);

        if (auto resized = m_target.resize(size); !resized)
            return resized;

        // ---- what the pass needs, before it starts -----------------------

        record_uploads(frame);

        // What belongs to the selected object is drawn this much of the way
        // towards the selection's colour.
        const auto tinted = [&scene](const math::vec3f &color, object_id object) {
            if (object == 0 || object != scene.selected)
                return color;
            const float t = 0.35f;
            return color * (1.0f - t) + scene.selection_color * t;
        };

        // Boxes first, then the marks (unlit, see board_box.frag), then the
        // mesh instances grouped by mesh, so that each mesh is one indexed
        // draw of a run of instances, then the marks again, for where they
        // are behind something (see m_marks_behind). Whatever is hidden is
        // left out here.
        std::vector<instance> instances;
        instances.reserve(scene.boxes.size() + scene.marks.size() + scene.meshes.size());
        const auto add_box = [&instances](const box &box, const math::vec3f &color, float lighting) {
            // The unit cube: scaled to size, turned, then put in place.
            const math::mat4f model =
                math::affine(math::rotation_z(box.rotation) * math::scaling(box.size), box.center);
            instances.push_back(instance_of(model, color, lighting));
        };
        for (const box &box : scene.boxes)
        {
            if (scene.drawn(box.object))
                add_box(box, tinted(box.color, box.object), 1.0f);
        }
        for (const box &box : scene.marks)
            add_box(box, box.color, 2.0f);
        const auto box_count = static_cast<std::uint32_t>(instances.size());

        struct run
        {
            std::uint32_t slot = 0;
            std::uint32_t first = 0;
            std::uint32_t count = 0;
        };
        std::vector<const mesh_instance *> placed;
        placed.reserve(scene.meshes.size());
        for (const mesh_instance &m : scene.meshes)
        {
            if (find(m.mesh) != nullptr && scene.drawn(m.object))
                placed.push_back(&m);
        }
        std::ranges::stable_sort(placed, {}, [](const mesh_instance *m) { return m->mesh.index; });

        std::vector<run> runs;
        for (const mesh_instance *m : placed)
        {
            if (runs.empty() || runs.back().slot != m->mesh.index)
                runs.push_back({m->mesh.index, static_cast<std::uint32_t>(instances.size()), 0});
            ++runs.back().count;
            instances.push_back(instance_of(m->transform, tinted(m->color, m->object), handedness(m->transform)));
        }

        const auto behind_first = static_cast<std::uint32_t>(instances.size());
        for (const box &box : scene.marks)
            add_box(box, box.color, 3.0f);
        const auto behind_count = static_cast<std::uint32_t>(instances.size()) - behind_first;

        VkBuffer instance_buffer = VK_NULL_HANDLE;
        if (!instances.empty())
        {
            const auto written = m_instances.write(frame.slot, std::span<const instance>(instances));
            if (!written)
                return std::unexpected(written.error());
            instance_buffer = *written;
        }

        const camera &view = scene.view;
        const float aspect = static_cast<float>(size.width) / static_cast<float>(size.height);

        // As picking undoes it (<app/board/scene.hpp>).
        pass_constants constants{};
        store_columns(view_projection(view, aspect), constants.view_projection);
        for (std::size_t i = 0; i < 3; ++i)
        {
            constants.light[i] = scene.light[i];
            constants.eye[i] = view.eye[i];
        }

        // ---- the pass ----------------------------------------------------

        m_target.begin(commands, {scene.background[0], scene.background[1], scene.background[2], 1.0f});

        if (instance_buffer != VK_NULL_HANDLE)
        {
            const VkDeviceSize offset = 0;
            vkCmdPushConstants(commands, m_layout, pass_stages, 0, sizeof(constants), &constants);
            vkCmdBindVertexBuffers(commands, 0, 1, &instance_buffer, &offset);

            if (box_count > 0)
            {
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, m_boxes);
                vkCmdDraw(commands, 36, box_count, 0, 0);
            }

            if (!runs.empty())
            {
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, m_meshes);
                for (const run &r : runs)
                {
                    const mesh &m = m_slots[r.slot];
                    const VkBuffer vertices = m.vertices->handle();
                    vkCmdBindVertexBuffers(commands, 1, 1, &vertices, &offset);
                    vkCmdBindIndexBuffer(commands, m.indices->handle(), 0, VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexed(commands, m.index_count, r.count, 0, 0, r.first);
                }
            }

            if (behind_count > 0)
            {
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, m_marks_behind);
                vkCmdDraw(commands, 36, behind_count, 0, behind_first);
            }
        }

        m_target.end(commands);
        return {};
    }

} // namespace volt::board
