#include "volt/io/text/Json.hpp"

#include <iostream>
#include <string>

namespace {

using volt::io::Json;
using volt::io::JsonParseResult;
using volt::io::JsonStringifyOptions;
using volt::io::JsonStringifyResult;

bool jsonEquivalent(const Json& lhs, const Json& rhs) {
  if (lhs.value().index() != rhs.value().index()) {
    return false;
  }

  if (lhs.isNull()) {
    return true;
  }

  if (const bool* lhsBoolean = lhs.asBoolean(); lhsBoolean != nullptr) {
    const bool* rhsBoolean = rhs.asBoolean();
    return rhsBoolean != nullptr && *lhsBoolean == *rhsBoolean;
  }

  if (const double* lhsNumber = lhs.asNumber(); lhsNumber != nullptr) {
    const double* rhsNumber = rhs.asNumber();
    return rhsNumber != nullptr && *lhsNumber == *rhsNumber;
  }

  if (const std::string* lhsString = lhs.asString(); lhsString != nullptr) {
    const std::string* rhsString = rhs.asString();
    return rhsString != nullptr && *lhsString == *rhsString;
  }

  if (const Json::Array* lhsArray = lhs.asArray(); lhsArray != nullptr) {
    const Json::Array* rhsArray = rhs.asArray();
    if (rhsArray == nullptr || lhsArray->size() != rhsArray->size()) {
      return false;
    }

    for (std::size_t i = 0; i < lhsArray->size(); ++i) {
      if (!jsonEquivalent((*lhsArray)[i], (*rhsArray)[i])) {
        return false;
      }
    }
    return true;
  }

  const Json::Object* lhsObject = lhs.asObject();
  const Json::Object* rhsObject = rhs.asObject();
  if (lhsObject == nullptr || rhsObject == nullptr || lhsObject->size() != rhsObject->size()) {
    return false;
  }

  for (const auto& [key, lhsValue] : *lhsObject) {
    const auto rhsIt = rhsObject->find(key);
    if (rhsIt == rhsObject->end()) {
      return false;
    }

    if (!jsonEquivalent(lhsValue, rhsIt->second)) {
      return false;
    }
  }

  return true;
}

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "[json-test] FAIL: " << message << '\n';
    return false;
  }
  return true;
}

bool expectParseSuccess(const std::string& input, JsonParseResult& out, const std::string& testName) {
  out = volt::io::parseJson(input);
  if (!out.success) {
    std::cerr << "[json-test] FAIL: " << testName << " parse failed: " << out.errorMessage << '\n';
    return false;
  }
  return true;
}

bool expectParseFailure(const std::string& input, const std::string& testName) {
  const JsonParseResult parseResult = volt::io::parseJson(input);
  if (parseResult.success) {
    std::cerr << "[json-test] FAIL: " << testName << " expected parse failure" << '\n';
    return false;
  }
  return true;
}

bool expectStringifySuccess(
    const Json& value,
    JsonStringifyResult& out,
    const std::string& testName,
    const JsonStringifyOptions* options = nullptr) {
  out = options != nullptr ? volt::io::stringifyJson(value, *options) : volt::io::stringifyJson(value);
  if (!out.success) {
    std::cerr << "[json-test] FAIL: " << testName << " stringify failed: " << out.errorMessage << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  bool ok = true;

  {
    JsonParseResult result{};
    ok = expectParseSuccess("null", result, "parse-null") && ok;
    ok = expect(result.value.isNull(), "parse-null expected null") && ok;
  }

  {
    JsonParseResult result{};
    ok = expectParseSuccess("true", result, "parse-true") && ok;
    ok = expect(result.value.asBoolean() != nullptr && *result.value.asBoolean(), "parse-true expected boolean true") && ok;
  }

  {
    JsonParseResult result{};
    ok = expectParseSuccess("-1.25e+2", result, "parse-scientific-number") && ok;
    ok = expect(result.value.asNumber() != nullptr && *result.value.asNumber() == -125.0, "parse-scientific-number expected -125") && ok;
  }

  {
    JsonParseResult result{};
    ok = expectParseSuccess("\"A: \\u0041, Smile: \\uD83D\\uDE00\"", result, "parse-unicode-escapes") && ok;
    const std::string* parsed = result.value.asString();
    ok = expect(parsed != nullptr, "parse-unicode-escapes expected string") && ok;
    ok = expect(parsed != nullptr && parsed->find("A: A") != std::string::npos, "parse-unicode-escapes expected decoded A") && ok;
    const volt::core::Text parsedText = result.value.asText();
    ok = expect(parsedText.toUtf8() == *parsed, "parse-unicode-escapes asText bridge") && ok;
  }

  {
    JsonParseResult result{};
    ok = expectParseSuccess(
             "{\"name\":\"volt\",\"items\":[1,2,3],\"meta\":{\"enabled\":true}}",
             result,
             "parse-nested") && ok;
    ok = expect(result.value.isObject(), "parse-nested expected object") && ok;
  }

  ok = expectParseFailure("{\"a\":1,}", "parse-invalid-trailing-comma") && ok;
  ok = expectParseFailure("1e", "parse-invalid-exponent") && ok;
  ok = expectParseFailure("\"\\uD800\"", "parse-invalid-surrogate") && ok;

  {
    JsonParseResult parsed{};
    ok = expectParseSuccess("{\"a\":1,\"b\":[true,false,null,\"x\"]}", parsed, "roundtrip-input-parse") && ok;

    JsonStringifyResult compact{};
    ok = expectStringifySuccess(parsed.value, compact, "roundtrip-compact-stringify") && ok;

    JsonParseResult reparsed{};
    ok = expectParseSuccess(compact.jsonText, reparsed, "roundtrip-compact-reparse") && ok;
    ok = expect(jsonEquivalent(parsed.value, reparsed.value), "roundtrip-compact semantic equivalence") && ok;

    JsonStringifyOptions prettyOptions{};
    prettyOptions.pretty = true;
    prettyOptions.indentSize = 2;

    JsonStringifyResult pretty{};
    ok = expectStringifySuccess(parsed.value, pretty, "pretty-stringify", &prettyOptions) && ok;
    ok = expect(pretty.jsonText.find('\n') != std::string::npos, "pretty-stringify expected newlines") && ok;
    ok = expect(pretty.jsonText.find("  \"") != std::string::npos, "pretty-stringify expected indentation") && ok;

    JsonParseResult prettyReparsed{};
    ok = expectParseSuccess(pretty.jsonText, prettyReparsed, "pretty-reparse") && ok;
    ok = expect(jsonEquivalent(parsed.value, prettyReparsed.value), "pretty-roundtrip semantic equivalence") && ok;
  }

  if (!ok) {
    std::cerr << "[json-test] One or more tests failed." << '\n';
    return 1;
  }

  std::cout << "[json-test] All JSON tests passed." << '\n';
  return 0;
}
