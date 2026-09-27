#ifndef ROCKET_LOGGER_H_
#define ROCKET_LOGGER_H_

#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <version>

#include "rocket/level.h"
#include "rocket/queue.h"
#include "rocket/record.h"
#include "rocket/sink.h"

namespace rocket {

enum class Mode { kAsync, kSync };

enum class OverflowPolicy { kBlock, kDropNewest, kDropOldest };

struct Options {
  std::string name = "rocket";
  Mode mode = Mode::kAsync;
  OverflowPolicy overflow_policy = OverflowPolicy::kBlock;
  size_t queue_capacity = 8192;
  Level level = Level::kInfo;
  Level flush_level = Level::kError;
  std::chrono::milliseconds flush_interval{1000};
};

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

struct Entry {
  Record record;
  std::string text;
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
    if (options_.mode == Mode::kAsync) {
      queue_ = std::make_unique<internal::BoundedQueue<internal::Entry>>(
          options_.queue_capacity);
      worker_ = std::thread([this] { Run(); });
    }
  }

  ~Logger() {
    if (!worker_.joinable()) {
      FlushSinks();
      return;
    }
    stopping_.store(true, std::memory_order_seq_cst);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      wake_.notify_one();
    }
    worker_.join();
  }

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  const std::string& name() const { return options_.name; }
  Mode mode() const { return options_.mode; }

  void set_level(Level level) {
    level_.store(level, std::memory_order_relaxed);
  }
  Level level() const { return level_.load(std::memory_order_relaxed); }

  bool ShouldLog(Level level) const {
    return level != Level::kOff && level >= this->level();
  }

  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

  template <typename... Args>
  void Log(Level level, SourceLocation location, const Args&... args) {
    if (!ShouldLog(level)) return;
    Record record;
    record.level = level;
    record.time = std::chrono::system_clock::now();
    record.thread_id = internal::CurrentThreadId();
    record.logger_name = options_.name;
    record.location = location;
    if (!worker_.joinable()) {
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
    Enqueue(record);
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
    if (!worker_.joinable()) {
      FlushSinks();
      return;
    }
    const size_t target = queue_->tail();
    size_t requested = flush_requested_.load(std::memory_order_relaxed);
    while (requested < target &&
           !flush_requested_.compare_exchange_weak(requested, target)) {
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      wake_.notify_one();
    }
    std::unique_lock<std::mutex> lock(mutex_);
    flushed_.wait(lock, [&] { return flushed_through_ >= target; });
  }

 private:
  static constexpr size_t kBatchSize = 256;
  static constexpr size_t kMaxRetainedText = 4096;
  static constexpr int kBlockedSpins = 16;
  static constexpr std::chrono::microseconds kBlockedBackoff{50};
  static constexpr int kIdleSpins = 64;

  void WriteInline(const Record& record) {
    Dispatch(record);
    CommitSinks();
    if (record.level >= options_.flush_level) FlushSinks();
  }

  void Enqueue(const Record& record) {
    const auto fill = [&record](internal::Entry& entry) {
      entry.text.assign(record.message);
      entry.record = record;
      entry.record.message = entry.text;
    };
    for (int attempt = 0; !queue_->TryPush(fill); ++attempt) {
      switch (options_.overflow_policy) {
        case OverflowPolicy::kBlock:
          WakeWorker();
          if (attempt < kBlockedSpins) {
            std::this_thread::yield();
          } else {
            std::this_thread::sleep_for(kBlockedBackoff);
          }
          break;
        case OverflowPolicy::kDropNewest:
          dropped_.fetch_add(1, std::memory_order_relaxed);
          return;
        case OverflowPolicy::kDropOldest:
          if (queue_->TryPop([](internal::Entry&) {})) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
          }
          break;
      }
    }
    WakeWorker();
  }

  void WakeWorker() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!sleeping_.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    wake_.notify_one();
  }

  bool Park(size_t flushed_through) {
    std::unique_lock<std::mutex> lock(mutex_);
    sleeping_.store(true, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    bool timed_out = false;
    if (queue_->Empty() && !stopping_.load(std::memory_order_relaxed) &&
        flush_requested_.load(std::memory_order_relaxed) <= flushed_through) {
      timed_out = wake_.wait_for(lock, options_.flush_interval) ==
                  std::cv_status::timeout;
    }
    sleeping_.store(false, std::memory_order_relaxed);
    return !timed_out;
  }

  void PublishFlushed(size_t position) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      flushed_through_ = position;
    }
    flushed_.notify_all();
  }

  void Dispatch(const Record& record) {
    for (const std::shared_ptr<Sink>& sink : sinks_) sink->Consume(record);
  }

  void CommitSinks() {
    for (const std::shared_ptr<Sink>& sink : sinks_) sink->Commit();
  }

  void FlushSinks() {
    for (const std::shared_ptr<Sink>& sink : sinks_) sink->Flush();
  }

  void Run() {
    internal::Entry current;
    const auto take = [&current](internal::Entry& entry) {
      std::swap(current, entry);
    };
    size_t flushed_through = 0;
    bool dirty = false;
    int idle_spins = 0;
    while (true) {
      size_t drained = 0;
      bool urgent = false;
      while (drained < kBatchSize && queue_->TryPop(take)) {
        current.record.message = current.text;
        Dispatch(current.record);
        urgent = urgent || current.record.level >= options_.flush_level;
        if (current.text.capacity() > kMaxRetainedText) {
          std::string().swap(current.text);
        }
        ++drained;
      }
      if (drained > 0) {
        CommitSinks();
        dirty = true;
      }

      const size_t head = queue_->head();
      const size_t requested = flush_requested_.load(std::memory_order_acquire);
      const bool answer = requested > flushed_through && head >= requested;
      const bool stopping = stopping_.load(std::memory_order_acquire);
      const bool finished = stopping && drained == 0 && queue_->Empty();
      if ((urgent || answer || finished) && dirty) {
        FlushSinks();
        dirty = false;
      }
      if (answer) {
        flushed_through = head;
        PublishFlushed(head);
      }
      if (finished) return;

      if (drained > 0 || stopping) {
        idle_spins = 0;
      } else if (++idle_spins < kIdleSpins) {
        std::this_thread::yield();
      } else {
        idle_spins = 0;
        if (!Park(flushed_through) && dirty) {
          FlushSinks();
          dirty = false;
        }
      }
    }
  }

  Options options_;
  std::vector<std::shared_ptr<Sink>> sinks_;
  std::atomic<Level> level_;
  std::atomic<uint64_t> dropped_{0};
  std::unique_ptr<internal::BoundedQueue<internal::Entry>> queue_;

  std::atomic<bool> stopping_{false};
  std::atomic<bool> sleeping_{false};
  std::atomic<size_t> flush_requested_{0};

  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable flushed_;
  size_t flushed_through_ = 0;
  std::thread worker_;
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
