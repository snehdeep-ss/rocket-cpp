#ifndef ROCKET_WRITER_H_
#define ROCKET_WRITER_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "rocket/args.h"
#include "rocket/clock.h"
#include "rocket/options.h"
#include "rocket/queue.h"
#include "rocket/record.h"
#include "rocket/sink.h"

#if defined(_MSC_VER)
#define ROCKET_NOINLINE __declspec(noinline)
#else
#define ROCKET_NOINLINE __attribute__((noinline))
#endif

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
  uint64_t ticks = 0;
  uint64_t key = 0;
};

struct ThreadQueue {
  explicit ThreadQueue(size_t capacity) : queue(capacity) {}

  BoundedQueue<Entry> queue;
  std::atomic<bool> abandoned{false};
};

inline uint64_t NextWriterId() {
  static std::atomic<uint64_t> next_id{1};
  return next_id.fetch_add(1, std::memory_order_relaxed);
}

class ThreadQueueCache {
 public:
  ThreadQueueCache() = default;
  ~ThreadQueueCache() {
    for (Slot& slot : slots_) Release(slot);
  }

  ThreadQueueCache(const ThreadQueueCache&) = delete;
  ThreadQueueCache& operator=(const ThreadQueueCache&) = delete;

  template <typename Create>
  ThreadQueue& Find(uint64_t writer_id, Create&& create) {
    for (Slot& slot : slots_) {
      if (slot.writer_id == writer_id) return *slot.queue;
    }
    Slot& slot = slots_[next_victim_++ % kSlots];
    Release(slot);
    slot.writer_id = writer_id;
    slot.queue = create();
    return *slot.queue;
  }

 private:
  static constexpr size_t kSlots = 4;

  struct Slot {
    uint64_t writer_id = 0;
    std::shared_ptr<ThreadQueue> queue;
  };

  static void Release(Slot& slot) {
    if (slot.queue)
      slot.queue->abandoned.store(true, std::memory_order_release);
    slot = Slot{};
  }

  std::array<Slot, kSlots> slots_;
  size_t next_victim_ = 0;
};

inline ThreadQueueCache& LocalQueues() {
  thread_local ThreadQueueCache cache;
  return cache;
}

class Writer {
 public:
  Writer(const Options& options, SinkList sinks,
         std::shared_ptr<TickTimeline> timeline)
      : id_(NextWriterId()),
        capacity_(options.queue_capacity),
        overflow_policy_(options.overflow_policy),
        flush_level_(options.flush_level),
        flush_interval_(options.flush_interval),
        sinks_(std::move(sinks)),
        needs_text_(NeedText(sinks_)),
        use_ticks_(timeline != nullptr),
        clock_(std::move(timeline)) {
    if (options.queues == Queues::kShared) shared_ = Register();
    thread_ = std::thread([this] { Run(); });
  }

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

  void EnqueueShared(const Record& record, uint64_t ticks) {
    EnqueueTo(shared_->queue, record, ticks);
  }

  void EnqueueLocal(const Record& record, uint64_t ticks) {
    EnqueueTo(LocalQueue(), record, ticks);
  }

