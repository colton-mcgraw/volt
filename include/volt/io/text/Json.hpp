#pragma once

#include "volt/core/Text.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace volt::io {

class Json {
 public:
  using Object = std::unordered_map<std::string, Json>;
  using Array = std::vector<Json>;
  using Value = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

  Json() = default;
  Json(std::nullptr_t) : value_(nullptr) {}
  Json(bool value) : value_(value) {}
  Json(double value) : value_(value) {}
  Json(std::int32_t value) : value_(static_cast<double>(value)) {}
  Json(std::int64_t value) : value_(static_cast<double>(value)) {}
  Json(std::string value) : value_(std::move(value)) {}
  Json(const char* value) : value_(std::string(value != nullptr ? value : "")) {}
  Json(volt::core::Text value) : value_(value.toUtf8()) {}
  Json(Array value) : value_(std::move(value)) {}
  Json(Object value) : value_(std::move(value)) {}

  [[nodiscard]] bool isNull() const { return std::holds_alternative<std::nullptr_t>(value_); }
  [[nodiscard]] bool isBoolean() const { return std::holds_alternative<bool>(value_); }
  [[nodiscard]] bool isNumber() const { return std::holds_alternative<double>(value_); }
  [[nodiscard]] bool isString() const { return std::holds_alternative<std::string>(value_); }
  [[nodiscard]] bool isArray() const { return std::holds_alternative<Array>(value_); }
  [[nodiscard]] bool isObject() const { return std::holds_alternative<Object>(value_); }

  [[nodiscard]] const Value& value() const { return value_; }
  [[nodiscard]] Value& value() { return value_; }

  [[nodiscard]] const bool* asBoolean() const { return std::get_if<bool>(&value_); }
  [[nodiscard]] const double* asNumber() const { return std::get_if<double>(&value_); }
  [[nodiscard]] const std::string* asString() const { return std::get_if<std::string>(&value_); }
  [[nodiscard]] volt::core::Text asText() const {
    const std::string* value = asString();
    if (value == nullptr) {
      return {};
    }
    return volt::core::Text::fromUtf8(*value);
  }
  [[nodiscard]] const Array* asArray() const { return std::get_if<Array>(&value_); }
  [[nodiscard]] const Object* asObject() const { return std::get_if<Object>(&value_); }

  [[nodiscard]] bool* asBoolean() { return std::get_if<bool>(&value_); }
  [[nodiscard]] double* asNumber() { return std::get_if<double>(&value_); }
  [[nodiscard]] std::string* asString() { return std::get_if<std::string>(&value_); }
  [[nodiscard]] Array* asArray() { return std::get_if<Array>(&value_); }
  [[nodiscard]] Object* asObject() { return std::get_if<Object>(&value_); }

 private:
  Value value_{nullptr};
};

using JsonValue = Json::Value;
using JsonObject = Json::Object;
using JsonArray = Json::Array;
using JsonString = std::string;
using JsonNumber = double;
using JsonBoolean = bool;

struct JsonNull {};

struct JsonParseResult {
  bool success{false};
  std::string errorMessage;
  Json value;
};

[[nodiscard]] JsonParseResult parseJson(const std::string& text);

struct JsonStringifyResult {
  bool success{false};
  std::string errorMessage;
  std::string jsonText;
};

struct JsonStringifyOptions {
  bool pretty{false};
  std::uint32_t indentSize{2};
};

[[nodiscard]] JsonStringifyResult stringifyJson(const Json& value);
[[nodiscard]] JsonStringifyResult stringifyJson(
    const Json& value,
    const JsonStringifyOptions& options);

}  // namespace volt::io