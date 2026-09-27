#ifndef ROCKET_QUEUE_H_
#define ROCKET_QUEUE_H_

#include <atomic>
#include <cstddef>
#include <memory>

namespace rocket {
namespace internal {

template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(size_t capacity)
      : mask_(RoundUpToPowerOfTwo(capacity) - 1),
        cells_(std::make_unique<Cell[]>(mask_ + 1)) {
    for (size_t i = 0; i <= mask_; ++i) {
      cells_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  size_t capacity() const { return mask_ + 1; }
  size_t head() const { return head_.load(std::memory_order_acquire); }
  size_t tail() const { return tail_.load(std::memory_order_acquire); }

  template <typename Fill>
  bool TryPush(Fill&& fill) {
    size_t position = tail_.load(std::memory_order_relaxed);
    while (true) {
      Cell& cell = cells_[position & mask_];
      const size_t sequence = cell.sequence.load(std::memory_order_acquire);
      const auto distance = static_cast<std::ptrdiff_t>(sequence - position);
      if (distance == 0) {
        if (tail_.compare_exchange_weak(position, position + 1,
                                        std::memory_order_seq_cst,
                                        std::memory_order_relaxed)) {
          fill(cell.value);
          cell.sequence.store(position + 1, std::memory_order_release);
          return true;
        }
      } else if (distance < 0) {
        return false;
      } else {
        position = tail_.load(std::memory_order_relaxed);
      }
    }
  }

  template <typename Consume>
  bool TryPop(Consume&& consume, size_t* popped = nullptr) {
    size_t position = head_.load(std::memory_order_relaxed);
    while (true) {
      Cell& cell = cells_[position & mask_];
      const size_t sequence = cell.sequence.load(std::memory_order_acquire);
      const auto distance =
          static_cast<std::ptrdiff_t>(sequence - (position + 1));
      if (distance == 0) {
        if (head_.compare_exchange_weak(position, position + 1,
                                        std::memory_order_relaxed)) {
          consume(cell.value);
          cell.sequence.store(position + mask_ + 1, std::memory_order_release);
          if (popped != nullptr) *popped = position;
          return true;
        }
      } else if (distance < 0) {
        return false;
      } else {
        position = head_.load(std::memory_order_relaxed);
      }
    }
  }

  bool Idle() const {
    return tail_.load(std::memory_order_seq_cst) ==
           head_.load(std::memory_order_seq_cst);
  }

  bool Empty() const {
    const size_t position = head_.load(std::memory_order_acquire);
    const size_t sequence =
        cells_[position & mask_].sequence.load(std::memory_order_acquire);
    return static_cast<std::ptrdiff_t>(sequence - (position + 1)) < 0;
  }

 private:
  static constexpr size_t kCacheLine = 64;

  struct alignas(kCacheLine) Cell {
    std::atomic<size_t> sequence{0};
    T value{};
  };

  static size_t RoundUpToPowerOfTwo(size_t value) {
    size_t result = 2;
    while (result < value) result <<= 1;
    return result;
  }

  const size_t mask_;
  const std::unique_ptr<Cell[]> cells_;
  alignas(kCacheLine) std::atomic<size_t> tail_{0};
  alignas(kCacheLine) std::atomic<size_t> head_{0};
};

}  // namespace internal
}  // namespace rocket

#endif  // ROCKET_QUEUE_H_
