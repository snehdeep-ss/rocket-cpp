#ifndef ROCKET_WRITER_H_
#define ROCKET_WRITER_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "rocket/args.h"
#include "rocket/options.h"
#include "rocket/queue.h"
#include "rocket/record.h"
#include "rocket/sink.h"

namespace rocket {
namespace internal {

using SinkList = std::vector<std::shared_ptr<Sink>>;

inline void DispatchAll(const SinkList& sinks, const Record& record) {
  for (const std::shared_ptr<Sink>& sink : sinks) sink->Consume(record);
}

inline void CommitAll(const SinkList& sinks) {
  for (const std::shared_ptr<Sink>& sink : sinks) sink->Commit();
}

inline void FlushAll(const SinkList& sinks) {
  for (const std::shared_ptr<Sink>& sink : sinks) sink->Flush();
}

inline bool NeedText(const SinkList& sinks) {
  for (const std::shared_ptr<Sink>& sink : sinks) {
    if (sink->NeedsText()) return true;
  }
  return false;
}

struct Entry {
  Record record;
  std::string text;
  bool deferred = false;
};

class Writer {
 public:
  Writer(const Options& options, SinkList sinks)
      : overflow_policy_(options.overflow_policy),
        flush_level_(options.flush_level),
        flush_interval_(options.flush_interval),
        sinks_(std::move(sinks)),
        needs_text_(NeedText(sinks_)),
        queue_(options.queue_capacity),
        thread_([this] { Run(); }) {}

  ~Writer() {
    stopping_.store(true, std::memory_order_seq_cst);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      wake_.notify_one();
    }
    thread_.join();
  }

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

  void Enqueue(const Record& record) {
    const auto fill = [&record](Entry& entry) {
      entry.deferred = !record.args.empty();
      entry.text.assign(entry.deferred ? record.args : record.message);
      entry.record = record;
    };
    for (int attempt = 0; !queue_.TryPush(fill); ++attempt) {
      switch (overflow_policy_) {
        case OverflowPolicy::kBlock:
          WakeThread();
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
          if (queue_.TryPop([](Entry&) {})) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
          }
          break;
      }
    }
    WakeThread();
  }

  size_t RequestFlush() {
    const size_t target = queue_.tail();
    size_t requested = flush_requested_.load(std::memory_order_relaxed);
    while (requested < target &&
           !flush_requested_.compare_exchange_weak(requested, target)) {
    }
    std::lock_guard<std::mutex> lock(mutex_);
    wake_.notify_one();
    return target;
  }

  void AwaitFlush(size_t target) {
    std::unique_lock<std::mutex> lock(mutex_);
    flushed_.wait(lock, [&] { return flushed_through_ >= target; });
  }

 private:
  static constexpr size_t kBatchSize = 256;
  static constexpr size_t kMaxRetainedText = 4096;
  static constexpr int kIdleSpins = 64;
  static constexpr int kBlockedSpins = 16;
  static constexpr std::chrono::microseconds kBlockedBackoff{50};

  void WakeThread() {
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
    if (queue_.Empty() && !stopping_.load(std::memory_order_relaxed) &&
        flush_requested_.load(std::memory_order_relaxed) <= flushed_through) {
      timed_out =
          wake_.wait_for(lock, flush_interval_) == std::cv_status::timeout;
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

  void Resolve(Entry& entry) {
    if (!entry.deferred) {
      entry.record.message = entry.text;
      entry.record.args = {};
      return;
    }
    entry.record.args = entry.text;
    entry.record.message = {};
    if (!needs_text_) return;
    rendered_.clear();
    RenderArgs(entry.text, rendered_);
    entry.record.message = rendered_;
  }

  void Run() {
    Entry current;
    const auto take = [&current](Entry& entry) { std::swap(current, entry); };
    size_t flushed_through = 0;
    bool dirty = false;
    int idle_spins = 0;
    while (true) {
      size_t drained = 0;
      bool urgent = false;
      while (drained < kBatchSize && queue_.TryPop(take)) {
        Resolve(current);
        DispatchAll(sinks_, current.record);
        urgent = urgent || current.record.level >= flush_level_;
        if (current.text.capacity() > kMaxRetainedText) {
          std::string().swap(current.text);
        }
        ++drained;
      }
      if (drained > 0) {
        CommitAll(sinks_);
        dirty = true;
      }

      const size_t head = queue_.head();
      const size_t requested = flush_requested_.load(std::memory_order_acquire);
      const bool answer = requested > flushed_through && head >= requested;
      const bool stopping = stopping_.load(std::memory_order_acquire);
      const bool finished = stopping && drained == 0 && queue_.Empty();
      if ((urgent || answer || finished) && dirty) {
        FlushAll(sinks_);
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
          FlushAll(sinks_);
          dirty = false;
        }
      }
    }
  }

  const OverflowPolicy overflow_policy_;
  const Level flush_level_;
  const std::chrono::milliseconds flush_interval_;
  const SinkList sinks_;
  const bool needs_text_;
  std::string rendered_;
  BoundedQueue<Entry> queue_;
  std::atomic<uint64_t> dropped_{0};

  std::atomic<bool> stopping_{false};
  std::atomic<bool> sleeping_{false};
  std::atomic<size_t> flush_requested_{0};

  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable flushed_;
  size_t flushed_through_ = 0;
  std::thread thread_;
};

}  // namespace internal
}  // namespace rocket

#endif  // ROCKET_WRITER_H_
