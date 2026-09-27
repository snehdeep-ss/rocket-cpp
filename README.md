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
| `mode`             | `Mode::kAsync`  | `kAsync` uses writer threads, `kSync` writes inline.    |
| `writers`          | `kShared`       | `kShared`: one writer for all sinks. `kPerSink`: one each. |
| `overflow_policy`  | `kBlock`        | `kBlock`, `kDropNewest` or `kDropOldest` when full.     |
| `queue_capacity`   | `8192`          | Pending records per writer, rounded to a power of 2.    |
| `level`            | `Level::kInfo`  | Minimum level accepted. Changeable with `set_level`.    |
| `flush_level`      | `Level::kError` | Records at or above this level flush sinks immediately. |
| `flush_interval`   | `1000ms`        | Idle interval after which buffered output is flushed.   |

`logger.Flush()` blocks until every record logged before the call is written and flushed by every writer. The destructor drains the queues. `logger.dropped()` reports records discarded by a drop policy, summed across writers, and `logger.writer_threads()` reports how many writers are running.

## Writer threads

With `Writers::kShared` a single background thread formats and writes every record to every sink. With `Writers::kPerSink` each sink gets its own thread and its own lock-free queue: the message is composed once on the calling thread and copied into each queue. Sinks then format and write in parallel, and a slow sink (a network socket, a full disk) cannot stall the others. The overflow policy applies to each queue independently.

Measured on 8 cores with 2M messages from 8 threads, in ns per message:

| File sinks | `kShared` | `kPerSink` |
| ---------- | --------- | ---------- |
| 1          | 204       | 205        |
| 2          | 331       | 240        |
| 3          | 449       | 321        |

Use `kShared` for a single sink or to keep thread count down, and `kPerSink` when several sinks are busy or one of them is slow.

Each sink always sees one thread's records in the order that thread logged them. With `kPerSink`, records from different threads may interleave differently in each sink because every sink has its own queue.

`rocket::ParseLevel("warn")` turns environment variables or config values into a `Level`.

## Threading model

Logging calls never take a lock. Each record is claimed and published with a single compare-and-swap on the ring buffer, and the worker drains it in batches. When the worker has nothing to do it parks on a condition variable; producers only touch that mutex to wake a parked worker, never while it is running. `kDropOldest` evicts from the head of the same lock-free queue.

The hot path allocates nothing in steady state. Callers compose text in a reused per-thread buffer and copy it into the slot's own string, whose capacity survives across laps. The worker swaps each slot with a spare entry, so the slot is released before any sink runs. Slots are cache-line aligned to avoid false sharing between the worker and producers.

The worker writes each batch through a 64 KB buffer per sink and commits it once per batch rather than once per line. When the queue is full under `kBlock`, callers yield briefly and then back off in 50 µs sleeps, which keeps them from contending with the worker for the very slots it is freeing.

Sinks serialise their own I/O, so sync mode and sinks shared between loggers remain safe.

## Sinks

| Sink               | Behaviour                                              |
| ------------------ | ------------------------------------------------------ |
| `ConsoleSink`      | stdout or stderr, optional ANSI colours per level.     |
| `FileSink`         | Appends or truncates a file, creating parent folders.  |
| `RotatingFileSink` | Rolls to `file.1 … file.N` once `max_bytes` is hit.    |
| `CallbackSink`     | Hands each record and formatted line to a lambda.      |
| `BinaryFileSink`   | Compact binary `.blog` file, decoded later.            |
| `NullSink`         | Discards everything.                                   |

Every sink takes an optional formatter and has its own level:

```cpp
auto errors = std::make_shared<rocket::FileSink>("logs/errors.log");
errors->set_level(rocket::Level::kError);
```

Write your own text sink by deriving from `TextSink` and overriding `Write`, plus `DoCommit` and `DoFlush` if it buffers:

```cpp
class SyslogSink : public rocket::TextSink {
 protected:
  void Write(const rocket::Record& record, std::string_view line) override;
  void DoCommit() override;
  void DoFlush() override;
};
```

To handle raw records without formatting, as `BinaryFileSink` does, derive from `Sink` and override `Process(const Record&)` instead.

The writer calls `Commit` at the end of every batch and `Flush` on `flush_level`, `flush_interval` and `Logger::Flush`. All three are serialised by the base class, so one sink can be shared between loggers. `Record::message` and `line` are views that are valid only for the duration of the call.

## Binary logs

`BinaryFileSink` skips text formatting on the writer thread and stores each record as a level byte, a zig-zag varint nanosecond delta from the previous record, varint thread id and line, and the message bytes. Logger names and source files are written once per file and referenced by id afterwards.

```cpp
rocket::Logger logger(options, {std::make_shared<rocket::BinaryFileSink>("logs/app.blog")});
```

Turn it back into a normal log with the bundled decoder, using any `PatternFormatter` pattern:

```bash
rocket_decode logs/app.blog logs/app.log
rocket_decode --utc --pattern "{time} {level} {file}:{line} {message}" logs/app.blog
```

With the same pattern and `Writers::kShared`, the decoded output is byte-for-byte what `FileSink` would have written. Opening an existing file appends a new session, so restarts never corrupt earlier records. A file cut short by a crash decodes up to the last complete record, and `rocket_decode` exits with status 2 to flag it. To read records programmatically, use `rocket::BinaryReader`:

```cpp
rocket::BinaryReader reader("logs/app.blog");
rocket::Record record;
while (reader.Next(record)) Process(record);
if (reader.status() != rocket::BinaryReader::Status::kOk) Alert();
```

Compared with a text `FileSink` on the same records, a binary file is about a third smaller and the writer spends about 35% less CPU per record. Messages are still composed as text on the calling thread, so the saving is on the writer side.

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
./build/examples/binary
./build/tools/rocket_decode logs/orders.blog
```

The test suite uses GoogleTest, fetched automatically, and passes under ThreadSanitizer, AddressSanitizer and UndefinedBehaviorSanitizer.

## License

MIT
