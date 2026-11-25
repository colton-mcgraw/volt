#pragma once

#include <gpu/vulkan.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace volt::gpu
{

    class context;

    // A compiled shader: SPIR-V, as the build embeds it (see the shader
    // section of src/CMakeLists.txt).
    using shader_code = std::span<const std::uint32_t>;

    enum class blending
    {
        // The pass replaces what is there.
        opaque,

        // Colors come out of the shader already multiplied by their alpha.
        // That is the form in which coverage and transparency compose
        // correctly, and the only one the renderers use.
        premultiplied,
    };

    // What the renderers' pipelines differ in. Everything else is the same
    // for all of them: one color attachment, no multisampling, viewport and
    // scissor set while drawing.
    struct graphics_pipeline_description
    {
        shader_code vertex;
        shader_code fragment;
        VkPipelineLayout layout = VK_NULL_HANDLE;

        // The formats of the images the pass draws into. With a depth format
        // the pipeline tests against the depth buffer with `depth_compare`,
        // and writes it unless `depth_write` is off (what is drawn over
        // what is there without hiding what comes after).
        VkFormat color_format = VK_FORMAT_UNDEFINED;
        VkFormat depth_format = VK_FORMAT_UNDEFINED;
        VkCompareOp depth_compare = VK_COMPARE_OP_LESS;
        bool depth_write = true;

        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        gpu::blending blending = blending::opaque;

        // The renderers draw instances: a shape the vertex shader makes up
        // from the vertex index, once per element of one buffer, binding 0.
        // A shape that comes from a mesh instead reads its vertices from
        // binding 1, one element of `vertex_stride` bytes per vertex; zero
        // leaves binding 1 out. Each attribute names its binding.
        std::uint32_t instance_stride = 0;
        std::uint32_t vertex_stride = 0;
        std::span<const VkVertexInputAttributeDescription> attributes;
    };

    // The caller owns the pipeline.
    std::expected<VkPipeline, std::string> create_graphics_pipeline(context &context,
                                                                    const graphics_pipeline_description &description);

    std::expected<VkPipeline, std::string> create_compute_pipeline(context &context, shader_code code,
                                                                   VkPipelineLayout layout);

} // namespace volt::gpu
