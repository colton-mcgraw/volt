#include <gpu/buffer.hpp>

#include <gpu/context.hpp>

#include <algorithm>
#include <cstring>

namespace volt::gpu
{

    std::optional<std::uint32_t> find_memory_type(VkPhysicalDevice device, std::uint32_t allowed,
                                                  VkMemoryPropertyFlags properties)
    {
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(device, &memory);

        // The device lists its types best first, so the first match is the
        // one to take.
        for (std::uint32_t type = 0; type < memory.memoryTypeCount; ++type)
        {
            const bool is_allowed = (allowed & (1u << type)) != 0;
            if (is_allowed && (memory.memoryTypes[type].propertyFlags & properties) == properties)
                return type;
        }
        return std::nullopt;
    }

    // ---------------------------------------------------------------------
    // buffer
    // ---------------------------------------------------------------------

    std::expected<void, std::string> buffer::create(context &context, const buffer_description &description)
    {
        if (m_handle != VK_NULL_HANDLE)
            return std::unexpected("the buffer is already created");
        if (description.size == 0)
            return std::unexpected("a buffer cannot be empty");

        m_context = &context;
        const VkDevice device = context.device();

        const auto fail = [this](std::unexpected<std::string> error) {
            destroy();
            return error;
        };

        const std::uint32_t families[] = {context.graphics().family(), context.compute().family()};
        const bool concurrent = description.shared && families[0] != families[1];

        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = description.size;
        info.usage = description.usage;
        info.sharingMode = concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
        info.queueFamilyIndexCount = concurrent ? 2u : 0u;
        info.pQueueFamilyIndices = concurrent ? families : nullptr;
        if (const VkResult result = vkCreateBuffer(device, &info, nullptr, &m_handle); result != VK_SUCCESS)
            return fail(failure("vkCreateBuffer", result));

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, m_handle, &requirements);

        // Coherent, so that what is written needs no flush to reach the GPU.
        const VkMemoryPropertyFlags properties =
            description.mapped ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                               : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        const auto type = find_memory_type(context.physical_device(), requirements.memoryTypeBits, properties);
        if (!type)
            return fail(std::unexpected(std::string("the GPU has no memory type fit for the buffer")));

        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = *type;
        if (const VkResult result = vkAllocateMemory(device, &allocation, nullptr, &m_memory); result != VK_SUCCESS)
            return fail(failure("vkAllocateMemory", result));

        if (const VkResult result = vkBindBufferMemory(device, m_handle, m_memory, 0); result != VK_SUCCESS)
            return fail(failure("vkBindBufferMemory", result));

        if (description.mapped)
        {
            void *mapped = nullptr;
            if (const VkResult result = vkMapMemory(device, m_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
                result != VK_SUCCESS)
                return fail(failure("vkMapMemory", result));
            m_data = static_cast<std::byte *>(mapped);
        }

        m_size = description.size;
        return {};
    }

    void buffer::destroy()
    {
        if (m_context == nullptr)
            return;

        const VkDevice device = m_context->device();
        if (m_handle != VK_NULL_HANDLE)
            vkDestroyBuffer(device, m_handle, nullptr);
        if (m_memory != VK_NULL_HANDLE)
            vkFreeMemory(device, m_memory, nullptr); // unmaps it as well

        m_context = nullptr;
        m_handle = VK_NULL_HANDLE;
        m_memory = VK_NULL_HANDLE;
        m_size = 0;
        m_data = nullptr;
    }

    // ---------------------------------------------------------------------
    // stream
    // ---------------------------------------------------------------------

    void stream::create(context &context, VkBufferUsageFlags usage)
    {
        m_context = &context;
        m_usage = usage;
    }

    void stream::destroy()
    {
        for (slot &slot : m_slots)
            slot = {};
        m_context = nullptr;
    }

    std::expected<void, std::string> stream::grow(slot &slot, VkDeviceSize size, bool keep)
    {
        // Doubling keeps a scene that grows steadily from reallocating every
        // frame.
        const VkDeviceSize current = slot.current ? slot.current->size() : 0;

        buffer_description description;
        description.size = std::max<VkDeviceSize>({size, current * 2, 4096});
        description.usage = m_usage;
        description.mapped = true;

        if (slot.current && keep)
            slot.outgrown.push_back(std::move(slot.current));
        slot.current = std::make_unique<buffer>();
        if (auto created = slot.current->create(*m_context, description); !created)
        {
            slot.current.reset();
            return std::unexpected(std::move(created).error());
        }
        return {};
    }

    std::expected<VkBuffer, std::string> stream::write(std::size_t index, std::span<const std::byte> bytes)
    {
        slot &slot = m_slots[index];

        // The frame that used this slot last is through the GPU (the
        // presenter waited for it), so an old buffer can go right away.
        if (!slot.current || slot.current->size() < bytes.size())
        {
            if (auto grown = grow(slot, bytes.size(), false); !grown)
                return std::unexpected(std::move(grown).error());
        }

        std::memcpy(slot.current->data(), bytes.data(), bytes.size());
        return slot.current->handle();
    }

    std::expected<stream_range, std::string> stream::append(const frame &frame, std::span<const std::byte> bytes)
    {
        slot &slot = m_slots[frame.slot];

        // A new frame in the slot: the one before it is through the GPU, and
        // so is everything it appended, outgrown buffers included.
        if (slot.number != frame.number)
        {
            slot.outgrown.clear();
            slot.used = 0;
            slot.number = frame.number;
        }

        const VkDeviceSize offset = (slot.used + append_alignment - 1) / append_alignment * append_alignment;
        if (!slot.current || slot.current->size() < offset + bytes.size())
        {
            // What this frame appended before stays in the old buffer, which
            // its commands name; the rest of the frame goes to the new one.
            if (auto grown = grow(slot, offset + bytes.size(), true); !grown)
                return std::unexpected(std::move(grown).error());

            slot.used = 0;
            std::memcpy(slot.current->data(), bytes.data(), bytes.size());
            slot.used = bytes.size();
            return stream_range{slot.current->handle(), 0};
        }

        std::memcpy(slot.current->data() + offset, bytes.data(), bytes.size());
        slot.used = offset + bytes.size();
        return stream_range{slot.current->handle(), offset};
    }

} // namespace volt::gpu
