#pragma once

#include <gpu/color.hpp>
#include <gpu/vulkan.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace platform
{
    struct native_window;
} // namespace platform

namespace volt::gpu
{

    class context;
    class presenter;

    // What the monitor showing a window does, as far as it concerns the
    // images sent to it. The window knows (volt::ui::window::display()).
    struct display
    {
        // The monitor is in HDR mode, so an image may go beyond SDR white.
        bool hdr = false;

        // The luminance the system gives SDR white on it, in nits.
        float white = 80.0f;
    };

    // What presenter::acquire() hands out for one window in one frame: the
    // image to draw that window into.
    struct window_image
    {
        // The window's next image, in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        // with undefined contents. It has to be in that layout again when the
        // frame ends.
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};

        // How the values written to the image are interpreted. The pass that
        // writes the image encodes for it, see <gpu/color.hpp>. Like `format`
        // it can differ from one frame to the next, and from one window to
        // the next within a frame: each follows its own monitor.
        gpu::output output;
    };

    // ---------------------------------------------------------------------
    // swapchain
    // ---------------------------------------------------------------------
    //
    // One window's part of presenting: its surface, and the Vulkan swapchain
    // with its images. What a frame is (command buffer, fence, slot, number)
    // is the presenter's, and the same for every window; a frame draws into
    // as many windows as it acquires (presenter::acquire()) and presents
    // them all at once.
    //
    // The swapchain follows the display. On a monitor in HDR mode it is scRGB
    // (16 bit float) or, failing that, HDR10 (10 bit PQ); otherwise 8 bit
    // sRGB. Either way the image format is never one of the *_SRGB ones: the
    // hardware stores what the shaders write unchanged, and the pass that
    // writes the image does the encoding itself, for whichever of the three
    // window_image::output names. Everything before that pass works in the
    // linear working space of <gpu/color.hpp> and never sees the difference.
    // Two windows on two monitors can be in two different formats in the
    // same frame.
    //
    // It also follows the window's size, rebuilding itself when an acquire
    // is given another size than the last, or the last present found it out
    // of date. While the window has no area (minimized), acquire() skips it
    // and the old images stay until there is something to replace them.
    //
    // Threads, and how a window comes and goes while frames run. There are
    // two threads: the one that owns the window (the platform thread, which
    // creates and destroys windows) and the render thread (the one that runs
    // the presenter's frames). The swapchain belongs to one of them at a
    // time, and moves between them only through something that synchronizes
    // (a mutex, or an atomic written with release and read with acquire), as
    // any object handed from thread to thread:
    //
    //   Adding a window
    //     1. Window thread: create the window, then create() this with its
    //        native handles. That only makes the surface; there are no
    //        images yet, and the queue is not touched, so it is cheap and
    //        safe while the render thread is in the middle of a frame.
    //     2. Hand the swapchain to the render thread, along with a way to
    //        read the window's size and display there (the window itself is
    //        not the render thread's to ask; the application keeps both in
    //        atomics the window's callbacks update).
    //     3. Render thread: from the next frame on, presenter::acquire() it
    //        in the frames that should draw into the window. The first
    //        acquire builds the Vulkan swapchain.
    //
    //   Removing a window
    //     1. Window thread: ask the render thread to stop presenting to the
    //        window (it may hide() the window meanwhile, so it disappears at
    //        once). Do not destroy the window: the surface needs it.
    //     2. Render thread: stop acquiring the swapchain, then release() it,
    //        outside the stretch between acquiring it and end_frame() (so
    //        between frames, or in a frame that has not acquired it).
    //        release() waits for the GPU to be through with it and destroys
    //        the Vulkan swapchain and its images. Then hand it back.
    //     3. Window thread: destroy() the swapchain, which now only destroys
    //        the surface, and only then destroy the window.
    //
    //   While no frames run (before the render thread starts, after it has
    //   stopped), the window thread may skip the hand-overs: destroy() of a
    //   built swapchain releases it first.
    //
    // A window that is merely hidden, or minimized, needs none of this: it
    // keeps its swapchain, and acquire() skips it while it has no area.
    class swapchain
    {
    public:
        swapchain() = default;
        ~swapchain() { destroy(); }

        // The presenter keeps pointers to the swapchains a frame acquired.
        swapchain(const swapchain &) = delete;
        swapchain &operator=(const swapchain &) = delete;

        // Window thread. Makes the window's surface. `context` and the
        // window must outlive the swapchain (until destroy()).
        std::expected<void, std::string> create(context &context, const platform::native_window &window);

        // Render thread, not while acquired in the frame being recorded.
        // Waits for the graphics queue to go idle (the frames that drew
        // into the images may still be on the GPU, their presents still
        // waiting on its semaphores), then destroys the Vulkan swapchain,
        // its views and semaphores. The surface stays; a later acquire
        // would build the swapchain again. Does nothing if it is not built.
        void release();

        // Window thread, once the render thread has released the swapchain
        // and handed it back, or while no frames run (then it releases what
        // is still built itself). Destroys the surface; the window may go
        // after this.
        void destroy();

        bool is_created() const noexcept { return m_surface != VK_NULL_HANDLE; }

        // Render thread. There is a Vulkan swapchain: acquired at least once
        // and not released since.
        bool is_built() const noexcept { return m_swapchain != VK_NULL_HANDLE; }

        // Render thread. The format and encoding of the images as of the last
        // acquire. Both change when the display enters or leaves HDR mode, so
        // a pipeline that writes a window's image is built per format and
        // looked up by window_image::format, not built once from this.
        VkFormat format() const noexcept { return m_format.format; }
        const gpu::output &output() const noexcept { return m_output; }

        // Render thread. Size of the images; zero while there are none.
        VkExtent2D extent() const noexcept { return m_extent; }

        // Render thread. frame::number of the last frame that acquired an
        // image, or 0 if none has.
        std::uint64_t last_frame() const noexcept { return m_last_frame; }

    private:
        friend class presenter;

        // One per swapchain image.
        struct target
        {
            VkImage image = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;

            // Signaled when the frame drawn into the image has finished, which
            // is what presenting waits for. Per image rather than per frame:
            // it is only free for reuse once the image comes back from the
            // display.
            VkSemaphore rendered = VK_NULL_HANDLE;
        };

        // The presenter's side: frame_slots is presenter::frames_in_flight,
        // passed in so that this header need not know it.
        std::expected<void, std::string> rebuild(VkExtent2D size, const display &display, std::size_t frame_slots);
        void destroy_targets();

        context *m_context = nullptr;

        VkSurfaceKHR m_surface = VK_NULL_HANDLE;
        VkSurfaceFormatKHR m_format{};
        gpu::output m_output;
        VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
        std::vector<target> m_targets;

        // One per frame in flight, by frame::slot: signaled when the image
        // acquired in that frame is ready to draw into. The frame's
        // submission waits on it, and the next frame with the same slot
        // only begins once that submission is through, so it is free again
        // by then. Made with the first swapchain, kept until release().
        std::vector<VkSemaphore> m_acquired;

        // Size of the swapchain images; zero while there is no usable
        // swapchain. And the window size and display mode it was last built
        // for.
        VkExtent2D m_extent{};
        VkExtent2D m_size{};
        bool m_hdr = false;

        // The swapchain no longer matches the surface and has to be rebuilt
        // before the next acquire.
        bool m_stale = true;

        // Set from acquire() to end_frame(): the frame being recorded holds
        // one of the images.
        bool m_pending = false;
        std::uint32_t m_image = 0;
        std::uint64_t m_last_frame = 0;
    };

} // namespace volt::gpu
