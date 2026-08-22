#include "tokmon/cbor.hpp"
#include "tokmon/hash.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace tokmon::cbor {
namespace {

void write_argument(std::vector<std::uint8_t>& output, const std::uint8_t major,
                    const std::uint64_t value) {
  const auto prefix = static_cast<std::uint8_t>(major << 5u);
  if (value < 24u) {
    output.push_back(static_cast<std::uint8_t>(prefix | value));
  } else if (value <= 0xffu) {
    output.push_back(static_cast<std::uint8_t>(prefix | 24u));
    output.push_back(static_cast<std::uint8_t>(value));
  } else if (value <= 0xffffu) {
    output.push_back(static_cast<std::uint8_t>(prefix | 25u));
    output.push_back(static_cast<std::uint8_t>(value >> 8u));
    output.push_back(static_cast<std::uint8_t>(value));
  } else if (value <= 0xffffffffu) {
    output.push_back(static_cast<std::uint8_t>(prefix | 26u));
    for (int shift = 24; shift >= 0; shift -= 8)
      output.push_back(static_cast<std::uint8_t>(value >> shift));
  } else {
    output.push_back(static_cast<std::uint8_t>(prefix | 27u));
    for (int shift = 56; shift >= 0; shift -= 8)
      output.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void encode_value(std::vector<std::uint8_t>& output, const Value& value) {
  if (std::holds_alternative<std::monostate>(value.data)) {
    output.push_back(0xf6u);
  } else if (const auto* boolean = std::get_if<bool>(&value.data)) {
    output.push_back(*boolean ? 0xf5u : 0xf4u);
  } else if (const auto* integer = std::get_if<std::int64_t>(&value.data)) {
    if (*integer >= 0) write_argument(output, 0u, static_cast<std::uint64_t>(*integer));
    else write_argument(output, 1u, static_cast<std::uint64_t>(-1 - *integer));
  } else if (const auto* number = std::get_if<double>(&value.data)) {
    output.push_back(0xfbu);
    const auto bits = std::bit_cast<std::uint64_t>(*number);
    for (int shift = 56; shift >= 0; shift -= 8)
      output.push_back(static_cast<std::uint8_t>(bits >> shift));
  } else if (const auto* text = std::get_if<std::string>(&value.data)) {
    write_argument(output, 3u, text->size());
    output.insert(output.end(), text->begin(), text->end());
  } else if (const auto* bytes = std::get_if<Value::Bytes>(&value.data)) {
    write_argument(output, 2u, bytes->size());
    output.insert(output.end(), bytes->begin(), bytes->end());
  } else if (const auto* array = std::get_if<Value::Array>(&value.data)) {
    write_argument(output, 4u, array->size());
    for (const auto& item : *array) encode_value(output, item);
  } else if (const auto* map = std::get_if<Value::Map>(&value.data)) {
    write_argument(output, 5u, map->size());
    std::vector<const Value::Map::value_type*> ordered;
    ordered.reserve(map->size());
    for (const auto& entry : *map) ordered.push_back(&entry);
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
      if (left->first.size() != right->first.size())
        return left->first.size() < right->first.size();
      return left->first < right->first;
    });
    for (const auto* entry : ordered) {
      encode_value(output, Value(entry->first));
      encode_value(output, entry->second);
    }
  }
}

class Decoder {
 public:
  Decoder(const std::span<const std::uint8_t> bytes, const std::size_t max_depth)
      : bytes_(bytes), max_depth_(max_depth) {}

  Result<Value> decode() {
    auto value = read(0);
    if (!value) return value;
    if (cursor_ != bytes_.size())
      return tl::unexpected(make_error(ErrorCode::protocol_error,
                                       "trailing bytes after CBOR value"));
    return value;
  }

 private:
  Result<std::uint64_t> argument(const std::uint8_t additional) {
    if (additional < 24u) return additional;
    std::size_t count = 0;
    if (additional == 24u) count = 1;
    else if (additional == 25u) count = 2;
    else if (additional == 26u) count = 4;
    else if (additional == 27u) count = 8;
    else return tl::unexpected(make_error(ErrorCode::protocol_error,
                                          "indefinite or reserved CBOR length"));
    if (cursor_ + count > bytes_.size())
      return tl::unexpected(make_error(ErrorCode::protocol_error,
                                       "truncated CBOR argument"));
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < count; ++index)
      value = (value << 8u) | bytes_[cursor_++];
    return value;
  }

