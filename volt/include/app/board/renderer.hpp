#pragma once

#include <app/board/scene.hpp>
#include <gpu/buffer.hpp>
#include <gpu/presenter.hpp>
#include <gpu/retire.hpp>
#include <gpu/target.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace volt::gpu
{
    class context;
} // namespace volt::gpu

namespace volt::board
{

    // ---------------------------------------------------------------------
    // renderer
    // ---------------------------------------------------------------------
    //
    // Draws the 3D board view (a scene, <app/board/scene.hpp>) into a target
    // of its own, color and depth, which the interface then shows in a
    // viewport. Colors stay in the working space; nothing here knows about
    // the display.
    //
    // Boxes and mesh instances are drawn the same way: one instance each of
    // a pipeline, with its transform and colour per instance, depth tested,
    // lit by one light, a hemisphere of ambient light and a soft highlight
    // (assets/shaders/board.glsl). The scene's marks are boxes drawn unlit,
    // twice: depth tested with the rest, then, after everything, once more
    // where something is in front of them, faintly, with no depth written
    // (m_marks_behind). So a measurement or a selection's outline that runs
    // under a part still shows where it goes, and still looks behind it.
    // What belongs to a hidden object (scene::drawn()) is left out; what
    // belongs to the selected one is tinted towards the selection colour.
    // The projection is the scene camera's (view_projection()), perspective
    // or parallel.
    //
    // Meshes. The renderer owns the meshes it draws and hands out handles to
    // them. A mesh never changes once made; a model that changes is new
    // meshes. Its life:
    //
    //   * create_mesh() copies the vertices and indices into a staging buffer
    //     the CPU can write, right away, so the caller's arrays are free
    //     again when it returns; it also makes the mesh's vertex and index
    //     buffers, in device-local memory, where the GPU reads them fastest
    //     (on a discrete GPU, host-visible memory is read over the bus every
    //     frame the board is drawn). Nothing is submitted: the copy into them
    //     is recorded by the next draw(), into that frame's command buffer
    //     before the pass, with a barrier after it that makes the copy
    //     visible to vertex input in this frame and every later one on the
    //     queue. So a handle can go into the scene at once.
    //
    //   * The staging buffer is retired with the frame that recorded its
    //     copy, and freed once that frame is through the GPU.
    //
    //   * release_mesh() makes the handle invalid at once: instances that
    //     still name it are skipped from the next draw() on. Its buffers are
    //     retired with the last frame that drew the board, and freed at the
    //     start of the first frame for which the presenter has waited on
    //     that one (gpu::retire_queue). So releasing never stalls, and never
    //     frees a buffer a frame in flight still reads. A mesh released
    //     before any draw() uploaded it is freed at the next collect().
    //
    // collect() is what frees: call it at the start of every frame, before
    // anything is recorded (draw() does it too, for the frames it draws).
    // destroy() waits for the device and frees everything.
    //
    // Threads: create() and destroy() belong on the thread that owns the
    // context; everything else on the render thread, between frames or, for
    // collect() and draw(), inside one. Before the render thread starts, the
    // thread that created the renderer may make meshes too (the demo board's
    // parts are made at startup).
    class renderer
    {
    public:
        renderer() = default;
        ~renderer() { destroy(); }

        renderer(const renderer &) = delete;
        renderer &operator=(const renderer &) = delete;

        std::expected<void, std::string> create(gpu::context &context);
        void destroy();

        // A mesh of `vertices` and `indices` (three per triangle, each below
        // vertices.size(), counter-clockwise seen from the side the normals
        // point to), in its own coordinates, millimeters. Neither may be
        // empty.
        std::expected<mesh_handle, std::string> create_mesh(std::span<const vertex> vertices,
                                                            std::span<const std::uint32_t> indices);

        // Lets go of a mesh; see above. A handle that names nothing is
        // ignored.
        void release_mesh(mesh_handle mesh);

        // Whether the handle names a mesh, and how big that is.
        bool contains(mesh_handle mesh) const noexcept;
        std::uint32_t triangle_count(mesh_handle mesh) const noexcept;
        std::size_t mesh_count() const noexcept;

        // Frees what `frame`, which has just begun, makes safe to free.
        void collect(const gpu::frame &frame);

        // Records the view's pass into `frame`, before the interface's. The
        // target becomes `size` pixels big, which should be the size of the
        // viewport it is shown in.
        std::expected<void, std::string> draw(const gpu::frame &frame, VkExtent2D size, const scene &scene);

        // Where the view was drawn. A frame that does not call draw() can
        // show what the last one that did left there.
        const gpu::target &target() const noexcept { return m_target; }

    private:
        struct mesh
        {
            std::uint32_t generation = 0; // meshes the slot has held
            bool live = false;

            std::unique_ptr<gpu::buffer> vertices;
            std::unique_ptr<gpu::buffer> indices;
            std::uint32_t index_count = 0;

            // Until a draw() records the copy: where it comes from.
            std::unique_ptr<gpu::buffer> staging;
            VkDeviceSize vertex_bytes = 0;
            VkDeviceSize index_bytes = 0;
        };

        mesh *find(mesh_handle handle) noexcept;
        const mesh *find(mesh_handle handle) const noexcept;

        // The copies of every mesh made since the last draw(), into `frame`.
        void record_uploads(const gpu::frame &frame);

        gpu::context *m_context = nullptr;
        gpu::target m_target;

        VkPipelineLayout m_layout = VK_NULL_HANDLE;
        VkPipeline m_boxes = VK_NULL_HANDLE;
        VkPipeline m_meshes = VK_NULL_HANDLE;
        VkPipeline m_marks_behind = VK_NULL_HANDLE; // the box shaders, where depth is GREATER, blended

        // Per frame in flight: the boxes and marks, the mesh instances, and
        // the marks again, one after the other.
        gpu::stream m_instances;

        // Slots, reused once released; a handle's generation tells an old
        // mesh from the one that took its slot.
        std::vector<mesh> m_slots;
        std::vector<std::uint32_t> m_free;
        std::vector<std::uint32_t> m_pending; // slots whose copy is not recorded yet

        gpu::retire_queue m_retired;

        // The number of the last frame that drew the board, or that began
        // with collect(): the last one that may have read any mesh.
        std::uint64_t m_last_frame = 0;
    };

} // namespace volt::board
