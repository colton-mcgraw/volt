#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Reading and writing the integers file formats are made of. Internal to the
// image library.

namespace volt::image::detail
{

    // Reads from a span, bounds-checked. A read past the end returns zeros
    // and marks the reader failed for good, so a parser can read a whole
    // header and check ok() once instead of after every field.
    class byte_reader
    {
    public:
        explicit byte_reader(std::span<const std::uint8_t> data) noexcept : m_data(data) {}

        bool ok() const noexcept { return !m_failed; }
        std::size_t position() const noexcept { return m_position; }
        std::size_t size() const noexcept { return m_data.size(); }
        std::size_t remaining() const noexcept { return m_data.size() - m_position; }
        bool at_end() const noexcept { return m_position >= m_data.size(); }

        std::uint8_t u8() noexcept
        {
            if (m_position >= m_data.size())
            {
                m_failed = true;
                return 0;
            }
            return m_data[m_position++];
        }

        std::uint16_t be16() noexcept
        {
            const std::uint32_t a = u8();
            return static_cast<std::uint16_t>(a << 8 | u8());
        }

        std::uint32_t be32() noexcept
        {
            const std::uint32_t a = be16();
            return a << 16 | be16();
        }

        std::uint16_t le16() noexcept
        {
            const std::uint32_t a = u8();
            return static_cast<std::uint16_t>(a | std::uint32_t{u8()} << 8);
        }

        std::uint32_t le32() noexcept
        {
            const std::uint32_t a = le16();
            return a | std::uint32_t{le16()} << 16;
        }

        // The next `count` bytes, or an empty span (and failure) if there
        // are not that many.
        std::span<const std::uint8_t> bytes(std::size_t count) noexcept
        {
            if (count > remaining())
            {
                m_failed = true;
                m_position = m_data.size();
                return {};
            }
            const auto result = m_data.subspan(m_position, count);
            m_position += count;
            return result;
        }

        void skip(std::size_t count) noexcept { (void)bytes(count); }

        void seek(std::size_t position) noexcept
        {
            if (position > m_data.size())
            {
                m_failed = true;
                position = m_data.size();
            }
            m_position = position;
        }

    private:
        std::span<const std::uint8_t> m_data;
        std::size_t m_position = 0;
        bool m_failed = false;
    };

    inline void put_be16(std::vector<std::uint8_t> &out, std::uint32_t value)
    {
        out.push_back(static_cast<std::uint8_t>(value >> 8));
        out.push_back(static_cast<std::uint8_t>(value));
    }

    inline void put_be32(std::vector<std::uint8_t> &out, std::uint32_t value)
    {
        put_be16(out, value >> 16);
        put_be16(out, value & 0xFFFFu);
    }

    inline void put_le16(std::vector<std::uint8_t> &out, std::uint32_t value)
    {
        out.push_back(static_cast<std::uint8_t>(value));
        out.push_back(static_cast<std::uint8_t>(value >> 8));
    }

    inline void put_le32(std::vector<std::uint8_t> &out, std::uint32_t value)
    {
        put_le16(out, value & 0xFFFFu);
        put_le16(out, value >> 16);
    }

} // namespace volt::image::detail
