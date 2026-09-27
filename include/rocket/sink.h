#ifndef ROCKET_SINK_H_
#define ROCKET_SINK_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "rocket/formatter.h"
#include "rocket/level.h"
#include "rocket/record.h"

namespace rocket {

class Sink {
 public:
  Sink() = default;
  virtual ~Sink() = default;

  Sink(const Sink&) = delete;
  Sink& operator=(const Sink&) = delete;

  void set_level(Level level) {
    level_.store(level, std::memory_order_relaxed);
  }
  Level level() const { return level_.load(std::memory_order_relaxed); }

  virtual bool NeedsText() const { return true; }

  void Consume(const Record& record) {
    if (record.level < level()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    Process(record);
  }

  void Commit() {
    std::lock_guard<std::mutex> lock(mutex_);
    DoCommit();
  }

  void Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    DoFlush();
  }

 protected:
  virtual void Process(const Record& record) = 0;
  virtual void DoCommit() {}
  virtual void DoFlush() {}

 private:
  std::atomic<Level> level_{Level::kTrace};
  std::mutex mutex_;
};

class TextSink : public Sink {
 public:
  explicit TextSink(std::unique_ptr<Formatter> formatter = nullptr)
      : formatter_(formatter ? std::move(formatter)
                             : std::make_unique<PatternFormatter>()) {}

 protected:
  virtual void Write(const Record& record, std::string_view line) = 0;

 private:
  void Process(const Record& record) final {
    line_.clear();
    formatter_->Format(record, line_);
    line_.push_back('\n');
    Write(record, line_);
  }

  std::unique_ptr<Formatter> formatter_;
  std::string line_;
};

}  // namespace rocket

#endif  // ROCKET_SINK_H_
