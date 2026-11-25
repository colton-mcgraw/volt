#pragma once

// The Vulkan API without any window system headers. The platform specific
// part (creating a surface for a window) stays inside src/gpu/<os>/.
#include <vulkan/vulkan_core.h>

#include <expected>
#include <string>
#include <string_view>

namespace volt::gpu
{

    // The name of a result code, e.g. "VK_ERROR_DEVICE_LOST".
    std::string_view to_string(VkResult result) noexcept;

    // The error for a Vulkan call that failed: "<what> failed: <result>".
    std::unexpected<std::string> failure(std::string_view what, VkResult result);

} // namespace volt::gpu
