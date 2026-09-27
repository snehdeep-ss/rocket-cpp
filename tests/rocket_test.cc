#include "rocket/rocket.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace rocket {
namespace {

class Collector {
 public:
  std::shared_ptr<CallbackSink> MakeSink(
      std::string_view pattern = "{message}") {
    return std::make_shared<CallbackSink>(
        [this](const Record&, std::string_view line) {
          std::lock_guard<std::mutex> lock(mutex_);
          lines_.emplace_back(line.substr(0, line.size() - 1));
        },
        std::make_unique<PatternFormatter>(pattern));
  }

  std::vector<std::string> lines() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> lines_;
};

Options MakeOptions(Mode mode = Mode::kAsync) {
  Options options;
  options.name = "test";
  options.mode = mode;
  options.level = Level::kTrace;
  return options;
}

std::filesystem::path FreshDirectory(std::string_view name) {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "rocket_test" / name;
  std::filesystem::remove_all(directory);
  return directory;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

TEST(LevelTest, NamesRoundTrip) {
  EXPECT_EQ(LevelName(Level::kWarn), "WARN");
  EXPECT_EQ(ParseLevel("warn"), Level::kWarn);
  EXPECT_EQ(ParseLevel("ERROR"), Level::kError);
  EXPECT_FALSE(ParseLevel("loud").has_value());
}

TEST(BoundedQueueTest, RoundsCapacityAndPreservesOrder) {
  internal::BoundedQueue<int> queue(5);
  EXPECT_EQ(queue.capacity(), 8u);
  EXPECT_TRUE(queue.Empty());

  for (int i = 0; i < 8; ++i) {
    EXPECT_TRUE(queue.TryPush([i](int& slot) { slot = i; }));
  }
  EXPECT_FALSE(queue.TryPush([](int& slot) { slot = 8; }));

  int value = -1;
  const auto take = [&value](int& slot) { value = slot; };
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(queue.TryPop(take));
    EXPECT_EQ(value, i);
  }
  EXPECT_FALSE(queue.TryPop(take));
  EXPECT_TRUE(queue.Empty());
}

TEST(BoundedQueueTest, TransfersEveryItemAcrossThreads) {
  constexpr int kProducers = 4;
  constexpr int kConsumers = 4;
  constexpr int kPerProducer = 50000;
  internal::BoundedQueue<int> queue(128);
  std::atomic<int> consumed{0};
  std::atomic<long long> sum{0};

  std::vector<std::thread> threads;
  for (int p = 0; p < kProducers; ++p) {
    threads.emplace_back([&queue] {
      for (int i = 1; i <= kPerProducer; ++i) {
        while (!queue.TryPush([i](int& slot) { slot = i; })) {
          std::this_thread::yield();
        }
      }
    });
  }
  for (int c = 0; c < kConsumers; ++c) {
    threads.emplace_back([&] {
      const auto take = [&](int& slot) {
        sum.fetch_add(slot);
        consumed.fetch_add(1);
      };
      while (consumed.load() < kProducers * kPerProducer) {
        if (!queue.TryPop(take)) {
          std::this_thread::yield();
        }
      }
    });
  }
  for (std::thread& thread : threads) thread.join();

  const long long per_producer =
      static_cast<long long>(kPerProducer) * (kPerProducer + 1) / 2;
  EXPECT_EQ(sum.load(), kProducers * per_producer);
  EXPECT_TRUE(queue.Empty());
}

TEST(PatternFormatterTest, ExpandsPlaceholders) {
  Record record;
  record.level = Level::kError;
  record.thread_id = 7;
  record.logger_name = "core";
  record.location = {"src/net/socket.cc", 42};
  record.message = "boom";

  std::string out;
  PatternFormatter("{logger}|{level}|{thread}|{file}:{line}|{message}|{nope}")
      .Format(record, out);

  EXPECT_EQ(out, "core|ERROR|7|socket.cc:42|boom|{nope}");
}

TEST(PatternFormatterTest, FormatsUtcTime) {
  Record record;
  record.time = std::chrono::system_clock::time_point(
      std::chrono::milliseconds(1700000000123));

  std::string out;
  PatternFormatter("{time}", PatternFormatter::Clock::kUtc).Format(record, out);

  EXPECT_EQ(out, "2023-11-14 22:13:20.123Z");
}

TEST(LoggerTest, AppendsMixedArguments) {
  Collector collector;
  Logger logger(MakeOptions(Mode::kSync), {collector.MakeSink()});

  logger.Info("n=", 42, " ok=", true, " c=", 'x', " f=", 1.5);

  EXPECT_EQ(collector.lines(),
            std::vector<std::string>{"n=42 ok=true c=x f=1.5"});
}

TEST(LoggerTest, AsyncPreservesOrder) {
  Collector collector;
  Logger logger(MakeOptions(), {collector.MakeSink()});

  for (int i = 0; i < 1000; ++i) logger.Info(i);
  logger.Flush();

  const std::vector<std::string> lines = collector.lines();
  ASSERT_EQ(lines.size(), 1000u);
  for (int i = 0; i < 1000; ++i) EXPECT_EQ(lines[i], std::to_string(i));
}

TEST(LoggerTest, AsyncDeliversFromManyThreads) {
  constexpr int kThreads = 8;
  constexpr int kPerThread = 5000;
  Collector collector;
  Options options = MakeOptions();
  options.queue_capacity = 64;
  {
    Logger logger(options, {collector.MakeSink("{thread} {message}")});
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&logger] {
        for (int i = 0; i < kPerThread; ++i) logger.Info(i);
      });
    }
    for (std::thread& thread : threads) thread.join();
    EXPECT_EQ(logger.dropped(), 0u);
  }

  std::map<std::string, int> next;
  for (const std::string& line : collector.lines()) {
    const size_t space = line.find(' ');
    int& expected = next[line.substr(0, space)];
    EXPECT_EQ(line.substr(space + 1), std::to_string(expected));
    ++expected;
  }
  EXPECT_EQ(next.size(), static_cast<size_t>(kThreads));
  for (const auto& [thread, count] : next) EXPECT_EQ(count, kPerThread);
}

