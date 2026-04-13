#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace volt::io::codec {

inline bool readBinaryFile(const std::filesystem::path& path, std::vector<std::uint8_t>& outBytes) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return false;
  }

  in.seekg(0, std::ios::end);
  const std::streamoff length = in.tellg();
  if (length < 0) {
    return false;
  }
  in.seekg(0, std::ios::beg);

  outBytes.resize(static_cast<std::size_t>(length));
  if (!outBytes.empty()) {
    in.read(reinterpret_cast<char*>(outBytes.data()), length);
  }

  return in.good() || in.eof();
}

inline bool writeBinaryFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    return false;
  }

  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return out.good();
}

inline bool readU16Le(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t& out) {
  if (offset + 2U > bytes.size()) {
    return false;
  }

  out = static_cast<std::uint16_t>(bytes[offset]) |
        (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
  return true;
}

inline bool readU32Le(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t& out) {
  if (offset + 4U > bytes.size()) {
    return false;
  }

  out = static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
  return true;
}

inline bool readI32Le(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::int32_t& out) {
  std::uint32_t raw = 0U;
  if (!readU32Le(bytes, offset, raw)) {
    return false;
  }

  out = static_cast<std::int32_t>(raw);
  return true;
}

inline bool readU32Be(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t& out) {
  if (offset + 4U > bytes.size()) {
    return false;
  }

  out = (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
        static_cast<std::uint32_t>(bytes[offset + 3U]);
  return true;
}

inline void appendU16Le(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

inline void appendU32Le(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

inline void appendU32Be(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

inline std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
  static const std::array<std::uint32_t, 256> kTable = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0U; i < table.size(); ++i) {
      std::uint32_t c = i;
      for (int bit = 0; bit < 8; ++bit) {
        c = (c & 1U) != 0U ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
      }
      table[i] = c;
    }
    return table;
  }();

  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t i = 0; i < size; ++i) {
    const std::uint8_t index = static_cast<std::uint8_t>(crc ^ data[i]);
    crc = (crc >> 8U) ^ kTable[index];
  }
  return ~crc;
}

inline std::uint8_t pathPredictor(std::uint8_t a, std::uint8_t b, std::uint8_t c) {
  const int p = static_cast<int>(a) + static_cast<int>(b) - static_cast<int>(c);
  const int pa = std::abs(p - static_cast<int>(a));
  const int pb = std::abs(p - static_cast<int>(b));
  const int pc = std::abs(p - static_cast<int>(c));

  if (pa <= pb && pa <= pc) {
    return a;
  }
  if (pb <= pc) {
    return b;
  }
  return c;
}

inline bool hasPngSignature(const std::vector<std::uint8_t>& bytes) {
  constexpr std::array<std::uint8_t, 8> kPngSignature = {
      0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU,
  };

  return bytes.size() >= kPngSignature.size() &&
         std::equal(kPngSignature.begin(), kPngSignature.end(), bytes.begin());
}

inline bool hasBmpSignature(const std::vector<std::uint8_t>& bytes) {
  return bytes.size() >= 2U && bytes[0] == 'B' && bytes[1] == 'M';
}

inline bool hasJpegSignature(const std::vector<std::uint8_t>& bytes) {
  return bytes.size() >= 3U && bytes[0] == 0xFFU && bytes[1] == 0xD8U && bytes[2] == 0xFFU;
}

}  // namespace volt::io::codec
