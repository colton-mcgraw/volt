#pragma once

#include <gpu/vulkan.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace platform
{
    struct native_window;
} // namespace platform

// The three places where Vulkan meets the window system. Each backend
// implements them in src/gpu/<os>/surface.cpp.
namespace volt::gpu
{

    // The instance extensions needed to present to this window system.
    std::span<const char *const> surface_extensions() noexcept;

    // Whether queues of `family` can present to windows of the window system
    // `window` lives on. Needs no surface, so a device can be chosen before
    // the first swapchain exists.
    bool can_present(VkPhysicalDevice device, std::uint32_t family, const platform::native_window &window);

    // The caller owns the surface and must destroy it before the window.
    std::expected<VkSurfaceKHR, std::string> create_surface(VkInstance instance,
                                                            const platform::native_window &window);

} // namespace volt::gpu