class OverflowTest : public ::testing::TestWithParam<OverflowPolicy> {};

TEST_P(OverflowTest, AppliesPolicyWhenQueueIsFull) {
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  std::vector<std::string> lines;
  bool first = true;
  auto sink = std::make_shared<CallbackSink>(
      [&](const Record& record, std::string_view) {
        if (first) {
          first = false;
          entered.set_value();
          released.wait();
        }
        lines.emplace_back(record.message);
      });

  Options options = MakeOptions();
  options.queue_capacity = 4;
  options.overflow_policy = GetParam();
  Logger logger(options, {sink});

  logger.Info("start");
  entered.get_future().wait();
  std::thread producer([&logger] {
    for (int i = 0; i < 10; ++i) logger.Info(i);
  });
  if (GetParam() != OverflowPolicy::kBlock) producer.join();
  release.set_value();
  if (GetParam() == OverflowPolicy::kBlock) producer.join();
  logger.Flush();

  switch (GetParam()) {
    case OverflowPolicy::kBlock:
      EXPECT_EQ(logger.dropped(), 0u);
      EXPECT_EQ(lines.size(), 11u);
      break;
    case OverflowPolicy::kDropNewest:
      EXPECT_EQ(logger.dropped(), 6u);
      EXPECT_EQ(lines, (std::vector<std::string>{"start", "0", "1", "2", "3"}));
      break;
    case OverflowPolicy::kDropOldest:
      EXPECT_EQ(logger.dropped(), 6u);
      EXPECT_EQ(lines, (std::vector<std::string>{"start", "6", "7", "8", "9"}));
      break;
  }
}

INSTANTIATE_TEST_SUITE_P(
    Policies, OverflowTest,
    ::testing::Values(OverflowPolicy::kBlock, OverflowPolicy::kDropNewest,
                      OverflowPolicy::kDropOldest),
    [](const ::testing::TestParamInfo<OverflowPolicy>& param_info) {
      switch (param_info.param) {
        case OverflowPolicy::kBlock:
          return "Block";
        case OverflowPolicy::kDropNewest:
          return "DropNewest";
        case OverflowPolicy::kDropOldest:
          return "DropOldest";
      }
      return "Unknown";
    });

TEST(LoggerTest, SyncWritesImmediately) {
  Collector collector;
  Logger logger(MakeOptions(Mode::kSync), {collector.MakeSink()});

  logger.Warn("now");

  EXPECT_EQ(collector.lines(), std::vector<std::string>{"now"});
}

TEST(LoggerTest, FiltersByLoggerAndSinkLevel) {
  Collector all;
  Collector errors;
  auto error_sink = errors.MakeSink();
  error_sink->set_level(Level::kError);
  Options options = MakeOptions(Mode::kSync);
  options.level = Level::kInfo;
  Logger logger(options, {all.MakeSink(), error_sink});

  logger.Debug("hidden");
  logger.Info("info");
  logger.Error("error");
  logger.set_level(Level::kOff);
  logger.Fatal("muted");

  EXPECT_EQ(all.lines(), (std::vector<std::string>{"info", "error"}));
  EXPECT_EQ(errors.lines(), std::vector<std::string>{"error"});
}

