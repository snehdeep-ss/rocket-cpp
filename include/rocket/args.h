#ifndef ROCKET_ARGS_H_
#define ROCKET_ARGS_H_

#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <version>

namespace rocket {
namespace internal {

static_assert(std::numeric_limits<double>::is_iec559 &&
                  std::numeric_limits<float>::is_iec559,
              "rocket-cpp encodes floating point values as IEEE 754");

template <typename T>
void Append(std::string& out, const T& value) {
  if constexpr (std::is_convertible_v<const T&, std::string_view>) {
    out.append(std::string_view(value));
  } else if constexpr (std::is_same_v<T, char>) {
    out.push_back(value);
  } else if constexpr (std::is_same_v<T, bool>) {
    out.append(value ? "true" : "false");
  } else if constexpr (std::is_integral_v<T>) {
    char buffer[24];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
#if defined(__cpp_lib_to_chars)
  } else if constexpr (std::is_floating_point_v<T>) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
#endif
  } else if constexpr (std::is_enum_v<T>) {
    Append(out, static_cast<std::underlying_type_t<T>>(value));
  } else {
    std::ostringstream stream;
    stream << value;
    out.append(stream.str());
  }
}

inline void PutVarint(std::string& out, uint64_t value) {
  while (value >= 0x80) {
    out.push_back(static_cast<char>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<char>(value));
}

inline bool GetVarint(std::string_view& in, uint64_t& value) {
  value = 0;
  for (int shift = 0; shift < 64 && !in.empty(); shift += 7) {
    const auto byte = static_cast<uint8_t>(in.front());
    in.remove_prefix(1);
    value |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) return true;
  }
  return false;
}

inline uint64_t ZigZagEncode(int64_t value) {
  return (static_cast<uint64_t>(value) << 1) ^
         static_cast<uint64_t>(value >> 63);
}

inline int64_t ZigZagDecode(uint64_t value) {
  return static_cast<int64_t>(value >> 1) ^ -static_cast<int64_t>(value & 1);
}

enum class ArgTag : uint8_t {
  kString,
  kChar,
  kBool,
  kSigned,
  kUnsigned,
  kFloat,
  kDouble
};

template <typename Bits>
void PutFixed(std::string& out, Bits bits) {
  for (size_t i = 0; i < sizeof(Bits); ++i) {
    out.push_back(static_cast<char>((bits >> (8 * i)) & 0xff));
  }
}

template <typename Bits>
bool GetFixed(std::string_view& in, Bits& bits) {
  if (in.size() < sizeof(Bits)) return false;
  bits = 0;
  for (size_t i = 0; i < sizeof(Bits); ++i) {
    bits |= static_cast<Bits>(static_cast<uint8_t>(in[i])) << (8 * i);
  }
  in.remove_prefix(sizeof(Bits));
  return true;
}

template <typename T>
void Capture(std::string& out, const T& value) {
  if constexpr (std::is_convertible_v<const T&, std::string_view>) {
    const std::string_view text(value);
    out.push_back(static_cast<char>(ArgTag::kString));
    PutVarint(out, text.size());
    out.append(text);
  } else if constexpr (std::is_same_v<T, char>) {
    out.push_back(static_cast<char>(ArgTag::kChar));
    out.push_back(value);
  } else if constexpr (std::is_same_v<T, bool>) {
    out.push_back(static_cast<char>(ArgTag::kBool));
    out.push_back(value ? 1 : 0);
  } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
    out.push_back(static_cast<char>(ArgTag::kSigned));
    PutVarint(out, ZigZagEncode(value));
  } else if constexpr (std::is_integral_v<T>) {
    out.push_back(static_cast<char>(ArgTag::kUnsigned));
    PutVarint(out, value);
  } else if constexpr (std::is_enum_v<T>) {
    Capture(out, static_cast<std::underlying_type_t<T>>(value));
  } else if constexpr (std::is_same_v<T, float>) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    out.push_back(static_cast<char>(ArgTag::kFloat));
    PutFixed(out, bits);
  } else if constexpr (std::is_same_v<T, double>) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    out.push_back(static_cast<char>(ArgTag::kDouble));
    PutFixed(out, bits);
  } else {
    std::string text;
    Append(text, value);
    Capture(out, std::string_view(text));
  }
}

inline bool RenderArgs(std::string_view args, std::string& out) {
  while (!args.empty()) {
    const auto tag = static_cast<ArgTag>(args.front());
    args.remove_prefix(1);
    uint64_t number = 0;
    switch (tag) {
      case ArgTag::kString:
        if (!GetVarint(args, number) || number > args.size()) return false;
        out.append(args.substr(0, number));
        args.remove_prefix(number);
        break;
      case ArgTag::kChar:
        if (args.empty()) return false;
        out.push_back(args.front());
        args.remove_prefix(1);
        break;
      case ArgTag::kBool:
        if (args.empty()) return false;
        Append(out, args.front() != 0);
        args.remove_prefix(1);
        break;
      case ArgTag::kSigned:
        if (!GetVarint(args, number)) return false;
        Append(out, ZigZagDecode(number));
        break;
      case ArgTag::kUnsigned:
        if (!GetVarint(args, number)) return false;
        Append(out, number);
        break;
      case ArgTag::kFloat: {
        uint32_t bits = 0;
        if (!GetFixed(args, bits)) return false;
        float value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        Append(out, value);
        break;
      }
      case ArgTag::kDouble: {
        uint64_t bits = 0;
        if (!GetFixed(args, bits)) return false;
        double value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        Append(out, value);
        break;
      }
      default:
        return false;
    }
  }
  return true;
}

}  // namespace internal
}  // namespace rocket

#endif  // ROCKET_ARGS_H_
