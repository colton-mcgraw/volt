#include "volt/io/assets/Font.hpp"
#include "volt/io/image/ImageEncoder.hpp"
#include "io/assets/SfntFontAtlas.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "[font-gpu-atlas-test] FAIL: " << message << '\n';
    return false;
  }
  return true;
}

enum class FontRenderMode {
  kSdf,
  kMsdf,
};

const char* modeString(FontRenderMode mode) {
  return mode == FontRenderMode::kMsdf ? "msdf" : "sdf";
}

std::optional<std::filesystem::path> findWorkspaceRoot() {
  std::error_code ec;
  std::filesystem::path cursor = std::filesystem::current_path(ec);
  if (ec) {
    return std::nullopt;
  }

  for (int depth = 0; depth < 12; ++depth) {
    const auto fontPath = cursor / "assets" / "fonts" / "DefaultFont.ttf";
    if (std::filesystem::exists(fontPath, ec) && !ec) {
      return cursor;
    }

    if (!cursor.has_parent_path()) {
      break;
    }

    const std::filesystem::path parent = cursor.parent_path();
    if (parent == cursor) {
      break;
    }
    cursor = parent;
  }

  return std::nullopt;
}

bool writeManifest(const std::filesystem::path& path, FontRenderMode mode) {
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    return false;
  }

  out << "{\n"
      << "  \"font\": \"./fonts/DefaultFont.ttf\",\n"
      << "  \"ui-font-default\": \"./images/ui-font-default.png\",\n"
      << "  \"font-render-mode\": \"" << modeString(mode) << "\",\n"
      << "  \"font-sdf-spread-px\": \"8\"\n"
      << "}\n";
  return out.good();
}

std::vector<int> testCodepoints() {
  return {
      static_cast<int>(U' '),
      static_cast<int>(U'?'),
      static_cast<int>(U'A'),
      static_cast<int>(U'B'),
      static_cast<int>(U'H'),
      static_cast<int>(U'O'),
      static_cast<int>(U'W'),
      static_cast<int>(U'g'),
      static_cast<int>(U'@'),
      static_cast<int>(U'\u20AC'),
      static_cast<int>(U'\u03A9'),
      static_cast<int>(U'\u0416'),
  };
}

std::optional<std::filesystem::path> findCompiledComputeShader(const char* argv0) {
  std::error_code ec;
  const std::filesystem::path executablePath = std::filesystem::absolute(argv0, ec);
  if (!ec) {
    const std::filesystem::path candidate = executablePath.parent_path() / "assets" / "shaders" / "msdfgen.comp.spv";
    if (std::filesystem::exists(candidate, ec) && !ec) {
      return candidate;
    }
  }

  const auto workspaceRoot = findWorkspaceRoot();
  if (!workspaceRoot.has_value()) {
    return std::nullopt;
  }

  const std::array<std::filesystem::path, 3> candidates = {
      *workspaceRoot / "build" / "windows-msvc-vs" / "tests" / "Debug" / "assets" / "shaders" / "msdfgen.comp.spv",
      *workspaceRoot / "build" / "windows-msvc-vs" / "src" / "Debug" / "assets" / "shaders" / "msdfgen.comp.spv",
      *workspaceRoot / "build" / "windows-msvc-vcpkg-vs" / "tests" / "Debug" / "assets" / "shaders" / "msdfgen.comp.spv",
  };

  for (const auto& candidate : candidates) {
    if (std::filesystem::exists(candidate, ec) && !ec) {
      return candidate;
    }
  }

  return std::nullopt;
}

std::optional<std::vector<char>> readBinaryFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    return std::nullopt;
  }

  const std::streamsize size = file.tellg();
  if (size <= 0) {
    return std::nullopt;
  }

  std::vector<char> bytes(static_cast<std::size_t>(size));
  file.seekg(0, std::ios::beg);
  file.read(bytes.data(), size);
  if (!file.good()) {
    return std::nullopt;
  }

  return bytes;
}

float halfToFloat(std::uint16_t half) {
  const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000U) << 16U;
  const std::uint32_t exponent = (half >> 10U) & 0x1FU;
  const std::uint32_t mantissa = half & 0x03FFU;

  std::uint32_t bits = 0U;
  if (exponent == 0U) {
    if (mantissa == 0U) {
      bits = sign;
    } else {
      std::uint32_t normalizedMantissa = mantissa;
      std::int32_t adjustedExponent = -14;
      while ((normalizedMantissa & 0x0400U) == 0U) {
        normalizedMantissa <<= 1U;
        --adjustedExponent;
      }
      normalizedMantissa &= 0x03FFU;
      bits = sign |
             static_cast<std::uint32_t>(adjustedExponent + 127) << 23U |
             (normalizedMantissa << 13U);
    }
  } else if (exponent == 0x1FU) {
    bits = sign | 0x7F800000U | (mantissa << 13U);
  } else {
    bits = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
  }

  float value = 0.0F;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

