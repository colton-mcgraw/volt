#include <gpu/surface.hpp>

#include <platform/windows/window.hpp>

#include <vulkan/vulkan_win32.h> // after <windows.h>

namespace volt::gpu
{

    std::span<const char *const> surface_extensions() noexcept
    {
        static constexpr const char *extensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        };
        return extensions;
    }

    bool can_present(VkPhysicalDevice device, std::uint32_t family, const platform::native_window &)
    {
        return vkGetPhysicalDeviceWin32PresentationSupportKHR(device, family) == VK_TRUE;
    }

    std::expected<VkSurfaceKHR, std::string> create_surface(VkInstance instance,
                                                            const platform::native_window &window)
    {
        const VkWin32SurfaceCreateInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
            .pNext = nullptr,
            .flags = 0,
            .hinstance = window.instance,
            .hwnd = window.handle,
        };

        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (const VkResult result = vkCreateWin32SurfaceKHR(instance, &info, nullptr, &surface); result != VK_SUCCESS)
            return failure("vkCreateWin32SurfaceKHR", result);
        return surface;
    }

} // namespace volt::gpu
