#include "JpegCodec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

thread_local std::string gLastJpegCodecError;

bool failJpeg(std::string message) {
  gLastJpegCodecError = std::move(message);
  return false;
}

constexpr std::uint8_t kMarkerPrefix = 0xFFU;
constexpr std::uint8_t kSoi = 0xD8U;
constexpr std::uint8_t kEoi = 0xD9U;
constexpr std::uint8_t kSof0 = 0xC0U;
constexpr std::uint8_t kDht = 0xC4U;
constexpr std::uint8_t kDqt = 0xDBU;
constexpr std::uint8_t kDri = 0xDDU;
constexpr std::uint8_t kSos = 0xDAU;
constexpr std::uint8_t kApp0 = 0xE0U;

constexpr std::size_t kBlockSize = 8U;
constexpr std::size_t kBlockElements = 64U;
constexpr std::size_t kMaxJpegImageBytes = 256U * 1024U * 1024U;

constexpr std::array<std::uint8_t, 64> kZigZagToNatural = {
    0,  1,  8,  16, 9,  2,  3,  10,
    17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

constexpr std::array<std::uint8_t, 64> kStdLumaQuant = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99,
};

constexpr std::array<std::uint8_t, 64> kStdChromaQuant = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
};

constexpr std::array<std::uint8_t, 16> kStdDcLumaCounts = {
    0x00, 0x01, 0x05, 0x01,
    0x01, 0x01, 0x01, 0x01,
  0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

constexpr std::array<std::uint8_t, 16> kStdDcChromaCounts = {
    0x00, 0x03, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01,
  0x01, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

constexpr std::array<std::uint8_t, 16> kStdAcLumaCounts = {
    0x00, 0x02, 0x01, 0x03,
    0x03, 0x02, 0x04, 0x03,
    0x05, 0x05, 0x04, 0x04,
    0x00, 0x00, 0x01, 0x7D,
};

constexpr std::array<std::uint8_t, 16> kStdAcChromaCounts = {
    0x00, 0x02, 0x01, 0x02,
    0x04, 0x04, 0x03, 0x04,
    0x07, 0x05, 0x04, 0x04,
    0x00, 0x01, 0x02, 0x77,
};

constexpr std::array<std::uint8_t, 12> kStdDcValues = {
    0, 1, 2, 3, 4, 5,
    6, 7, 8, 9, 10, 11,
};

constexpr std::array<std::uint8_t, 162> kStdAcLumaValues = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
    0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08,
    0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16,
    0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
    0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
    0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4,
    0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA,
    0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA,
};

constexpr std::array<std::uint8_t, 162> kStdAcChromaValues = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
    0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0,
    0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34,
    0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
    0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5,
    0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4,
    0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
    0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2,
    0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
    0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9,
    0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA,
};

double clampDouble(double value, double lo, double hi) {
  return std::min(hi, std::max(lo, value));
}

std::uint8_t clampByte(double value) {
  return static_cast<std::uint8_t>(clampDouble(std::round(value), 0.0, 255.0));
}

std::uint16_t readU16Be(const std::vector<std::uint8_t>& bytes,
                        std::size_t offset,
                        bool& ok) {
  if (offset + 2U > bytes.size()) {
    ok = false;
    return 0U;
  }

  ok = true;
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(bytes[offset]) << 8U) |
      static_cast<std::uint16_t>(bytes[offset + 1U]));
}

void appendMarker(std::vector<std::uint8_t>& out, std::uint8_t marker) {
  out.push_back(kMarkerPrefix);
  out.push_back(marker);
}

void appendU16Be(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void appendSegment(std::vector<std::uint8_t>& out,
                   std::uint8_t marker,
                   const std::vector<std::uint8_t>& payload) {
  appendMarker(out, marker);
  appendU16Be(out, static_cast<std::uint16_t>(payload.size() + 2U));
  out.insert(out.end(), payload.begin(), payload.end());
}

struct HuffmanEncoderCode {
  std::uint16_t code{0U};
  std::uint8_t length{0U};
};

struct HuffmanEncoderTable {
  std::array<HuffmanEncoderCode, 256> symbols{};
};

struct HuffmanDecoderTable {
  bool defined{false};
  std::array<int, 17> minCode{};
  std::array<int, 17> maxCode{};
  std::array<int, 17> valuePtr{};
  std::array<std::uint8_t, 256> values{};
  std::array<std::uint8_t, 256> fastSymbol{};
  std::array<std::uint8_t, 256> fastLength{};
  std::size_t valueCount{0U};
};

bool buildHuffmanTables(const std::array<std::uint8_t, 16>& counts,
                        const std::vector<std::uint8_t>& values,
                        HuffmanEncoderTable& enc,
                        HuffmanDecoderTable& dec,
                        std::string& error) {
  std::size_t expectedValues = 0U;
  for (std::uint8_t c : counts) {
    expectedValues += c;
  }

  if (values.size() != expectedValues) {
    error = "jpeg huffman value count mismatch";
    return false;
  }

  dec = {};
  enc = {};
  dec.defined = true;
  dec.minCode.fill(-1);
  dec.maxCode.fill(-1);
  dec.valuePtr.fill(0);
  dec.fastSymbol.fill(0U);
  dec.fastLength.fill(0U);
  dec.valueCount = values.size();
  for (std::size_t i = 0U; i < values.size(); ++i) {
    dec.values[i] = values[i];
  }

  std::uint16_t code = 0U;
  std::size_t k = 0U;
  for (int bits = 1; bits <= 16; ++bits) {
    const std::uint8_t count = counts[static_cast<std::size_t>(bits - 1)];
    if (count == 0U) {
      code = static_cast<std::uint16_t>(code << 1U);
      continue;
    }

    dec.minCode[bits] = static_cast<int>(code);
    dec.valuePtr[bits] = static_cast<int>(k);

    for (std::uint8_t i = 0U; i < count; ++i) {
      if (k >= values.size()) {
        error = "jpeg huffman table overrun";
        return false;
      }

      const std::uint8_t symbol = values[k++];
      enc.symbols[symbol].code = code;
      enc.symbols[symbol].length = static_cast<std::uint8_t>(bits);

      if (bits <= 8) {
        const std::uint16_t base = static_cast<std::uint16_t>(code << (8 - bits));
        const std::uint16_t span = static_cast<std::uint16_t>(1U << (8 - bits));
        for (std::uint16_t j = 0U; j < span; ++j) {
          const std::uint16_t idx = static_cast<std::uint16_t>(base + j);
          dec.fastSymbol[idx] = symbol;
          dec.fastLength[idx] = static_cast<std::uint8_t>(bits);
        }
      }

      ++code;
    }

    dec.maxCode[bits] = static_cast<int>(code - 1U);
    code = static_cast<std::uint16_t>(code << 1U);
  }

  return true;
}

std::array<std::uint16_t, 64> scaledQuantTable(const std::array<std::uint8_t, 64>& base,
                                               int quality) {
  quality = std::clamp(quality, 1, 100);
  const int scale = quality < 50 ? 5000 / quality : (200 - quality * 2);

  std::array<std::uint16_t, 64> out{};
  for (std::size_t i = 0U; i < base.size(); ++i) {
    int q = (static_cast<int>(base[i]) * scale + 50) / 100;
    q = std::clamp(q, 1, 255);
    out[i] = static_cast<std::uint16_t>(q);
  }
  return out;
}

const std::array<std::array<double, 8>, 8>& dctCosTable() {
  static const std::array<std::array<double, 8>, 8> table = [] {
    std::array<std::array<double, 8>, 8> t{};
    constexpr double kPi = 3.14159265358979323846;
    for (int u = 0; u < 8; ++u) {
      for (int x = 0; x < 8; ++x) {
        t[static_cast<std::size_t>(u)][static_cast<std::size_t>(x)] =
            std::cos(((2.0 * x + 1.0) * u * kPi) / 16.0);
      }
    }
    return t;
  }();
  return table;
}

constexpr std::array<double, 8> kDctAlpha = {
    0.7071067811865475244,
    1.0,
    1.0,
    1.0,
    1.0,
    1.0,
    1.0,
    1.0,
};

void forwardDct8x8(const std::array<double, 64>& input,
                   std::array<double, 64>& output) {
  const auto& c = dctCosTable();
  std::array<double, 64> tmp{};

  for (int y = 0; y < 8; ++y) {
    for (int u = 0; u < 8; ++u) {
      double sum = 0.0;
      for (int x = 0; x < 8; ++x) {
        sum += input[static_cast<std::size_t>(y * 8 + x)] *
               c[static_cast<std::size_t>(u)][static_cast<std::size_t>(x)];
      }
      tmp[static_cast<std::size_t>(y * 8 + u)] = sum;
    }
  }

  for (int v = 0; v < 8; ++v) {
    for (int u = 0; u < 8; ++u) {
      double sum = 0.0;
      for (int y = 0; y < 8; ++y) {
        sum += tmp[static_cast<std::size_t>(y * 8 + u)] *
               c[static_cast<std::size_t>(v)][static_cast<std::size_t>(y)];
      }

      output[static_cast<std::size_t>(v * 8 + u)] =
          0.25 * kDctAlpha[static_cast<std::size_t>(u)] *
          kDctAlpha[static_cast<std::size_t>(v)] * sum;
    }
  }
}

void inverseDct8x8(const std::array<double, 64>& input,
                   std::array<double, 64>& output) {
  const auto& c = dctCosTable();
  std::array<double, 64> tmp{};

  for (int v = 0; v < 8; ++v) {
    for (int x = 0; x < 8; ++x) {
      double sum = 0.0;
      for (int u = 0; u < 8; ++u) {
        sum += kDctAlpha[static_cast<std::size_t>(u)] *
               input[static_cast<std::size_t>(v * 8 + u)] *
               c[static_cast<std::size_t>(u)][static_cast<std::size_t>(x)];
      }
      tmp[static_cast<std::size_t>(v * 8 + x)] = sum;
    }
  }

  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      double sum = 0.0;
      for (int v = 0; v < 8; ++v) {
        sum += kDctAlpha[static_cast<std::size_t>(v)] *
               tmp[static_cast<std::size_t>(v * 8 + x)] *
               c[static_cast<std::size_t>(v)][static_cast<std::size_t>(y)];
      }

      output[static_cast<std::size_t>(y * 8 + x)] = 0.25 * sum;
    }
  }
}