struct VulkanComputeAtlasRenderer {
  VkInstance instance{VK_NULL_HANDLE};
  VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
  VkDevice device{VK_NULL_HANDLE};
  std::uint32_t queueFamilyIndex{std::numeric_limits<std::uint32_t>::max()};
  VkQueue queue{VK_NULL_HANDLE};
  VkCommandPool commandPool{VK_NULL_HANDLE};
  VkDescriptorSetLayout descriptorSetLayout{VK_NULL_HANDLE};
  VkDescriptorPool descriptorPool{VK_NULL_HANDLE};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  VkPipeline pipeline{VK_NULL_HANDLE};

  ~VulkanComputeAtlasRenderer() { cleanup(); }

  void cleanup() {
    if (device != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(device);
    }
    if (pipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(device, pipeline, nullptr);
      pipeline = VK_NULL_HANDLE;
    }
    if (pipelineLayout != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
      pipelineLayout = VK_NULL_HANDLE;
    }
    if (descriptorPool != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device, descriptorPool, nullptr);
      descriptorPool = VK_NULL_HANDLE;
    }
    if (descriptorSetLayout != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
      descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (commandPool != VK_NULL_HANDLE) {
      vkDestroyCommandPool(device, commandPool, nullptr);
      commandPool = VK_NULL_HANDLE;
    }
    if (device != VK_NULL_HANDLE) {
      vkDestroyDevice(device, nullptr);
      device = VK_NULL_HANDLE;
    }
    if (instance != VK_NULL_HANDLE) {
      vkDestroyInstance(instance, nullptr);
      instance = VK_NULL_HANDLE;
    }
  }

