#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "tokmon/error.hpp"

namespace tokmon::cbor {

struct Value {
  using Bytes = std::vector<std::uint8_t>;
  using Array = std::vector<Value>;
  using Map = std::map<std::string, Value, std::less<>>;
  using Storage = std::variant<std::monostate, bool, std::int64_t, double,
                               std::string, Bytes, Array, Map>;

  Storage data;

  Value() = default;
  Value(std::nullptr_t) : data(std::monostate{}) {}
  Value(bool value) : data(value) {}
  Value(std::int32_t value) : data(static_cast<std::int64_t>(value)) {}
  Value(std::uint32_t value) : data(static_cast<std::int64_t>(value)) {}
  Value(std::int64_t value) : data(value) {}
  Value(double value) : data(value) {}
  Value(const char* value) : data(std::string(value)) {}
  Value(std::string value) : data(std::move(value)) {}
  Value(Bytes value) : data(std::move(value)) {}
  Value(Array value) : data(std::move(value)) {}
  Value(Map value) : data(std::move(value)) {}

  [[nodiscard]] bool is_null() const noexcept;
  [[nodiscard]] bool is_map() const noexcept;
  [[nodiscard]] bool is_array() const noexcept;
  [[nodiscard]] const Map* as_map() const noexcept;
  [[nodiscard]] Map* as_map() noexcept;
  [[nodiscard]] const Array* as_array() const noexcept;
  [[nodiscard]] std::string_view as_string(std::string_view fallback = {}) const noexcept;
  [[nodiscard]] std::int64_t as_integer(std::int64_t fallback = 0) const noexcept;
  [[nodiscard]] bool as_bool(bool fallback = false) const noexcept;
};

[[nodiscard]] std::vector<std::uint8_t> encode(const Value& value);
[[nodiscard]] Result<Value> decode(std::span<const std::uint8_t> bytes,
                                   std::size_t max_depth = 64);
[[nodiscard]] const Value* find(const Value& map, std::string_view key) noexcept;
[[nodiscard]] Value object(std::initializer_list<Value::Map::value_type> values);
[[nodiscard]] std::string diagnostic(const Value& value);

}  // namespace tokmon::cbor

