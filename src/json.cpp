// Fabric Evolution — bounded, deterministic JSON value model (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>

namespace fabric::evolution {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

void append_escaped(std::string& out, const std::string& text) {
  out.push_back('"');
  for (char raw : text) {
    const unsigned char c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20U) {
          out += "\\u00";
          out.push_back(kHexDigits[(c >> 4U) & 0x0FU]);
          out.push_back(kHexDigits[c & 0x0FU]);
        } else {
          out.push_back(raw);
        }
        break;
    }
  }
  out.push_back('"');
}

void append_double(std::string& out, double value) {
  if (!std::isfinite(value)) {
    out += "null";
    return;
  }
  std::array<char, 64> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (result.ec != std::errc{}) {
    out += "null";
    return;
  }
  std::string_view text(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
  out.append(text);
  // Keep the value recognisable as a floating point literal in the canonical form.
  if (text.find_first_of(".eE") == std::string_view::npos) {
    out += ".0";
  }
}

void append_u64(std::string& out, std::uint64_t value) {
  std::array<char, 32> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (result.ec == std::errc{}) {
    out.append(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
  }
}

void append_i64(std::string& out, std::int64_t value) {
  std::array<char, 32> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (result.ec == std::errc{}) {
    out.append(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
  }
}

// ---------------------------------------------------------------------------
// Strict recursive-descent parser.
// ---------------------------------------------------------------------------
class Parser {
 public:
  Parser(std::string_view text, const JsonParseLimits& limits) : text_(text), limits_(limits) {}

  [[nodiscard]] JsonParseResult run() {
    JsonParseResult result;
    skip_whitespace();
    if (index_ >= text_.size()) {
      result.error = "empty input";
      result.error_offset = index_;
      return result;
    }
    auto value = parse_value(0);
    if (!value.has_value()) {
      result.error = error_;
      result.error_offset = error_offset_;
      return result;
    }
    skip_whitespace();
    if (index_ != text_.size()) {
      result.error = "trailing content after top-level value";
      result.error_offset = index_;
      return result;
    }
    result.value = std::move(value);
    return result;
  }

 private:
  void skip_whitespace() noexcept {
    while (index_ < text_.size()) {
      const char c = text_[index_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++index_;
      } else {
        break;
      }
    }
  }

  bool fail(std::string message) {
    if (error_.empty()) {
      error_ = std::move(message);
      error_offset_ = index_;
    }
    return false;
  }

  [[nodiscard]] std::optional<Json> parse_value(std::size_t depth) {
    if (depth > limits_.max_depth) {
      fail("maximum nesting depth exceeded");
      return std::nullopt;
    }
    if (index_ >= text_.size()) {
      fail("unexpected end of input");
      return std::nullopt;
    }
    switch (text_[index_]) {
      case '{':
        return parse_object(depth);
      case '[':
        return parse_array(depth);
      case '"': {
        auto text = parse_string();
        if (!text.has_value()) {
          return std::nullopt;
        }
        return Json(std::move(*text));
      }
      case 't':
        if (literal("true")) {
          return Json(true);
        }
        return std::nullopt;
      case 'f':
        if (literal("false")) {
          return Json(false);
        }
        return std::nullopt;
      case 'n':
        if (literal("null")) {
          return Json(nullptr);
        }
        return std::nullopt;
      default:
        return parse_number();
    }
  }

  [[nodiscard]] bool literal(std::string_view word) {
    if (text_.compare(index_, word.size(), word) != 0) {
      fail("invalid literal");
      return false;
    }
    index_ += word.size();
    return true;
  }

  [[nodiscard]] std::optional<Json> parse_object(std::size_t depth) {
    ++index_;  // consume '{'
    Json::Object object;
    skip_whitespace();
    if (index_ < text_.size() && text_[index_] == '}') {
      ++index_;
      return Json(std::move(object));
    }
    while (true) {
      skip_whitespace();
      if (index_ >= text_.size() || text_[index_] != '"') {
        fail("expected object key string");
        return std::nullopt;
      }
      auto key = parse_string();
      if (!key.has_value()) {
        return std::nullopt;
      }
      if (object.size() >= limits_.max_container_entries) {
        fail("object entry limit exceeded");
        return std::nullopt;
      }
      if (object.find(*key) != object.end()) {
        fail("duplicate object key: " + *key);
        return std::nullopt;
      }
      skip_whitespace();
      if (index_ >= text_.size() || text_[index_] != ':') {
        fail("expected ':' after object key");
        return std::nullopt;
      }
      ++index_;
      skip_whitespace();
      auto value = parse_value(depth + 1);
      if (!value.has_value()) {
        return std::nullopt;
      }
      object.emplace(std::move(*key), std::move(*value));
      skip_whitespace();
      if (index_ >= text_.size()) {
        fail("unterminated object");
        return std::nullopt;
      }
      if (text_[index_] == ',') {
        ++index_;
        continue;
      }
      if (text_[index_] == '}') {
        ++index_;
        return Json(std::move(object));
      }
      fail("expected ',' or '}' in object");
      return std::nullopt;
    }
  }

  [[nodiscard]] std::optional<Json> parse_array(std::size_t depth) {
    ++index_;  // consume '['
    Json::Array array;
    skip_whitespace();
    if (index_ < text_.size() && text_[index_] == ']') {
      ++index_;
      return Json(std::move(array));
    }
    while (true) {
      skip_whitespace();
      if (array.size() >= limits_.max_container_entries) {
        fail("array entry limit exceeded");
        return std::nullopt;
      }
      auto value = parse_value(depth + 1);
      if (!value.has_value()) {
        return std::nullopt;
      }
      array.push_back(std::move(*value));
      skip_whitespace();
      if (index_ >= text_.size()) {
        fail("unterminated array");
        return std::nullopt;
      }
      if (text_[index_] == ',') {
        ++index_;
        continue;
      }
      if (text_[index_] == ']') {
        ++index_;
        return Json(std::move(array));
      }
      fail("expected ',' or ']' in array");
      return std::nullopt;
    }
  }

  void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point <= 0x7FU) {
      out.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FFU) {
      out.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
      out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point <= 0xFFFFU) {
      out.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
      out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
      out.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
      out.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
  }

  [[nodiscard]] std::optional<std::uint32_t> parse_hex4() {
    if (index_ + 4 > text_.size()) {
      fail("truncated \\u escape");
      return std::nullopt;
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[index_ + static_cast<std::size_t>(i)];
      int digit = -1;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      }
      if (digit < 0) {
        fail("invalid hex digit in \\u escape");
        return std::nullopt;
      }
      value = (value << 4U) | static_cast<std::uint32_t>(digit);
    }
    index_ += 4;
    return value;
  }

  [[nodiscard]] std::optional<std::string> parse_string() {
    ++index_;  // consume opening quote
    std::string out;
    while (true) {
      if (index_ >= text_.size()) {
        fail("unterminated string");
        return std::nullopt;
      }
      const unsigned char c = static_cast<unsigned char>(text_[index_]);
      if (c == '"') {
        ++index_;
        return out;
      }
      if (c < 0x20U) {
        fail("unescaped control character in string");
        return std::nullopt;
      }
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        ++index_;
      } else {
        ++index_;
        if (index_ >= text_.size()) {
          fail("unterminated escape sequence");
          return std::nullopt;
        }
        const char escape = text_[index_];
        ++index_;
        switch (escape) {
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
            auto first = parse_hex4();
            if (!first.has_value()) {
              return std::nullopt;
            }
            std::uint32_t code_point = *first;
            if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
              if (index_ + 1 >= text_.size() || text_[index_] != '\\' || text_[index_ + 1] != 'u') {
                fail("unpaired high surrogate");
                return std::nullopt;
              }
              index_ += 2;
              auto second = parse_hex4();
              if (!second.has_value()) {
                return std::nullopt;
              }
              if (*second < 0xDC00U || *second > 0xDFFFU) {
                fail("invalid low surrogate");
                return std::nullopt;
              }
              code_point = 0x10000U + ((code_point - 0xD800U) << 10U) + (*second - 0xDC00U);
            } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
              fail("unpaired low surrogate");
              return std::nullopt;
            }
            append_utf8(out, code_point);
            break;
          }
          default:
            fail("invalid escape sequence");
            return std::nullopt;
        }
      }
      if (out.size() > limits_.max_string_bytes) {
        fail("string length limit exceeded");
        return std::nullopt;
      }
    }
  }

  [[nodiscard]] std::optional<Json> parse_number() {
    const std::size_t start = index_;
    if (index_ < text_.size() && text_[index_] == '-') {
      ++index_;
    }
    if (index_ >= text_.size() || text_[index_] < '0' || text_[index_] > '9') {
      fail("invalid number");
      return std::nullopt;
    }
    if (text_[index_] == '0') {
      ++index_;
      if (index_ < text_.size() && text_[index_] >= '0' && text_[index_] <= '9') {
        fail("leading zeros are not permitted");
        return std::nullopt;
      }
    } else {
      while (index_ < text_.size() && text_[index_] >= '0' && text_[index_] <= '9') {
        ++index_;
      }
    }
    bool integral = true;
    if (index_ < text_.size() && text_[index_] == '.') {
      integral = false;
      ++index_;
      if (index_ >= text_.size() || text_[index_] < '0' || text_[index_] > '9') {
        fail("fraction requires at least one digit");
        return std::nullopt;
      }
      while (index_ < text_.size() && text_[index_] >= '0' && text_[index_] <= '9') {
        ++index_;
      }
    }
    if (index_ < text_.size() && (text_[index_] == 'e' || text_[index_] == 'E')) {
      integral = false;
      ++index_;
      if (index_ < text_.size() && (text_[index_] == '+' || text_[index_] == '-')) {
        ++index_;
      }
      if (index_ >= text_.size() || text_[index_] < '0' || text_[index_] > '9') {
        fail("exponent requires at least one digit");
        return std::nullopt;
      }
      while (index_ < text_.size() && text_[index_] >= '0' && text_[index_] <= '9') {
        ++index_;
      }
    }

    const std::string_view token = text_.substr(start, index_ - start);
    if (integral) {
      if (!token.empty() && token.front() == '-') {
        std::int64_t value = 0;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec == std::errc{} && result.ptr == token.data() + token.size()) {
          return Json(value);
        }
      } else {
        std::uint64_t value = 0;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec == std::errc{} && result.ptr == token.data() + token.size()) {
          return Json(value);
        }
      }
    }

    // Fall back to a double, matching strtod semantics but without locale
    // sensitivity: the token grammar has already been validated above.
    std::string buffer(token);
    double value = 0.0;
    const auto result = std::from_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{} || result.ptr != buffer.data() + buffer.size()) {
      fail("number out of representable range");
      return std::nullopt;
    }
    return Json(value);
  }

  std::string_view text_;
  const JsonParseLimits& limits_;
  std::size_t index_ = 0;
  std::string error_;
  std::size_t error_offset_ = 0;
};

}  // namespace

