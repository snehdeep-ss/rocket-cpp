#ifndef ROCKET_OPTIONS_H_
#define ROCKET_OPTIONS_H_

#include <chrono>
#include <cstddef>
#include <string>

#include "rocket/level.h"

namespace rocket {

enum class Mode { kAsync, kSync };

enum class OverflowPolicy { kBlock, kDropNewest, kDropOldest };

enum class Writers { kShared, kPerSink };

enum class Formatting { kEager, kDeferred };

struct Options {
  std::string name = "rocket";
  Mode mode = Mode::kAsync;
  Writers writers = Writers::kShared;
  Formatting formatting = Formatting::kEager;
  OverflowPolicy overflow_policy = OverflowPolicy::kBlock;
  size_t queue_capacity = 8192;
  Level level = Level::kInfo;
  Level flush_level = Level::kError;
  std::chrono::milliseconds flush_interval{1000};
};

}  // namespace rocket

#endif  // ROCKET_OPTIONS_H_