int magnitudeCategory(int value) {
  const int mag = std::abs(value);
  int bits = 0;
  while ((mag >> bits) != 0) {
    ++bits;
  }
  return bits;
}

std::uint16_t amplitudeBits(int value, int category) {
  if (category == 0) {
    return 0U;
  }

  if (value >= 0) {
    return static_cast<std::uint16_t>(value);
  }

  const int base = (1 << category) - 1;
  return static_cast<std::uint16_t>(base + value);
}

int decodeAmplitude(std::uint16_t bits, int category) {
  if (category == 0) {
    return 0;
  }

  const int threshold = 1 << (category - 1);
  if (static_cast<int>(bits) >= threshold) {
    return static_cast<int>(bits);
  }

  return static_cast<int>(bits) - ((1 << category) - 1);
}

class JpegBitWriter {
 public:
  void write(std::uint16_t bits, std::uint8_t bitCount) {
    if (bitCount == 0U) {
      return;
    }

    buffer_ = static_cast<std::uint32_t>((buffer_ << bitCount) | (bits & ((1U << bitCount) - 1U)));
    bitsInBuffer_ = static_cast<std::uint8_t>(bitsInBuffer_ + bitCount);

    while (bitsInBuffer_ >= 8U) {
      const std::uint8_t byte = static_cast<std::uint8_t>(
          (buffer_ >> static_cast<unsigned int>(bitsInBuffer_ - 8U)) & 0xFFU);
      out_.push_back(byte);
      if (byte == 0xFFU) {
        out_.push_back(0x00U);
      }
      bitsInBuffer_ = static_cast<std::uint8_t>(bitsInBuffer_ - 8U);
      if (bitsInBuffer_ == 0U) {
        buffer_ = 0U;
      } else {
        buffer_ &= (1U << bitsInBuffer_) - 1U;
      }
    }
  }

  void flush() {
    if (bitsInBuffer_ == 0U) {
      return;
    }

    const std::uint16_t pad = static_cast<std::uint16_t>((1U << (8U - bitsInBuffer_)) - 1U);
    write(pad, static_cast<std::uint8_t>(8U - bitsInBuffer_));
  }

  const std::vector<std::uint8_t>& bytes() const {
    return out_;
  }

 private:
  std::vector<std::uint8_t> out_;
  std::uint32_t buffer_{0U};
  std::uint8_t bitsInBuffer_{0U};
};

class JpegBitReader {
 public:
  explicit JpegBitReader(const std::vector<std::uint8_t>& bytes)
      : bytes_(bytes) {}

  bool peekBits(std::uint8_t count, std::uint16_t& out) {
    if (!fillBits(count)) {
      return false;
    }

    if (count == 0U) {
      out = 0U;
      return true;
    }

    out = static_cast<std::uint16_t>(
        (bitBuffer_ >> static_cast<unsigned int>(bitsInBuffer_ - count)) &
        ((1U << count) - 1U));
    return true;
  }

  bool dropBits(std::uint8_t count) {
    if (count > bitsInBuffer_) {
      return false;
    }

    bitsInBuffer_ = static_cast<std::uint8_t>(bitsInBuffer_ - count);
    if (bitsInBuffer_ == 0U) {
      bitBuffer_ = 0U;
    } else {
      bitBuffer_ &= (1U << bitsInBuffer_) - 1U;
    }
    return true;
  }

  bool readBit(std::uint8_t& bit) {
    std::uint16_t value = 0U;
    if (!readBits(1U, value)) {
      return false;
    }
    bit = static_cast<std::uint8_t>(value & 1U);
    return true;
  }

  bool readBits(std::uint8_t count, std::uint16_t& out) {
    if (!peekBits(count, out)) {
      return false;
    }
    return dropBits(count);
  }

 private:
  bool fillBits(std::uint8_t count) {
    while (bitsInBuffer_ < count) {
      if (offset_ >= bytes_.size()) {
        return false;
      }

      bitBuffer_ = (bitBuffer_ << 8U) | bytes_[offset_++];
      bitsInBuffer_ = static_cast<std::uint8_t>(bitsInBuffer_ + 8U);
    }

    return true;
  }

  const std::vector<std::uint8_t>& bytes_;
  std::size_t offset_{0U};
  std::uint32_t bitBuffer_{0U};
  std::uint8_t bitsInBuffer_{0U};
};

