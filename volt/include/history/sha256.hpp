#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// -------------------------------------------------------------------------
// sha256
// -------------------------------------------------------------------------
//
// SHA-256 (FIPS 180-4), which names what the history keeps on disk: a
// file's bytes are stored under the hash of those bytes, and a snapshot
// under the hash of its text, so the same bytes are stored once, and an
// object that has been damaged no longer matches its name.
//
//   history::sha256_hex("abc")
//       "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
//
//   history::sha256 hasher;          // for bytes that arrive in pieces
//   hasher.update(first);
//   hasher.update(second);
//   const history::sha256_digest digest = hasher.finish();
//
// Written here rather than taken from a library, as everything in Volt is.
// It is for naming and checking, not for secrets: it does nothing to resist
// timing attacks, which naming files does not need.

namespace volt::history
{

    // The 32 bytes of a hash.
    using sha256_digest = std::array<std::uint8_t, 32>;

    // A hash computed over bytes given in any number of pieces.
    class sha256
    {
    public:
        sha256() noexcept;

        // Adds bytes to what is hashed.
        void update(std::string_view bytes) noexcept;

        // The hash of everything given since construction (or the last
        // finish), after which the hasher starts over, empty.
        sha256_digest finish() noexcept;

    private:
        void compress(const std::uint8_t *block) noexcept;
        void reset() noexcept;

        std::array<std::uint32_t, 8> m_state{};
        std::array<std::uint8_t, 64> m_block{};
        std::size_t m_used = 0;     // bytes waiting in m_block
        std::uint64_t m_length = 0; // bytes given in all
    };

    // The hash of the bytes, as bytes.
    sha256_digest sha256_of(std::string_view bytes) noexcept;

    // The hash of the bytes as 64 lowercase hexadecimal digits.
    std::string sha256_hex(std::string_view bytes);

    // A digest as 64 lowercase hexadecimal digits.
    std::string to_hex(const sha256_digest &digest);

} // namespace volt::history
