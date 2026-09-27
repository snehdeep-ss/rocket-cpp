# rocket-cpp

A header-only, lock-free, asynchronous, configurable logger for C++17.

- Drop-in: copy `include/rocket` or link one CMake target.
- Async by default: callers hand records to a background worker through a lock-free bounded MPMC ring buffer. Switch to synchronous with one option.
- Pluggable behaviour: swap sinks, formatters and overflow policies without touching call sites.
- Thread-safe sinks, per-sink levels, compile-time level stripping.
- No dependencies beyond the standard library and threads.

## Quick start

```cpp
#include "rocket/rocket.h"

int main() {
  rocket::Options options;
  options.name = "app";
  options.level = rocket::Level::kDebug;

  rocket::Logger logger(options, {std::make_shared<rocket::ConsoleSink>()});

  ROCKET_INFO(logger, "listening on port ", 8080);
  logger.Warn("cache hit ratio ", 0.42);
}
```

```
2026-09-27 09:24:06.208 [INFO] [1] listening on port 8080
2026-09-27 09:24:06.208 [WARN] [1] cache hit ratio 0.42
```

Arguments are concatenated. Strings, characters, booleans, integers, floats and enums are formatted without streams; anything else falls back to `operator<<`.

## Configuration

| `Options` field    | Default         | Meaning                                                 |
| ------------------ | --------------- | ------------------------------------------------------- |
| `name`             | `"rocket"`      | Exposed to formatters as `{logger}`.                    |
| `mode`             | `Mode::kAsync`  | `kAsync` uses a worker thread, `kSync` writes inline.   |
| `overflow_policy`  | `kBlock`        | `kBlock`, `kDropNewest` or `kDropOldest` when full.     |
| `queue_capacity`   | `8192`          | Pending records in async mode, rounded to a power of 2. |
| `level`            | `Level::kInfo`  | Minimum level accepted. Changeable with `set_level`.    |
| `flush_level`      | `Level::kError` | Records at or above this level flush sinks immediately. |
| `flush_interval`   | `1000ms`        | Idle interval after which buffered output is flushed.   |

`logger.Flush()` blocks until every record logged before the call is written and flushed. The destructor drains the queue. `logger.dropped()` reports records discarded by a drop policy.

`rocket::ParseLevel("warn")` turns environment variables or config values into a `Level`.

## Threading model

Logging calls never take a lock. Each record is claimed and published with a single compare-and-swap on the ring buffer, and the worker drains it in batches. When the worker has nothing to do it parks on a condition variable; producers only touch that mutex to wake a parked worker, never while it is running. `kBlock` spins with `yield` instead of sleeping, and `kDropOldest` evicts from the head of the same lock-free queue.

Sinks serialise their own I/O, so sync mode and sinks shared between loggers remain safe.

## Sinks

| Sink               | Behaviour                                              |
| ------------------ | ------------------------------------------------------ |
| `ConsoleSink`      | stdout or stderr, optional ANSI colours per level.     |
| `FileSink`         | Appends or truncates a file, creating parent folders.  |
| `RotatingFileSink` | Rolls to `file.1 … file.N` once `max_bytes` is hit.    |
| `CallbackSink`     | Hands each record and formatted line to a lambda.      |
| `NullSink`         | Discards everything.                                   |

Every sink takes an optional formatter and has its own level:

```cpp
auto errors = std::make_shared<rocket::FileSink>("logs/errors.log");
errors->set_level(rocket::Level::kError);
```

Write your own by overriding two methods:

```cpp
class SyslogSink : public rocket::Sink {
 protected:
  void Write(const rocket::Record& record, std::string_view line) override;
  void DoFlush() override;
};
```

`Write` and `DoFlush` are serialised by the base class, so one sink can be shared between loggers.

## Formatting

`PatternFormatter` understands `{time}`, `{level}`, `{thread}`, `{logger}`, `{message}`, `{file}` and `{line}`. Unknown placeholders are printed verbatim.

```cpp
std::make_unique<rocket::PatternFormatter>(
    "{time} {level} {logger} {file}:{line} {message}",
    rocket::PatternFormatter::Clock::kUtc);
```

For JSON or any other layout, derive from `rocket::Formatter` and implement `Format(const Record&, std::string&)`.

## Macros

`ROCKET_TRACE`, `ROCKET_DEBUG`, `ROCKET_INFO`, `ROCKET_WARN`, `ROCKET_ERROR` and `ROCKET_FATAL` capture `{file}` and `{line}` and skip argument evaluation when the level is disabled. Remove low levels from a release build entirely with:

```
-DROCKET_MIN_LEVEL=kInfo
```

## Integration

Copy `include/rocket` into your project, or with CMake:

```cmake
include(FetchContent)
FetchContent_Declare(rocket GIT_REPOSITORY https://github.com/snehdeep-ss/rocket-cpp.git GIT_TAG main)
FetchContent_MakeAvailable(rocket)
target_link_libraries(your_app PRIVATE rocket::rocket)
```

`add_subdirectory(rocket-cpp)` and `find_package(rocket)` after `cmake --install` work too.

## Building the tests and examples

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/examples/basic
./build/examples/benchmark
```

The test suite uses GoogleTest, fetched automatically, and passes under ThreadSanitizer, AddressSanitizer and UndefinedBehaviorSanitizer.

## License

MIT