bool decodeHuffmanSymbol(JpegBitReader& reader,
                         const HuffmanDecoderTable& table,
                         std::uint8_t& symbol) {
  std::uint16_t lookahead = 0U;
  if (reader.peekBits(8U, lookahead)) {
    const std::uint8_t bitLen = table.fastLength[lookahead];
    if (bitLen != 0U) {
      if (!reader.dropBits(bitLen)) {
        return false;
      }
      symbol = table.fastSymbol[lookahead];
      return true;
    }
  }

  int code = 0;
  for (int bits = 1; bits <= 16; ++bits) {
    std::uint8_t bit = 0U;
    if (!reader.readBit(bit)) {
      return false;
    }

    code = (code << 1) | bit;
    if (table.maxCode[bits] >= 0 && code <= table.maxCode[bits]) {
      const int idx = table.valuePtr[bits] + (code - table.minCode[bits]);
      if (idx < 0 || static_cast<std::size_t>(idx) >= table.valueCount) {
        return false;
      }

      symbol = table.values[static_cast<std::size_t>(idx)];
      return true;
    }
  }

  return false;
}

struct QuantTable {
  bool defined{false};
  std::array<std::uint16_t, 64> values{};
};

struct ComponentSpec {
  std::uint8_t id{0U};
  std::uint8_t h{1U};
  std::uint8_t v{1U};
  std::uint8_t quantTable{0U};
};

struct ScanComponentSpec {
  std::uint8_t id{0U};
  std::uint8_t dcTable{0U};
  std::uint8_t acTable{0U};
};

std::size_t componentIndexById(const std::vector<ComponentSpec>& components,
                               std::uint8_t id) {
  for (std::size_t i = 0U; i < components.size(); ++i) {
    if (components[i].id == id) {
      return i;
    }
  }
  return std::numeric_limits<std::size_t>::max();
}

void sampleRgbaLumaBlock(const volt::io::RawImage& image,
                         std::size_t originX,
                         std::size_t originY,
                         std::array<double, 64>& yBlock) {
  for (std::size_t by = 0U; by < 8U; ++by) {
    const std::size_t iy = std::min<std::size_t>(image.height - 1U, originY + by);
    for (std::size_t bx = 0U; bx < 8U; ++bx) {
      const std::size_t ix = std::min<std::size_t>(image.width - 1U, originX + bx);
      const std::size_t pixel = (iy * image.width + ix) * 4U;

      const double r = static_cast<double>(image.rgba[pixel + 0U]);
      const double g = static_cast<double>(image.rgba[pixel + 1U]);
      const double b = static_cast<double>(image.rgba[pixel + 2U]);
      const double y = 0.299 * r + 0.587 * g + 0.114 * b;
      yBlock[by * 8U + bx] = y - 128.0;
    }
  }
}

void sampleRgbaChroma420Block(const volt::io::RawImage& image,
                              std::size_t originX,
                              std::size_t originY,
                              std::array<double, 64>& cbBlock,
                              std::array<double, 64>& crBlock) {
  for (std::size_t by = 0U; by < 8U; ++by) {
    for (std::size_t bx = 0U; bx < 8U; ++bx) {
      double cbSum = 0.0;
      double crSum = 0.0;

      for (std::size_t sy = 0U; sy < 2U; ++sy) {
        const std::size_t iy = std::min<std::size_t>(image.height - 1U, originY + by * 2U + sy);
        for (std::size_t sx = 0U; sx < 2U; ++sx) {
          const std::size_t ix = std::min<std::size_t>(image.width - 1U, originX + bx * 2U + sx);
          const std::size_t pixel = (iy * image.width + ix) * 4U;

          const double r = static_cast<double>(image.rgba[pixel + 0U]);
          const double g = static_cast<double>(image.rgba[pixel + 1U]);
          const double b = static_cast<double>(image.rgba[pixel + 2U]);

          cbSum += -0.168736 * r - 0.331264 * g + 0.5 * b + 128.0;
          crSum += 0.5 * r - 0.418688 * g - 0.081312 * b + 128.0;
        }
      }

      cbBlock[by * 8U + bx] = (cbSum * 0.25) - 128.0;
      crBlock[by * 8U + bx] = (crSum * 0.25) - 128.0;
    }
  }
}

void sampleRgbaChroma422Block(const volt::io::RawImage& image,
                              std::size_t originX,
                              std::size_t originY,
                              std::array<double, 64>& cbBlock,
                              std::array<double, 64>& crBlock) {
  for (std::size_t by = 0U; by < 8U; ++by) {
    const std::size_t iy = std::min<std::size_t>(image.height - 1U, originY + by);
    for (std::size_t bx = 0U; bx < 8U; ++bx) {
      double cbSum = 0.0;
      double crSum = 0.0;

      for (std::size_t sx = 0U; sx < 2U; ++sx) {
        const std::size_t ix = std::min<std::size_t>(image.width - 1U, originX + bx * 2U + sx);
        const std::size_t pixel = (iy * image.width + ix) * 4U;

        const double r = static_cast<double>(image.rgba[pixel + 0U]);
        const double g = static_cast<double>(image.rgba[pixel + 1U]);
        const double b = static_cast<double>(image.rgba[pixel + 2U]);

        cbSum += -0.168736 * r - 0.331264 * g + 0.5 * b + 128.0;
        crSum += 0.5 * r - 0.418688 * g - 0.081312 * b + 128.0;
      }

      cbBlock[by * 8U + bx] = (cbSum * 0.5) - 128.0;
      crBlock[by * 8U + bx] = (crSum * 0.5) - 128.0;
    }
  }
}

void quantizeBlock(const std::array<double, 64>& dct,
                   const std::array<std::uint16_t, 64>& quant,
                   std::array<int, 64>& zigzagOut) {
  std::array<int, 64> natural{};
  for (std::size_t i = 0U; i < 64U; ++i) {
    natural[i] = static_cast<int>(std::lrint(dct[i] / static_cast<double>(quant[i])));
  }

  for (std::size_t zz = 0U; zz < 64U; ++zz) {
    zigzagOut[zz] = natural[kZigZagToNatural[zz]];
  }
}

bool writeHuffmanSymbol(JpegBitWriter& writer,
                        const HuffmanEncoderTable& table,
                        std::uint8_t symbol,
                        std::string& error) {
  const HuffmanEncoderCode code = table.symbols[symbol];
  if (code.length == 0U) {
    error = "jpeg missing huffman symbol";
    return false;
  }

  writer.write(code.code, code.length);
  return true;
}