Json::Json() noexcept : value_(nullptr) {}
Json::Json(std::nullptr_t) noexcept : value_(nullptr) {}
Json::Json(bool value) noexcept : value_(value) {}
Json::Json(int value) noexcept : value_(static_cast<std::int64_t>(value)) {}
Json::Json(long value) noexcept : value_(static_cast<std::int64_t>(value)) {}
Json::Json(long long value) noexcept : value_(static_cast<std::int64_t>(value)) {}
Json::Json(unsigned int value) noexcept : value_(static_cast<std::uint64_t>(value)) {}
Json::Json(unsigned long value) noexcept : value_(static_cast<std::uint64_t>(value)) {}
Json::Json(unsigned long long value) noexcept : value_(static_cast<std::uint64_t>(value)) {}
Json::Json(double value) noexcept : value_(value) {}
Json::Json(const char* value) : value_(std::string(value == nullptr ? "" : value)) {}
Json::Json(std::string value) : value_(std::move(value)) {}
Json::Json(std::string_view value) : value_(std::string(value)) {}
Json::Json(Array value) : value_(std::move(value)) {}
Json::Json(Object value) : value_(std::move(value)) {}

Json Json::array() { return Json(Array{}); }

Json Json::array(std::initializer_list<Json> values) {
  Array result;
  result.reserve(values.size());
  for (const Json& value : values) {
    result.push_back(value);
  }
  return Json(std::move(result));
}

