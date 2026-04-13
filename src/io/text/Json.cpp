#include "volt/io/text/Json.hpp"
#include "volt/io/text/Utf.hpp"

#include "volt/core/Logging.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

namespace volt::io {
namespace {

constexpr std::size_t kMaxParseDepth = 128;

struct SourceLocation {
  std::size_t line{1};
  std::size_t column{1};
};

[[nodiscard]] SourceLocation computeSourceLocation(const std::string& text, std::size_t offset) {
  SourceLocation location{};
  const std::size_t clamped = std::min(offset, text.size());
  for (std::size_t i = 0; i < clamped; ++i) {
    if (text[i] == '\n') {
      ++location.line;
      location.column = 1;
    } else {
      ++location.column;
    }
  }
  return location;
}

[[nodiscard]] std::string formatSourceLocation(const std::string& text, std::size_t offset) {
  const SourceLocation location = computeSourceLocation(text, offset);
  return "line " + std::to_string(location.line) + ", column " + std::to_string(location.column);
}

[[nodiscard]] bool isDigit(char ch) {
  return ch >= '0' && ch <= '9';
}

class JsonParser {
 public:
  explicit JsonParser(const std::string& text)
      : text_(text) {}

  [[nodiscard]] JsonParseResult parse() {
    JsonParseResult result{};
    skipWhitespace();

    Json parsedValue{};
    if (!parseValue(parsedValue, 0)) {
      result.success = false;
      result.errorMessage = buildErrorMessage();
      VOLT_LOG_WARN_CAT(volt::core::logging::Category::kIO, "JSON parse failed: ", result.errorMessage);
      return result;
    }

    skipWhitespace();
    if (!atEnd()) {
      setError("Unexpected trailing characters");
      result.success = false;
      result.errorMessage = buildErrorMessage();
      VOLT_LOG_WARN_CAT(volt::core::logging::Category::kIO, "JSON parse failed: ", result.errorMessage);
      return result;
    }

    result.success = true;
    result.value = std::move(parsedValue);
    return result;
  }

 private:
  const std::string& text_;
  std::size_t position_{0};
  std::string errorMessage_{};
  std::size_t errorOffset_{0};
  bool hasError_{false};

  [[nodiscard]] bool atEnd() const {
    return position_ >= text_.size();
  }

  [[nodiscard]] char peek() const {
    return atEnd() ? '\0' : text_[position_];
  }

  char advance() {
    if (atEnd()) {
      return '\0';
    }
    return text_[position_++];
  }

  [[nodiscard]] bool consume(char expected) {
    if (peek() != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  void skipWhitespace() {
    while (!atEnd()) {
      const char ch = peek();
      if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
        ++position_;
      } else {
        break;
      }
    }
  }

  void setError(const std::string& message) {
    if (!hasError_) {
      hasError_ = true;
      errorMessage_ = message;
      errorOffset_ = position_;
    }
  }

  [[nodiscard]] std::string buildErrorMessage() const {
    if (!hasError_) {
      return "Unknown parse error";
    }
    return errorMessage_ + " at " + formatSourceLocation(text_, errorOffset_);
  }

  [[nodiscard]] bool parseValue(Json& out, std::size_t depth) {
    if (depth > kMaxParseDepth) {
      setError("Exceeded maximum JSON nesting depth");
      return false;
    }

    skipWhitespace();
    if (atEnd()) {
      setError("Unexpected end of input while parsing value");
      return false;
    }

    const char next = peek();
    if (next == '{') {
      return parseObject(out, depth + 1);
    }

    if (next == '[') {
      return parseArray(out, depth + 1);
    }

    if (next == '"') {
      std::string value;
      if (!parseString(value)) {
        return false;
      }
      out = Json(std::move(value));
      return true;
    }

    if (next == 't') {
      if (!consumeLiteral("true")) {
        setError("Invalid literal; expected 'true'");
        return false;
      }
      out = Json(true);
      return true;
    }

    if (next == 'f') {
      if (!consumeLiteral("false")) {
        setError("Invalid literal; expected 'false'");
        return false;
      }
      out = Json(false);
      return true;
    }

    if (next == 'n') {
      if (!consumeLiteral("null")) {
        setError("Invalid literal; expected 'null'");
        return false;
      }
      out = Json(nullptr);
      return true;
    }

    if (next == '-' || isDigit(next)) {
      return parseNumber(out);
    }

    setError("Unexpected token while parsing value");
    return false;
  }

  [[nodiscard]] bool parseObject(Json& out, std::size_t depth) {
    if (!consume('{')) {
      setError("Expected '{' at object start");
      return false;
    }

    skipWhitespace();

    JsonObject object;
    if (consume('}')) {
      out = Json(std::move(object));
      return true;
    }

    while (true) {
      skipWhitespace();

      std::string key;
      if (!parseString(key)) {
        return false;
      }

      skipWhitespace();
      if (!consume(':')) {
        setError("Expected ':' after object key");
        return false;
      }

      Json value;
      if (!parseValue(value, depth + 1)) {
        return false;
      }
      object[std::move(key)] = std::move(value);

      skipWhitespace();
      if (consume('}')) {
        out = Json(std::move(object));
        return true;
      }

      if (!consume(',')) {
        setError("Expected ',' or '}' in object");
        return false;
      }
    }
  }

  [[nodiscard]] bool parseArray(Json& out, std::size_t depth) {
    if (!consume('[')) {
      setError("Expected '[' at array start");
      return false;
    }

    skipWhitespace();

    JsonArray array;
    if (consume(']')) {
      out = Json(std::move(array));
      return true;
    }

    while (true) {
      Json value;
      if (!parseValue(value, depth + 1)) {
        return false;
      }
      array.push_back(std::move(value));

      skipWhitespace();
      if (consume(']')) {
        out = Json(std::move(array));
        return true;
      }

      if (!consume(',')) {
        setError("Expected ',' or ']' in array");
        return false;
      }
    }
  }

  [[nodiscard]] bool parseString(std::string& out) {
    if (!consume('"')) {
      setError("Expected '\"' to start string");
      return false;
    }

    out.clear();

    while (!atEnd()) {
      const char ch = advance();

      if (ch == '"') {
        return true;
      }

      if (static_cast<unsigned char>(ch) < 0x20U) {
        setError("Unescaped control character in string");
        return false;
      }

      if (ch != '\\') {
        out.push_back(ch);
        continue;
      }

      if (atEnd()) {
        setError("Unexpected end of input in string escape sequence");
        return false;
      }

      const char escaped = advance();
      switch (escaped) {
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '/':
          out.push_back('/');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          if (!parseUnicodeEscape(out)) {
            return false;
          }
          break;
        }
        default:
          setError("Invalid escape sequence in string");
          return false;
      }
    }

    setError("Unterminated string");
    return false;
  }