  std::uint32_t findMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
      const bool typeSupported = (typeBits & (1U << i)) != 0U;
      const bool propertyMatch =
          (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
      if (typeSupported && propertyMatch) {
        return i;
      }
    }
    throw std::runtime_error("No suitable Vulkan memory type found");
  }

  bool initialize(const std::filesystem::path& shaderPath, std::string& outError) {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "volt_font_gpu_atlas_tests";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "VoltEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
      outError = "failed to create Vulkan instance";
      return false;
    }

    std::uint32_t deviceCount = 0U;
    if (vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0U) {
      outError = "no Vulkan physical devices available";
      return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    for (VkPhysicalDevice candidate : devices) {
      std::uint32_t queueCount = 0U;
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
      std::vector<VkQueueFamilyProperties> queueFamilies(queueCount);
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queueFamilies.data());

      for (std::uint32_t i = 0U; i < queueCount; ++i) {
        if ((queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0U) {
          continue;
        }

        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(candidate, VK_FORMAT_R16G16B16A16_SFLOAT, &formatProperties);
        if ((formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0U) {
          continue;
        }

        physicalDevice = candidate;
        queueFamilyIndex = i;
        break;
      }

      if (physicalDevice != VK_NULL_HANDLE) {
        break;
      }
    }

    if (physicalDevice == VK_NULL_HANDLE) {
      outError = "no compute-capable Vulkan device with rgba16f storage image support";
      return false;
    }

    const float queuePriority = 1.0F;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamilyIndex;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
      outError = "failed to create Vulkan device";
      return false;
    }

    vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);

    VkCommandPoolCreateInfo commandPoolInfo{};
    commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolInfo.queueFamilyIndex = queueFamilyIndex;
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool) != VK_SUCCESS) {
      outError = "failed to create command pool";
      return false;
    }

    const std::array<VkDescriptorSetLayoutBinding, 3> bindings = {
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };

    VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
    setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    setLayoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &descriptorSetLayout) != VK_SUCCESS) {
      outError = "failed to create descriptor set layout";
      return false;
    }

    const VkPushConstantRange pushConstantRange{
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(float),
    };

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
      outError = "failed to create compute pipeline layout";
      return false;
    }

    const std::array<VkDescriptorPoolSize, 3> poolSizes = {
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 1;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
      outError = "failed to create descriptor pool";
      return false;
    }

    const auto shaderBytes = readBinaryFile(shaderPath);
    if (!shaderBytes.has_value()) {
      outError = "failed to read compute shader bytes";
      return false;
    }

    VkShaderModuleCreateInfo shaderModuleInfo{};
    shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderModuleInfo.codeSize = shaderBytes->size();
    shaderModuleInfo.pCode = reinterpret_cast<const std::uint32_t*>(shaderBytes->data());

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &shaderModuleInfo, nullptr, &shaderModule) != VK_SUCCESS) {
      outError = "failed to create compute shader module";
      return false;
    }

    VkPipelineShaderStageCreateInfo shaderStageInfo{};
    shaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStageInfo.module = shaderModule;
    shaderStageInfo.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = shaderStageInfo;
    pipelineInfo.layout = pipelineLayout;

    const VkResult result =
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    vkDestroyShaderModule(device, shaderModule, nullptr);

    if (result != VK_SUCCESS) {
      outError = "failed to create compute pipeline";
      return false;
    }

    return true;
  }

  bool createBuffer(VkDeviceSize size,
                    VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags memoryProperties,
                    VkBuffer& outBuffer,
                    VkDeviceMemory& outMemory,
                    const void* initialData,
                    std::string& outError) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &outBuffer) != VK_SUCCESS) {
      outError = "failed to create buffer";
      return false;
    }

    VkMemoryRequirements memoryRequirements{};
    vkGetBufferMemoryRequirements(device, outBuffer, &memoryRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memoryRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memoryRequirements.memoryTypeBits, memoryProperties);
    if (vkAllocateMemory(device, &allocInfo, nullptr, &outMemory) != VK_SUCCESS) {
      outError = "failed to allocate buffer memory";
      return false;
    }

    if (vkBindBufferMemory(device, outBuffer, outMemory, 0) != VK_SUCCESS) {
      outError = "failed to bind buffer memory";
      return false;
    }

    if (initialData != nullptr && size > 0) {
      void* mapped = nullptr;
      if (vkMapMemory(device, outMemory, 0, size, 0, &mapped) != VK_SUCCESS) {
        outError = "failed to map buffer memory";
        return false;
      }
      std::memcpy(mapped, initialData, static_cast<std::size_t>(size));
      vkUnmapMemory(device, outMemory);
    }

    return true;
  }

  bool renderAtlas(const volt::io::FontGpuAtlas& atlas,
                   volt::io::RawImage& outImage,
                   std::string& outError) {
    if (atlas.atlasWidth == 0U || atlas.atlasHeight == 0U || atlas.jobs.empty() || atlas.curves.empty()) {
      outError = "font gpu atlas is empty";
      return false;
    }

    VkBuffer curveBuffer = VK_NULL_HANDLE;
    VkDeviceMemory curveMemory = VK_NULL_HANDLE;
    VkBuffer jobBuffer = VK_NULL_HANDLE;
    VkDeviceMemory jobMemory = VK_NULL_HANDLE;
    VkBuffer readbackBuffer = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
    VkImage atlasImage = VK_NULL_HANDLE;
    VkDeviceMemory atlasImageMemory = VK_NULL_HANDLE;
    VkImageView atlasImageView = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

    auto cleanupLocal = [&]() {
      if (descriptorSet != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(device, descriptorPool, 1, &descriptorSet);
      }
      if (commandBuffer != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
      }
      if (atlasImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, atlasImageView, nullptr);
      }
      if (atlasImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, atlasImage, nullptr);
      }
      if (atlasImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, atlasImageMemory, nullptr);
      }
      if (readbackBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, readbackBuffer, nullptr);
      }
      if (readbackMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, readbackMemory, nullptr);
      }
      if (jobBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, jobBuffer, nullptr);
      }
      if (jobMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, jobMemory, nullptr);
      }
      if (curveBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, curveBuffer, nullptr);
      }
      if (curveMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, curveMemory, nullptr);
      }
    };

    const VkDeviceSize curveBytes =
        static_cast<VkDeviceSize>(atlas.curves.size() * sizeof(volt::io::FontGpuCurveSegment));
    if (!createBuffer(
            curveBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            curveBuffer,
            curveMemory,
            atlas.curves.data(),
            outError)) {
      cleanupLocal();
      return false;
    }

    const VkDeviceSize jobBytes =
        static_cast<VkDeviceSize>(atlas.jobs.size() * sizeof(volt::io::FontGpuGlyphJob));
    if (!createBuffer(
            jobBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            jobBuffer,
            jobMemory,
            atlas.jobs.data(),
            outError)) {
      cleanupLocal();
      return false;
    }

    const VkDeviceSize readbackBytes =
        static_cast<VkDeviceSize>(atlas.atlasWidth) * static_cast<VkDeviceSize>(atlas.atlasHeight) * 8U;
    if (!createBuffer(
            readbackBytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            readbackBuffer,
            readbackMemory,
            nullptr,
            outError)) {
      cleanupLocal();
      return false;
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {atlas.atlasWidth, atlas.atlasHeight, 1U};
    imageInfo.mipLevels = 1U;
    imageInfo.arrayLayers = 1U;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device, &imageInfo, nullptr, &atlasImage) != VK_SUCCESS) {
      outError = "failed to create atlas image";
      cleanupLocal();
      return false;
    }

    VkMemoryRequirements imageMemoryRequirements{};
    vkGetImageMemoryRequirements(device, atlasImage, &imageMemoryRequirements);

    VkMemoryAllocateInfo imageAllocInfo{};
    imageAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imageAllocInfo.allocationSize = imageMemoryRequirements.size;
    imageAllocInfo.memoryTypeIndex = findMemoryType(
        imageMemoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &imageAllocInfo, nullptr, &atlasImageMemory) != VK_SUCCESS) {
      outError = "failed to allocate atlas image memory";
      cleanupLocal();
      return false;
    }

    if (vkBindImageMemory(device, atlasImage, atlasImageMemory, 0) != VK_SUCCESS) {
      outError = "failed to bind atlas image memory";
      cleanupLocal();
      return false;
    }

    VkImageViewCreateInfo imageViewInfo{};
    imageViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageViewInfo.image = atlasImage;
    imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    imageViewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageViewInfo.subresourceRange.baseMipLevel = 0U;
    imageViewInfo.subresourceRange.levelCount = 1U;
    imageViewInfo.subresourceRange.baseArrayLayer = 0U;
    imageViewInfo.subresourceRange.layerCount = 1U;
    if (vkCreateImageView(device, &imageViewInfo, nullptr, &atlasImageView) != VK_SUCCESS) {
      outError = "failed to create atlas image view";
      cleanupLocal();
      return false;
    }

    VkDescriptorSetAllocateInfo setAllocInfo{};
    setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAllocInfo.descriptorPool = descriptorPool;
    setAllocInfo.descriptorSetCount = 1U;
    setAllocInfo.pSetLayouts = &descriptorSetLayout;
    if (vkAllocateDescriptorSets(device, &setAllocInfo, &descriptorSet) != VK_SUCCESS) {
      outError = "failed to allocate descriptor set";
      cleanupLocal();
      return false;
    }

    const VkDescriptorBufferInfo curveBufferInfo{curveBuffer, 0U, curveBytes};
    const VkDescriptorBufferInfo jobBufferInfo{jobBuffer, 0U, jobBytes};
    const VkDescriptorImageInfo imageDescriptorInfo{VK_NULL_HANDLE, atlasImageView, VK_IMAGE_LAYOUT_GENERAL};
    const std::array<VkWriteDescriptorSet, 3> writes = {
        VkWriteDescriptorSet{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            nullptr,
            descriptorSet,
            0U,
            0U,
            1U,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            nullptr,
            &curveBufferInfo,
            nullptr,
        },
        VkWriteDescriptorSet{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            nullptr,
            descriptorSet,
            1U,
            0U,
            1U,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            nullptr,
            &jobBufferInfo,
            nullptr,
        },
        VkWriteDescriptorSet{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            nullptr,
            descriptorSet,
            2U,
            0U,
            1U,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            &imageDescriptorInfo,
            nullptr,
            nullptr,
        },
    };
    vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0U, nullptr);

    VkCommandBufferAllocateInfo commandAllocInfo{};
    commandAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandAllocInfo.commandPool = commandPool;
    commandAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocInfo.commandBufferCount = 1U;
    if (vkAllocateCommandBuffers(device, &commandAllocInfo, &commandBuffer) != VK_SUCCESS) {
      outError = "failed to allocate command buffer";
      cleanupLocal();
      return false;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
      outError = "failed to begin command buffer";
      cleanupLocal();
      return false;
    }

    VkImageMemoryBarrier toGeneral{};
    toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.image = atlasImage;
    toGeneral.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toGeneral.subresourceRange.baseMipLevel = 0U;
    toGeneral.subresourceRange.levelCount = 1U;
    toGeneral.subresourceRange.baseArrayLayer = 0U;
    toGeneral.subresourceRange.layerCount = 1U;
    toGeneral.srcAccessMask = 0U;
    toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0U,
        0U,
        nullptr,
        0U,
        nullptr,
        1U,
        &toGeneral);

      VkClearColorValue clearColor{};
      clearColor.float32[0] = 0.0F;
      clearColor.float32[1] = 0.0F;
      clearColor.float32[2] = 0.0F;
      clearColor.float32[3] = 0.0F;
      VkImageSubresourceRange clearRange{};
      clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      clearRange.baseMipLevel = 0U;
      clearRange.levelCount = 1U;
      clearRange.baseArrayLayer = 0U;
      clearRange.layerCount = 1U;
      vkCmdClearColorImage(
        commandBuffer,
        atlasImage,
        VK_IMAGE_LAYOUT_GENERAL,
        &clearColor,
        1U,
        &clearRange);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        pipelineLayout,
        0U,
        1U,
        &descriptorSet,
        0U,
        nullptr);

    const float spreadPx = std::max(1.0F, atlas.sdfSpreadPx);
    vkCmdPushConstants(
        commandBuffer,
        pipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0U,
        sizeof(float),
        &spreadPx);

    const std::uint32_t groupCountX = std::max(1U, (atlas.maxGlyphWidth + 7U) / 8U);
    const std::uint32_t groupCountY = std::max(1U, (atlas.maxGlyphHeight + 7U) / 8U);
    const std::uint32_t groupCountZ = static_cast<std::uint32_t>(atlas.jobs.size());
    vkCmdDispatch(commandBuffer, groupCountX, groupCountY, groupCountZ);

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = atlasImage;
    toTransfer.subresourceRange = toGeneral.subresourceRange;
    toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0U,
        0U,
        nullptr,
        0U,
        nullptr,
        1U,
        &toTransfer);

    VkBufferImageCopy copyRegion{};
    copyRegion.bufferOffset = 0U;
    copyRegion.bufferRowLength = 0U;
    copyRegion.bufferImageHeight = 0U;
    copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.mipLevel = 0U;
    copyRegion.imageSubresource.baseArrayLayer = 0U;
    copyRegion.imageSubresource.layerCount = 1U;
    copyRegion.imageOffset = {0, 0, 0};
    copyRegion.imageExtent = {atlas.atlasWidth, atlas.atlasHeight, 1U};
    vkCmdCopyImageToBuffer(
        commandBuffer,
        atlasImage,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        readbackBuffer,
        1U,
        &copyRegion);

    VkBufferMemoryBarrier hostBarrier{};
    hostBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostBarrier.buffer = readbackBuffer;
    hostBarrier.offset = 0U;
    hostBarrier.size = readbackBytes;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0U,
        0U,
        nullptr,
        1U,
        &hostBarrier,
        0U,
        nullptr);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
      outError = "failed to end command buffer";
      cleanupLocal();
      return false;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1U;
    submitInfo.pCommandBuffers = &commandBuffer;
    if (vkQueueSubmit(queue, 1U, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
      outError = "failed to submit compute work";
      cleanupLocal();
      return false;
    }
    if (vkQueueWaitIdle(queue) != VK_SUCCESS) {
      outError = "failed to wait for compute queue";
      cleanupLocal();
      return false;
    }

    void* mapped = nullptr;
    if (vkMapMemory(device, readbackMemory, 0U, readbackBytes, 0U, &mapped) != VK_SUCCESS) {
      outError = "failed to map readback buffer";
      cleanupLocal();
      return false;
    }

    outImage.width = atlas.atlasWidth;
    outImage.height = atlas.atlasHeight;
    outImage.rgba.resize(static_cast<std::size_t>(atlas.atlasWidth) * static_cast<std::size_t>(atlas.atlasHeight) * 4U);

    const std::uint16_t* halfPixels = static_cast<const std::uint16_t*>(mapped);
    for (std::size_t i = 0U; i < static_cast<std::size_t>(atlas.atlasWidth) * static_cast<std::size_t>(atlas.atlasHeight); ++i) {
      for (std::size_t channel = 0U; channel < 4U; ++channel) {
        const float value = std::clamp(halfToFloat(halfPixels[i * 4U + channel]), 0.0F, 1.0F);
        outImage.rgba[i * 4U + channel] = static_cast<std::uint8_t>(std::lround(value * 255.0F));
      }
    }
    vkUnmapMemory(device, readbackMemory);

    cleanupLocal();
    return true;
  }
};