  Result<Value> read(const std::size_t depth) {
    if (depth > max_depth_)
      return tl::unexpected(make_error(ErrorCode::protocol_error,
                                       "CBOR nesting limit exceeded"));
    if (cursor_ >= bytes_.size())
      return tl::unexpected(make_error(ErrorCode::protocol_error,
                                       "truncated CBOR value"));
    const auto initial = bytes_[cursor_++];
    const auto major = static_cast<std::uint8_t>(initial >> 5u);
    const auto additional = static_cast<std::uint8_t>(initial & 0x1fu);

    if (major <= 5u) {
      auto length = argument(additional);
      if (!length) return tl::unexpected(length.error());
      if (major == 0u) {
        if (*length > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
          return tl::unexpected(make_error(ErrorCode::protocol_error,
                                           "CBOR integer exceeds int64"));
        return Value(static_cast<std::int64_t>(*length));
      }
      if (major == 1u) {
        if (*length > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
          return tl::unexpected(make_error(ErrorCode::protocol_error,
                                           "CBOR negative integer exceeds int64"));
        return Value(-1 - static_cast<std::int64_t>(*length));
      }
      if (major == 2u || major == 3u) {
        if (*length > bytes_.size() - cursor_)
          return tl::unexpected(make_error(ErrorCode::protocol_error,
                                           "truncated CBOR string"));
        if (major == 2u) {
          Value::Bytes bytes(bytes_.begin() + static_cast<std::ptrdiff_t>(cursor_),
                             bytes_.begin() + static_cast<std::ptrdiff_t>(cursor_ + *length));
          cursor_ += static_cast<std::size_t>(*length);
          return Value(std::move(bytes));
        }
        std::string text(reinterpret_cast<const char*>(bytes_.data() + cursor_),
                         static_cast<std::size_t>(*length));
        cursor_ += static_cast<std::size_t>(*length);
        return Value(std::move(text));
      }
      if (major == 4u) {
        Value::Array array;
        if (*length > 1'000'000u)
          return tl::unexpected(make_error(ErrorCode::protocol_error,
                                           "CBOR array limit exceeded"));
        array.reserve(static_cast<std::size_t>(*length));
        for (std::uint64_t index = 0; index < *length; ++index) {
          auto item = read(depth + 1u);
          if (!item) return item;
          array.push_back(std::move(*item));
        }
        return Value(std::move(array));
      }
      Value::Map map;
      if (*length > 1'000'000u)
        return tl::unexpected(make_error(ErrorCode::protocol_error,
                                         "CBOR map limit exceeded"));
      for (std::uint64_t index = 0; index < *length; ++index) {
        auto key = read(depth + 1u);
        if (!key) return key;
        const auto* text = std::get_if<std::string>(&key->data);
        if (text == nullptr)
          return tl::unexpected(make_error(ErrorCode::protocol_error,
                                           "CBOR map key must be text"));
        auto item = read(depth + 1u);
        if (!item) return item;
        if (!map.emplace(*text, std::move(*item)).second)
          return tl::unexpected(make_error(ErrorCode::protocol_error,
                                           "duplicate CBOR map key"));
      }
      return Value(std::move(map));
    }

    if (major == 7u) {
      if (additional == 20u) return Value(false);
      if (additional == 21u) return Value(true);
      if (additional == 22u) return Value(nullptr);
      if (additional == 27u) {
        if (cursor_ + 8u > bytes_.size())
          return tl::unexpected(make_error(ErrorCode::protocol_error,
                                           "truncated CBOR float"));
        std::uint64_t bits = 0;
        for (int index = 0; index < 8; ++index) bits = (bits << 8u) | bytes_[cursor_++];
        return Value(std::bit_cast<double>(bits));
      }
    }
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "unsupported CBOR type"));
  }

  std::span<const std::uint8_t> bytes_;
  std::size_t cursor_{0};
  std::size_t max_depth_;
};

void print(std::ostringstream& stream, const Value& value) {
  if (std::holds_alternative<std::monostate>(value.data)) stream << "null";
  else if (const auto* boolean = std::get_if<bool>(&value.data)) stream << (*boolean ? "true" : "false");
  else if (const auto* integer = std::get_if<std::int64_t>(&value.data)) stream << *integer;
  else if (const auto* number = std::get_if<double>(&value.data)) stream << *number;
  else if (const auto* text = std::get_if<std::string>(&value.data)) stream << std::quoted(*text);
  else if (const auto* bytes = std::get_if<Value::Bytes>(&value.data)) stream << "h'" << hex(*bytes) << "'";
  else if (const auto* array = std::get_if<Value::Array>(&value.data)) {
    stream << '[';
    for (std::size_t i = 0; i < array->size(); ++i) { if (i) stream << ','; print(stream, (*array)[i]); }
    stream << ']';
  } else if (const auto* map = std::get_if<Value::Map>(&value.data)) {
    stream << '{';
    bool first = true;
    for (const auto& [key, item] : *map) { if (!first) stream << ','; first = false; stream << std::quoted(key) << ':'; print(stream, item); }
    stream << '}';
  }
}

}  // namespace

bool Value::is_null() const noexcept { return std::holds_alternative<std::monostate>(data); }
bool Value::is_map() const noexcept { return std::holds_alternative<Map>(data); }
bool Value::is_array() const noexcept { return std::holds_alternative<Array>(data); }
const Value::Map* Value::as_map() const noexcept { return std::get_if<Map>(&data); }
Value::Map* Value::as_map() noexcept { return std::get_if<Map>(&data); }
const Value::Array* Value::as_array() const noexcept { return std::get_if<Array>(&data); }
std::string_view Value::as_string(const std::string_view fallback) const noexcept {
  if (const auto* value = std::get_if<std::string>(&data)) return *value;
  return fallback;
}
std::int64_t Value::as_integer(const std::int64_t fallback) const noexcept {
  if (const auto* value = std::get_if<std::int64_t>(&data)) return *value;
  return fallback;
}
bool Value::as_bool(const bool fallback) const noexcept {
  if (const auto* value = std::get_if<bool>(&data)) return *value;
  return fallback;
}

std::vector<std::uint8_t> encode(const Value& value) {
  std::vector<std::uint8_t> output;
  output.reserve(256);
  encode_value(output, value);
  return output;
}

Result<Value> decode(const std::span<const std::uint8_t> bytes,
                     const std::size_t max_depth) {
  return Decoder(bytes, max_depth).decode();
}

const Value* find(const Value& map, const std::string_view key) noexcept {
  const auto* object = map.as_map();
  if (object == nullptr) return nullptr;
  const auto iterator = object->find(key);
  return iterator == object->end() ? nullptr : &iterator->second;
}

Value object(const std::initializer_list<Value::Map::value_type> values) {
  return Value(Value::Map(values));
}

std::string diagnostic(const Value& value) {
  std::ostringstream stream;
  print(stream, value);
  return stream.str();
}

}  // namespace tokmon::cbor