bool encodeJpegBlock(JpegBitWriter& writer,
                     const std::array<int, 64>& coeff,
                     int& prevDc,
                     const HuffmanEncoderTable& dcTable,
                     const HuffmanEncoderTable& acTable,
                     std::string& error) {
  const int dcDiff = coeff[0] - prevDc;
  prevDc = coeff[0];

  const int dcCategory = magnitudeCategory(dcDiff);
  if (dcCategory > 11) {
    error = "jpeg dc category out of range";
    return false;
  }

  if (!writeHuffmanSymbol(writer, dcTable, static_cast<std::uint8_t>(dcCategory), error)) {
    return false;
  }

  if (dcCategory > 0) {
    writer.write(amplitudeBits(dcDiff, dcCategory), static_cast<std::uint8_t>(dcCategory));
  }

  int run = 0;
  for (std::size_t i = 1U; i < 64U; ++i) {
    const int value = coeff[i];
    if (value == 0) {
      ++run;
      continue;
    }

    while (run >= 16) {
      if (!writeHuffmanSymbol(writer, acTable, 0xF0U, error)) {
        return false;
      }
      run -= 16;
    }

    const int acCategory = magnitudeCategory(value);
    if (acCategory <= 0 || acCategory > 10) {
      error = "jpeg ac category out of range";
      return false;
    }

    const std::uint8_t symbol = static_cast<std::uint8_t>((run << 4) | acCategory);
    if (!writeHuffmanSymbol(writer, acTable, symbol, error)) {
      return false;
    }

    writer.write(amplitudeBits(value, acCategory), static_cast<std::uint8_t>(acCategory));
    run = 0;
  }

  if (run > 0) {
    if (!writeHuffmanSymbol(writer, acTable, 0x00U, error)) {
      return false;
    }
  }

  return true;
}

bool decodeJpegBlock(JpegBitReader& reader,
                     const HuffmanDecoderTable& dcTable,
                     const HuffmanDecoderTable& acTable,
                     int& prevDc,
                     const std::array<std::uint16_t, 64>& quant,
                     std::array<double, 64>& outNatural,
                     std::string& error) {
  std::array<int, 64> zigzag{};
  zigzag.fill(0);

  std::uint8_t dcSymbol = 0U;
  if (!decodeHuffmanSymbol(reader, dcTable, dcSymbol)) {
    error = "jpeg dc huffman decode failed";
    return false;
  }

  if (dcSymbol > 11U) {
    error = "jpeg dc symbol invalid";
    return false;
  }

  std::uint16_t dcBits = 0U;
  if (dcSymbol > 0U && !reader.readBits(dcSymbol, dcBits)) {
    error = "jpeg dc bits truncated";
    return false;
  }

  const int dcDiff = decodeAmplitude(dcBits, dcSymbol);
  const int dc = prevDc + dcDiff;
  prevDc = dc;
  zigzag[0] = dc;

  std::size_t k = 1U;
  while (k < 64U) {
    std::uint8_t acSymbol = 0U;
    if (!decodeHuffmanSymbol(reader, acTable, acSymbol)) {
      error = "jpeg ac huffman decode failed";
      return false;
    }

    if (acSymbol == 0x00U) {
      break;
    }

    if (acSymbol == 0xF0U) {
      k += 16U;
      if (k > 64U) {
        error = "jpeg ac run exceeds block";
        return false;
      }
      continue;
    }

    const std::size_t run = static_cast<std::size_t>((acSymbol >> 4U) & 0x0FU);
    const std::uint8_t size = static_cast<std::uint8_t>(acSymbol & 0x0FU);
    if (size == 0U || size > 10U) {
      error = "jpeg ac size invalid";
      return false;
    }

    k += run;
    if (k >= 64U) {
      error = "jpeg ac coefficient index exceeds block";
      return false;
    }

    std::uint16_t bits = 0U;
    if (!reader.readBits(size, bits)) {
      error = "jpeg ac bits truncated";
      return false;
    }

    zigzag[k++] = decodeAmplitude(bits, size);
  }

  std::array<double, 64> dequant{};
  for (std::size_t zz = 0U; zz < 64U; ++zz) {
    const std::size_t natural = kZigZagToNatural[zz];
    dequant[natural] = static_cast<double>(zigzag[zz] * static_cast<int>(quant[natural]));
  }

  inverseDct8x8(dequant, outNatural);
  return true;
}

bool readScanEntropySegments(const std::vector<std::uint8_t>& bytes,
                             std::size_t scanOffset,
                             std::uint16_t restartInterval,
                             std::vector<std::vector<std::uint8_t>>& entropySegments,
                             std::uint8_t& marker,
                             std::string& error) {
  entropySegments.clear();
  std::vector<std::uint8_t> current;
  std::uint8_t expectedRst = 0U;

  std::size_t i = scanOffset;
  while (i < bytes.size()) {
    const std::uint8_t b = bytes[i];
    if (b != 0xFFU) {
      current.push_back(b);
      ++i;
      continue;
    }

    if (i + 1U >= bytes.size()) {
      error = "jpeg scan truncated after 0xFF";
      return false;
    }

    const std::uint8_t next = bytes[i + 1U];
    if (next == 0x00U) {
      current.push_back(0xFFU);
      i += 2U;
      continue;
    }

    if (next == 0xFFU) {
      ++i;
      continue;
    }

    if (next >= 0xD0U && next <= 0xD7U) {
      if (restartInterval == 0U) {
        error = "jpeg restart marker encountered without DRI";
        return false;
      }

      if (next != static_cast<std::uint8_t>(0xD0U + expectedRst)) {
        error = "jpeg restart marker sequence invalid";
        return false;
      }

      entropySegments.push_back(std::move(current));
      current.clear();
      expectedRst = static_cast<std::uint8_t>((expectedRst + 1U) & 0x07U);
      i += 2U;
      continue;
    }

    marker = next;
    entropySegments.push_back(std::move(current));
    return true;
  }

  error = "jpeg missing EOI marker";
  return false;
}

void writeDqt(std::vector<std::uint8_t>& out,
              const std::array<std::uint16_t, 64>& luma,
              const std::array<std::uint16_t, 64>& chroma) {
  std::vector<std::uint8_t> payload;
  payload.reserve(2U + 64U + 64U);

  payload.push_back(0x00U);
  for (std::size_t i = 0U; i < 64U; ++i) {
    payload.push_back(static_cast<std::uint8_t>(luma[kZigZagToNatural[i]]));
  }

  payload.push_back(0x01U);
  for (std::size_t i = 0U; i < 64U; ++i) {
    payload.push_back(static_cast<std::uint8_t>(chroma[kZigZagToNatural[i]]));
  }

  appendSegment(out, kDqt, payload);
}

void appendDhtTablePayload(std::vector<std::uint8_t>& payload,
                           std::uint8_t tableClass,
                           std::uint8_t tableId,
                           const std::array<std::uint8_t, 16>& counts,
                           const std::vector<std::uint8_t>& values) {
  payload.push_back(static_cast<std::uint8_t>((tableClass << 4U) | tableId));
  payload.insert(payload.end(), counts.begin(), counts.end());
  payload.insert(payload.end(), values.begin(), values.end());
}

void writeDht(std::vector<std::uint8_t>& out) {
  std::vector<std::uint8_t> payload;
  payload.reserve(420U);

  appendDhtTablePayload(
      payload,
      0U,
      0U,
      kStdDcLumaCounts,
      std::vector<std::uint8_t>(kStdDcValues.begin(), kStdDcValues.end()));
  appendDhtTablePayload(
      payload,
      1U,
      0U,
      kStdAcLumaCounts,
      std::vector<std::uint8_t>(kStdAcLumaValues.begin(), kStdAcLumaValues.end()));
  appendDhtTablePayload(
      payload,
      0U,
      1U,
      kStdDcChromaCounts,
      std::vector<std::uint8_t>(kStdDcValues.begin(), kStdDcValues.end()));
  appendDhtTablePayload(
      payload,
      1U,
      1U,
      kStdAcChromaCounts,
      std::vector<std::uint8_t>(kStdAcChromaValues.begin(), kStdAcChromaValues.end()));

  appendSegment(out, kDht, payload);
}

}  // namespace