Json Json::object() { return Json(Object{}); }

Json Json::object(std::initializer_list<std::pair<const std::string, Json>> values) {
  Object result;
  for (const auto& entry : values) {
    result.insert_or_assign(entry.first, entry.second);
  }
  return Json(std::move(result));
}

bool Json::is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(value_); }
bool Json::is_bool() const noexcept { return std::holds_alternative<bool>(value_); }
bool Json::is_int() const noexcept { return std::holds_alternative<std::int64_t>(value_); }
bool Json::is_uint() const noexcept { return std::holds_alternative<std::uint64_t>(value_); }
bool Json::is_number() const noexcept {
  return is_int() || is_uint() || std::holds_alternative<double>(value_);
}
bool Json::is_string() const noexcept { return std::holds_alternative<std::string>(value_); }
bool Json::is_array() const noexcept { return std::holds_alternative<Array>(value_); }
bool Json::is_object() const noexcept { return std::holds_alternative<Object>(value_); }

std::optional<bool> Json::try_bool() const noexcept {
  if (const bool* value = std::get_if<bool>(&value_)) {
    return *value;
  }
  return std::nullopt;
}

std::optional<std::int64_t> Json::try_i64() const noexcept {
  if (const std::int64_t* value = std::get_if<std::int64_t>(&value_)) {
    return *value;
  }
  if (const std::uint64_t* value = std::get_if<std::uint64_t>(&value_)) {
    if (*value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return static_cast<std::int64_t>(*value);
    }
  }
  return std::nullopt;
}

