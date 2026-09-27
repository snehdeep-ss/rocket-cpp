#ifndef ROCKET_RECORD_H_
#define ROCKET_RECORD_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string_view>

#include "rocket/level.h"

namespace rocket {

struct SourceLocation {
  const char* file = "";
  int line = 0;
};

struct CallSite {
  Level level = Level::kInfo;
  std::string_view format;
  const char* file = "";
  int line = 0;
  std::string_view arg_types;
};

enum class Payload : uint8_t { kText, kArgs, kSite };

struct Record {
  Level level = Level::kInfo;
  std::chrono::system_clock::time_point time;
  uint32_t thread_id = 0;
  std::string_view logger_name;
  SourceLocation location;
  Payload payload = Payload::kText;
  std::string_view message;
  std::string_view args;
  const CallSite* site = nullptr;
};

namespace internal {

inline uint32_t CurrentThreadId() {
  static std::atomic<uint32_t> next_id{1};
  thread_local const uint32_t id =
      next_id.fetch_add(1, std::memory_order_relaxed);
  return id;
}

}  // namespace internal
}  // namespace rocket

#endif  // ROCKET_RECORD_H_