namespace volt::io {

const std::string& lastJpegCodecError() {
  return gLastJpegCodecError;
}

bool decodeJpegFile(const std::vector<std::uint8_t>& bytes,
                    RawImage& outImage,
                    std::string& error) {
  outImage = {};
  gLastJpegCodecError.clear();

  if (bytes.size() < 4U || bytes[0] != 0xFFU || bytes[1] != kSoi) {
    error = "jpeg signature missing";
    gLastJpegCodecError = error;
    return false;
  }

  std::array<QuantTable, 4> quantTables{};
  std::array<std::array<HuffmanDecoderTable, 4>, 2> huffmanDec{};
  std::vector<ComponentSpec> frameComponents;
  std::uint16_t restartInterval = 0U;
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  bool sawSof0 = false;
  bool sawSos = false;

  std::size_t offset = 2U;
  while (offset + 1U < bytes.size()) {
    if (bytes[offset] != 0xFFU) {
      error = "jpeg marker sync lost";
      return false;
    }

    while (offset < bytes.size() && bytes[offset] == 0xFFU) {
      ++offset;
    }
    if (offset >= bytes.size()) {
      error = "jpeg marker truncated";
      return false;
    }

    const std::uint8_t marker = bytes[offset++];
    if (marker == kEoi) {
      break;
    }

    if (marker == kSos) {
      sawSos = true;
      bool ok = false;
      const std::uint16_t segmentLen = readU16Be(bytes, offset, ok);
      if (!ok || segmentLen < 2U || offset + segmentLen > bytes.size()) {
        error = "jpeg SOS segment truncated";
        return false;
      }

      const std::size_t segStart = offset + 2U;
      const std::size_t segEnd = segStart + static_cast<std::size_t>(segmentLen - 2U);
      if (segEnd > bytes.size()) {
        error = "jpeg SOS segment overrun";
        return false;
      }

      std::size_t p = segStart;
      if (p >= segEnd) {
        error = "jpeg SOS malformed";
        return false;
      }

      const std::uint8_t scanComponentCount = bytes[p++];
      if (scanComponentCount == 0U || (scanComponentCount != 1U && scanComponentCount != 3U)) {
        error = "jpeg scan component count unsupported";
        return false;
      }

      std::vector<ScanComponentSpec> scanComponents;
      scanComponents.reserve(scanComponentCount);

      for (std::uint8_t i = 0U; i < scanComponentCount; ++i) {
        if (p + 2U > segEnd) {
          error = "jpeg SOS component table truncated";
          gLastJpegCodecError = error;
          return false;
        }

        ScanComponentSpec s{};
        s.id = bytes[p++];
        const std::uint8_t tableSel = bytes[p++];
        s.dcTable = static_cast<std::uint8_t>((tableSel >> 4U) & 0x0FU);
        s.acTable = static_cast<std::uint8_t>(tableSel & 0x0FU);
        if (s.dcTable > 3U || s.acTable > 3U) {
          error = "jpeg SOS table selector invalid";
          return false;
        }

        scanComponents.push_back(s);
      }

      if (p + 3U > segEnd) {
        error = "jpeg SOS spectral bounds truncated";
        return false;
      }

      const std::uint8_t spectralStart = bytes[p++];
      const std::uint8_t spectralEnd = bytes[p++];
      const std::uint8_t approx = bytes[p++];
      if (spectralStart != 0U || spectralEnd != 63U || approx != 0U) {
        error = "jpeg progressive scans unsupported";
        return false;
      }

      if (p != segEnd) {
        error = "jpeg SOS payload malformed";
        return false;
      }

      if (!sawSof0 || width == 0U || height == 0U || frameComponents.empty()) {
        error = "jpeg missing SOF0 before SOS";
        return false;
      }

      if (scanComponentCount != frameComponents.size()) {
        error = "jpeg multi-scan streams unsupported";
        return false;
      }

      std::uint8_t maxH = 0U;
      std::uint8_t maxV = 0U;
      for (const ComponentSpec& comp : frameComponents) {
        maxH = std::max(maxH, comp.h);
        maxV = std::max(maxV, comp.v);
      }

      if (maxH == 0U || maxV == 0U) {
        error = "jpeg sampling factors invalid";
        return false;
      }

      if (frameComponents.size() == 1U) {
        if (frameComponents[0].h != 1U || frameComponents[0].v != 1U) {
          error = "jpeg grayscale sampling unsupported";
          return false;
        }
      } else if (frameComponents.size() == 3U) {
        const std::size_t yIdx = componentIndexById(frameComponents, 1U);
        const std::size_t cbIdx = componentIndexById(frameComponents, 2U);
        const std::size_t crIdx = componentIndexById(frameComponents, 3U);
        if (yIdx == std::numeric_limits<std::size_t>::max() ||
            cbIdx == std::numeric_limits<std::size_t>::max() ||
            crIdx == std::numeric_limits<std::size_t>::max()) {
          error = "jpeg component ids unsupported";
          return false;
        }

        const bool sampling444 =
            frameComponents[yIdx].h == 1U && frameComponents[yIdx].v == 1U &&
            frameComponents[cbIdx].h == 1U && frameComponents[cbIdx].v == 1U &&
            frameComponents[crIdx].h == 1U && frameComponents[crIdx].v == 1U;
        const bool sampling422 =
            frameComponents[yIdx].h == 2U && frameComponents[yIdx].v == 1U &&
            frameComponents[cbIdx].h == 1U && frameComponents[cbIdx].v == 1U &&
            frameComponents[crIdx].h == 1U && frameComponents[crIdx].v == 1U;
        const bool sampling420 =
            frameComponents[yIdx].h == 2U && frameComponents[yIdx].v == 2U &&
            frameComponents[cbIdx].h == 1U && frameComponents[cbIdx].v == 1U &&
            frameComponents[crIdx].h == 1U && frameComponents[crIdx].v == 1U;

        if (!sampling444 && !sampling422 && !sampling420) {
          error = "jpeg sampling factors unsupported";
          return false;
        }
      }

      if (height != 0U && width > std::numeric_limits<std::size_t>::max() / height) {
        error = "jpeg dimensions overflow";
        return false;
      }
      const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
      if (pixelCount > kMaxJpegImageBytes / 4U) {
        error = "jpeg dimensions exceed safety limit";
        return false;
      }

      const std::size_t mcuCols =
          (static_cast<std::size_t>(width) + static_cast<std::size_t>(maxH) * 8U - 1U) /
          (static_cast<std::size_t>(maxH) * 8U);
      const std::size_t mcuRows =
          (static_cast<std::size_t>(height) + static_cast<std::size_t>(maxV) * 8U - 1U) /
          (static_cast<std::size_t>(maxV) * 8U);
      if (mcuCols != 0U && mcuRows > std::numeric_limits<std::size_t>::max() / mcuCols) {
        error = "jpeg mcu dimensions overflow";
        return false;
      }
      const std::size_t totalMcus = mcuCols * mcuRows;

      std::vector<std::size_t> planeStrides(frameComponents.size(), 0U);
      std::vector<std::vector<std::uint8_t>> planes(frameComponents.size());
      for (std::size_t i = 0U; i < frameComponents.size(); ++i) {
        const ComponentSpec& comp = frameComponents[i];
        const std::size_t planeStride = mcuCols * static_cast<std::size_t>(comp.h) * 8U;
        const std::size_t planeHeight = mcuRows * static_cast<std::size_t>(comp.v) * 8U;
        if (planeStride != 0U && planeHeight > std::numeric_limits<std::size_t>::max() / planeStride) {
          error = "jpeg padded dimensions overflow";
          return false;
        }

        const std::size_t planePixels = planeStride * planeHeight;
        planeStrides[i] = planeStride;
        planes[i].assign(planePixels, 0U);
      }

      std::vector<std::vector<std::uint8_t>> entropySegments;
      std::uint8_t nextMarker = 0U;
      if (!readScanEntropySegments(bytes, segEnd, restartInterval, entropySegments, nextMarker, error)) {
        return false;
      }

      if (entropySegments.empty()) {
        error = "jpeg SOS entropy data missing";
        return false;
      }

      std::vector<int> prevDc(frameComponents.size(), 0);

      auto decodeMcu = [&](std::size_t mcuIndex, JpegBitReader& bitReader) {
        const std::size_t mx = mcuIndex % mcuCols;
        const std::size_t my = mcuIndex / mcuCols;

        for (const ScanComponentSpec& scanComp : scanComponents) {
          const std::size_t compIdx = componentIndexById(frameComponents, scanComp.id);
          if (compIdx == std::numeric_limits<std::size_t>::max()) {
            error = "jpeg scan component not found in frame";
            return false;
          }

          const ComponentSpec& comp = frameComponents[compIdx];

          if (comp.quantTable > 3U || !quantTables[comp.quantTable].defined) {
            error = "jpeg quantization table missing";
            return false;
          }

          if (!huffmanDec[0][scanComp.dcTable].defined || !huffmanDec[1][scanComp.acTable].defined) {
            error = "jpeg huffman table missing";
            return false;
          }

          std::vector<std::uint8_t>& plane = planes[compIdx];
          const std::size_t planeStride = planeStrides[compIdx];

          for (std::size_t vy = 0U; vy < comp.v; ++vy) {
            for (std::size_t hx = 0U; hx < comp.h; ++hx) {
              std::array<double, 64> block{};
              if (!decodeJpegBlock(
                      bitReader,
                      huffmanDec[0][scanComp.dcTable],
                      huffmanDec[1][scanComp.acTable],
                      prevDc[compIdx],
                      quantTables[comp.quantTable].values,
                      block,
                      error)) {
                return false;
              }

              const std::size_t blockOriginX =
                  mx * static_cast<std::size_t>(comp.h) * 8U + hx * 8U;
              const std::size_t blockOriginY =
                  my * static_cast<std::size_t>(comp.v) * 8U + vy * 8U;

              for (std::size_t by = 0U; by < 8U; ++by) {
                const std::size_t py = blockOriginY + by;
                for (std::size_t bx = 0U; bx < 8U; ++bx) {
                  const std::size_t px = blockOriginX + bx;
                  const std::size_t dst = py * planeStride + px;
                  plane[dst] = clampByte(block[by * 8U + bx] + 128.0);
                }
              }
            }
          }
        }

        return true;
      };

      if (restartInterval == 0U) {
        JpegBitReader bitReader(entropySegments[0]);
        for (std::size_t mcuIndex = 0U; mcuIndex < totalMcus; ++mcuIndex) {
          if (!decodeMcu(mcuIndex, bitReader)) {
            return false;
          }
        }
      } else {
        std::size_t mcuIndex = 0U;
        std::size_t segmentIndex = 0U;

        while (mcuIndex < totalMcus) {
          if (segmentIndex >= entropySegments.size()) {
            error = "jpeg restart interval entropy truncated";
            return false;
          }

          std::fill(prevDc.begin(), prevDc.end(), 0);
          JpegBitReader bitReader(entropySegments[segmentIndex]);
          const std::size_t mcuCount = std::min<std::size_t>(
              static_cast<std::size_t>(restartInterval),
              totalMcus - mcuIndex);
          for (std::size_t i = 0U; i < mcuCount; ++i) {
            if (!decodeMcu(mcuIndex, bitReader)) {
              return false;
            }
            ++mcuIndex;
          }

          ++segmentIndex;
        }

        for (; segmentIndex < entropySegments.size(); ++segmentIndex) {
          if (!entropySegments[segmentIndex].empty()) {
            error = "jpeg contains extra restart entropy segments";
            return false;
          }
        }
      }

      outImage.width = width;
      outImage.height = height;
      outImage.rgba.resize(pixelCount * 4U);

      if (frameComponents.size() == 1U) {
        const std::vector<std::uint8_t>& yPlane = planes[0];
        const ComponentSpec& yComp = frameComponents[0];
        const std::size_t yStride = planeStrides[0];
        for (std::size_t y = 0U; y < height; ++y) {
          for (std::size_t x = 0U; x < width; ++x) {
            const std::size_t srcX = x * static_cast<std::size_t>(yComp.h) / static_cast<std::size_t>(maxH);
            const std::size_t srcY = y * static_cast<std::size_t>(yComp.v) / static_cast<std::size_t>(maxV);
            const std::size_t src = srcY * yStride + srcX;
            const std::size_t dst = (y * width + x) * 4U;
            const std::uint8_t v = yPlane[src];
            outImage.rgba[dst + 0U] = v;
            outImage.rgba[dst + 1U] = v;
            outImage.rgba[dst + 2U] = v;
            outImage.rgba[dst + 3U] = 255U;
          }
        }
      } else if (frameComponents.size() == 3U) {
        const std::size_t yIdx = componentIndexById(frameComponents, 1U);
        const std::size_t cbIdx = componentIndexById(frameComponents, 2U);
        const std::size_t crIdx = componentIndexById(frameComponents, 3U);
        if (yIdx == std::numeric_limits<std::size_t>::max() ||
            cbIdx == std::numeric_limits<std::size_t>::max() ||
            crIdx == std::numeric_limits<std::size_t>::max()) {
          error = "jpeg component ids unsupported";
          return false;
        }

        const std::vector<std::uint8_t>& yPlane = planes[yIdx];
        const std::vector<std::uint8_t>& cbPlane = planes[cbIdx];
        const std::vector<std::uint8_t>& crPlane = planes[crIdx];
        const std::size_t yStride = planeStrides[yIdx];
        const std::size_t cbStride = planeStrides[cbIdx];
        const std::size_t crStride = planeStrides[crIdx];
        const ComponentSpec& yComp = frameComponents[yIdx];
        const ComponentSpec& cbComp = frameComponents[cbIdx];
        const ComponentSpec& crComp = frameComponents[crIdx];

        for (std::size_t y = 0U; y < height; ++y) {
          for (std::size_t x = 0U; x < width; ++x) {
            const std::size_t yX = x * static_cast<std::size_t>(yComp.h) / static_cast<std::size_t>(maxH);
            const std::size_t yY = y * static_cast<std::size_t>(yComp.v) / static_cast<std::size_t>(maxV);
            const std::size_t cbX = x * static_cast<std::size_t>(cbComp.h) / static_cast<std::size_t>(maxH);
            const std::size_t cbY = y * static_cast<std::size_t>(cbComp.v) / static_cast<std::size_t>(maxV);
            const std::size_t crX = x * static_cast<std::size_t>(crComp.h) / static_cast<std::size_t>(maxH);
            const std::size_t crY = y * static_cast<std::size_t>(crComp.v) / static_cast<std::size_t>(maxV);

            const double yy = static_cast<double>(yPlane[yY * yStride + yX]);
            const double cb = static_cast<double>(cbPlane[cbY * cbStride + cbX]) - 128.0;
            const double cr = static_cast<double>(crPlane[crY * crStride + crX]) - 128.0;

            const double r = yy + 1.402 * cr;
            const double g = yy - 0.344136 * cb - 0.714136 * cr;
            const double b = yy + 1.772 * cb;

            const std::size_t dst = (y * width + x) * 4U;
            outImage.rgba[dst + 0U] = clampByte(r);
            outImage.rgba[dst + 1U] = clampByte(g);
            outImage.rgba[dst + 2U] = clampByte(b);
            outImage.rgba[dst + 3U] = 255U;
          }
        }
      } else {
        error = "jpeg component count unsupported";
        return false;
      }

      if (nextMarker != kEoi) {
        error = "jpeg multi-scan streams unsupported";
        return false;
      }

      return true;
    }

    bool ok = false;
    const std::uint16_t segmentLen = readU16Be(bytes, offset, ok);
    if (!ok || segmentLen < 2U || offset + segmentLen > bytes.size()) {
      error = "jpeg segment truncated";
      gLastJpegCodecError = error;
      return false;
    }

    const std::size_t segStart = offset + 2U;
    const std::size_t segEnd = segStart + static_cast<std::size_t>(segmentLen - 2U);
    if (segEnd > bytes.size()) {
      error = "jpeg segment overrun";
      return false;
    }

    if (marker == kDqt) {
      std::size_t p = segStart;
      while (p < segEnd) {
        const std::uint8_t pqTq = bytes[p++];
        const std::uint8_t precision = static_cast<std::uint8_t>((pqTq >> 4U) & 0x0FU);
        const std::uint8_t tableId = static_cast<std::uint8_t>(pqTq & 0x0FU);
        if (tableId > 3U) {
          error = "jpeg DQT table id invalid";
          return false;
        }

        if (precision != 0U) {
          error = "jpeg 16-bit quant tables unsupported";
          return false;
        }

        if (p + 64U > segEnd) {
          error = "jpeg DQT payload truncated";
          return false;
        }

        quantTables[tableId].defined = true;
        for (std::size_t i = 0U; i < 64U; ++i) {
          const std::size_t natural = kZigZagToNatural[i];
          quantTables[tableId].values[natural] = bytes[p++];
        }
      }
    } else if (marker == kDht) {
      std::size_t p = segStart;
      while (p < segEnd) {
        const std::uint8_t tcTh = bytes[p++];
        const std::uint8_t tableClass = static_cast<std::uint8_t>((tcTh >> 4U) & 0x0FU);
        const std::uint8_t tableId = static_cast<std::uint8_t>(tcTh & 0x0FU);
        if (tableClass > 1U || tableId > 3U) {
          error = "jpeg DHT table selector invalid";
          return false;
        }

        if (p + 16U > segEnd) {
          error = "jpeg DHT code counts truncated";
          return false;
        }

        std::array<std::uint8_t, 16> counts{};
        std::size_t valueCount = 0U;
        for (std::size_t i = 0U; i < 16U; ++i) {
          counts[i] = bytes[p++];
          valueCount += counts[i];
        }

        if (p + valueCount > segEnd || valueCount > 256U) {
          error = "jpeg DHT values truncated";
          return false;
        }

        std::vector<std::uint8_t> values(valueCount, 0U);
        for (std::size_t i = 0U; i < valueCount; ++i) {
          values[i] = bytes[p++];
        }

        HuffmanEncoderTable dummyEnc{};
        std::string huffError;
        if (!buildHuffmanTables(counts, values, dummyEnc, huffmanDec[tableClass][tableId], huffError)) {
          error = "jpeg DHT invalid: " + huffError;
          return false;
        }
      }
    } else if (marker == kSof0) {
      if (segEnd - segStart < 6U) {
        error = "jpeg SOF0 truncated";
        return false;
      }

      std::size_t p = segStart;
      const std::uint8_t precision = bytes[p++];
      if (precision != 8U) {
        error = "jpeg sample precision unsupported";
        return false;
      }

      bool dimOk = false;
      height = readU16Be(bytes, p, dimOk);
      if (!dimOk) {
        error = "jpeg SOF0 height truncated";
        return false;
      }
      p += 2U;

      width = readU16Be(bytes, p, dimOk);
      if (!dimOk) {
        error = "jpeg SOF0 width truncated";
        return false;
      }
      p += 2U;

      if (width == 0U || height == 0U) {
        error = "jpeg dimensions invalid";
        return false;
      }

      const std::uint8_t componentCount = bytes[p++];
      if (componentCount != 1U && componentCount != 3U) {
        error = "jpeg component count unsupported";
        return false;
      }

      if (p + static_cast<std::size_t>(componentCount) * 3U != segEnd) {
        error = "jpeg SOF0 payload malformed";
        return false;
      }

      frameComponents.clear();
      frameComponents.reserve(componentCount);
      for (std::uint8_t i = 0U; i < componentCount; ++i) {
        ComponentSpec c{};
        c.id = bytes[p++];
        const std::uint8_t hv = bytes[p++];
        c.h = static_cast<std::uint8_t>((hv >> 4U) & 0x0FU);
        c.v = static_cast<std::uint8_t>(hv & 0x0FU);
        c.quantTable = bytes[p++];
        if (c.h == 0U || c.v == 0U || c.quantTable > 3U) {
          error = "jpeg SOF0 component parameters invalid";
          return false;
        }
        frameComponents.push_back(c);
      }

      sawSof0 = true;
    } else if (marker == kDri) {
      if (segEnd - segStart != 2U) {
        error = "jpeg DRI size invalid";
        return false;
      }

      bool driOk = false;
      const std::uint16_t driInterval = readU16Be(bytes, segStart, driOk);
      if (!driOk) {
        error = "jpeg DRI truncated";
        return false;
      }

      restartInterval = driInterval;
    }

    offset = segEnd;
  }

  if (!sawSos) {
    error = "jpeg missing SOS";
    return false;
  }

  error = "jpeg decode failed";
  gLastJpegCodecError = error;
  return false;
}

bool encodeJpegRgbaFile(const std::filesystem::path& path,
                        const RawImage& image,
                        int jpegQuality) {
  gLastJpegCodecError.clear();
  if (image.width == 0U || image.height == 0U) {
    return failJpeg("jpeg encode invalid dimensions");
  }

  if (image.height != 0U && image.width > std::numeric_limits<std::size_t>::max() / image.height) {
    return failJpeg("jpeg encode dimensions overflow");
  }
  const std::size_t pixelCount = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
  if (pixelCount > std::numeric_limits<std::size_t>::max() / 4U || image.rgba.size() < pixelCount * 4U) {
    return failJpeg("jpeg encode rgba buffer invalid");
  }
  if (image.width > std::numeric_limits<std::uint16_t>::max() ||
      image.height > std::numeric_limits<std::uint16_t>::max()) {
    return failJpeg("jpeg encode dimensions exceed SOF0 limits");
  }
  if (pixelCount > kMaxJpegImageBytes / 4U) {
    return failJpeg("jpeg encode exceeds safety limit");
  }

  const std::array<std::uint16_t, 64> qY = scaledQuantTable(kStdLumaQuant, jpegQuality);
  const std::array<std::uint16_t, 64> qC = scaledQuantTable(kStdChromaQuant, jpegQuality);

  HuffmanEncoderTable dcYEnc{};
  HuffmanEncoderTable acYEnc{};
  HuffmanEncoderTable dcCEnc{};
  HuffmanEncoderTable acCEnc{};
  HuffmanDecoderTable unusedDec{};
  std::string huffError;

  if (!buildHuffmanTables(
          kStdDcLumaCounts,
          std::vector<std::uint8_t>(kStdDcValues.begin(), kStdDcValues.end()),
          dcYEnc,
          unusedDec,
          huffError) ||
      !buildHuffmanTables(
          kStdAcLumaCounts,
          std::vector<std::uint8_t>(kStdAcLumaValues.begin(), kStdAcLumaValues.end()),
          acYEnc,
          unusedDec,
          huffError) ||
      !buildHuffmanTables(
          kStdDcChromaCounts,
          std::vector<std::uint8_t>(kStdDcValues.begin(), kStdDcValues.end()),
          dcCEnc,
          unusedDec,
          huffError) ||
      !buildHuffmanTables(
          kStdAcChromaCounts,
          std::vector<std::uint8_t>(kStdAcChromaValues.begin(), kStdAcChromaValues.end()),
          acCEnc,
          unusedDec,
          huffError)) {
    return failJpeg("jpeg huffman init failed: " + huffError);
  }

    constexpr std::size_t kYH = 2U;
    constexpr std::size_t kYV = 1U;
    const std::size_t mcuCols =
      (static_cast<std::size_t>(image.width) + (kYH * 8U - 1U)) / (kYH * 8U);
    const std::size_t mcuRows =
      (static_cast<std::size_t>(image.height) + (kYV * 8U - 1U)) / (kYV * 8U);
  if (mcuCols != 0U && mcuRows > std::numeric_limits<std::size_t>::max() / mcuCols) {
    return failJpeg("jpeg encode mcu dimensions overflow");
  }
  const std::size_t totalMcus = mcuCols * mcuRows;
  const std::uint16_t restartInterval = totalMcus > 8U ? 8U : 0U;

  std::vector<std::uint8_t> entropy;
  entropy.reserve(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height));

