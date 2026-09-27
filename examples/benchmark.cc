#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "rocket/rocket.h"

namespace {

void Run(const char* label, rocket::Mode mode, int threads, int per_thread) {
  rocket::Options options;
  options.mode = mode;
  options.queue_capacity = 1 << 16;

  const auto start = std::chrono::steady_clock::now();
  std::chrono::steady_clock::duration producer_time{};
  {
    rocket::Logger logger(options, {std::make_shared<rocket::FileSink>(
                                       "logs/benchmark.log", true)});
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
      workers.emplace_back([&logger, per_thread] {
        for (int i = 0; i < per_thread; ++i) {
          logger.Info("benchmark message number ", i, " value ", 3.14159);
        }
      });
    }
    for (std::thread& worker : workers) worker.join();
    producer_time = std::chrono::steady_clock::now() - start;
  }
  const auto total_time = std::chrono::steady_clock::now() - start;

  const double messages = static_cast<double>(threads) * per_thread;
  const double producer_ms =
      std::chrono::duration<double, std::milli>(producer_time).count();
  const double total_ms =
      std::chrono::duration<double, std::milli>(total_time).count();
  std::printf(
      "%-6s threads=%d  producer %8.1f ms (%6.0f ns/msg)  total %8.1f ms\n",
      label, threads, producer_ms, producer_ms * 1e6 / messages, total_ms);
}

}  // namespace

int main() {
  constexpr int kMessages = 1'000'000;
  for (int threads : {1, 4}) {
    Run("async", rocket::Mode::kAsync, threads, kMessages / threads);
    Run("sync", rocket::Mode::kSync, threads, kMessages / threads);
  }
  return 0;
}
