// Minimal JSON value type, parser, and serializer.
//
// common/protocol treats payloads as opaque strings (payload_json,
// arguments_json, ...) and leaves actual JSON encode/decode to each
// device's transport. This is the K230's transport-layer JSON support:
// deliberately small (flat objects/arrays, double-precision numbers) rather
// than a vendored general-purpose library, matching how little the
// `/reconclave/v1/announce` and `/reconclave/v1/message` contract actually
// needs (see tools/desktop-node/reconclave_node.py for the reference
// implementation this must interoperate with).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace reconclave::json {

class Value {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Value() = default;

  static Value makeNull() { return Value(); }
  static Value makeBool(bool value);
  static Value makeNumber(double value);
  static Value makeString(std::string value);
  static Value makeArray();
  static Value makeObject();

  Type type() const { return type_; }
  bool isObject() const { return type_ == Type::Object; }
  bool isArray() const { return type_ == Type::Array; }
  bool isString() const { return type_ == Type::String; }

  // Object helpers. set() replaces an existing key or appends, preserving
  // insertion order (JSON key order carries no semantic meaning here, but a
  // stable order keeps hand-inspection of serialized output sane).
  void set(std::string key, Value value);
  const Value* find(const std::string& key) const;

  // Array helper.
  void push_back(Value value);
  const std::vector<Value>& items() const { return array_; }

  // Object entries in insertion order, for serialization/iteration.
  const std::vector<std::pair<std::string, Value>>& entries() const { return object_; }

  // Scalar accessors with a fallback for a missing/mismatched field, so
  // callers do not need to null-check every lookup individually.
  std::string asString(const std::string& fallback = "") const;
  double asNumber(double fallback = 0.0) const;
  std::uint64_t asUInt64(std::uint64_t fallback = 0) const;
  bool asBool(bool fallback = false) const;

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  std::vector<Value> array_;
  std::vector<std::pair<std::string, Value>> object_;
};

std::string stringify(const Value& value);

// Stable compact JSON with object keys sorted lexicographically. `omit_key`
// is omitted at every object level when non-empty; scope-token signatures use
// this to canonicalise the unsigned object while excluding its `tag` member.
std::string canonicalStringify(const Value& value, const std::string& omit_key = "");

// Parses `text` into `out`. Returns false and leaves a description in
// `error` on malformed input; `out` is unspecified in that case.
bool parse(const std::string& text, Value& out, std::string& error);

}  // namespace reconclave::json
