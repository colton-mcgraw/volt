#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Building Huffman codes for the encoders: DEFLATE's (lengths up to 15 bits,
// or 7 for the code length code) and JPEG's (up to 16 bits). Internal to the
// image library.

namespace volt::image::detail
{

    // Code lengths for the given symbol frequencies, none longer than
    // `limit`. Symbols with frequency 0 get length 0. A lone used symbol gets
    // length 1. `limit` must leave room for every used symbol
    // (2^limit >= used).
    inline std::vector<std::uint8_t> huffman_lengths(std::span<const std::uint32_t> frequencies, int limit)
    {
        std::vector<std::uint8_t> lengths(frequencies.size(), 0);

        std::vector<std::uint32_t> used; // symbols, by ascending frequency
        for (std::uint32_t s = 0; s < frequencies.size(); ++s)
            if (frequencies[s] != 0)
                used.push_back(s);
        if (used.empty())
            return lengths;
        if (used.size() == 1)
        {
            lengths[used[0]] = 1;
            return lengths;
        }
        std::stable_sort(used.begin(), used.end(),
                         [&](std::uint32_t a, std::uint32_t b) { return frequencies[a] < frequencies[b]; });

        // The two-queue construction: leaves in ascending order, and the
        // merged nodes, which come out in ascending order by themselves.
        const std::size_t n = used.size();
        std::vector<std::uint64_t> weight(2 * n - 1);
        std::vector<std::size_t> parent(2 * n - 1, 0);
        for (std::size_t i = 0; i < n; ++i)
            weight[i] = frequencies[used[i]];

        std::size_t leaf = 0;
        std::size_t inner = n;
        std::size_t next = n;
        const auto take = [&]() -> std::size_t {
            if (leaf < n && (inner >= next || weight[leaf] <= weight[inner]))
                return leaf++;
            return inner++;
        };
        for (; next < 2 * n - 1; ++next)
        {
            const std::size_t a = take();
            const std::size_t b = take();
            weight[next] = weight[a] + weight[b];
            parent[a] = next;
            parent[b] = next;
        }

        // Depths, from the root (the last node) down.
        std::vector<int> depth(2 * n - 1, 0);
        for (std::size_t i = 2 * n - 1; i-- > 0;)
            depth[i] = i == 2 * n - 2 ? 0 : depth[parent[i]] + 1;

        // Count per length; then pull over-long codes up the way JPEG's
        // Annex K.3 does, which keeps the code complete.
        std::vector<std::uint32_t> count(static_cast<std::size_t>(std::max(limit, 1)) + 64, 0);
        int longest = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            longest = std::max(longest, depth[i]);
            if (static_cast<std::size_t>(depth[i]) >= count.size())
                count.resize(static_cast<std::size_t>(depth[i]) + 1, 0);
            ++count[static_cast<std::size_t>(depth[i])];
        }
        for (int i = longest; i > limit; --i)
        {
            const auto at = static_cast<std::size_t>(i);
            while (count[at] > 0)
            {
                std::size_t j = at - 2;
                while (count[j] == 0)
                    --j;
                count[at] -= 2;
                count[at - 1] += 1;
                count[j + 1] += 2;
                count[j] -= 1;
            }
        }

        // The most frequent symbols get the shortest codes.
        std::size_t symbol = n;
        for (std::size_t length = 1; length <= static_cast<std::size_t>(limit) && length < count.size(); ++length)
            for (std::uint32_t k = 0; k < count[length]; ++k)
                lengths[used[--symbol]] = static_cast<std::uint8_t>(length);
        return lengths;
    }

    // Canonical codes for the lengths, most significant bit first, as both
    // DEFLATE (before bit reversal) and JPEG assign them.
    inline std::vector<std::uint16_t> canonical_codes(std::span<const std::uint8_t> lengths)
    {
        std::uint32_t count[33] = {};
        for (const std::uint8_t length : lengths)
            ++count[length];
        count[0] = 0;
        std::uint32_t next[33] = {};
        std::uint32_t code = 0;
        for (int length = 1; length <= 32; ++length)
        {
            code = (code + count[length - 1]) << 1;
            next[length] = code;
        }
        std::vector<std::uint16_t> codes(lengths.size(), 0);
        for (std::size_t s = 0; s < lengths.size(); ++s)
            if (lengths[s] != 0)
                codes[s] = static_cast<std::uint16_t>(next[lengths[s]]++);
        return codes;
    }

    inline std::uint32_t reverse_bits(std::uint32_t value, int count) noexcept
    {
        std::uint32_t result = 0;
        for (int i = 0; i < count; ++i)
        {
            result = (result << 1) | (value & 1u);
            value >>= 1;
        }
        return result;
    }

} // namespace volt::image::detail
