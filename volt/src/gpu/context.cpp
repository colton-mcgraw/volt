#include <gpu/context.hpp>
#include <gpu/surface.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <optional>
#include <vector>

namespace volt::gpu
{
    namespace
    {
        constexpr std::uint32_t required_version = VK_API_VERSION_1_3;
        constexpr const char *validation_layer = "VK_LAYER_KHRONOS_validation";

        constexpr const char *device_extensions[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        };

        std::string version_text(std::uint32_t version)
        {
            return std::format("{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version));
        }

        // Vulkan's way of returning arrays: ask for the count, then for the
        // elements. `enumerate` is the call with everything but those two bound.
        template <typename T, typename Enumerate>
        std::vector<T> list(Enumerate &&enumerate)
        {
            std::uint32_t count = 0;
            enumerate(&count, nullptr);

            std::vector<T> items(count);
            enumerate(&count, items.data());
            items.resize(count);
            return items;
        }

        bool has_extension(const std::vector<VkExtensionProperties> &available, const char *name)
        {
            return std::ranges::any_of(
                available, [&](const VkExtensionProperties &e) { return std::strcmp(e.extensionName, name) == 0; });
        }

        // Called by the validation layer, on whichever thread made the
        // offending call.
        VKAPI_ATTR VkBool32 VKAPI_CALL on_debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                        VkDebugUtilsMessageTypeFlagsEXT,
                                                        const VkDebugUtilsMessengerCallbackDataEXT *data, void *)
        {
            const char *label = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning";
            std::fputs(std::format("[vulkan {}] {}\n", label, data->pMessage).c_str(), stderr);

            // False: do not make the call that triggered the message fail.
            return VK_FALSE;
        }

        VkDebugUtilsMessengerCreateInfoEXT messenger_info()
        {
            VkDebugUtilsMessengerCreateInfoEXT info{};
            info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            info.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            info.pfnUserCallback = on_debug_message;
            return info;
        }

        // A GPU that can run Volt, and the queue families to use on it.
        struct candidate
        {
            VkPhysicalDevice device = VK_NULL_HANDLE;
            std::string name;
            std::uint32_t graphics_family = 0;
            std::uint32_t compute_family = 0;
            int score = 0;
        };

        // The candidate for `device`, or the reason it cannot be used.
        std::expected<candidate, std::string> inspect(VkPhysicalDevice device, const platform::native_window &window)
        {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(device, &properties);

            if (properties.apiVersion < required_version)
            {
                return std::unexpected(std::format("supports Vulkan {} only, {} is required",
                                                   version_text(properties.apiVersion),
                                                   version_text(required_version)));
            }

            const auto extensions = list<VkExtensionProperties>([&](std::uint32_t *count, VkExtensionProperties *out) {
                vkEnumerateDeviceExtensionProperties(device, nullptr, count, out);
            });
            for (const char *extension : device_extensions)
            {
                if (!has_extension(extensions, extension))
                    return std::unexpected(std::format("lacks {}", extension));
            }

            VkPhysicalDeviceVulkan13Features features13{};
            features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            VkPhysicalDeviceFeatures2 features{};
            features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features.pNext = &features13;
            vkGetPhysicalDeviceFeatures2(device, &features);

            if (features13.dynamicRendering != VK_TRUE || features13.synchronization2 != VK_TRUE)
                return std::unexpected("lacks dynamic rendering or synchronization2");

            const auto families = list<VkQueueFamilyProperties>([&](std::uint32_t *count, VkQueueFamilyProperties *out) {
                vkGetPhysicalDeviceQueueFamilyProperties(device, count, out);
            });

            // One family draws and presents. Compute goes to a family without
            // graphics if there is one: those are separate queues in the
            // driver too, not just another name for the graphics queue.
            std::optional<std::uint32_t> graphics;
            std::optional<std::uint32_t> compute_only;
            std::optional<std::uint32_t> compute_any;

            for (std::uint32_t family = 0; family < families.size(); ++family)
            {
                const VkQueueFlags flags = families[family].queueFlags;
                const bool draws = (flags & VK_QUEUE_GRAPHICS_BIT) != 0;
                const bool computes = (flags & VK_QUEUE_COMPUTE_BIT) != 0;

                if (draws && !graphics && can_present(device, family, window))
                    graphics = family;
                if (computes && !draws && !compute_only)
                    compute_only = family;
                if (computes && !compute_any)
                    compute_any = family;
            }

            if (!graphics)
                return std::unexpected("has no queue family that can both draw and present");
            if (!compute_any)
                return std::unexpected("has no compute queue family");

            const bool graphics_computes = (families[*graphics].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;

            candidate result;
            result.device = device;
            result.name = properties.deviceName;
            result.graphics_family = *graphics;
            result.compute_family = compute_only ? *compute_only : graphics_computes ? *graphics : *compute_any;

            switch (properties.deviceType)
            {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: result.score = 3; break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: result.score = 2; break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: result.score = 1; break;
            default: result.score = 0; break; // software rasterizers
            }
            return result;
        }
    } // namespace