std::size_t countMidrangeAlphaSamples(const volt::io::RawImage& image) {
  std::size_t count = 0U;
  for (std::size_t i = 3U; i < image.rgba.size(); i += 4U) {
    const std::uint8_t alpha = image.rgba[i];
    if (alpha > 24U && alpha < 231U) {
      ++count;
    }
  }
  return count;
}

std::size_t countOpaqueAlphaSamples(const volt::io::RawImage& image) {
  std::size_t count = 0U;
  for (std::size_t i = 3U; i < image.rgba.size(); i += 4U) {
    if (image.rgba[i] >= 128U) {
      ++count;
    }
  }
  return count;
}

std::size_t countOpaqueAlphaSamples(const std::vector<std::uint8_t>& rgba) {
  std::size_t count = 0U;
  for (std::size_t i = 3U; i < rgba.size(); i += 4U) {
    if (rgba[i] >= 128U) {
      ++count;
    }
  }
  return count;
}

std::size_t countChannelSeparatedSamples(const volt::io::RawImage& image) {
  std::size_t count = 0U;
  for (std::size_t i = 0U; i + 3U < image.rgba.size(); i += 4U) {
    const std::uint8_t r = image.rgba[i + 0U];
    const std::uint8_t g = image.rgba[i + 1U];
    const std::uint8_t b = image.rgba[i + 2U];
    const std::uint8_t maxValue = std::max(r, std::max(g, b));
    const std::uint8_t minValue = std::min(r, std::min(g, b));
    if (maxValue > minValue + 6U) {
      ++count;
    }
  }
  return count;
}