TEST(LoggerTest, MacroCapturesSourceLocation) {
  Collector collector;
  Logger logger(MakeOptions(Mode::kSync),
                {collector.MakeSink("{file}:{line} {message}")});

  const int line = __LINE__ + 1;
  ROCKET_INFO(logger, "at ", "line");

  EXPECT_EQ(collector.lines(),
            std::vector<std::string>{"rocket_test.cc:" + std::to_string(line) +
                                     " at line"});
}

TEST(LoggerTest, FlushWaitsForRecordsFromOtherThreads) {
  Collector collector;
  Logger logger(MakeOptions(), {collector.MakeSink()});

  std::thread producer([&logger] {
    for (int i = 0; i < 1000; ++i) logger.Info(i);
  });
  producer.join();
  logger.Flush();

  EXPECT_EQ(collector.lines().size(), 1000u);
}

TEST(LoggerTest, RecycledSlotsKeepMessagesIntact) {
  Collector collector;
  Options options = MakeOptions();
  options.queue_capacity = 4;
  std::vector<std::string> expected;
  {
    Logger logger(options, {collector.MakeSink()});
    for (int i = 0; i < 3000; ++i) {
      const size_t length = i % 3 == 0 ? 3 : i % 3 == 1 ? 100 : 5000;
      std::string message(length, static_cast<char>('a' + i % 26));
      message += std::to_string(i);
      logger.Info(message);
      expected.push_back(std::move(message));
    }
  }
  EXPECT_EQ(collector.lines(), expected);
}

TEST(WritersTest, SharedModeUsesOneThread) {
  Collector first;
  Collector second;
  Logger logger(MakeOptions(), {first.MakeSink(), second.MakeSink()});

  EXPECT_EQ(logger.writer_threads(), 1u);
}

TEST(WritersTest, PerSinkModeDeliversToEverySink) {
  constexpr int kThreads = 4;
  constexpr int kPerThread = 5000;
  Collector first;
  Collector second;
  Options options = MakeOptions();
  options.writers = Writers::kPerSink;
  options.queue_capacity = 64;
  Logger logger(options, {first.MakeSink("{thread} {message}"),
                          second.MakeSink("{thread} {message}")});
  EXPECT_EQ(logger.writer_threads(), 2u);

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&logger] {
      for (int i = 0; i < kPerThread; ++i) logger.Info(i);
    });
  }
  for (std::thread& thread : threads) thread.join();
  logger.Flush();

  for (const Collector* collector : {&first, &second}) {
    std::map<std::string, int> next;
    for (const std::string& line : collector->lines()) {
      const size_t space = line.find(' ');
      int& expected = next[line.substr(0, space)];
      EXPECT_EQ(line.substr(space + 1), std::to_string(expected));
      ++expected;
    }
    EXPECT_EQ(next.size(), static_cast<size_t>(kThreads));
    for (const auto& [thread, count] : next) EXPECT_EQ(count, kPerThread);
  }
  EXPECT_EQ(logger.dropped(), 0u);
}