    // ---------------------------------------------------------------------
    // queue
    // ---------------------------------------------------------------------

    VkResult queue::submit(const VkSubmitInfo2 &info, VkFence fence)
    {
        const std::scoped_lock lock(m_mutex);
        return vkQueueSubmit2(m_handle, 1, &info, fence);
    }

    VkResult queue::present(const VkPresentInfoKHR &info)
    {
        const std::scoped_lock lock(m_mutex);
        return vkQueuePresentKHR(m_handle, &info);
    }

    VkResult queue::wait_idle()
    {
        const std::scoped_lock lock(m_mutex);
        return vkQueueWaitIdle(m_handle);
    }

    // ---------------------------------------------------------------------
    // context
    // ---------------------------------------------------------------------

    std::expected<void, std::string> context::create(const context_configuration &config,
                                                     const platform::native_window &window)
    {
        if (m_instance != VK_NULL_HANDLE)
            return std::unexpected("the GPU context is already created");

        // Nothing half built survives a failure.
        const auto fail = [this](std::unexpected<std::string> error) {
            destroy();
            return error;
        };

        // ---- instance ----------------------------------------------------

        std::uint32_t loader_version = VK_API_VERSION_1_0;
        vkEnumerateInstanceVersion(&loader_version);
        if (loader_version < required_version)
        {
            return std::unexpected(std::format("the installed Vulkan runtime is version {}, {} is required",
                                               version_text(loader_version), version_text(required_version)));
        }

        const auto instance_extensions = list<VkExtensionProperties>([](std::uint32_t *count, VkExtensionProperties *out) {
            vkEnumerateInstanceExtensionProperties(nullptr, count, out);
        });
        const auto layers = list<VkLayerProperties>(
            [](std::uint32_t *count, VkLayerProperties *out) { vkEnumerateInstanceLayerProperties(count, out); });

        std::vector<const char *> enabled_extensions;
        for (const char *extension : surface_extensions())
        {
            if (!has_extension(instance_extensions, extension))
                return std::unexpected(std::format("the Vulkan runtime lacks {}", extension));
            enabled_extensions.push_back(extension);
        }

        const bool validate =
            config.validation && has_extension(instance_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) &&
            std::ranges::any_of(
                layers, [](const VkLayerProperties &l) { return std::strcmp(l.layerName, validation_layer) == 0; });
        if (validate)
            enabled_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        // Makes surfaces offer the color spaces beyond sRGB. Without it the
        // presenter simply never finds an HDR format.
        if (has_extension(instance_extensions, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))
            enabled_extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);

        VkApplicationInfo application{};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.pApplicationName = config.application_name.c_str();
        application.pEngineName = "Volt";
        application.apiVersion = required_version;

        // Chained into the instance as well, which covers the creation and
        // destruction of the instance itself.
        const VkDebugUtilsMessengerCreateInfoEXT messenger = messenger_info();

        VkInstanceCreateInfo instance_info{};
        instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_info.pNext = validate ? &messenger : nullptr;
        instance_info.pApplicationInfo = &application;
        instance_info.enabledLayerCount = validate ? 1u : 0u;
        instance_info.ppEnabledLayerNames = &validation_layer;
        instance_info.enabledExtensionCount = static_cast<std::uint32_t>(enabled_extensions.size());
        instance_info.ppEnabledExtensionNames = enabled_extensions.data();

        if (const VkResult result = vkCreateInstance(&instance_info, nullptr, &m_instance); result != VK_SUCCESS)
            return failure("vkCreateInstance", result);

