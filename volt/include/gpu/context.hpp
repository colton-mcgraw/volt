#pragma once

#include <gpu/vulkan.hpp>

#include <cstdint>
#include <expected>
#include <mutex>
#include <string>

namespace platform
{
    struct native_window;
} // namespace platform

namespace volt::gpu
{

    struct context_configuration
    {
        std::string application_name = "Volt";

        // Ask for the Khronos validation layer and print what it reports to
        // stderr. Silently skipped when the layer is not installed (it comes
        // with the Vulkan SDK, not with the drivers).
#ifdef NDEBUG
        bool validation = false;
#else
        bool validation = true;
#endif
    };

    // ---------------------------------------------------------------------
    // queue
    // ---------------------------------------------------------------------
    //
    // Vulkan leaves locking a queue to the application. The render thread and
    // the simulation may end up on the same queue (see context::compute()),
    // so everything that touches one goes through here.
    class queue
    {
    public:
        VkQueue handle() const noexcept { return m_handle; }
        std::uint32_t family() const noexcept { return m_family; }

        VkResult submit(const VkSubmitInfo2 &info, VkFence fence = VK_NULL_HANDLE);
        VkResult present(const VkPresentInfoKHR &info);

        // Blocks until everything submitted so far has finished. Other
        // threads cannot submit in the meantime.
        VkResult wait_idle();

    private:
        friend class context;

        VkQueue m_handle = VK_NULL_HANDLE;
        std::uint32_t m_family = 0;
        std::mutex m_mutex;
    };

    // ---------------------------------------------------------------------
    // context
    // ---------------------------------------------------------------------
    //
    // The instance, the chosen GPU and the logical device: the part of Vulkan
    // every renderer and the simulation share. It owns no window resources;
    // those belong to a presenter.
    //
    // Requires Vulkan 1.3 and enables dynamic rendering and synchronization2
    // from it, so there are no render pass or framebuffer objects anywhere.
    //
    // create() and destroy() are not thread safe. In between, the handles may
    // be used from any thread under Vulkan's own rules; the queues lock
    // themselves.
    class context
    {
    public:
        context() = default;
        ~context() { destroy(); }

        // Everything created from the device refers back to it.
        context(const context &) = delete;
        context &operator=(const context &) = delete;

        // `window` only tells which window system the GPU has to be able to
        // present to; the context does not keep it.
        std::expected<void, std::string> create(const context_configuration &config,
                                                const platform::native_window &window);

        // Everything created from the device must be gone by now.
        void destroy();

        bool is_created() const noexcept { return m_device != VK_NULL_HANDLE; }

        VkInstance instance() const noexcept { return m_instance; }
        VkPhysicalDevice physical_device() const noexcept { return m_physical_device; }
        VkDevice device() const noexcept { return m_device; }

        // Rendering and presenting.
        queue &graphics() noexcept { return m_graphics; }

        // Simulation work. A queue of its own where the GPU has a compute-only
        // family, so that long dispatches do not sit in front of frames in the
        // graphics queue; otherwise the graphics queue itself.
        queue &compute() noexcept { return *m_compute; }
        bool has_dedicated_compute() const noexcept { return m_compute != &m_graphics; }

        // VK_EXT_hdr_metadata is enabled on the device.
        bool has_hdr_metadata() const noexcept { return m_hdr_metadata; }

        // The GPU's name as its driver reports it.
        const std::string &device_name() const noexcept { return m_device_name; }

    private:
        VkInstance m_instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
        VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;

        queue m_graphics;
        queue m_dedicated_compute;
        queue *m_compute = &m_graphics;
        bool m_hdr_metadata = false;

        std::string m_device_name;
    };

} // namespace volt::gpu
