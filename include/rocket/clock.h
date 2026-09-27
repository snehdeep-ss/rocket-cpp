#ifndef ROCKET_CLOCK_H_
#define ROCKET_CLOCK_H_

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace rocket {
namespace internal {

inline uint64_t ReadTicks() {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || \
    defined(_M_IX86)
  return __rdtsc();
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
  uint64_t ticks = 0;
  asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
  return ticks;
#else
  return static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

struct TickSample {
  uint64_t ticks;
  std::chrono::system_clock::time_point time;
};

inline TickSample TakeTickSample() {
  constexpr int kAttempts = 3;
  TickSample best{};
  uint64_t best_window = std::numeric_limits<uint64_t>::max();
  for (int i = 0; i < kAttempts; ++i) {
    const uint64_t before = ReadTicks();
    const auto time = std::chrono::system_clock::now();
    const uint64_t after = ReadTicks();
    if (after - before < best_window) {
      best_window = after - before;
      best = {before + (after - before) / 2, time};
    }
  }
  return best;
}

struct TickSegment {
  uint64_t index = std::numeric_limits<uint64_t>::max();
  TickSample anchor{};
  double nanos_per_tick = 0;

  std::chrono::system_clock::time_point ToTime(uint64_t ticks) const {
    const auto offset =
        static_cast<double>(static_cast<int64_t>(ticks - anchor.ticks)) *
        nanos_per_tick;
    return anchor.time +
           std::chrono::duration_cast<std::chrono::system_clock::duration>(
               std::chrono::nanoseconds(std::llround(offset)));
  }
};

class TickTimeline {
 public:
  static constexpr int kSegmentBits = 22;

  TickTimeline() : base_(TakeTickSample()) {}

  TickTimeline(const TickTimeline&) = delete;
  TickTimeline& operator=(const TickTimeline&) = delete;

  TickSegment Segment(uint64_t ticks) {
    const uint64_t index = ticks >> kSegmentBits;
    std::lock_guard<std::mutex> lock(mutex_);
    TickSegment& segment = segments_[index % kSegments];
    if (segment.index != index) {
      segment.index = index;
      segment.anchor = TakeTickSample();
      const uint64_t elapsed = segment.anchor.ticks - base_.ticks;
      const std::chrono::duration<double, std::nano> span =
          segment.anchor.time - base_.time;
      segment.nanos_per_tick =
          elapsed == 0 ? 0 : span.count() / static_cast<double>(elapsed);
    }
    return segment;
  }

 private:
  static constexpr size_t kSegments = 1024;

  const TickSample base_;
  std::mutex mutex_;
  std::array<TickSegment, kSegments> segments_;
};

class TickConverter {
 public:
  explicit TickConverter(std::shared_ptr<TickTimeline> timeline)
      : timeline_(std::move(timeline)) {}

  std::chrono::system_clock::time_point ToTime(uint64_t ticks) {
    if ((ticks >> TickTimeline::kSegmentBits) != segment_.index) {
      segment_ = timeline_->Segment(ticks);
    }
    return segment_.ToTime(ticks);
  }

 private:
  std::shared_ptr<TickTimeline> timeline_;
  TickSegment segment_;
};

}  // namespace internal
}  // namespace rocket

#endif  // ROCKET_CLOCK_H_
