#include "rocket/rocket.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
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

  for (int i = 0; i < 8; ++i) EXPECT_TRUE(queue.TryPush(int{i}));
  EXPECT_FALSE(queue.TryPush(8));

  int value = -1;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value, i);
  }
  EXPECT_FALSE(queue.TryPop(value));
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
        while (!queue.TryPush(int{i})) std::this_thread::yield();
      }
    });
  }
  for (int c = 0; c < kConsumers; ++c) {
    threads.emplace_back([&] {
      int value = 0;
      while (consumed.load() < kProducers * kPerProducer) {
        if (queue.TryPop(value)) {
          sum.fetch_add(value);
          consumed.fetch_add(1);
        } else {
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
        lines.push_back(record.message);
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
    [](const ::testing::TestParamInfo<OverflowPolicy>& info) {
      switch (info.param) {
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

}  // namespace
}  // namespace rocket