  std::string encodeError;
  std::uint8_t rstIndex = 0U;
  std::size_t mcuIndex = 0U;
  while (mcuIndex < totalMcus) {
    JpegBitWriter entropyWriter;
    int prevDcY = 0;
    int prevDcCb = 0;
    int prevDcCr = 0;

    const std::size_t segmentMcuCount = restartInterval == 0U
                                            ? totalMcus
                                            : std::min<std::size_t>(
                                                  static_cast<std::size_t>(restartInterval),
                                                  totalMcus - mcuIndex);

    for (std::size_t segmentMcu = 0U; segmentMcu < segmentMcuCount; ++segmentMcu, ++mcuIndex) {
      const std::size_t mx = mcuIndex % mcuCols;
      const std::size_t my = mcuIndex / mcuCols;
      const std::size_t baseX = mx * 16U;
      const std::size_t baseY = my * 8U;

      for (std::size_t vy = 0U; vy < kYV; ++vy) {
        for (std::size_t hx = 0U; hx < kYH; ++hx) {
          std::array<double, 64> yBlock{};
          std::array<double, 64> yDct{};
          std::array<int, 64> yCoeff{};

          sampleRgbaLumaBlock(image, baseX + hx * 8U, baseY + vy * 8U, yBlock);
          forwardDct8x8(yBlock, yDct);
          quantizeBlock(yDct, qY, yCoeff);
          if (!encodeJpegBlock(entropyWriter, yCoeff, prevDcY, dcYEnc, acYEnc, encodeError)) {
            return failJpeg("jpeg block encode failed: " + encodeError);
          }
        }
      }

      std::array<double, 64> cbBlock{};
      std::array<double, 64> crBlock{};
      std::array<double, 64> cbDct{};
      std::array<double, 64> crDct{};
      std::array<int, 64> cbCoeff{};
      std::array<int, 64> crCoeff{};

      sampleRgbaChroma422Block(image, baseX, baseY, cbBlock, crBlock);
      forwardDct8x8(cbBlock, cbDct);
      forwardDct8x8(crBlock, crDct);
      quantizeBlock(cbDct, qC, cbCoeff);
      quantizeBlock(crDct, qC, crCoeff);

      if (!encodeJpegBlock(entropyWriter, cbCoeff, prevDcCb, dcCEnc, acCEnc, encodeError) ||
          !encodeJpegBlock(entropyWriter, crCoeff, prevDcCr, dcCEnc, acCEnc, encodeError)) {
        return failJpeg("jpeg block encode failed: " + encodeError);
      }
    }

    entropyWriter.flush();
    const std::vector<std::uint8_t>& segmentBytes = entropyWriter.bytes();
    entropy.insert(entropy.end(), segmentBytes.begin(), segmentBytes.end());

    if (restartInterval != 0U && mcuIndex < totalMcus) {
      entropy.push_back(0xFFU);
      entropy.push_back(static_cast<std::uint8_t>(0xD0U + (rstIndex & 0x07U)));
      rstIndex = static_cast<std::uint8_t>((rstIndex + 1U) & 0x07U);
    }
  }

