#include "json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace reconclave::json {

Value Value::makeBool(bool value) {
  Value v;
  v.type_ = Type::Bool;
  v.bool_ = value;
  return v;
}

Value Value::makeNumber(double value) {
  Value v;
  v.type_ = Type::Number;
  v.number_ = value;
  return v;
}

Value Value::makeString(std::string value) {
  Value v;
  v.type_ = Type::String;
  v.string_ = std::move(value);
  return v;
}

Value Value::makeArray() {
  Value v;
  v.type_ = Type::Array;
  return v;
}

Value Value::makeObject() {
  Value v;
  v.type_ = Type::Object;
  return v;
}

void Value::set(std::string key, Value value) {
  type_ = Type::Object;
  for (auto& entry : object_) {
    if (entry.first == key) {
      entry.second = std::move(value);
      return;
    }
  }
  object_.emplace_back(std::move(key), std::move(value));
}

const Value* Value::find(const std::string& key) const {
  if (type_ != Type::Object) return nullptr;
  for (const auto& entry : object_) {
    if (entry.first == key) return &entry.second;
  }
  return nullptr;
}

void Value::push_back(Value value) {
  type_ = Type::Array;
  array_.push_back(std::move(value));
}

std::string Value::asString(const std::string& fallback) const {
  return type_ == Type::String ? string_ : fallback;
}

double Value::asNumber(double fallback) const {
  return type_ == Type::Number ? number_ : fallback;
}

std::uint64_t Value::asUInt64(std::uint64_t fallback) const {
  if (type_ != Type::Number || number_ < 0) return fallback;
  return static_cast<std::uint64_t>(number_);
}

bool Value::asBool(bool fallback) const {
  return type_ == Type::Bool ? bool_ : fallback;
}

namespace {

void appendEscaped(std::string& out, const std::string& value) {
  out.push_back('"');
  for (unsigned char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

void appendNumber(std::string& out, double value) {
  // Every current caller only ever puts integers (timestamps, sequence
  // numbers, ports, byte counts) into a Number, so prefer plain integer
  // formatting; fall back to a general float format for anything else
  // rather than assuming that will always hold.
  if (std::isfinite(value) && value == std::floor(value) &&
      std::fabs(value) < 1e15) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
    out += buf;
  } else {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", value);
    out += buf;
  }
}

void appendValue(std::string& out, const Value& value) {
  switch (value.type()) {
    case Value::Type::Null:
      out += "null";
      break;
    case Value::Type::Bool:
      out += value.asBool() ? "true" : "false";
      break;
    case Value::Type::Number:
      appendNumber(out, value.asNumber());
      break;
    case Value::Type::String:
      appendEscaped(out, value.asString());
      break;
    case Value::Type::Array: {
      out.push_back('[');
      bool first = true;
      for (const auto& item : value.items()) {
        if (!first) out.push_back(',');
        first = false;
        appendValue(out, item);
      }
      out.push_back(']');
      break;
    }
    case Value::Type::Object: {
      out.push_back('{');
      bool first = true;
      for (const auto& entry : value.entries()) {
        if (!first) out.push_back(',');
        first = false;
        appendEscaped(out, entry.first);
        out.push_back(':');
        appendValue(out, entry.second);
      }
      out.push_back('}');
      break;
    }
  }
}

}  // namespace

std::string stringify(const Value& value) {
  std::string out;
  appendValue(out, value);
  return out;
}

// Parser -----------------------------------------------------------------

namespace {

class Parser {
 public:
  explicit Parser(const std::string& text) : text_(text) {}

  bool parseValue(Value& out) {
    skipWhitespace();
    if (pos_ >= text_.size()) return fail("unexpected end of input");
    switch (text_[pos_]) {
      case '{': return parseObject(out);
      case '[': return parseArray(out);
      case '"': {
        std::string s;
        if (!parseString(s)) return false;
        out = Value::makeString(std::move(s));
        return true;
      }
      case 't':
        if (text_.compare(pos_, 4, "true") == 0) {
          pos_ += 4;
          out = Value::makeBool(true);
          return true;
        }
        return fail("invalid literal");
      case 'f':
        if (text_.compare(pos_, 5, "false") == 0) {
          pos_ += 5;
          out = Value::makeBool(false);
          return true;
        }
        return fail("invalid literal");
      case 'n':
        if (text_.compare(pos_, 4, "null") == 0) {
          pos_ += 4;
          out = Value::makeNull();
          return true;
        }
        return fail("invalid literal");
      default:
        return parseNumber(out);
    }
  }