std::optional<std::uint64_t> Json::try_u64() const noexcept {
  if (const std::uint64_t* value = std::get_if<std::uint64_t>(&value_)) {
    return *value;
  }
  if (const std::int64_t* value = std::get_if<std::int64_t>(&value_)) {
    if (*value >= 0) {
      return static_cast<std::uint64_t>(*value);
    }
  }
  return std::nullopt;
}

std::optional<double> Json::try_f64() const noexcept {
  if (const double* value = std::get_if<double>(&value_)) {
    return *value;
  }
  if (const std::int64_t* value = std::get_if<std::int64_t>(&value_)) {
    return static_cast<double>(*value);
  }
  if (const std::uint64_t* value = std::get_if<std::uint64_t>(&value_)) {
    return static_cast<double>(*value);
  }
  return std::nullopt;
}

const std::string* Json::try_string() const noexcept { return std::get_if<std::string>(&value_); }
const Json::Array* Json::try_array() const noexcept { return std::get_if<Array>(&value_); }
Json::Array* Json::try_array() noexcept { return std::get_if<Array>(&value_); }
const Json::Object* Json::try_object() const noexcept { return std::get_if<Object>(&value_); }
Json::Object* Json::try_object() noexcept { return std::get_if<Object>(&value_); }

const Json* Json::find(std::string_view key) const noexcept {
  const Object* object = try_object();
  if (object == nullptr) {
    return nullptr;
  }
  const auto it = object->find(key);
  return it == object->end() ? nullptr : &it->second;
}

bool Json::has(std::string_view key) const noexcept { return find(key) != nullptr; }

Json& Json::set(std::string key, Json value) {
  if (!is_object()) {
    value_ = Object{};
  }
  Object& object = std::get<Object>(value_);
  auto [it, inserted] = object.insert_or_assign(std::move(key), std::move(value));
  (void)inserted;
  return it->second;
}

void Json::erase(std::string_view key) {
  Object* object = try_object();
  if (object != nullptr) {
    object->erase(std::string(key));
  }
}

std::size_t Json::size() const noexcept {
  if (const Array* array = try_array()) {
    return array->size();
  }
  if (const Object* object = try_object()) {
    return object->size();
  }
  if (const std::string* text = try_string()) {
    return text->size();
  }
  return 0;
}

const Json& Json::at(std::size_t index) const {
  static const Json kNull;
  const Array* array = try_array();
  if (array == nullptr || index >= array->size()) {
    return kNull;
  }
  return (*array)[index];
}

Json& Json::push_back(Json value) {
  if (!is_array()) {
    value_ = Array{};
  }
  Array& array = std::get<Array>(value_);
  array.push_back(std::move(value));
  return array.back();
}

