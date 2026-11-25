#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// -------------------------------------------------------------------------
// reader
// -------------------------------------------------------------------------
//
// Every byte of a font file is untrusted, so every access goes through this
// view. A read past the end does not fail loudly: it yields zero, the same
// as a truncated table would if it had been padded. Structure that matters
// (a count of records, an offset to follow) is checked by the caller with
// contains() before it is trusted, and loops over counts read from the file
// clamp them to what the data can actually hold, so that a count of four
// billion costs nothing.
//
// All sfnt and CFF numbers are big-endian.

namespace volt::font::detail
{

    class bytes
    {
    public:
        bytes() = default;
        explicit bytes(std::span<const std::uint8_t> data) : m_data(data) {}

        std::size_t size() const noexcept { return m_data.size(); }
        bool empty() const noexcept { return m_data.empty(); }
        std::span<const std::uint8_t> span() const noexcept { return m_data; }

        // Whether [offset, offset + length) lies inside, without overflow.
        bool contains(std::size_t offset, std::size_t length) const noexcept
        {
            return offset <= m_data.size() && length <= m_data.size() - offset;
        }

        std::uint8_t u8(std::size_t offset) const noexcept
        {
            return offset < m_data.size() ? m_data[offset] : std::uint8_t{0};
        }

        std::uint16_t u16(std::size_t offset) const noexcept
        {
            if (!contains(offset, 2))
                return 0;
            return static_cast<std::uint16_t>((m_data[offset] << 8) | m_data[offset + 1]);
        }

        std::int16_t i16(std::size_t offset) const noexcept { return static_cast<std::int16_t>(u16(offset)); }

        std::uint32_t u24(std::size_t offset) const noexcept
        {
            if (!contains(offset, 3))
                return 0;
            return (std::uint32_t{m_data[offset]} << 16) | (std::uint32_t{m_data[offset + 1]} << 8) |
                   std::uint32_t{m_data[offset + 2]};
        }

        std::uint32_t u32(std::size_t offset) const noexcept
        {
            if (!contains(offset, 4))
                return 0;
            return (std::uint32_t{m_data[offset]} << 24) | (std::uint32_t{m_data[offset + 1]} << 16) |
                   (std::uint32_t{m_data[offset + 2]} << 8) | std::uint32_t{m_data[offset + 3]};
        }

        std::int32_t i32(std::size_t offset) const noexcept { return static_cast<std::int32_t>(u32(offset)); }

        // A big-endian unsigned integer of 1 to 4 bytes, as CFF offsets are.
        std::uint32_t uint(std::size_t offset, std::size_t width) const noexcept
        {
            switch (width)
            {
            case 1:
                return u8(offset);
            case 2:
                return u16(offset);
            case 3:
                return u24(offset);
            case 4:
                return u32(offset);
            default:
                return 0;
            }
        }

        // The part [offset, offset + length), cut short at the end; empty if
        // it starts beyond.
        bytes sub(std::size_t offset, std::size_t length) const noexcept
        {
            if (offset >= m_data.size())
                return {};
            if (length > m_data.size() - offset)
                length = m_data.size() - offset;
            return bytes(m_data.subspan(offset, length));
        }

        bytes from(std::size_t offset) const noexcept
        {
            if (offset >= m_data.size())
                return {};
            return bytes(m_data.subspan(offset));
        }

        // How many records of `record` bytes starting at `offset` actually
        // fit, at most `count`. What every loop over a count from the file is
        // bounded by.
        std::size_t fit(std::size_t offset, std::size_t count, std::size_t record) const noexcept
        {
            if (offset >= m_data.size() || record == 0)
                return 0;
            const std::size_t available = (m_data.size() - offset) / record;
            return count < available ? count : available;
        }

    private:
        std::span<const std::uint8_t> m_data;
    };

    constexpr std::uint32_t tag(char a, char b, char c, char d) noexcept
    {
        return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8) |
               static_cast<std::uint32_t>(static_cast<unsigned char>(d));
    }

} // namespace volt::font::detail