  const std::string& error() const { return error_; }

 private:
  const std::string& text_;
  std::size_t pos_ = 0;
  std::string error_;

  bool fail(const std::string& message) {
    error_ = message + " at offset " + std::to_string(pos_);
    return false;
  }

  void skipWhitespace() {
    while (pos_ < text_.size()) {
      char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool parseObject(Value& out) {
    out = Value::makeObject();
    ++pos_;  // consume '{'
    skipWhitespace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      skipWhitespace();
      if (pos_ >= text_.size() || text_[pos_] != '"') return fail("expected object key");
      std::string key;
      if (!parseString(key)) return false;
      skipWhitespace();
      if (pos_ >= text_.size() || text_[pos_] != ':') return fail("expected ':'");
      ++pos_;
      Value value;
      if (!parseValue(value)) return false;
      out.set(std::move(key), std::move(value));
      skipWhitespace();
      if (pos_ >= text_.size()) return fail("unterminated object");
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }

  bool parseArray(Value& out) {
    out = Value::makeArray();
    ++pos_;  // consume '['
    skipWhitespace();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      Value value;
      if (!parseValue(value)) return false;
      out.push_back(std::move(value));
      skipWhitespace();
      if (pos_ >= text_.size()) return fail("unterminated array");
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  bool parseString(std::string& out) {
    ++pos_;  // consume opening quote
    out.clear();
    while (true) {
      if (pos_ >= text_.size()) return fail("unterminated string");
      char c = text_[pos_++];
      if (c == '"') return true;
      if (c == '\\') {
        if (pos_ >= text_.size()) return fail("unterminated escape");
        char esc = text_[pos_++];
        switch (esc) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'n': out.push_back('\n'); break;
          case 't': out.push_back('\t'); break;
          case 'r': out.push_back('\r'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'u': {
            if (pos_ + 4 > text_.size()) return fail("truncated unicode escape");
            unsigned code = 0;
            for (int i = 0; i < 4; ++i) {
              char h = text_[pos_++];
              code <<= 4;
              if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
              else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
              else return fail("invalid unicode escape");
            }
            // Encode as UTF-8. Surrogate pairs are not handled; every
            // string this protocol actually carries (identifiers,
            // capability names, messages) is ASCII, so this covers the
            // codepoints that occur in practice without pulling in a full
            // UTF-16-to-UTF-8 pairing implementation for no exercised case.
            if (code < 0x80) {
              out.push_back(static_cast<char>(code));
            } else if (code < 0x800) {
              out.push_back(static_cast<char>(0xC0 | (code >> 6)));
              out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            } else {
              out.push_back(static_cast<char>(0xE0 | (code >> 12)));
              out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
              out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            break;
          }
          default:
            return fail("invalid escape sequence");
        }
      } else {
        out.push_back(c);
      }
    }
  }

  bool parseNumber(Value& out) {
    std::size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }
    if (pos_ == start) return fail("invalid number");
    out = Value::makeNumber(std::strtod(text_.substr(start, pos_ - start).c_str(), nullptr));
    return true;
  }
};

}  // namespace

bool parse(const std::string& text, Value& out, std::string& error) {
  Parser parser(text);
  if (!parser.parseValue(out)) {
    error = parser.error();
    return false;
  }
  return true;
}

namespace {

Value canonicalValue(const Value& value, const std::string& omit_key) {
  if (value.isArray()) {
    Value result = Value::makeArray();
    for (const auto& item : value.items()) result.push_back(canonicalValue(item, omit_key));
    return result;
  }
  if (value.isObject()) {
    std::vector<std::pair<std::string, const Value*>> entries;
    for (const auto& entry : value.entries()) {
      if (!omit_key.empty() && entry.first == omit_key) continue;
      entries.push_back({entry.first, &entry.second});
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
      return left.first < right.first;
    });
    Value result = Value::makeObject();
    for (const auto& entry : entries) result.set(entry.first, canonicalValue(*entry.second, omit_key));
    return result;
  }
  return value;
}

}  // namespace

std::string canonicalStringify(const Value& value, const std::string& omit_key) {
  return stringify(canonicalValue(value, omit_key));
}

}  // namespace reconclave::json
