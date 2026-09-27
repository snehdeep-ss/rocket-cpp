#ifndef ROCKET_FORMATTER_H_
#define ROCKET_FORMATTER_H_

#include <charconv>
#include <chrono>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include "rocket/level.h"
#include "rocket/record.h"

namespace rocket {

class Formatter {
 public:
  virtual ~Formatter() = default;

  virtual void Format(const Record& record, std::string& out) const = 0;
};

class PatternFormatter : public Formatter {
 public:
  static constexpr std::string_view kDefaultPattern =
      "{time} [{level}] [{thread}] {message}";

  enum class Clock { kLocal, kUtc };

  explicit PatternFormatter(std::string_view pattern = kDefaultPattern,
                            Clock clock = Clock::kLocal)
      : clock_(clock) {
    Parse(pattern);
  }

  void Format(const Record& record, std::string& out) const override {
    for (const Token& token : tokens_) {
      switch (token.field) {
        case Field::kLiteral:
          out.append(token.literal);
          break;
        case Field::kTime:
          AppendTime(record.time, out);
          break;
        case Field::kLevel:
          out.append(LevelName(record.level));
          break;
        case Field::kThread:
          AppendNumber(record.thread_id, out);
          break;
        case Field::kLogger:
          out.append(record.logger_name);
          break;
        case Field::kMessage:
          out.append(record.message);
          break;
        case Field::kFile:
          out.append(BaseName(record.location.file));
          break;
        case Field::kLine:
          AppendNumber(record.location.line, out);
          break;
      }
    }
  }

 private:
  enum class Field {
    kLiteral,
    kTime,
    kLevel,
    kThread,
    kLogger,
    kMessage,
    kFile,
    kLine
  };

  struct Token {
    Field field;
    std::string literal;
  };

  struct Placeholder {
    std::string_view name;
    Field field;
  };

  static constexpr Placeholder kPlaceholders[] = {
      {"time", Field::kTime},       {"level", Field::kLevel},
      {"thread", Field::kThread},   {"logger", Field::kLogger},
      {"message", Field::kMessage}, {"file", Field::kFile},
      {"line", Field::kLine}};

  void Parse(std::string_view pattern) {
    std::string literal;
    size_t pos = 0;
    while (pos < pattern.size()) {
      const size_t open = pattern.find('{', pos);
      const size_t close = pattern.find('}', open);
      if (open == std::string_view::npos || close == std::string_view::npos) {
        literal.append(pattern.substr(pos));
        break;
      }
      literal.append(pattern.substr(pos, open - pos));
      const std::string_view name = pattern.substr(open + 1, close - open - 1);
      const Placeholder* match = nullptr;
      for (const Placeholder& placeholder : kPlaceholders) {
        if (placeholder.name == name) match = &placeholder;
      }
      if (match == nullptr) {
        literal.append(pattern.substr(open, close - open + 1));
      } else {
        if (!literal.empty()) {
          tokens_.push_back({Field::kLiteral, std::move(literal)});
          literal.clear();
        }
        tokens_.push_back({match->field, {}});
      }
      pos = close + 1;
    }
    if (!literal.empty()) tokens_.push_back({Field::kLiteral, literal});
  }

  void AppendTime(std::chrono::system_clock::time_point time,
                  std::string& out) const {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    if (cached_time_.empty() || seconds != cached_seconds_) {
      cached_seconds_ = seconds;
      cached_time_ = FormatSeconds(seconds);
    }
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            time.time_since_epoch())
                            .count() %
                        1000;
    out.append(cached_time_);
    out.push_back('.');
    out.push_back(static_cast<char>('0' + millis / 100));
    out.push_back(static_cast<char>('0' + millis / 10 % 10));
    out.push_back(static_cast<char>('0' + millis % 10));
    if (clock_ == Clock::kUtc) out.push_back('Z');
  }

  std::string FormatSeconds(std::time_t seconds) const {
    std::tm parts{};
    if (clock_ == Clock::kUtc) {
#ifdef _WIN32
      gmtime_s(&parts, &seconds);
#else
      gmtime_r(&seconds, &parts);
#endif
    } else {
#ifdef _WIN32
      localtime_s(&parts, &seconds);
#else
      localtime_r(&seconds, &parts);
#endif
    }
    char buffer[32];
    const size_t length =
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts);
    return std::string(buffer, length);
  }

  template <typename Integer>
  static void AppendNumber(Integer value, std::string& out) {
    char buffer[24];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
  }

  static std::string_view BaseName(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
  }

  std::vector<Token> tokens_;
  Clock clock_;
  mutable std::time_t cached_seconds_ = 0;
  mutable std::string cached_time_;
};

}  // namespace rocket

#endif  // ROCKET_FORMATTER_H_