  [[nodiscard]] bool parseUnicodeEscape(std::string& out) {
    char16_t firstUnit = 0;
    if (!parseHexUnit(firstUnit)) {
      return false;
    }

    char32_t codePoint = static_cast<char32_t>(firstUnit);
    if (utf::isHighSurrogate(firstUnit)) {
      if (!consume('\\') || !consume('u')) {
        setError("Expected low surrogate after high surrogate");
        return false;
      }

      char16_t secondUnit = 0;
      if (!parseHexUnit(secondUnit)) {
        return false;
      }

      if (!utf::isLowSurrogate(secondUnit)) {
        setError("Invalid low surrogate in unicode escape");
        return false;
      }

      if (!utf::decodeUtf16SurrogatePair(firstUnit, secondUnit, codePoint)) {
        setError("Invalid surrogate pair in unicode escape");
        return false;
      }
    } else if (utf::isLowSurrogate(firstUnit)) {
      setError("Unexpected low surrogate in unicode escape");
      return false;
    }

    if (!utf::appendCodepointUtf8(codePoint, out)) {
      setError("Invalid unicode codepoint in escape sequence");
      return false;
    }

    return true;
  }

  [[nodiscard]] bool parseHexUnit(char16_t& valueOut) {
    if (position_ + 4U > text_.size()) {
      setError("Truncated unicode escape sequence");
      return false;
    }

    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4U; ++i) {
      const int nibble = utf::hexToNibble(text_[position_ + i]);
      if (nibble < 0) {
        setError("Invalid hexadecimal digit in unicode escape");
        return false;
      }
      value = (value << 4U) | static_cast<std::uint32_t>(nibble);
    }

    position_ += 4U;
    valueOut = static_cast<char16_t>(value);
    return true;
  }

  [[nodiscard]] bool parseNumber(Json& out) {
    const std::size_t start = position_;

    if (consume('-') && atEnd()) {
      setError("Unexpected end of input after '-' in number");
      return false;
    }

    if (consume('0')) {
      if (isDigit(peek())) {
        setError("Leading zeroes are not allowed in JSON numbers");
        return false;
      }
    } else {
      if (!isDigit(peek())) {
        setError("Expected digit while parsing number");
        return false;
      }
      while (isDigit(peek())) {
        advance();
      }
    }

    if (consume('.')) {
      if (!isDigit(peek())) {
        setError("Expected digit after decimal point");
        return false;
      }
      while (isDigit(peek())) {
        advance();
      }
    }

    if (peek() == 'e' || peek() == 'E') {
      advance();
      if (peek() == '+' || peek() == '-') {
        advance();
      }
      if (!isDigit(peek())) {
        setError("Expected exponent digits in number");
        return false;
      }
      while (isDigit(peek())) {
        advance();
      }
    }

    const char* begin = text_.data() + start;
    const char* end = text_.data() + position_;

    double parsed = 0.0;
    const auto parseResult = std::from_chars(begin, end, parsed, std::chars_format::general);
    if (parseResult.ec != std::errc() || parseResult.ptr != end) {
      setError("Failed to parse number");
      return false;
    }

    if (!std::isfinite(parsed)) {
      setError("Number out of range");
      return false;
    }

    out = Json(parsed);
    return true;
  }

