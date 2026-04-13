#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace volt::io::compression::detail {

enum class BitOrder : std::uint8_t {
  kMsbFirst,
  kLsbFirst,
};

template <BitOrder TBitOrder>
class BitReader {
 public:
  explicit BitReader(const std::vector<std::uint8_t>& data)
      : data_(data.data()), dataSize_(data.size()) {}

  BitReader(const std::vector<std::uint8_t>& data, std::size_t offset, std::size_t size)
      : data_(data.data()), dataSize_(data.size()) {
    if (offset > data.size()) {
      data_ = nullptr;
      dataSize_ = 0U;
      return;
    }

    data_ += offset;
    dataSize_ = std::min(size, data.size() - offset);
  }

  bool peekBits(std::uint8_t count, std::uint32_t& out) {
    out = 0U;
    if (count == 0U) {
      return true;
    }
    if (count > 24U || !fillBits(count)) {
      return false;
    }

    const std::uint64_t mask = (1ULL << static_cast<unsigned int>(count)) - 1ULL;
    if constexpr (TBitOrder == BitOrder::kMsbFirst) {
      out = static_cast<std::uint32_t>((bitBuffer_ >> static_cast<unsigned int>(bitCount_ - count)) & mask);
    } else {
      out = static_cast<std::uint32_t>(bitBuffer_ & mask);
    }
    return true;
  }

  bool dropBits(std::uint8_t count) {
    if (count == 0U) {
      return true;
    }

    std::uint32_t ignored = 0U;
    if (!peekBits(count, ignored)) {
      return false;
    }

    if constexpr (TBitOrder == BitOrder::kMsbFirst) {
      bitCount_ = static_cast<std::uint8_t>(bitCount_ - count);
      if (bitCount_ == 0U) {
        bitBuffer_ = 0U;
        return true;
      }

      const std::uint64_t keepMask = (1ULL << static_cast<unsigned int>(bitCount_)) - 1ULL;
      bitBuffer_ &= keepMask;
      return true;
    } else {
      bitBuffer_ >>= count;
      bitCount_ = static_cast<std::uint8_t>(bitCount_ - count);
      if (bitCount_ == 0U) {
        bitBuffer_ = 0U;
      }
      return true;
    }
  }

  bool readBits(std::uint8_t count, std::uint32_t& out) {
    if (!peekBits(count, out)) {
      return false;
    }
    return dropBits(count);
  }

  void alignToByte() {
    const std::uint8_t drop = static_cast<std::uint8_t>(bitCount_ % 8U);
    if (drop == 0U) {
      return;
    }

    if constexpr (TBitOrder == BitOrder::kMsbFirst) {
      bitCount_ = static_cast<std::uint8_t>(bitCount_ - drop);
      if (bitCount_ == 0U) {
        bitBuffer_ = 0U;
      } else {
        const std::uint64_t keepMask = (1ULL << static_cast<unsigned int>(bitCount_)) - 1ULL;
        bitBuffer_ &= keepMask;
      }
    } else {
      bitBuffer_ >>= drop;
      bitCount_ = static_cast<std::uint8_t>(bitCount_ - drop);
      if (bitCount_ == 0U) {
        bitBuffer_ = 0U;
      }
    }
  }

  bool readBytesAligned(std::uint8_t* dst, std::size_t count) {
    if (dst == nullptr || bitCount_ != 0U) {
      return false;
    }
    if (byteOffset_ + count > dataSize_) {
      return false;
    }

    std::memcpy(dst, data_ + byteOffset_, count);
    byteOffset_ += count;
    return true;
  }

  [[nodiscard]] bool remainingBitsAreZero() const {
    if (bitCount_ > 0U) {
      const std::uint64_t mask = (1ULL << static_cast<unsigned int>(bitCount_)) - 1ULL;
      if ((bitBuffer_ & mask) != 0U) {
        return false;
      }
    }

    for (std::size_t i = byteOffset_; i < dataSize_; ++i) {
      if (data_[i] != 0U) {
        return false;
      }
    }

    return true;
  }

 private:
  bool fillBits(std::uint8_t count) {
    while (bitCount_ < count) {
      if (byteOffset_ >= dataSize_) {
        return false;
      }

      const std::uint8_t next = data_[byteOffset_++];
      if constexpr (TBitOrder == BitOrder::kMsbFirst) {
        bitBuffer_ = (bitBuffer_ << 8U) | static_cast<std::uint64_t>(next);
      } else {
        bitBuffer_ |= static_cast<std::uint64_t>(next) << static_cast<unsigned int>(bitCount_);
      }
      bitCount_ = static_cast<std::uint8_t>(bitCount_ + 8U);
    }

    return true;
  }

  const std::uint8_t* data_{nullptr};
  std::size_t dataSize_{0U};
  std::size_t byteOffset_{0U};
  std::uint64_t bitBuffer_{0U};
  std::uint8_t bitCount_{0U};
};

template <BitOrder TBitOrder>
class BitWriter {
 public:
  void writeBits(std::uint32_t value, std::uint8_t bitLength) {
    if (bitLength == 0U) {
      return;
    }

    if constexpr (TBitOrder == BitOrder::kMsbFirst) {
      for (int bit = static_cast<int>(bitLength) - 1; bit >= 0; --bit) {
        const std::uint8_t currentBit =
            static_cast<std::uint8_t>((value >> static_cast<unsigned int>(bit)) & 0x01U);
        bitBuffer_ = static_cast<std::uint8_t>((bitBuffer_ << 1U) | currentBit);
        ++bitCount_;
        if (bitCount_ == 8U) {
          bytes_.push_back(bitBuffer_);
          bitBuffer_ = 0U;
          bitCount_ = 0U;
        }
      }
    } else {
      for (std::uint8_t bit = 0U; bit < bitLength; ++bit) {
        const std::uint8_t currentBit =
            static_cast<std::uint8_t>((value >> static_cast<unsigned int>(bit)) & 0x01U);
        bitBuffer_ = static_cast<std::uint8_t>(bitBuffer_ | (currentBit << bitCount_));
        ++bitCount_;
        if (bitCount_ == 8U) {
          bytes_.push_back(bitBuffer_);
          bitBuffer_ = 0U;
          bitCount_ = 0U;
        }
      }
    }
  }

  void flush() {
    if (bitCount_ == 0U) {
      return;
    }

    if constexpr (TBitOrder == BitOrder::kMsbFirst) {
      const std::uint8_t out = static_cast<std::uint8_t>(
          bitBuffer_ << static_cast<unsigned int>(8U - bitCount_));
      bytes_.push_back(out);
    } else {
      bytes_.push_back(bitBuffer_);
    }

    bitBuffer_ = 0U;
    bitCount_ = 0U;
  }

  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const {
    return bytes_;
  }

 private:
  std::vector<std::uint8_t> bytes_;
  std::uint8_t bitBuffer_{0U};
  std::uint8_t bitCount_{0U};
};

}  // namespace volt::io::compression::detail