  std::vector<std::uint8_t> out;
  out.reserve(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) + 1024U);

  appendMarker(out, kSoi);

  {
    std::vector<std::uint8_t> app0 = {
        'J', 'F', 'I', 'F', 0x00,
        0x01, 0x01,
        0x00,
        0x00, 0x01,
        0x00, 0x01,
        0x00,
        0x00,
    };
    appendSegment(out, kApp0, app0);
  }

  writeDqt(out, qY, qC);

  {
    std::vector<std::uint8_t> sof0;
    sof0.reserve(6U + 3U * 3U);
    sof0.push_back(8U);
    appendU16Be(sof0, static_cast<std::uint16_t>(image.height));
    appendU16Be(sof0, static_cast<std::uint16_t>(image.width));
    sof0.push_back(3U);

    sof0.push_back(1U);
    sof0.push_back(0x21U);
    sof0.push_back(0U);

    sof0.push_back(2U);
    sof0.push_back(0x11U);
    sof0.push_back(1U);

    sof0.push_back(3U);
    sof0.push_back(0x11U);
    sof0.push_back(1U);

    appendSegment(out, kSof0, sof0);
  }

  writeDht(out);

  if (restartInterval != 0U) {
    std::vector<std::uint8_t> dri;
    dri.reserve(2U);
    appendU16Be(dri, restartInterval);
    appendSegment(out, kDri, dri);
  }

  {
    std::vector<std::uint8_t> sos;
    sos.reserve(10U);
    sos.push_back(3U);

    sos.push_back(1U);
    sos.push_back(0x00U);

    sos.push_back(2U);
    sos.push_back(0x11U);

    sos.push_back(3U);
    sos.push_back(0x11U);

    sos.push_back(0x00U);
    sos.push_back(0x3FU);
    sos.push_back(0x00U);

    appendSegment(out, kSos, sos);
  }

  out.insert(out.end(), entropy.begin(), entropy.end());

  appendMarker(out, kEoi);
  if (!codec::writeBinaryFile(path, out)) {
    return failJpeg("jpeg output write failed");
  }

  return true;
}

}  // namespace volt::io