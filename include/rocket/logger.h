#ifndef ROCKET_LOGGER_H_
#define ROCKET_LOGGER_H_

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rocket/args.h"
#include "rocket/level.h"
#include "rocket/options.h"
#include "rocket/record.h"
#include "rocket/sink.h"
#include "rocket/writer.h"

namespace rocket {

namespace internal {

struct SiteInfo {
  Level level;
  std::string_view format;
  const char* file;
  int line;
};

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
    std::shared_ptr<internal::TickTimeline> timeline;
    if (options_.time_source == TimeSource::kCycleCounter) {
      timeline = std::make_shared<internal::TickTimeline>();
    }
    if (options_.writers == Writers::kShared) {
      writers_.push_back(
          std::make_unique<internal::Writer>(options_, sinks_, timeline));
      return;
    }
    for (const std::shared_ptr<Sink>& sink : sinks_) {
      writers_.push_back(std::make_unique<internal::Writer>(
          options_, internal::SinkList{sink}, timeline));
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
    record.thread_id = internal::CurrentThreadId();
    record.logger_name = options_.name;
    record.location = location;
    if (writers_.empty()) {
      record.time = std::chrono::system_clock::now();
      std::string text;
      Compose(text, record, args...);
      WriteInline(record);
      return;
    }
    const uint64_t ticks = Stamp(record);
    std::string& text = internal::ScratchText();
    text.clear();
    Compose(text, record, args...);
    Enqueue(record, ticks);
  }

  template <typename Site, typename... Args>
  void LogFormat(Site, const Args&... args) {
    constexpr internal::SiteInfo kInfo = Site::Get();
    static_assert(internal::CountPlaceholders(kInfo.format) == sizeof...(Args),
                  "the number of {} placeholders must match the arguments");
    static constexpr CallSite kSite{kInfo.level, kInfo.format, kInfo.file,
                                    kInfo.line,
                                    internal::Signature<Args...>::kValue};
    if (!ShouldLog(kSite.level)) return;
    Record record;
    record.level = kSite.level;
    record.thread_id = internal::CurrentThreadId();
    record.logger_name = options_.name;
    record.location = {kSite.file, kSite.line};
    if (writers_.empty()) {
      record.time = std::chrono::system_clock::now();
      std::string text;
      ComposeFormat(text, record, kSite, args...);
      WriteInline(record);
      return;
    }
    const uint64_t ticks = Stamp(record);
    std::string& text = internal::ScratchText();
    text.clear();
    ComposeFormat(text, record, kSite, args...);
    Enqueue(record, ticks);
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
    std::vector<uint64_t> targets;
    targets.reserve(writers_.size());
    for (const auto& writer : writers_) {
      targets.push_back(writer->RequestFlush());
    }
    for (size_t i = 0; i < writers_.size(); ++i) {
      writers_[i]->AwaitFlush(targets[i]);
    }
  }

 private:
  void Enqueue(const Record& record, uint64_t ticks) {
    if (options_.queues == Queues::kPerThread) {
      for (const auto& writer : writers_) writer->EnqueueLocal(record, ticks);
    } else {
      for (const auto& writer : writers_) writer->EnqueueShared(record, ticks);
    }
  }

  uint64_t Stamp(Record& record) const {
    if (options_.time_source == TimeSource::kCycleCounter) {
      return internal::ReadTicks();
    }
    record.time = std::chrono::system_clock::now();
    return 0;
  }

  template <typename... Args>
  void Compose(std::string& text, Record& record, const Args&... args) const {
    if (options_.formatting == Formatting::kDeferred) {
      (internal::Capture(text, args), ...);
      record.payload = Payload::kArgs;
      record.args = text;
    } else {
      (internal::Append(text, args), ...);
      record.message = text;
    }
  }

  template <typename... Args>
  void ComposeFormat(std::string& text, Record& record, const CallSite& site,
                     const Args&... args) const {
    if (options_.formatting == Formatting::kDeferred) {
      (internal::CaptureValue(text, args), ...);
      record.payload = Payload::kSite;
      record.site = &site;
      record.args = text;
    } else {
      internal::FormatEager(text, site.format, args...);
      record.message = text;
    }
  }

  void WriteInline(Record& record) {
    std::string rendered;
    if (record.payload != Payload::kText && internal::NeedText(sinks_)) {
      internal::Render(record, rendered);
      record.message = rendered;
    }
    internal::DispatchAll(sinks_, record);
    internal::CommitAll(sinks_);
    if (record.level >= options_.flush_level) internal::FlushAll(sinks_);
  }

  Options options_;
  internal::SinkList sinks_;
  std::atomic<Level> level_;
  std::vector<std::unique_ptr<internal::Writer>> writers_;
};

namespace internal {

template <typename Site, typename Format, typename... Args>
void CallLogFormat(Logger& logger, Site site, const Format&,
                   const Args&... args) {
  logger.LogFormat(site, args...);
}

}  // namespace internal
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

#define ROCKET_INTERNAL_FIRST(...) ROCKET_INTERNAL_FIRST_IMPL(__VA_ARGS__, 0)
#define ROCKET_INTERNAL_FIRST_IMPL(first, ...) first

#define ROCKET_LOGF(logger, level, ...)                                 \
  do {                                                                  \
    struct RocketSite {                                                 \
      static constexpr ::rocket::internal::SiteInfo Get() {             \
        return {level, "" ROCKET_INTERNAL_FIRST(__VA_ARGS__), __FILE__, \
                __LINE__};                                              \
      }                                                                 \
    };                                                                  \
    ::rocket::Logger& rocket_logger_ = (logger);                        \
    if ((level) >= ::rocket::Level::ROCKET_MIN_LEVEL &&                 \
        rocket_logger_.ShouldLog(level)) {                              \
      ::rocket::internal::CallLogFormat(rocket_logger_, RocketSite{},   \
                                        __VA_ARGS__);                   \
    }                                                                   \
  } while (false)

#define ROCKET_TRACEF(logger, ...) \
  ROCKET_LOGF(logger, ::rocket::Level::kTrace, __VA_ARGS__)
#define ROCKET_DEBUGF(logger, ...) \
  ROCKET_LOGF(logger, ::rocket::Level::kDebug, __VA_ARGS__)
#define ROCKET_INFOF(logger, ...) \
  ROCKET_LOGF(logger, ::rocket::Level::kInfo, __VA_ARGS__)
#define ROCKET_WARNF(logger, ...) \
  ROCKET_LOGF(logger, ::rocket::Level::kWarn, __VA_ARGS__)
#define ROCKET_ERRORF(logger, ...) \
  ROCKET_LOGF(logger, ::rocket::Level::kError, __VA_ARGS__)
#define ROCKET_FATALF(logger, ...) \
  ROCKET_LOGF(logger, ::rocket::Level::kFatal, __VA_ARGS__)

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