void Json::dump_value(std::string& out, bool pretty, int indent) const {
  const std::string pad(pretty ? static_cast<std::size_t>(indent) * 2U : 0U, ' ');
  const std::string pad_inner(pretty ? static_cast<std::size_t>(indent + 1) * 2U : 0U, ' ');
  if (is_null()) {
    out += "null";
  } else if (const bool* boolean = std::get_if<bool>(&value_)) {
    out += *boolean ? "true" : "false";
  } else if (const std::int64_t* integer = std::get_if<std::int64_t>(&value_)) {
    append_i64(out, *integer);
  } else if (const std::uint64_t* unsigned_integer = std::get_if<std::uint64_t>(&value_)) {
    append_u64(out, *unsigned_integer);
  } else if (const double* real = std::get_if<double>(&value_)) {
    append_double(out, *real);
  } else if (const std::string* text = std::get_if<std::string>(&value_)) {
    append_escaped(out, *text);
  } else if (const Array* array = std::get_if<Array>(&value_)) {
    if (array->empty()) {
      out += "[]";
      return;
    }
    out.push_back('[');
    bool first = true;
    for (const Json& element : *array) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      if (pretty) {
        out.push_back('\n');
        out += pad_inner;
      }
      element.dump_value(out, pretty, indent + 1);
    }
    if (pretty) {
      out.push_back('\n');
      out += pad;
    }
    out.push_back(']');
  } else if (const Object* object = std::get_if<Object>(&value_)) {
    if (object->empty()) {
      out += "{}";
      return;
    }
    out.push_back('{');
    bool first = true;
    for (const auto& entry : *object) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      if (pretty) {
        out.push_back('\n');
        out += pad_inner;
      }
      append_escaped(out, entry.first);
      out.push_back(':');
      if (pretty) {
        out.push_back(' ');
      }
      entry.second.dump_value(out, pretty, indent + 1);
    }
    if (pretty) {
      out.push_back('\n');
      out += pad;
    }
    out.push_back('}');
  }
}

void Json::dump_to(std::string& out) const { dump_value(out, false, 0); }

std::string Json::dump() const {
  std::string out;
  dump_to(out);
  return out;
}

std::string Json::dump_pretty() const {
  std::string out;
  dump_value(out, true, 0);
  return out;
}

bool operator==(const Json& lhs, const Json& rhs) noexcept {
  if (lhs.value_.index() != rhs.value_.index()) {
    // Integers compare across the signed/unsigned split when both are integral.
    const auto left_int = lhs.try_i64();
    const auto right_int = rhs.try_i64();
    if (lhs.is_number() && rhs.is_number() && left_int.has_value() && right_int.has_value()) {
      return *left_int == *right_int;
    }
    return false;
  }
  return lhs.value_ == rhs.value_;
}

JsonParseResult parse_json(std::string_view text) { return parse_json(text, JsonParseLimits{}); }

JsonParseResult parse_json(std::string_view text, const JsonParseLimits& limits) {
  JsonParseResult result;
  if (text.size() > limits.max_bytes) {
    result.error = "input exceeds configured byte limit";
    return result;
  }
  Parser parser(text, limits);
  return parser.run();
}

std::optional<std::string> json_string(const Json& object, std::string_view key) {
  const Json* value = object.find(key);
  if (value == nullptr) {
    return std::nullopt;
  }
  if (const std::string* text = value->try_string()) {
    return *text;
  }
  return std::nullopt;
}

std::optional<std::int64_t> json_i64(const Json& object, std::string_view key) {
  const Json* value = object.find(key);
  return value == nullptr ? std::nullopt : value->try_i64();
}

std::optional<std::uint64_t> json_u64(const Json& object, std::string_view key) {
  const Json* value = object.find(key);
  return value == nullptr ? std::nullopt : value->try_u64();
}

std::optional<bool> json_bool(const Json& object, std::string_view key) {
  const Json* value = object.find(key);
  return value == nullptr ? std::nullopt : value->try_bool();
}

std::string json_string_or(const Json& object, std::string_view key, std::string fallback) {
  const auto value = json_string(object, key);
  return value.has_value() ? *value : std::move(fallback);
}

std::int64_t json_i64_or(const Json& object, std::string_view key, std::int64_t fallback) {
  const auto value = json_i64(object, key);
  return value.has_value() ? *value : fallback;
}

std::uint64_t json_u64_or(const Json& object, std::string_view key, std::uint64_t fallback) {
  const auto value = json_u64(object, key);
  return value.has_value() ? *value : fallback;
}

bool json_bool_or(const Json& object, std::string_view key, bool fallback) {
  const auto value = json_bool(object, key);
  return value.has_value() ? *value : fallback;
}

}  // namespace fabric::evolution
