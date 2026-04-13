#include "volt/ui/UIText.hpp"

#include <utility>

namespace volt::ui {

bool decodeUtf8(std::string_view text, std::vector<GlyphCluster>* outGlyphs) {
  if (outGlyphs == nullptr) {
    return false;
  }

  outGlyphs->clear();
  outGlyphs->reserve(text.size());

  bool valid = true;
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    std::uint32_t codepoint = 0;
    std::size_t sequenceLength = 1;

    if ((c & 0x80U) == 0U) {
      codepoint = c;
      sequenceLength = 1;
    } else if ((c & 0xE0U) == 0xC0U && i + 1 < text.size()) {
      codepoint = (static_cast<std::uint32_t>(c & 0x1FU) << 6U) |
                  static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + 1]) & 0x3FU);
      sequenceLength = 2;
    } else if ((c & 0xF0U) == 0xE0U && i + 2 < text.size()) {
      codepoint = (static_cast<std::uint32_t>(c & 0x0FU) << 12U) |
                  (static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + 1]) & 0x3FU) << 6U) |
                  static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + 2]) & 0x3FU);
      sequenceLength = 3;
    } else if ((c & 0xF8U) == 0xF0U && i + 3 < text.size()) {
      codepoint = (static_cast<std::uint32_t>(c & 0x07U) << 18U) |
                  (static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + 1]) & 0x3FU) << 12U) |
                  (static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + 2]) & 0x3FU) << 6U) |
                  static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + 3]) & 0x3FU);
      sequenceLength = 4;
    } else {
      codepoint = 0xFFFD;
      sequenceLength = 1;
      valid = false;
    }

    outGlyphs->push_back(GlyphCluster{codepoint, static_cast<std::uint32_t>(i)});
    i += sequenceLength;
  }

  return valid;
}

TextRun buildTextRun(std::string_view text, std::string fontFamily, float fontSizePx) {
  TextRun run{};
  run.text = volt::core::Text::fromUtf8(text);
  run.fontFamily = std::move(fontFamily);
  run.fontSizePx = fontSizePx;
  const bool decodeOk = decodeUtf8(text, &run.glyphs);
  (void)decodeOk;
  return run;
}

}  // namespace volt::ui
