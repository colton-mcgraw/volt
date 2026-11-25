#pragma once

#include <app/simulation/network.hpp>
#include <gpu/buffer.hpp>
#include <gpu/vulkan.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace volt::gpu
{
    class context;
} // namespace volt::gpu

namespace volt::simulation
{

    // ---------------------------------------------------------------------
    // compute
    // ---------------------------------------------------------------------
    //
    // Runs the simulation on the GPU's compute queue (context::compute()),
    // next to the frames rather than inside them, and keeps the buffer the
    // schematic view reads the results from.
    //
    // The work is cut into steps: one command list of `sweeps_per_step`
    // relaxation sweeps (see network.hpp). advance() hands a step to the
    // queue and, some frames later, finds it finished and makes its result
    // the latest.
    //
    // Results are never copied. The potentials live in one buffer, as
    // several sets one after the other: three that take turns being the
    // result, and two the sweeps of a step go back and forth between. A
    // frame draws from the set that was latest when it was recorded, while
    // the next steps write the other two. For that to hold, a set must not
    // come up for writing again while a frame that draws from it is still
    // on the GPU, which is what advance()'s one step per frame guarantees:
    // by the time the third step after it begins, that frame is two frames
    // old, and the presenter has waited for it.
    //
    // create(), destroy() and load() belong where no frame is being
    // recorded; the rest on the render thread.
    class compute
    {
    public:
        static constexpr std::uint32_t sweeps_per_step = 32;

        // Where the latest potentials are: `count` floats, one per node,
        // starting at element `first` of `buffer`.
        struct values
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            std::uint32_t first = 0;
            std::uint32_t count = 0;
        };

        compute() = default;
        ~compute() { destroy(); }

        compute(const compute &) = delete;
        compute &operator=(const compute &) = delete;

        std::expected<void, std::string> create(gpu::context &context);
        void destroy();

        // Replaces the network. Every potential starts over at zero, and
        // there are no results until the first step is through. Waits for
        // the GPU to finish with the old one.
        std::expected<void, std::string> load(const network &network);

        // Asks for this many more steps.
        void run(std::uint32_t steps) noexcept { m_remaining += steps; }

        // There is work left or under way: keep the frames coming.
        bool is_busy() const noexcept { return m_pending || (m_count != 0 && m_remaining != 0); }

        // Once per frame, between the presenter's begin_frame() and
        // end_frame(): picks up the step that finished, starts the next.
        // Never waits for the GPU.
        std::expected<void, std::string> advance();

        // Empty until a step has finished. Good for the frame being
        // recorded; ask again in the next.
        std::optional<values> latest() const noexcept;

    private:
        // The sets of potentials in the state buffer.
        static constexpr std::uint32_t result_sets = 3;
        static constexpr std::uint32_t scratch_sets = 2;

        void destroy_network();
        std::expected<void, std::string> start_step();

        gpu::context *m_context = nullptr;

        VkDescriptorSetLayout m_set_layout = VK_NULL_HANDLE;
        VkPipelineLayout m_layout = VK_NULL_HANDLE;
        VkPipeline m_pipeline = VK_NULL_HANDLE;
        VkDescriptorPool m_descriptors = VK_NULL_HANDLE;
        VkDescriptorSet m_set = VK_NULL_HANDLE;

        // One step is on the GPU at a time, so one command buffer does.
        VkCommandPool m_pool = VK_NULL_HANDLE;
        VkCommandBuffer m_commands = VK_NULL_HANDLE;
        VkFence m_done = VK_NULL_HANDLE;

        // The network, as packed by pack(), and the potentials.
        gpu::buffer m_nodes;
        gpu::buffer m_edges;
        gpu::buffer m_state;
        std::uint32_t m_count = 0; // nodes

        std::uint32_t m_remaining = 0;
        bool m_pending = false;       // a step is on the GPU
        std::uint32_t m_writing = 0;  // the result set it writes
        std::optional<std::uint32_t> m_front; // the result set that is the latest
    };

} // namespace volt::simulation
