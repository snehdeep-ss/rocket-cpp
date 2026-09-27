#ifndef ROCKET_LEVEL_H_
#define ROCKET_LEVEL_H_

#include <cstdint>
#include <optional>
#include <string_view>

namespace rocket {

enum class Level : uint8_t {
  kTrace,
  kDebug,
  kInfo,
  kWarn,
  kError,
  kFatal,
  kOff
};

constexpr std::string_view LevelName(Level level) {
  switch (level) {
    case Level::kTrace:
      return "TRACE";
    case Level::kDebug:
      return "DEBUG";
    case Level::kInfo:
      return "INFO";
    case Level::kWarn:
      return "WARN";
    case Level::kError:
      return "ERROR";
    case Level::kFatal:
      return "FATAL";
    case Level::kOff:
      return "OFF";
  }
  return "UNKNOWN";
}

inline std::optional<Level> ParseLevel(std::string_view name) {
  constexpr Level kLevels[] = {Level::kTrace, Level::kDebug, Level::kInfo,
                               Level::kWarn,  Level::kError, Level::kFatal,
                               Level::kOff};
  for (Level level : kLevels) {
    const std::string_view candidate = LevelName(level);
    if (candidate.size() != name.size()) continue;
    bool equal = true;
    for (size_t i = 0; i < name.size() && equal; ++i) {
      const char c = name[i];
      equal = (c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c) == candidate[i];
    }
    if (equal) return level;
  }
  return std::nullopt;
}

}  // namespace rocket

#endif  // ROCKET_LEVEL_H_
