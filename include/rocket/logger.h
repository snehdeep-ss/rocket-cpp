#ifndef ROCKET_LOGGER_H_
#define ROCKET_LOGGER_H_

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <version>

#include "rocket/level.h"
#include "rocket/options.h"
#include "rocket/record.h"
#include "rocket/sink.h"
#include "rocket/writer.h"

namespace rocket {

namespace internal {

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
    char buffer[32];
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

inline std::string& ScratchText() {
  thread_local std::string text;
  return text;
}

}  // namespace internal

class Logger {
 public:
  Logger(Options options, std::vector<std::shared_ptr<Sink>> sinks)
      : options_(std::move(options)),
        sinks_(std::move(sinks)),
        level_(options_.level) {
    if (options_.flush_interval <= std::chrono::milliseconds::zero()) {
      options_.flush_interval = std::chrono::milliseconds(1);
    }
    if (options_.mode == Mode::kSync || sinks_.empty()) return;
    if (options_.writers == Writers::kShared) {
      writers_.push_back(std::make_unique<internal::Writer>(options_, sinks_));
      return;
    }
    for (const std::shared_ptr<Sink>& sink : sinks_) {
      writers_.push_back(std::make_unique<internal::Writer>(
          options_, internal::SinkList{sink}));
    }
  }

  ~Logger() {
    if (writers_.empty()) internal::FlushAll(sinks_);
  }

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  const std::string& name() const { return options_.name; }
  Mode mode() const { return options_.mode; }
  size_t writer_threads() const { return writers_.size(); }

  void set_level(Level level) {
    level_.store(level, std::memory_order_relaxed);
  }
  Level level() const { return level_.load(std::memory_order_relaxed); }

  bool ShouldLog(Level level) const {
    return level != Level::kOff && level >= this->level();
  }

  uint64_t dropped() const {
    uint64_t total = 0;
    for (const auto& writer : writers_) total += writer->dropped();
    return total;
  }

  template <typename... Args>
  void Log(Level level, SourceLocation location, const Args&... args) {
    if (!ShouldLog(level)) return;
    Record record;
    record.level = level;
    record.time = std::chrono::system_clock::now();
    record.thread_id = internal::CurrentThreadId();
    record.logger_name = options_.name;
    record.location = location;
    if (writers_.empty()) {
      std::string text;
      (internal::Append(text, args), ...);
      record.message = text;
      WriteInline(record);
      return;
    }
    std::string& text = internal::ScratchText();
    text.clear();
    (internal::Append(text, args), ...);
    record.message = text;
    for (const auto& writer : writers_) writer->Enqueue(record);
  }

  template <typename... Args>
  void Trace(const Args&... args) {
    Log(Level::kTrace, {}, args...);
  }
  template <typename... Args>
  void Debug(const Args&... args) {
    Log(Level::kDebug, {}, args...);
  }
  template <typename... Args>
  void Info(const Args&... args) {
    Log(Level::kInfo, {}, args...);
  }
  template <typename... Args>
  void Warn(const Args&... args) {
    Log(Level::kWarn, {}, args...);
  }
  template <typename... Args>
  void Error(const Args&... args) {
    Log(Level::kError, {}, args...);
  }
  template <typename... Args>
  void Fatal(const Args&... args) {
    Log(Level::kFatal, {}, args...);
  }

  void Flush() {
    if (writers_.empty()) {
      internal::FlushAll(sinks_);
      return;
    }
    std::vector<size_t> targets;
    targets.reserve(writers_.size());
    for (const auto& writer : writers_) {
      targets.push_back(writer->RequestFlush());
    }
    for (size_t i = 0; i < writers_.size(); ++i) {
      writers_[i]->AwaitFlush(targets[i]);
    }
  }

 private:
  void WriteInline(const Record& record) {
    internal::DispatchAll(sinks_, record);
    internal::CommitAll(sinks_);
    if (record.level >= options_.flush_level) internal::FlushAll(sinks_);
  }

  Options options_;
  internal::SinkList sinks_;
  std::atomic<Level> level_;
  std::vector<std::unique_ptr<internal::Writer>> writers_;
};

}  // namespace rocket

#ifndef ROCKET_MIN_LEVEL
#define ROCKET_MIN_LEVEL kTrace
#endif

#define ROCKET_LOG(logger, level, ...)                                        \
  do {                                                                        \
    ::rocket::Logger& rocket_logger_ = (logger);                              \
    if ((level) >= ::rocket::Level::ROCKET_MIN_LEVEL &&                       \
        rocket_logger_.ShouldLog(level)) {                                    \
      rocket_logger_.Log(level, ::rocket::SourceLocation{__FILE__, __LINE__}, \
                         __VA_ARGS__);                                        \
    }                                                                         \
  } while (false)

#define ROCKET_TRACE(logger, ...) \
  ROCKET_LOG(logger, ::rocket::Level::kTrace, __VA_ARGS__)
#define ROCKET_DEBUG(logger, ...) \
  ROCKET_LOG(logger, ::rocket::Level::kDebug, __VA_ARGS__)
#define ROCKET_INFO(logger, ...) \
  ROCKET_LOG(logger, ::rocket::Level::kInfo, __VA_ARGS__)
#define ROCKET_WARN(logger, ...) \
  ROCKET_LOG(logger, ::rocket::Level::kWarn, __VA_ARGS__)
#define ROCKET_ERROR(logger, ...) \
  ROCKET_LOG(logger, ::rocket::Level::kError, __VA_ARGS__)
#define ROCKET_FATAL(logger, ...) \
  ROCKET_LOG(logger, ::rocket::Level::kFatal, __VA_ARGS__)

#endif  // ROCKET_LOGGER_H_