  uint64_t RequestFlush() {
    FlushRequest request;
    {
      std::lock_guard<std::mutex> lock(registry_mutex_);
      for (const std::shared_ptr<ThreadQueue>& queue : registry_) {
        request.targets.push_back({queue.get(), queue->queue.tail()});
      }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    request.ticket = ++last_ticket_;
    pending_.push_back(std::move(request));
    flush_requested_.store(last_ticket_, std::memory_order_seq_cst);
    wake_.notify_one();
    return last_ticket_;
  }

  void AwaitFlush(uint64_t ticket) {
    std::unique_lock<std::mutex> lock(mutex_);
    flushed_.wait(lock, [&] { return flushed_ticket_ >= ticket; });
  }

 private:
  static constexpr size_t kBatchSize = 256;
  static constexpr size_t kMaxRetainedText = 4096;
  static constexpr int kIdleSpins = 64;
  static constexpr int kBlockedSpins = 16;
  static constexpr std::chrono::microseconds kBlockedBackoff{50};

  struct FlushTarget {
    const ThreadQueue* queue;
    size_t tail;
  };

  struct FlushRequest {
    uint64_t ticket = 0;
    std::vector<FlushTarget> targets;
  };

  struct Source {
    std::shared_ptr<ThreadQueue> queue;
    Entry entry;
    size_t position = 0;
    bool staged = false;
  };

  void EnqueueTo(BoundedQueue<Entry>& queue, const Record& record,
                 uint64_t ticks) {
    const uint64_t key =
        use_ticks_ ? ticks
                   : static_cast<uint64_t>(
                         std::chrono::duration_cast<std::chrono::nanoseconds>(
                             record.time.time_since_epoch())
                             .count());
    const auto fill = [&record, ticks, key](Entry& entry) {
      entry.ticks = ticks;
      entry.key = key;
      entry.text.assign(record.payload == Payload::kText ? record.message
                                                         : record.args);
      entry.record = record;
    };
    for (int attempt = 0; !queue.TryPush(fill); ++attempt) {
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
          if (queue.TryPop([](Entry&) {})) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
          }
          break;
      }
    }
    WakeThread();
  }

  ROCKET_NOINLINE BoundedQueue<Entry>& LocalQueue() {
    return LocalQueues().Find(id_, [this] { return Register(); }).queue;
  }

  std::shared_ptr<ThreadQueue> Register() {
    auto queue = std::make_shared<ThreadQueue>(capacity_);
    {
      std::lock_guard<std::mutex> lock(registry_mutex_);
      registry_.push_back(queue);
    }
    registry_version_.fetch_add(1, std::memory_order_seq_cst);
    return queue;
  }

  void WakeThread() {
    if (!sleeping_.load(std::memory_order_seq_cst)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    wake_.notify_one();
  }

  void RefreshSources() {
    const uint64_t version = registry_version_.load(std::memory_order_seq_cst);
    if (version == seen_version_) return;
    seen_version_ = version;
    std::lock_guard<std::mutex> lock(registry_mutex_);
    for (const std::shared_ptr<ThreadQueue>& queue : registry_) {
      const bool known = std::any_of(
          sources_.begin(), sources_.end(),
          [&queue](const Source& source) { return source.queue == queue; });
      if (!known) sources_.emplace_back().queue = queue;
    }
  }

  void RemoveAbandoned() {
    const auto finished = [](const Source& source) {
      return !source.staged &&
             source.queue->abandoned.load(std::memory_order_acquire) &&
             source.queue->queue.Idle();
    };
    if (std::none_of(sources_.begin(), sources_.end(), finished)) return;
    {
      std::lock_guard<std::mutex> lock(registry_mutex_);
      for (const Source& source : sources_) {
        if (!finished(source)) continue;
        registry_.erase(
            std::find(registry_.begin(), registry_.end(), source.queue));
      }
    }
    sources_.erase(std::remove_if(sources_.begin(), sources_.end(), finished),
                   sources_.end());
    registry_version_.fetch_add(1, std::memory_order_seq_cst);
  }

  bool Idle() const {
    return std::all_of(sources_.begin(), sources_.end(),
                       [](const Source& source) {
                         return !source.staged && source.queue->queue.Idle();
                       });
  }

  void Stage(Source& source) {
    source.staged = source.queue->queue.TryPop(
        [&source](Entry& entry) { std::swap(source.entry, entry); },
        &source.position);
  }

  void StageAll() {
    bool staged_any = true;
    while (staged_any) {
      staged_any = false;
      RefreshSources();
      for (Source& source : sources_) {
        if (source.staged) continue;
        Stage(source);
        staged_any = staged_any || source.staged;
      }
      if (sources_.size() == 1) return;
    }
  }

  size_t DrainRound(bool& urgent) {
    size_t drained = 0;
    while (drained < kBatchSize) {
      StageAll();
      Source* next = nullptr;
      for (Source& source : sources_) {
        if (source.staged &&
            (next == nullptr || source.entry.key < next->entry.key)) {
          next = &source;
        }
      }
      if (next == nullptr) break;
      Resolve(next->entry);
      DispatchAll(sinks_, next->entry.record);
      urgent = urgent || next->entry.record.level >= flush_level_;
      if (next->entry.text.capacity() > kMaxRetainedText) {
        std::string().swap(next->entry.text);
      }
      next->staged = false;
      ++drained;
    }
    return drained;
  }

  bool Reached(const FlushTarget& target) const {
    for (const Source& source : sources_) {
      if (source.queue.get() != target.queue) continue;
      return source.staged ? source.position >= target.tail
                           : source.queue->queue.head() >= target.tail;
    }
    return true;
  }

  uint64_t ResolveFlushes(uint64_t flushed_ticket) {
    if (flush_requested_.load(std::memory_order_acquire) <= flushed_ticket) {
      return flushed_ticket;
    }
    RefreshSources();
    std::lock_guard<std::mutex> lock(mutex_);
    while (!pending_.empty() &&
           std::all_of(
               pending_.front().targets.begin(), pending_.front().targets.end(),
               [this](const FlushTarget& target) { return Reached(target); })) {
      flushed_ticket = pending_.front().ticket;
      pending_.pop_front();
    }
    return flushed_ticket;
  }

  void PublishFlushed(uint64_t ticket) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      flushed_ticket_ = ticket;
    }
    flushed_.notify_all();
  }

  bool Park(uint64_t flushed_ticket) {
    std::unique_lock<std::mutex> lock(mutex_);
    sleeping_.store(true, std::memory_order_seq_cst);
    bool timed_out = false;
    if (registry_version_.load(std::memory_order_seq_cst) == seen_version_ &&
        Idle() && !stopping_.load(std::memory_order_relaxed) &&
        flush_requested_.load(std::memory_order_relaxed) <= flushed_ticket) {
      timed_out =
          wake_.wait_for(lock, flush_interval_) == std::cv_status::timeout;
    }
    sleeping_.store(false, std::memory_order_relaxed);
    return !timed_out;
  }

  void Resolve(Entry& entry) {
    if (use_ticks_) entry.record.time = clock_.ToTime(entry.ticks);
    if (entry.record.payload == Payload::kText) {
      entry.record.message = entry.text;
      entry.record.args = {};
      return;
    }
    entry.record.args = entry.text;
    entry.record.message = {};
    if (!needs_text_) return;
    rendered_.clear();
    Render(entry.record, rendered_);
    entry.record.message = rendered_;
  }

  void Run() {
    uint64_t flushed_ticket = 0;
    bool dirty = false;
    int idle_spins = 0;
    while (true) {
      RefreshSources();
      bool urgent = false;
      const size_t drained = DrainRound(urgent);
      if (drained > 0) {
        CommitAll(sinks_);
        dirty = true;
      }

      const uint64_t resolved = ResolveFlushes(flushed_ticket);
      const bool stopping = stopping_.load(std::memory_order_acquire);
      const bool finished = stopping && drained == 0 && Idle();
      if ((urgent || resolved > flushed_ticket || finished) && dirty) {
        FlushAll(sinks_);
        dirty = false;
      }
      if (resolved > flushed_ticket) {
        flushed_ticket = resolved;
        PublishFlushed(resolved);
      }
      if (finished) return;
      RemoveAbandoned();

      if (drained > 0 || stopping) {
        idle_spins = 0;
      } else if (++idle_spins < kIdleSpins) {
        std::this_thread::yield();
      } else {
        idle_spins = 0;
        if (!Park(flushed_ticket) && dirty) {
          FlushAll(sinks_);
          dirty = false;
        }
      }
    }
  }

  const uint64_t id_;
  const size_t capacity_;
  const OverflowPolicy overflow_policy_;
  const Level flush_level_;
  const std::chrono::milliseconds flush_interval_;
  const SinkList sinks_;
  const bool needs_text_;
  const bool use_ticks_;
  TickConverter clock_;
  std::string rendered_;
  std::shared_ptr<ThreadQueue> shared_;
  std::vector<Source> sources_;
  uint64_t seen_version_ = 0;
  std::atomic<uint64_t> dropped_{0};

  std::mutex registry_mutex_;
  std::vector<std::shared_ptr<ThreadQueue>> registry_;
  std::atomic<uint64_t> registry_version_{0};

  std::atomic<bool> stopping_{false};
  std::atomic<bool> sleeping_{false};
  std::atomic<uint64_t> flush_requested_{0};

  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable flushed_;
  std::deque<FlushRequest> pending_;
  uint64_t last_ticket_ = 0;
  uint64_t flushed_ticket_ = 0;
  std::thread thread_;
};

}  // namespace internal
}  // namespace rocket

#endif  // ROCKET_WRITER_H_
