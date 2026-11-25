#pragma once

#include <gpu/swapchain.hpp>
#include <gpu/vulkan.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace volt::gpu
{

    class context;

    // What begin_frame() hands out: where to record. The same frame draws
    // into every window that takes part in it (presenter::acquire() hands
    // out a window_image for each), and into whatever offscreen targets the
    // renderers keep.
    struct frame
    {
        // Recording has begun. Whatever is recorded runs when end_frame()
        // submits it.
        VkCommandBuffer commands = VK_NULL_HANDLE;

        // Which of the frames in flight this is, below
        // presenter::frames_in_flight. The frame that had the number last is
        // through the GPU, so whatever a renderer keeps per slot (the
        // buffers it refills every frame) is free to be overwritten. One
        // frame for all windows, so a renderer that draws into several in a
        // frame shares the slot's resources among those draws (see
        // gpu::stream::append()).
        std::size_t slot = 0;

        // Counts the frames handed out, from 1. When a frame begins, every
        // frame numbered `number - frames_in_flight` or lower is through the
        // GPU: something a renderer stopped using after frame n can be freed
        // once frame n + frames_in_flight begins (see <gpu/retire.hpp>).
        // Every frame begun is submitted, whether or not it drew into a
        // window, so the numbers keep that promise.
        std::uint64_t number = 0;
    };

    // What presenter::acquire() found for a window.
    struct acquisition
    {
        // The image to draw into this frame; empty to leave the window out.
        std::optional<window_image> image;

        // Left out because its display did not give an image back within
        // the timeout (it holds them: a monitor asleep, a window on a
        // desktop that is not shown, a stalled compositor), not because
        // there is nothing to draw into: try it again soon, without
        // waiting, rather than when it next changes.
        bool busy = false;
    };

    // ---------------------------------------------------------------------
    // presenter
    // ---------------------------------------------------------------------
    //
    // The frames in flight: their command buffers and synchronization, shared
    // by every window. It knows nothing about what is drawn, nor which
    // windows there are; a frame goes like this, all on the render thread:
    //
    //     begin_frame()                   waits for the slot, starts recording
    //     acquire(window A, ...)          A's next image, or empty: skip A
    //     acquire(window B, ...)
    //     ...record: offscreen passes, then one pass per acquired image...
    //     end_frame()                     one submit, one present for all
    //
    // acquire() records the image's transition into the frame's commands, so
    // a window's pass comes after its acquire; offscreen work may come
    // before or after. Each swapchain at most once per frame.
    //
    // A frame that acquires nothing (every window minimized, or none open)
    // is still submitted by end_frame(), and presents nothing: the work it
    // recorded (offscreen targets, uploads, the simulation's hand-over) runs
    // all the same, and its fence keeps the frame numbers' promise to the
    // retire queues. There is no abandoning a frame: once begin_frame() has
    // returned one, end_frame() has to follow, even after a failure in
    // between.
    //
    // Presenting several swapchains at once. end_frame() hands every image
    // acquired in the frame to a single vkQueuePresentKHR, and looks at each
    // swapchain's result on its own: out of date or suboptimal marks only
    // that swapchain for rebuilding at its next acquire. Pacing is FIFO
    // (vsync) for each, which makes the render thread wait, in acquire() or
    // in the present, for whichever window's monitor is slowest to give an
    // image back. So all windows run at the rate of the slowest monitor
    // among them, and two monitors whose refreshes are not in step can
    // cost a frame more than either alone. For an editor that draws on
    // demand that is a fair price for one frame loop; a window on a slow
    // monitor that should not hold up the rest would need a frame of its
    // own. A display that does not give an image back at all (asleep, or
    // holding them) holds the others up only for acquire()'s timeout: the
    // window is then left out as busy, and tried again without waiting.
    //
    // create() and destroy() belong on the thread that owns the context, the
    // rest on the render thread (one thread at a time). See <gpu/swapchain.hpp>
    // for how windows come and go while it runs.
    class presenter
    {
    public:
        // The CPU records at most this many frames ahead of the GPU.
        static constexpr std::size_t frames_in_flight = 2;

        presenter() = default;
        ~presenter() { destroy(); }

        presenter(const presenter &) = delete;
        presenter &operator=(const presenter &) = delete;

        // `context` must outlive the presenter. Needs no window.
        std::expected<void, std::string> create(context &context);

        // Waits for the device. The swapchains are not the presenter's; it
        // forgets those of a frame that was never ended.
        void destroy();

        bool is_created() const noexcept { return m_context != nullptr; }

        // Starts a frame. Blocks while the GPU is `frames_in_flight` frames
        // behind.
        std::expected<frame, std::string> begin_frame();

        // How long acquire() waits for a display to give an image back, by
        // default: some frames' worth at any refresh rate, and short enough
        // that a window whose display holds its images does not hold up
        // every other window for long.
        static constexpr std::uint64_t acquire_timeout_ns = 100'000'000;

        // The next image of `swapchain` to draw into in the frame begun last,
        // in its layout for drawing. `size` is the window's current content
        // area and `display` the monitor it is on now; the swapchain follows
        // both, rebuilding itself if they changed (which waits for the
        // device). No image when there is nothing to draw into (the window
        // is minimized, or still changing size), or (busy) when the display
        // gave none back within `timeout_ns`: leave the window out of this
        // frame.
        //
        // With vsync this may block until the display gives an image back,
        // up to `timeout_ns`; 0 does not wait at all, for a window that was
        // busy last time.
        std::expected<acquisition, std::string> acquire(swapchain &swapchain, VkExtent2D size,
                                                        const display &display = {},
                                                        std::uint64_t timeout_ns = acquire_timeout_ns);

        // Submits the frame begun last and queues every image it acquired
        // for display, in one present. Returns how many were queued (zero
        // for a frame that acquired none). The swapchains are free for
        // release() again once this returns, whether or not it succeeded.
        std::expected<std::size_t, std::string> end_frame();

        // frame::number of the frame begun last; 0 before the first.
        std::uint64_t frame_number() const noexcept { return m_frames; }

    private:
        // One per frame in flight.
        struct slot
        {
            VkCommandPool pool = VK_NULL_HANDLE;
            VkCommandBuffer commands = VK_NULL_HANDLE;
            VkFence done = VK_NULL_HANDLE; // the GPU has finished the frame
        };

        context *m_context = nullptr;

        std::array<slot, frames_in_flight> m_slots{};
        std::size_t m_slot = 0;
        std::uint64_t m_frames = 0; // frame::number of the last frame handed out
        bool m_recording = false;

        // The swapchains the frame being recorded acquired an image of, in
        // the order it did.
        std::vector<swapchain *> m_acquired;
    };

} // namespace volt::gpu