std::uint8_t amplifyDistanceByte(std::uint8_t value, float gain) {
  const float normalized = static_cast<float>(value) / 255.0F;
  const float amplified = std::clamp((normalized - 0.5F) * gain + 0.5F, 0.0F, 1.0F);
  return static_cast<std::uint8_t>(std::lround(amplified * 255.0F));
}

volt::io::RawImage makePreviewImage(const volt::io::RawImage& rawImage, FontRenderMode mode) {
  volt::io::RawImage preview{};
  preview.width = rawImage.width;
  preview.height = rawImage.height;
  preview.rgba.resize(rawImage.rgba.size(), 255U);

  for (std::size_t i = 0U; i + 3U < rawImage.rgba.size(); i += 4U) {
    if (mode == FontRenderMode::kMsdf) {
      preview.rgba[i + 0U] = amplifyDistanceByte(rawImage.rgba[i + 0U], 8.0F);
      preview.rgba[i + 1U] = amplifyDistanceByte(rawImage.rgba[i + 1U], 8.0F);
      preview.rgba[i + 2U] = amplifyDistanceByte(rawImage.rgba[i + 2U], 8.0F);
    } else {
      const std::uint8_t alphaPreview = amplifyDistanceByte(rawImage.rgba[i + 3U], 8.0F);
      preview.rgba[i + 0U] = alphaPreview;
      preview.rgba[i + 1U] = alphaPreview;
      preview.rgba[i + 2U] = alphaPreview;
    }
    preview.rgba[i + 3U] = 255U;
  }

  return preview;
}

