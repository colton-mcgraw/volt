#pragma once

#include <gpu/buffer.hpp>
#include <gpu/presenter.hpp>
#include <gpu/target.hpp>
#include <math/vector.hpp>
#include <schematic/scene.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace volt::gpu
{
    class context;
} // namespace volt::gpu

namespace volt::schematic
{

    // camera, segment and scene, what the renderer draws, are plain data:
    // <schematic/scene.hpp>.

    // Simulation results to color the nodes' lines by: one float per node,
    // node 0 at element `first` of `buffer`, which must be a storage buffer
    // usable on the graphics queue. simulation::compute::latest() has them.
    struct values
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        std::uint32_t first = 0;

        // The values at the two ends of the color ramp.
        float low = 0.0f;
        float high = 1.0f;
    };

    // ---------------------------------------------------------------------
    // renderer
    // ---------------------------------------------------------------------
    //
    // Draws the schematic view into a target of its own, which the interface
    // then shows in a viewport. Colors stay in the working space; nothing
    // here knows about the display.
    //
    // create() and destroy() belong on the thread that owns the context;
    // draw() on the render thread.
    class renderer
    {
    public:
        renderer() = default;
        ~renderer() { destroy(); }

        renderer(const renderer &) = delete;
        renderer &operator=(const renderer &) = delete;

        std::expected<void, std::string> create(gpu::context &context);
        void destroy();

        // Records the view's pass into `frame`, before the interface's. The
        // target becomes `size` pixels big, which should be the size of the
        // viewport it is shown in. `values` may be null.
        std::expected<void, std::string> draw(const gpu::frame &frame, VkExtent2D size, const scene &scene,
                                              const values *values = nullptr);

        // Where the view was drawn. A frame that does not call draw() can
        // show what the last one that did left there.
        const gpu::target &target() const noexcept { return m_target; }

    private:
        gpu::context *m_context = nullptr;
        gpu::target m_target;

        VkDescriptorSetLayout m_set_layout = VK_NULL_HANDLE;
        VkPipelineLayout m_layout = VK_NULL_HANDLE;
        VkPipeline m_grid = VK_NULL_HANDLE;
        VkPipeline m_segments = VK_NULL_HANDLE;

        // Bound in place of the simulation's values while there are none: a
        // shader's buffer has to be something.
        gpu::buffer m_no_values;

        // Per frame in flight: the segments, and the set naming the buffer
        // of values, which can be another one every frame.
        gpu::stream m_instances;
        VkDescriptorPool m_descriptors = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, gpu::presenter::frames_in_flight> m_sets{};
    };

} // namespace volt::schematic
