#include <gpu/surface.hpp>

#include <platform/linux/window.hpp>

#include <vulkan/vulkan_wayland.h> // after <wayland-client.h>

namespace volt::gpu
{

    std::span<const char *const> surface_extensions() noexcept
    {
        static constexpr const char *extensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
        };
        return extensions;
    }

    bool can_present(VkPhysicalDevice device, std::uint32_t family, const platform::native_window &window)
    {
        return vkGetPhysicalDeviceWaylandPresentationSupportKHR(device, family, window.platform.display) == VK_TRUE;
    }

    // The driver talks to the compositor on this same connection, through
    // event queues of its own (see platform::poll_events), so presenting from
    // the render thread does not disturb the platform thread.
    std::expected<VkSurfaceKHR, std::string> create_surface(VkInstance instance,
                                                            const platform::native_window &window)
    {
        const VkWaylandSurfaceCreateInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
            .pNext = nullptr,
            .flags = 0,
            .display = window.platform.display,
            .surface = window.surface,
        };

        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (const VkResult result = vkCreateWaylandSurfaceKHR(instance, &info, nullptr, &surface);
            result != VK_SUCCESS)
            return failure("vkCreateWaylandSurfaceKHR", result);
        return surface;
    }

} // namespace volt::gpu