bool renderAndValidateMode(const std::filesystem::path& workspaceRoot,
                           const std::filesystem::path& sandbox,
                           const std::filesystem::path& shaderPath,
                           FontRenderMode mode,
                           bool* ok) {
  if (ok == nullptr || !*ok) {
    return false;
  }

  std::error_code ec;
  const std::filesystem::path sandboxFont = sandbox / "assets" / "fonts" / "DefaultFont.ttf";
  const std::vector<int> codepoints = testCodepoints();

  volt::io::detail::SfntAtlasBuildOptions buildOptions{};
  buildOptions.rasterMode =
      mode == FontRenderMode::kMsdf
          ? volt::io::detail::SfntAtlasRasterMode::kMultiChannelSignedDistanceField
          : volt::io::detail::SfntAtlasRasterMode::kSignedDistanceField;
  buildOptions.sdfSpreadPx = 8;

  volt::io::detail::SfntGpuAtlasResult gpuAtlasResult{};
  *ok = expect(
            volt::io::detail::buildSfntGpuAtlasFromFile(
                sandboxFont,
                48.0F,
                codepoints,
                buildOptions,
                gpuAtlasResult),
            std::string("build GPU ") + modeString(mode) + " atlas") && *ok;
  *ok = expect(gpuAtlasResult.success, std::string("GPU ") + modeString(mode) + " atlas should succeed") && *ok;

  volt::io::FontGpuAtlas atlas{};
  if (*ok) {
    atlas.atlasWidth = gpuAtlasResult.atlasWidth;
    atlas.atlasHeight = gpuAtlasResult.atlasHeight;
    atlas.maxGlyphWidth = gpuAtlasResult.maxGlyphWidth;
    atlas.maxGlyphHeight = gpuAtlasResult.maxGlyphHeight;
    atlas.sdfSpreadPx = static_cast<float>(buildOptions.sdfSpreadPx);
    atlas.msdfEnabled = mode == FontRenderMode::kMsdf;
    atlas.curves.resize(gpuAtlasResult.curves.size());
    for (std::size_t i = 0U; i < gpuAtlasResult.curves.size(); ++i) {
      const auto& in = gpuAtlasResult.curves[i];
      atlas.curves[i] = volt::io::FontGpuCurveSegment{
          in.p0x,
          in.p0y,
          in.p1x,
          in.p1y,
          in.p2x,
          in.p2y,
          in.p3x,
          in.p3y,
          in.type,
          in.channelMask,
          in.contourSign,
          in.pad2,
      };
    }
    atlas.jobs.resize(gpuAtlasResult.jobs.size());
    for (std::size_t i = 0U; i < gpuAtlasResult.jobs.size(); ++i) {
      const auto& in = gpuAtlasResult.jobs[i];
      atlas.jobs[i] = volt::io::FontGpuGlyphJob{
          in.atlasX,
          in.atlasY,
          in.glyphWidth,
          in.glyphHeight,
          in.curveOffset,
          in.curveCount,
          in.pad0,
          in.pad1,
      };
    }
  }

  *ok = expect(!atlas.jobs.empty(), std::string(modeString(mode)) + " gpu atlas should contain glyph jobs") && *ok;
  *ok = expect(!atlas.curves.empty(), std::string(modeString(mode)) + " gpu atlas should contain curve segments") && *ok;

  std::string error;
  VulkanComputeAtlasRenderer renderer{};
  *ok = expect(renderer.initialize(shaderPath, error), error.empty() ? "initialize compute renderer" : error) && *ok;

  volt::io::RawImage atlasImage{};
  if (*ok) {
    *ok = expect(renderer.renderAtlas(atlas, atlasImage, error), error.empty() ? "render compute atlas" : error) && *ok;
  }

  const std::filesystem::path outputPath =
      sandbox / "assets" / "images" / (std::string("ui-font-default-compute-") + modeString(mode) + ".png");
  const volt::io::RawImage previewImage = makePreviewImage(atlasImage, mode);
  const std::filesystem::path previewOutputPath =
      sandbox / "assets" / "images" / (std::string("ui-font-default-compute-") + modeString(mode) + "-preview.png");
  if (*ok) {
    *ok = expect(volt::io::encodeImageFile(outputPath, atlasImage, volt::io::ImageEncodeFormat::kPng),
                 std::string("encode ") + modeString(mode) + " compute atlas PNG") && *ok;
    *ok = expect(volt::io::encodeImageFile(previewOutputPath, previewImage, volt::io::ImageEncodeFormat::kPng),
                 std::string("encode ") + modeString(mode) + " preview atlas PNG") && *ok;
    *ok = expect(std::filesystem::exists(outputPath, ec) && !ec,
                 std::string(modeString(mode)) + " compute atlas PNG should be written") && *ok;
    *ok = expect(countMidrangeAlphaSamples(atlasImage) > 128U,
                 std::string(modeString(mode)) + " compute atlas should contain anti-aliased alpha samples") && *ok;
  }

  if (*ok) {
    std::error_code logEc;
    const std::filesystem::path logsDir = workspaceRoot / "logs";
    std::filesystem::create_directories(logsDir, logEc);
    if (!logEc) {
      const std::filesystem::path workspaceOutput =
          logsDir / (std::string("ui-font-default-compute-") + modeString(mode) + ".png");
      const std::filesystem::path workspacePreviewOutput =
          logsDir / (std::string("ui-font-default-compute-") + modeString(mode) + "-preview.png");
      *ok = expect(
                volt::io::encodeImageFile(workspaceOutput, atlasImage, volt::io::ImageEncodeFormat::kPng),
                std::string("write ") + modeString(mode) + " atlas PNG to workspace logs") && *ok;
      *ok = expect(
                volt::io::encodeImageFile(workspacePreviewOutput, previewImage, volt::io::ImageEncodeFormat::kPng),
                std::string("write ") + modeString(mode) + " preview atlas PNG to workspace logs") && *ok;
    }
  }

  volt::io::detail::SfntAtlasResult cpuAtlas{};
  *ok = expect(
        volt::io::detail::buildSfntAtlasFromFile(sandboxFont, 48.0F, codepoints, buildOptions, cpuAtlas),
            std::string("build CPU reference ") + modeString(mode) + " atlas") && *ok;
  *ok = expect(cpuAtlas.success, std::string("CPU reference ") + modeString(mode) + " atlas should succeed") && *ok;

  if (*ok) {
    const std::size_t gpuOpaque = countOpaqueAlphaSamples(atlasImage);
    const std::size_t cpuOpaque = countOpaqueAlphaSamples(cpuAtlas.rgba);
    const std::size_t maxOpaque = std::max<std::size_t>(1U, std::max(gpuOpaque, cpuOpaque));
    const std::size_t opaqueDelta = gpuOpaque > cpuOpaque ? gpuOpaque - cpuOpaque : cpuOpaque - gpuOpaque;
    if (!(opaqueDelta * 5U < maxOpaque * 4U)) {
      std::cerr << "[font-gpu-atlas-test] coverage mismatch mode=" << modeString(mode)
                << " gpuOpaque=" << gpuOpaque
                << " cpuOpaque=" << cpuOpaque
                << " delta=" << opaqueDelta
                << " max=" << maxOpaque << '\n';
    }
    *ok = expect(opaqueDelta * 5U < maxOpaque * 4U,
                 std::string(modeString(mode)) + " alpha coverage should roughly match CPU reference") && *ok;
  }

  if (*ok && mode == FontRenderMode::kMsdf) {
    *ok = expect(countChannelSeparatedSamples(atlasImage) > 512U,
                 "msdf compute atlas should contain RGB-separated edge samples") && *ok;
  }

  if (*ok) {
    std::cout << "[font-gpu-atlas-test] wrote " << modeString(mode) << " atlas image to "
              << outputPath.string() << '\n';
    std::cout << "[font-gpu-atlas-test] copied " << modeString(mode) << " atlas image to "
              << ((workspaceRoot / "logs" / (std::string("ui-font-default-compute-") + modeString(mode) + ".png")).string())
              << '\n';
    std::cout << "[font-gpu-atlas-test] copied " << modeString(mode) << " preview atlas image to "
          << ((workspaceRoot / "logs" / (std::string("ui-font-default-compute-") + modeString(mode) + "-preview.png")).string())
          << '\n';
  }

  return *ok;
}

}  // namespace

