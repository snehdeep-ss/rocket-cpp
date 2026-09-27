#include <cstdio>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include "rocket/rocket.h"

int main() {
  auto console = std::make_shared<rocket::ConsoleSink>();
  auto file = std::make_shared<rocket::RotatingFileSink>(
      "logs/basic.log", 1 << 20, 3,
      std::make_unique<rocket::PatternFormatter>(
          "{time} {level} {logger} {file}:{line} {message}",
          rocket::PatternFormatter::Clock::kUtc));
  auto alerts = std::make_shared<rocket::CallbackSink>(
      [](const rocket::Record& record, std::string_view) {
        std::fprintf(stderr, "alert raised on thread %u: %.*s\n",
                     record.thread_id, static_cast<int>(record.message.size()),
                     record.message.data());
      });
  alerts->set_level(rocket::Level::kError);

  rocket::Options options;
  options.name = "basic";
  options.level = rocket::Level::kDebug;
  options.overflow_policy = rocket::OverflowPolicy::kDropOldest;
  options.writers = rocket::Writers::kPerSink;
  rocket::Logger logger(options, {console, file, alerts});

  ROCKET_INFO(logger, "rocket-cpp ", "started");
  ROCKET_DEBUG(logger, "queue capacity=", options.queue_capacity);

  std::vector<std::thread> workers;
  for (int id = 0; id < 3; ++id) {
    workers.emplace_back([&logger, id] {
      for (int step = 0; step < 3; ++step) {
        ROCKET_INFO(logger, "worker ", id, " step ", step);
      }
    });
  }
  for (std::thread& worker : workers) worker.join();

  ROCKET_WARN(logger, "disk usage at ", 91.5, "%");
  ROCKET_ERROR(logger, "payment service unreachable");
  ROCKET_TRACE(logger, "filtered out by level");
  logger.Flush();
  return 0;
}