  [[nodiscard]] bool consumeLiteral(std::string_view literal) {
    if (position_ + literal.size() > text_.size()) {
      return false;
    }

    for (std::size_t i = 0; i < literal.size(); ++i) {
      if (text_[position_ + i] != literal[i]) {
        return false;
      }
    }

    position_ += literal.size();
    return true;
  }
};

void appendEscapedString(const std::string& input, std::string& out) {
  out.push_back('"');
  for (unsigned char ch : input) {
    switch (ch) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (ch < 0x20U) {
          constexpr char kHexDigits[] = "0123456789ABCDEF";
          out.append("\\u00");
          out.push_back(kHexDigits[(ch >> 4U) & 0x0FU]);
          out.push_back(kHexDigits[ch & 0x0FU]);
        } else {
          out.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  out.push_back('"');
}

void appendIndent(std::string& out, std::size_t depth, std::uint32_t indentSize) {
  if (indentSize == 0U || depth == 0U) {
    return;
  }
  out.append(depth * static_cast<std::size_t>(indentSize), ' ');
}

[[nodiscard]] bool stringifyValue(
    const Json& value,
    std::string& out,
    const JsonStringifyOptions& options,
    std::size_t depth,
    std::string& errorMessage) {
  if (value.isNull()) {
    out.append("null");
    return true;
  }

  if (const bool* booleanValue = value.asBoolean(); booleanValue != nullptr) {
    out.append(*booleanValue ? "true" : "false");
    return true;
  }

  if (const double* numberValue = value.asNumber(); numberValue != nullptr) {
    if (!std::isfinite(*numberValue)) {
      errorMessage = "Cannot stringify non-finite number";
      return false;
    }

    char numberBuffer[128]{};
    const auto numberResult =
        std::to_chars(std::begin(numberBuffer), std::end(numberBuffer), *numberValue, std::chars_format::general);
    if (numberResult.ec == std::errc()) {
      out.append(numberBuffer, numberResult.ptr);
      return true;
    }

    std::ostringstream fallback;
    fallback.precision(std::numeric_limits<double>::max_digits10);
    fallback << *numberValue;
    out.append(fallback.str());
    return true;
  }

  if (const std::string* stringValue = value.asString(); stringValue != nullptr) {
    appendEscapedString(*stringValue, out);
    return true;
  }

  if (const JsonArray* arrayValue = value.asArray(); arrayValue != nullptr) {
    out.push_back('[');
    if (!arrayValue->empty()) {
      if (options.pretty) {
        out.push_back('\n');
      }

      for (std::size_t i = 0; i < arrayValue->size(); ++i) {
        if (i > 0U) {
          if (options.pretty) {
            out.append(",\n");
          } else {
            out.push_back(',');
          }
        }

        if (options.pretty) {
          appendIndent(out, depth + 1U, options.indentSize);
        }

        if (!stringifyValue((*arrayValue)[i], out, options, depth + 1U, errorMessage)) {
          return false;
        }
      }

      if (options.pretty) {
        out.push_back('\n');
        appendIndent(out, depth, options.indentSize);
      }
    }
    out.push_back(']');
    return true;
  }

  if (const JsonObject* objectValue = value.asObject(); objectValue != nullptr) {
    out.push_back('{');
    if (!objectValue->empty()) {
      if (options.pretty) {
        out.push_back('\n');
      }

      std::size_t index = 0;
      for (const auto& [key, objectMemberValue] : *objectValue) {
        if (index > 0U) {
          if (options.pretty) {
            out.append(",\n");
          } else {
            out.push_back(',');
          }
        }

        if (options.pretty) {
          appendIndent(out, depth + 1U, options.indentSize);
        }

        appendEscapedString(key, out);
        out.append(options.pretty ? ": " : ":");

        if (!stringifyValue(objectMemberValue, out, options, depth + 1U, errorMessage)) {
          return false;
        }

        ++index;
      }

      if (options.pretty) {
        out.push_back('\n');
        appendIndent(out, depth, options.indentSize);
      }
    }
    out.push_back('}');
    return true;
  }

  errorMessage = "Unknown JSON value type";
  return false;
}

}  // namespace

JsonParseResult parseJson(const std::string& text) {
  JsonParser parser(text);
  return parser.parse();
}

JsonStringifyResult stringifyJson(const Json& value) {
  return stringifyJson(value, JsonStringifyOptions{});
}

JsonStringifyResult stringifyJson(const Json& value, const JsonStringifyOptions& options) {
  JsonStringifyResult result{};

  std::string errorMessage;
  if (!stringifyValue(value, result.jsonText, options, 0, errorMessage)) {
    result.success = false;
    result.errorMessage = errorMessage.empty() ? "Failed to stringify JSON" : std::move(errorMessage);
    VOLT_LOG_WARN_CAT(volt::core::logging::Category::kIO, "JSON stringify failed: ", result.errorMessage);
    return result;
  }

  result.success = true;
  return result;
}

}  // namespace volt::io