TEST(WritersTest, PerSinkModeIsolatesSlowSinks) {
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  auto slow = std::make_shared<CallbackSink>(
      [released](const Record&, std::string_view) { released.wait(); });
  Collector fast;
  Options options = MakeOptions();
  options.writers = Writers::kPerSink;
  Logger logger(options, {slow, fast.MakeSink()});

  for (int i = 0; i < 100; ++i) logger.Info(i);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (fast.lines().size() < 100 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_EQ(fast.lines().size(), 100u);
  release.set_value();
}

TEST(LoggerTest, DestructorDrainsQueue) {
  Collector collector;
  {
    Logger logger(MakeOptions(), {collector.MakeSink()});
    for (int i = 0; i < 100; ++i) logger.Info(i);
  }
  EXPECT_EQ(collector.lines().size(), 100u);
}

TEST(FileSinkTest, WritesLines) {
  const std::filesystem::path path = FreshDirectory("file") / "app.log";
  {
    Logger logger(
        MakeOptions(),
        {std::make_shared<FileSink>(
            path, true,
            std::make_unique<PatternFormatter>("[{level}] {message}"))});
    logger.Info("hello");
    logger.Error("world");
  }
  EXPECT_EQ(ReadFile(path), "[INFO] hello\n[ERROR] world\n");
}

TEST(RotatingFileSinkTest, RotatesAndCapsBackups) {
  const std::filesystem::path path = FreshDirectory("rotating") / "app.log";
  {
    auto sink = std::make_shared<RotatingFileSink>(
        path, 10, 2, std::make_unique<PatternFormatter>("{message}"));
    Logger logger(MakeOptions(Mode::kSync), {sink});
    for (const char* message : {"aaaa", "bbbb", "cccc", "dddd", "eeee"}) {
      logger.Info(message);
    }
  }
  EXPECT_EQ(ReadFile(path), "eeee\n");
  EXPECT_EQ(ReadFile(path.string() + ".1"), "cccc\ndddd\n");
  EXPECT_EQ(ReadFile(path.string() + ".2"), "aaaa\nbbbb\n");
  EXPECT_FALSE(std::filesystem::exists(path.string() + ".3"));
}

struct DecodedRecord {
  Level level;
  uint32_t thread_id;
  std::string logger;
  std::string file;
  int line;
  std::string message;
  int64_t nanos;

  bool operator==(const DecodedRecord& other) const {
    return level == other.level && thread_id == other.thread_id &&
           logger == other.logger && file == other.file && line == other.line &&
           message == other.message && nanos == other.nanos;
  }
};

DecodedRecord Decode(const Record& record) {
  return {record.level,
          record.thread_id,
          std::string(record.logger_name),
          record.location.file,
          record.location.line,
          std::string(record.message),
          internal::ToNanoseconds(record.time)};
}

std::vector<DecodedRecord> ReadAll(
    const std::filesystem::path& path,
    BinaryReader::Status expected_status = BinaryReader::Status::kOk) {
  BinaryReader reader(path);
  EXPECT_TRUE(reader.is_open());
  std::vector<DecodedRecord> records;
  Record record;
  while (reader.Next(record)) records.push_back(Decode(record));
  EXPECT_EQ(reader.status(), expected_status);
  return records;
}

TEST(BinaryTest, VarintAndZigZagRoundTrip) {
  for (int64_t value : {int64_t{0}, int64_t{-1}, int64_t{1}, int64_t{-300},
                        int64_t{1} << 40, std::numeric_limits<int64_t>::min(),
                        std::numeric_limits<int64_t>::max()}) {
    EXPECT_EQ(internal::ZigZagDecode(internal::ZigZagEncode(value)), value);
  }
  std::string out;
  internal::PutVarint(out, 300);
  EXPECT_EQ(out, std::string("\xac\x02"));
}

TEST(BinaryTest, RoundTripsEveryField) {
  const std::filesystem::path path = FreshDirectory("binary") / "app.blog";
  auto sink = std::make_shared<BinaryFileSink>(path, true);
  ASSERT_TRUE(sink->is_open());

  std::vector<DecodedRecord> expected;
  auto capture = std::make_shared<CallbackSink>(
      [&expected](const Record& record, std::string_view) {
        expected.push_back(Decode(record));
      });
  {
    Options options = MakeOptions();
    options.name = "api";
    options.writers = Writers::kShared;
    Options db_options = MakeOptions(Mode::kSync);
    db_options.name = "db";
    Logger api(options, {sink, capture});
    Logger db(db_options, {sink, capture});
    for (int i = 0; i < 500; ++i) {
      ROCKET_WARN(api, "request ", i, " took ", i * 0.5, "ms");
      if (i % 50 == 0) db.Error("slow query ", i);
    }
    api.Info(std::string(10000, 'x'));
    api.Flush();
  }

  const std::vector<DecodedRecord> decoded = ReadAll(path);
  ASSERT_EQ(decoded.size(), expected.size());
  std::vector<DecodedRecord> sorted_expected = expected;
  std::vector<DecodedRecord> sorted_decoded = decoded;
  const auto by_content = [](const DecodedRecord& a, const DecodedRecord& b) {
    return std::tie(a.logger, a.message, a.nanos) <
           std::tie(b.logger, b.message, b.nanos);
  };
  std::sort(sorted_expected.begin(), sorted_expected.end(), by_content);
  std::sort(sorted_decoded.begin(), sorted_decoded.end(), by_content);
  EXPECT_TRUE(sorted_decoded == sorted_expected);
  for (const DecodedRecord& record : decoded) {
    if (record.logger == "db" || record.message.size() == 10000) {
      EXPECT_EQ(record.file, "");
    } else {
      EXPECT_EQ(std::filesystem::path(record.file).filename(),
                "rocket_test.cc");
      EXPECT_EQ(record.level, Level::kWarn);
    }
  }
}

TEST(BinaryTest, DecodesToTheSameTextAsTheTextSink) {
  const std::filesystem::path directory = FreshDirectory("binary_text");
  const std::string pattern =
      "{time} [{level}] [{thread}] {logger} {file}:{line} {message}";
  {
    Options options = MakeOptions();
    options.writers = Writers::kPerSink;
    Logger logger(options, {std::make_shared<FileSink>(
                                directory / "app.log", true,
                                std::make_unique<PatternFormatter>(
                                    pattern, PatternFormatter::Clock::kUtc)),
                            std::make_shared<BinaryFileSink>(
                                directory / "app.blog", true)});
    for (int i = 0; i < 1000; ++i) ROCKET_INFO(logger, "event ", i, ' ', true);
  }

  std::string decoded;
  const PatternFormatter formatter(pattern, PatternFormatter::Clock::kUtc);
  BinaryReader reader(directory / "app.blog");
  Record record;
  while (reader.Next(record)) {
    formatter.Format(record, decoded);
    decoded.push_back('\n');
  }
  EXPECT_EQ(reader.status(), BinaryReader::Status::kOk);
  EXPECT_EQ(decoded, ReadFile(directory / "app.log"));
}

TEST(BinaryTest, AppendsNewSessions) {
  const std::filesystem::path path = FreshDirectory("binary_append") / "a.blog";
  for (const char* name : {"first", "second"}) {
    Options options = MakeOptions(Mode::kSync);
    options.name = name;
    Logger logger(options, {std::make_shared<BinaryFileSink>(path)});
    logger.Info(name, " run");
  }

  const std::vector<DecodedRecord> decoded = ReadAll(path);
  ASSERT_EQ(decoded.size(), 2u);
  EXPECT_EQ(decoded[0].logger, "first");
  EXPECT_EQ(decoded[0].message, "first run");
  EXPECT_EQ(decoded[1].logger, "second");
  EXPECT_EQ(decoded[1].message, "second run");
}

TEST(BinaryTest, ReportsTruncationAndKeepsCompleteRecords) {
  const std::filesystem::path path = FreshDirectory("binary_cut") / "a.blog";
  {
    Logger logger(MakeOptions(Mode::kSync),
                  {std::make_shared<BinaryFileSink>(path, true)});
    for (int i = 0; i < 10; ++i) logger.Info("record ", i);
  }
  std::filesystem::resize_file(path, std::filesystem::file_size(path) - 3);

  const std::vector<DecodedRecord> decoded =
      ReadAll(path, BinaryReader::Status::kTruncated);
  ASSERT_EQ(decoded.size(), 9u);
  EXPECT_EQ(decoded.back().message, "record 8");
}

TEST(BinaryTest, RejectsCorruptInput) {
  const std::filesystem::path path = FreshDirectory("binary_bad") / "a.blog";
  std::filesystem::create_directories(path.parent_path());
  {
    std::ofstream stream(path, std::ios::binary);
    stream << "not a rocket log";
  }
  EXPECT_TRUE(ReadAll(path, BinaryReader::Status::kCorrupt).empty());
  EXPECT_FALSE(BinaryReader(path.parent_path() / "missing.blog").is_open());
}

struct Point {
  int x;
  int y;
};

std::ostream& operator<<(std::ostream& stream, const Point& point) {
  return stream << '(' << point.x << ", " << point.y << ')';
}

enum class Color : uint8_t { kRed = 3 };
enum Legacy { kLegacyValue = -7 };

template <typename... Args>
void ExpectDeferredMatchesEager(const Args&... args) {
  std::string eager;
  (internal::Append(eager, args), ...);
  std::string captured;
  (internal::Capture(captured, args), ...);
  std::string rendered;
  ASSERT_TRUE(internal::RenderArgs(captured, rendered));
  EXPECT_EQ(rendered, eager);
}

TEST(DeferredTest, RendersEveryTypeLikeEagerFormatting) {
  const std::string owned = "owned";
  const char array[] = "array";
  const char* pointer = "pointer";
  ExpectDeferredMatchesEager("literal", owned, std::string_view("view"), array,
                             pointer, std::string());
  ExpectDeferredMatchesEager('c', true, false);
  ExpectDeferredMatchesEager(0, -1, 42, std::numeric_limits<int>::min(),
                             std::numeric_limits<int64_t>::min(),
                             std::numeric_limits<int64_t>::max());
  ExpectDeferredMatchesEager(
      0u, std::numeric_limits<uint64_t>::max(), static_cast<unsigned char>(200),
      static_cast<signed char>(-100), static_cast<short>(-3));
  ExpectDeferredMatchesEager(0.1f, -2.5f, std::numeric_limits<float>::max(),
                             0.1, -0.0, 1e300, 3.14159,
                             std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::denorm_min());
  ExpectDeferredMatchesEager(1.5L, Color::kRed, kLegacyValue, Point{1, -2});
}

TEST(DeferredTest, RejectsMalformedArgs) {
  std::string rendered;
  EXPECT_FALSE(internal::RenderArgs(std::string_view("\x09", 1), rendered));
  EXPECT_FALSE(internal::RenderArgs(std::string_view("\x00\x05"
                                                     "ab",
                                                     4),
                                    rendered));
  EXPECT_FALSE(
      internal::RenderArgs(std::string_view("\x06\x01\x02", 3), rendered));
}

std::vector<std::string> LogBoth(Options options) {
  Collector collector;
  {
    Logger logger(options,
                  {collector.MakeSink("{level} {file}:{line} {message}")});
    for (int i = 0; i < 300; ++i) {
      ROCKET_WARN(logger, "order ", i, " price ", 100.25 + i * 0.01, " side ",
                  i % 2 == 0 ? 'B' : 'S', " ok=", true, ' ', Point{i, -i});
    }
    logger.Info();
    logger.Error(std::string(6000, 'z'));
  }
  return collector.lines();
}

TEST(DeferredTest, TextOutputMatchesEagerInEveryMode) {
  for (Mode mode : {Mode::kAsync, Mode::kSync}) {
    Options eager = MakeOptions(mode);
    Options deferred = eager;
    deferred.formatting = Formatting::kDeferred;
    const std::vector<std::string> expected = LogBoth(eager);
    ASSERT_EQ(expected.size(), 302u);
    EXPECT_EQ(LogBoth(deferred), expected);
  }
}

class RawSink : public Sink {
 public:
  bool NeedsText() const override { return false; }

  std::vector<std::pair<std::string, std::string>> seen;

 protected:
  void Process(const Record& record) override {
    seen.emplace_back(record.message, record.args);
  }
};

TEST(DeferredTest, RawSinksReceiveArgsWithoutRendering) {
  auto raw = std::make_shared<RawSink>();
  Options options = MakeOptions();
  options.formatting = Formatting::kDeferred;
  {
    Logger logger(options, {raw});
    logger.Info("value ", 7);
  }
  ASSERT_EQ(raw->seen.size(), 1u);
  EXPECT_TRUE(raw->seen[0].first.empty());
  std::string rendered;
  ASSERT_TRUE(internal::RenderArgs(raw->seen[0].second, rendered));
  EXPECT_EQ(rendered, "value 7");
}

TEST(DeferredTest, BinaryFilesStoreArgsAndDecodeToText) {
  const std::filesystem::path directory = FreshDirectory("deferred_binary");
  const std::string pattern =
      "{time} [{level}] [{thread}] {file}:{line} {message}";
  for (Formatting formatting : {Formatting::kEager, Formatting::kDeferred}) {
    Options options = MakeOptions();
    options.formatting = formatting;
    const std::string name =
        formatting == Formatting::kEager ? "eager" : "deferred";
    Logger logger(options, {std::make_shared<FileSink>(
                                directory / (name + ".log"), true,
                                std::make_unique<PatternFormatter>(
                                    pattern, PatternFormatter::Clock::kUtc)),
                            std::make_shared<BinaryFileSink>(
                                directory / (name + ".blog"), true)});
    for (int i = 0; i < 2000; ++i) {
      ROCKET_INFO(logger, "fill ", i, " px ", 101.25 + i * 0.0001, " qty ",
                  i * 1000);
    }
  }

  for (const char* name : {"eager", "deferred"}) {
    std::string decoded;
    const PatternFormatter formatter(pattern, PatternFormatter::Clock::kUtc);
    BinaryReader reader(directory / (std::string(name) + ".blog"));
    Record record;
    while (reader.Next(record)) {
      EXPECT_EQ(record.args.empty(), std::string_view(name) == "eager");
      formatter.Format(record, decoded);
      decoded.push_back('\n');
    }
    EXPECT_EQ(reader.status(), BinaryReader::Status::kOk);
    EXPECT_EQ(decoded, ReadFile(directory / (std::string(name) + ".log")));
  }
}

TEST(DeferredTest, ReadsVersionOneFiles) {
  const std::filesystem::path path = FreshDirectory("version_one") / "a.blog";
  {
    Logger logger(MakeOptions(Mode::kSync),
                  {std::make_shared<BinaryFileSink>(path, true)});
    logger.Info("from v1");
  }
  {
    std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
    stream.seekp(
        static_cast<std::streamoff>(internal::kBinaryMagic.size() + 1));
    stream.put(1);
  }
  const std::vector<DecodedRecord> decoded = ReadAll(path);
  ASSERT_EQ(decoded.size(), 1u);
  EXPECT_EQ(decoded[0].message, "from v1");
}

static_assert(internal::CountPlaceholders("") == 0);
static_assert(internal::CountPlaceholders("{}") == 1);
static_assert(internal::CountPlaceholders("a {} b {}{} c") == 3);
static_assert(internal::CountPlaceholders("{x} { } {") == 0);
static_assert(internal::Signature<int, double, const char*, Point>::kValue ==
              std::string_view("\x03\x06\x00\x00", 4));

void LogWithFormats(Logger& logger) {
  for (int i = 0; i < 200; ++i) {
    ROCKET_WARNF(logger, "fill {} px {} side {} ok={} at {}", i,
                 100.25 + i * 0.01, i % 2 == 0 ? 'B' : 'S', true, Point{i, -i});
    ROCKET_INFOF(logger, "{}{}", static_cast<unsigned char>(i), "tail");
  }
  ROCKET_ERRORF(logger, "no args, literal braces {x} { } {");
  ROCKET_DEBUGF(logger, "big {} f {} e {} s {}",
                std::numeric_limits<int64_t>::min(), 0.1f, Color::kRed,
                std::string(5000, 'q'));
  ROCKET_TRACEF(logger, "filtered out {}", 1);
}

std::vector<std::string> CollectFormats(Mode mode, Formatting formatting,
                                        Writers writers = Writers::kShared) {
  Collector collector;
  Options options = MakeOptions(mode);
  options.level = Level::kDebug;
  options.formatting = formatting;
  options.writers = writers;
  {
    Logger logger(options,
                  {collector.MakeSink("{level} {file}:{line} {message}")});
    LogWithFormats(logger);
  }
  return collector.lines();
}

TEST(FormatTest, EagerRendersFormatStrings) {
  const std::vector<std::string> lines =
      CollectFormats(Mode::kSync, Formatting::kEager);
  ASSERT_EQ(lines.size(), 402u);
  EXPECT_EQ(lines[0].substr(lines[0].find(' ', 5) + 1),
            "fill 0 px 100.25 side B ok=true at (0, 0)");
  EXPECT_EQ(lines[1].substr(lines[1].find(' ', 5) + 1), "0tail");
  EXPECT_EQ(lines[400].substr(lines[400].find(' ', 6) + 1),
            "no args, literal braces {x} { } {");
  EXPECT_EQ(lines[401].substr(lines[401].find(' ', 6) + 1),
            "big -9223372036854775808 f 0.1 e 3 s " + std::string(5000, 'q'));
  EXPECT_EQ(lines[0].substr(0, lines[0].find(':')), "WARN rocket_test.cc");
}

TEST(FormatTest, DeferredMatchesEagerInEveryMode) {
  const std::vector<std::string> expected =
      CollectFormats(Mode::kSync, Formatting::kEager);
  for (Mode mode : {Mode::kSync, Mode::kAsync}) {
    for (Writers writers : {Writers::kShared, Writers::kPerSink}) {
      EXPECT_EQ(CollectFormats(mode, Formatting::kDeferred, writers), expected);
      EXPECT_EQ(CollectFormats(mode, Formatting::kEager, writers), expected);
    }
  }
}

TEST(FormatTest, CallSitesAreStaticAndDistinct) {
  std::vector<const CallSite*> sites;
  auto capture = std::make_shared<CallbackSink>(
      [&sites](const Record& record, std::string_view) {
        sites.push_back(record.site);
      });
  Options options = MakeOptions(Mode::kSync);
  options.formatting = Formatting::kDeferred;
  Logger logger(options, {capture});
  for (int i = 0; i < 3; ++i) ROCKET_INFOF(logger, "same {}", i);
  ROCKET_INFOF(logger, "same {}", 9);

  ASSERT_EQ(sites.size(), 4u);
  EXPECT_EQ(sites[0], sites[1]);
  EXPECT_EQ(sites[1], sites[2]);
  EXPECT_NE(sites[2], sites[3]);
  EXPECT_EQ(sites[0]->format, "same {}");
  EXPECT_EQ(sites[0]->level, Level::kInfo);
  EXPECT_EQ(sites[0]->arg_types, std::string_view("\x03", 1));
  EXPECT_EQ(sites[3]->line, sites[0]->line + 1);
}

TEST(FormatTest, BinaryStoresSiteIdsAndDecodesToText) {
  const std::filesystem::path directory = FreshDirectory("site_binary");
  const std::string pattern =
      "{time} [{level}] [{thread}] {logger} {file}:{line} {message}";
  {
    Options options = MakeOptions();
    options.level = Level::kDebug;
    options.formatting = Formatting::kDeferred;
    Logger logger(options, {std::make_shared<FileSink>(
                                directory / "app.log", true,
                                std::make_unique<PatternFormatter>(
                                    pattern, PatternFormatter::Clock::kUtc)),
                            std::make_shared<BinaryFileSink>(
                                directory / "app.blog", true)});
    LogWithFormats(logger);
    logger.Info("concatenated ", 1);
  }

  std::string decoded;
  size_t site_records = 0;
  const PatternFormatter formatter(pattern, PatternFormatter::Clock::kUtc);
  BinaryReader reader(directory / "app.blog");
  Record record;
  while (reader.Next(record)) {
    site_records += record.payload == Payload::kSite ? 1 : 0;
    formatter.Format(record, decoded);
    decoded.push_back('\n');
  }
  EXPECT_EQ(reader.status(), BinaryReader::Status::kOk);
  EXPECT_EQ(site_records, 402u);
  EXPECT_EQ(decoded, ReadFile(directory / "app.log"));
}

TEST(FormatTest, SiteRecordsAreSmallerThanConcatenatedRecords) {
  const std::filesystem::path directory = FreshDirectory("site_size");
  for (const char* name : {"concat", "format"}) {
    Options options = MakeOptions(Mode::kSync);
    options.formatting = Formatting::kDeferred;
    Logger logger(options,
                  {std::make_shared<BinaryFileSink>(
                      directory / (std::string(name) + ".blog"), true)});
    for (int i = 0; i < 1000; ++i) {
      if (std::string_view(name) == "concat") {
        ROCKET_INFO(logger, "fill ", i, " px ", 101.25 + i * 0.0001, " qty ",
                    i * 100);
      } else {
        ROCKET_INFOF(logger, "fill {} px {} qty {}", i, 101.25 + i * 0.0001,
                     i * 100);
      }
    }
  }
  const auto concat = std::filesystem::file_size(directory / "concat.blog");
  const auto format = std::filesystem::file_size(directory / "format.blog");
  EXPECT_LT(format * 2, concat);
}

TEST(FormatTest, RejectsCorruptSites) {
  const std::filesystem::path path = FreshDirectory("site_bad") / "a.blog";
  std::filesystem::create_directories(path.parent_path());
  std::string bytes;
  bytes.push_back(0);
  bytes.append(internal::kBinaryMagic);
  bytes.push_back(3);
  bytes.append(
      std::string("\x01\x00\x01"
                  "f",
                  4));
  bytes.append(std::string("\x04\x00\x02\x00\x07\x00\x01\x09", 8));
  {
    std::ofstream stream(path, std::ios::binary);
    stream << bytes;
  }
  EXPECT_TRUE(ReadAll(path, BinaryReader::Status::kCorrupt).empty());
}

class TimeSourceTest : public ::testing::TestWithParam<TimeSource> {};

TEST_P(TimeSourceTest, TimestampsTrackTheSystemClock) {
  using std::chrono::system_clock;
  constexpr int kRecords = 60;
  std::vector<system_clock::time_point> stamped;
  auto sink = std::make_shared<CallbackSink>(
      [&stamped](const Record& record, std::string_view) {
        stamped.push_back(record.time);
      });
  Options options = MakeOptions();
  options.time_source = GetParam();
  std::vector<std::pair<system_clock::time_point, system_clock::time_point>>
      windows;
  {
    Logger logger(options, {sink});
    for (int i = 0; i < kRecords; ++i) {
      const auto before = system_clock::now();
      ROCKET_INFOF(logger, "tick {}", i);
      windows.emplace_back(before, system_clock::now());
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  ASSERT_EQ(stamped.size(), static_cast<size_t>(kRecords));
  for (int i = 0; i < kRecords; ++i) {
    const auto slack =
        i < 10 ? std::chrono::milliseconds(5) : std::chrono::milliseconds(0);
    const auto jitter = std::chrono::microseconds(100);
    EXPECT_GE(stamped[i], windows[i].first - jitter - slack) << i;
    EXPECT_LE(stamped[i], windows[i].second + jitter + slack) << i;
  }
}

INSTANTIATE_TEST_SUITE_P(
    Sources, TimeSourceTest,
    ::testing::Values(TimeSource::kCycleCounter, TimeSource::kSystemClock),
    [](const ::testing::TestParamInfo<TimeSource>& param_info) {
      return param_info.param == TimeSource::kCycleCounter ? "CycleCounter"
                                                           : "SystemClock";
    });

TEST(WakeTest, ParkedWriterWakesForEveryRecord) {
  using std::chrono::steady_clock;
  std::vector<steady_clock::time_point> delivered;
  std::mutex mutex;
  auto sink =
      std::make_shared<CallbackSink>([&](const Record&, std::string_view) {
        std::lock_guard<std::mutex> lock(mutex);
        delivered.push_back(steady_clock::now());
      });
  Options options = MakeOptions();
  options.flush_interval = std::chrono::seconds(10);
  Logger logger(options, {sink});

  steady_clock::duration worst{};
  for (int i = 0; i < 200; ++i) {
    std::this_thread::sleep_for(std::chrono::microseconds(500 + i % 7 * 300));
    const auto logged = steady_clock::now();
    logger.Info(i);
    while (true) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (delivered.size() == static_cast<size_t>(i + 1)) {
          worst = std::max(worst, delivered.back() - logged);
          break;
        }
      }
      ASSERT_LT(steady_clock::now() - logged, std::chrono::seconds(5))
          << "record " << i << " was never delivered";
      std::this_thread::yield();
    }
  }
  EXPECT_LT(worst, std::chrono::milliseconds(500));
}

}  // namespace
}  // namespace rocket
