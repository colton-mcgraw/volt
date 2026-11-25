#pragma once

#include <gpu/presenter.hpp>
#include <gpu/vulkan.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace volt::gpu
{

    class context;

    // The index of a memory type that is one of `allowed` (the bit mask a
    // resource's memory requirements name) and has all of `properties`.
    std::optional<std::uint32_t> find_memory_type(VkPhysicalDevice device, std::uint32_t allowed,
                                                  VkMemoryPropertyFlags properties);

    struct buffer_description
    {
        VkDeviceSize size = 0;
        VkBufferUsageFlags usage = 0;

        // Memory the CPU can write: the buffer stays mapped for its whole
        // life, see buffer::data(). Otherwise it lives on the GPU only.
        bool mapped = false;

        // Used on the graphics and on the compute queue. Matters where those
        // are different queue families, which otherwise would have to hand
        // the buffer back and forth.
        bool shared = false;
    };

    // ---------------------------------------------------------------------
    // buffer
    // ---------------------------------------------------------------------
    //
    // A buffer with memory of its own. One allocation per buffer: there are
    // a few dozen of them, far from where that starts to matter.
    class buffer
    {
    public:
        buffer() = default;
        ~buffer() { destroy(); }

        buffer(const buffer &) = delete;
        buffer &operator=(const buffer &) = delete;

        std::expected<void, std::string> create(context &context, const buffer_description &description);

        // Nothing on the GPU may still be using it.
        void destroy();

        bool is_created() const noexcept { return m_handle != VK_NULL_HANDLE; }
        VkBuffer handle() const noexcept { return m_handle; }
        VkDeviceSize size() const noexcept { return m_size; }

        // Where a mapped buffer's bytes are. Null otherwise.
        std::byte *data() const noexcept { return m_data; }

    private:
        context *m_context = nullptr;
        VkBuffer m_handle = VK_NULL_HANDLE;
        VkDeviceMemory m_memory = VK_NULL_HANDLE;
        VkDeviceSize m_size = 0;
        std::byte *m_data = nullptr;
    };

    // Where stream::append() put its bytes.
    struct stream_range
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
    };

    // ---------------------------------------------------------------------
    // stream
    // ---------------------------------------------------------------------
    //
    // What a renderer rebuilds every frame: instances, vertices. One mapped
    // buffer per frame in flight, because the GPU may still be reading what
    // the previous frame wrote. Each grows to the largest frame it has seen.
    //
    // Two ways to fill it, and a stream is used one way only:
    //
    //   * write() replaces the slot's contents, once per frame: for a
    //     renderer that draws once per frame (a view into its target).
    //
    //   * append() adds to what the frame has put in so far: for a renderer
    //     that draws several times in one frame (the interface, once per
    //     window), where every draw's bytes have to stay where they are
    //     until the frame is through.
    class stream
    {
    public:
        // Offsets append() hands out are multiples of this. Vulkan caps
        // every buffer offset alignment a device may ask for (uniform,
        // storage, texel) at 256, so this fits any use.
        static constexpr VkDeviceSize append_alignment = 256;

        // `usage` as for a buffer, e.g. VK_BUFFER_USAGE_VERTEX_BUFFER_BIT.
        void create(context &context, VkBufferUsageFlags usage);
        void destroy();

        // Replaces the contents for the frame in `slot` (frame::slot) and
        // returns the buffer holding them. `bytes` must not be empty. Once
        // per frame: a second write would pull the first out from under the
        // commands already recorded.
        std::expected<VkBuffer, std::string> write(std::size_t slot, std::span<const std::byte> bytes);

        template <typename T>
        std::expected<VkBuffer, std::string> write(std::size_t slot, std::span<const T> items)
        {
            return write(slot, std::as_bytes(items));
        }

        // Adds `bytes` to what `frame` has appended so far and says where
        // they are. The first append of a frame (one with another number
        // than the last) starts the slot over. What earlier appends of the
        // same frame returned stays valid: when the slot's buffer is too
        // small, a bigger one takes over for the rest of the frame, and the
        // old one is kept until the slot comes round again, by when the
        // frame is through. `bytes` must not be empty.
        std::expected<stream_range, std::string> append(const frame &frame, std::span<const std::byte> bytes);

        template <typename T>
        std::expected<stream_range, std::string> append(const frame &frame, std::span<const T> items)
        {
            return append(frame, std::as_bytes(items));
        }

    private:
        struct slot
        {
            std::unique_ptr<buffer> current;

            // Buffers append() outgrew in the frame that has the slot. Their
            // commands are still to run.
            std::vector<std::unique_ptr<buffer>> outgrown;

            VkDeviceSize used = 0;     // append(): bytes in `current` so far
            std::uint64_t number = 0;  // append(): the frame they are from
        };

        // A buffer of at least `size` bytes for `slot`, replacing the one it
        // has; the old one goes to `outgrown` if `keep`, else right away.
        std::expected<void, std::string> grow(slot &slot, VkDeviceSize size, bool keep);

        context *m_context = nullptr;
        VkBufferUsageFlags m_usage = 0;
        std::array<slot, presenter::frames_in_flight> m_slots;
    };

} // namespace volt::gpu
