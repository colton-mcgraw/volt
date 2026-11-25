#include <history/sha256.hpp>

#include <bit>

namespace volt::history
{

    namespace
    {
        // The first 32 bits of the fractional parts of the cube roots of the
        // first 64 primes (FIPS 180-4, 4.2.2).
        constexpr std::array<std::uint32_t, 64> round_constants = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
        };

        // The first 32 bits of the fractional parts of the square roots of
        // the first 8 primes (5.3.3).
        constexpr std::array<std::uint32_t, 8> initial_state = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
        };

        constexpr std::uint32_t load_big_endian(const std::uint8_t *p) noexcept
        {
            return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) |
                   std::uint32_t{p[3]};
        }
    } // namespace

    sha256::sha256() noexcept
    {
        reset();
    }

    void sha256::reset() noexcept
    {
        m_state = initial_state;
        m_block.fill(0);
        m_used = 0;
        m_length = 0;
    }

    void sha256::compress(const std::uint8_t *block) noexcept
    {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t t = 0; t < 16; ++t)
            w[t] = load_big_endian(block + 4 * t);
        for (std::size_t t = 16; t < 64; ++t)
        {
            const std::uint32_t s0 = std::rotr(w[t - 15], 7) ^ std::rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
            const std::uint32_t s1 = std::rotr(w[t - 2], 17) ^ std::rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }

        std::uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
        std::uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
        for (std::size_t t = 0; t < 64; ++t)
        {
            const std::uint32_t big_s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t t1 = h + big_s1 + choose + round_constants[t] + w[t];
            const std::uint32_t big_s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = big_s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        m_state[0] += a;
        m_state[1] += b;
        m_state[2] += c;
        m_state[3] += d;
        m_state[4] += e;
        m_state[5] += f;
        m_state[6] += g;
        m_state[7] += h;
    }

    void sha256::update(std::string_view bytes) noexcept
    {
        const auto *data = reinterpret_cast<const std::uint8_t *>(bytes.data());
        std::size_t remaining = bytes.size();
        m_length += remaining;

        if (m_used > 0)
        {
            const std::size_t take = remaining < 64 - m_used ? remaining : 64 - m_used;
            for (std::size_t i = 0; i < take; ++i)
                m_block[m_used + i] = data[i];
            m_used += take;
            data += take;
            remaining -= take;
            if (m_used < 64)
                return;
            compress(m_block.data());
            m_used = 0;
        }
        while (remaining >= 64)
        {
            compress(data);
            data += 64;
            remaining -= 64;
        }
        for (std::size_t i = 0; i < remaining; ++i)
            m_block[i] = data[i];
        m_used = remaining;
    }

    sha256_digest sha256::finish() noexcept
    {
        // The message, a 1 bit, zeros, and the length in bits as 64 bits,
        // big-endian, to a multiple of 512 bits (5.1.1).
        const std::uint64_t bits = m_length * 8;
        m_block[m_used++] = 0x80;
        if (m_used > 56)
        {
            for (std::size_t i = m_used; i < 64; ++i)
                m_block[i] = 0;
            compress(m_block.data());
            m_used = 0;
        }
        for (std::size_t i = m_used; i < 56; ++i)
            m_block[i] = 0;
        for (std::size_t i = 0; i < 8; ++i)
            m_block[56 + i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
        compress(m_block.data());

        sha256_digest digest{};
        for (std::size_t i = 0; i < 8; ++i)
        {
            digest[4 * i] = static_cast<std::uint8_t>(m_state[i] >> 24);
            digest[4 * i + 1] = static_cast<std::uint8_t>(m_state[i] >> 16);
            digest[4 * i + 2] = static_cast<std::uint8_t>(m_state[i] >> 8);
            digest[4 * i + 3] = static_cast<std::uint8_t>(m_state[i]);
        }
        reset();
        return digest;
    }

    sha256_digest sha256_of(std::string_view bytes) noexcept
    {
        sha256 hasher;
        hasher.update(bytes);
        return hasher.finish();
    }

    std::string sha256_hex(std::string_view bytes)
    {
        return to_hex(sha256_of(bytes));
    }

    std::string to_hex(const sha256_digest &digest)
    {
        constexpr std::string_view digits = "0123456789abcdef";
        std::string text;
        text.reserve(64);
        for (const std::uint8_t byte : digest)
        {
            text += digits[byte >> 4];
            text += digits[byte & 0x0F];
        }
        return text;
    }

} // namespace volt::history