        if (validate)
        {
            // Extension functions are not exported by the loader library;
            // they have to be looked up.
            const auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
            if (create_messenger != nullptr)
                create_messenger(m_instance, &messenger, nullptr, &m_messenger);
        }

        // ---- physical device ---------------------------------------------

        const auto devices = list<VkPhysicalDevice>([this](std::uint32_t *count, VkPhysicalDevice *out) {
            vkEnumeratePhysicalDevices(m_instance, count, out);
        });

        std::optional<candidate> chosen;
        std::string rejected;
        for (const VkPhysicalDevice device : devices)
        {
            auto inspected = inspect(device, window);
            if (!inspected)
            {
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(device, &properties);
                rejected += std::format("\n  {}: {}", properties.deviceName, inspected.error());
                continue;
            }

            // Ties go to the device the system lists first.
            if (!chosen || inspected->score > chosen->score)
                chosen = std::move(*inspected);
        }

        if (!chosen)
        {
            return fail(std::unexpected(devices.empty() ? std::string("no Vulkan capable GPU was found")
                                                        : "no GPU can run Volt:" + rejected));
        }

        // ---- logical device ----------------------------------------------

        const float priority = 1.0f;
        const bool separate_compute = chosen->compute_family != chosen->graphics_family;

        VkDeviceQueueCreateInfo queue_infos[2]{};
        queue_infos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_infos[0].queueFamilyIndex = chosen->graphics_family;
        queue_infos[0].queueCount = 1;
        queue_infos[0].pQueuePriorities = &priority;
        queue_infos[1] = queue_infos[0];
        queue_infos[1].queueFamilyIndex = chosen->compute_family;

        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;

        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &features13;

        VkDeviceCreateInfo device_info{};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.pNext = &features;
        device_info.queueCreateInfoCount = separate_compute ? 2u : 1u;
        device_info.pQueueCreateInfos = queue_infos;
        std::vector<const char *> enabled_device_extensions(std::begin(device_extensions),
                                                            std::end(device_extensions));

        // Optional: lets a presenter describe its HDR images to the display.
        const auto available_device_extensions =
            list<VkExtensionProperties>([&](std::uint32_t *count, VkExtensionProperties *out) {
                vkEnumerateDeviceExtensionProperties(chosen->device, nullptr, count, out);
            });
        m_hdr_metadata = has_extension(available_device_extensions, VK_EXT_HDR_METADATA_EXTENSION_NAME);
        if (m_hdr_metadata)
            enabled_device_extensions.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);

        device_info.enabledExtensionCount = static_cast<std::uint32_t>(enabled_device_extensions.size());
        device_info.ppEnabledExtensionNames = enabled_device_extensions.data();

        if (const VkResult result = vkCreateDevice(chosen->device, &device_info, nullptr, &m_device);
            result != VK_SUCCESS)
            return fail(failure("vkCreateDevice", result));

        m_physical_device = chosen->device;
        m_device_name = std::move(chosen->name);

        m_graphics.m_family = chosen->graphics_family;
        vkGetDeviceQueue(m_device, chosen->graphics_family, 0, &m_graphics.m_handle);

        if (separate_compute)
        {
            m_dedicated_compute.m_family = chosen->compute_family;
            vkGetDeviceQueue(m_device, chosen->compute_family, 0, &m_dedicated_compute.m_handle);
            m_compute = &m_dedicated_compute;
        }

        return {};
    }

    void context::destroy()
    {
        if (m_device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(m_device);
            vkDestroyDevice(m_device, nullptr);
        }

        if (m_messenger != VK_NULL_HANDLE)
        {
            const auto destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy_messenger != nullptr)
                destroy_messenger(m_instance, m_messenger, nullptr);
        }

        if (m_instance != VK_NULL_HANDLE)
            vkDestroyInstance(m_instance, nullptr);

        m_device = VK_NULL_HANDLE;
        m_messenger = VK_NULL_HANDLE;
        m_instance = VK_NULL_HANDLE;
        m_physical_device = VK_NULL_HANDLE;
        m_graphics.m_handle = VK_NULL_HANDLE;
        m_dedicated_compute.m_handle = VK_NULL_HANDLE;
        m_compute = &m_graphics;
        m_hdr_metadata = false;
        m_device_name.clear();
    }

} // namespace volt::gpu