int main(int argc, char** argv) {
  bool ok = true;

  const auto workspaceRoot = findWorkspaceRoot();
  ok = expect(workspaceRoot.has_value(), "locate workspace root with assets/fonts/DefaultFont.ttf") && ok;
  if (!workspaceRoot.has_value()) {
    return 1;
  }

  const auto shaderPath = findCompiledComputeShader(argc > 0 ? argv[0] : "");
  ok = expect(shaderPath.has_value(), "locate compiled msdfgen.comp.spv shader") && ok;
  if (!shaderPath.has_value()) {
    return 1;
  }

  const std::filesystem::path sandbox = std::filesystem::temp_directory_path() / "volt-font-gpu-atlas-tests";
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
  std::filesystem::create_directories(sandbox / "assets" / "fonts", ec);
  std::filesystem::create_directories(sandbox / "assets" / "images", ec);
  ok = expect(!ec, "create sandbox directories") && ok;

  const std::filesystem::path sourceFont = *workspaceRoot / "assets" / "fonts" / "DefaultFont.ttf";
  std::filesystem::copy_file(
      sourceFont,
      sandbox / "assets" / "fonts" / "DefaultFont.ttf",
      std::filesystem::copy_options::overwrite_existing,
      ec);
  ok = expect(!ec, "copy default font into sandbox") && ok;
  ok = expect(writeManifest(sandbox / "assets" / "manifest.json", FontRenderMode::kSdf), "write sandbox manifest") && ok;
  if (!ok) {
    return 1;
  }

  const std::filesystem::path oldCwd = std::filesystem::current_path(ec);
  std::filesystem::current_path(sandbox, ec);
  ok = expect(!ec, "switch to sandbox cwd") && ok;

  if (ok) {
    renderAndValidateMode(*workspaceRoot, sandbox, *shaderPath, FontRenderMode::kSdf, &ok);
  }
  if (ok) {
    renderAndValidateMode(*workspaceRoot, sandbox, *shaderPath, FontRenderMode::kMsdf, &ok);
  }

  std::filesystem::current_path(oldCwd, ec);

  if (!ok) {
    std::cerr << "[font-gpu-atlas-test] One or more tests failed." << '\n';
    return 1;
  }

  std::cout << "[font-gpu-atlas-test] GPU atlas render test passed." << '\n';
  return 0;
}