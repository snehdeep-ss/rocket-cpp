#ifndef ROCKET_SINKS_H_
#define ROCKET_SINKS_H_

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "rocket/formatter.h"
#include "rocket/level.h"
#include "rocket/record.h"
#include "rocket/sink.h"

namespace rocket {
namespace internal {

class BufferedWriter {
 public:
  static constexpr size_t kCapacity = 64 * 1024;

  explicit BufferedWriter(std::FILE* stream = nullptr) : stream_(stream) {
    buffer_.reserve(kCapacity);
  }

  BufferedWriter(const BufferedWriter&) = delete;
  BufferedWriter& operator=(const BufferedWriter&) = delete;

  std::FILE* stream() const { return stream_; }
  void set_stream(std::FILE* stream) { stream_ = stream; }

  void Append(std::string_view data) {
    if (buffer_.size() + data.size() > kCapacity) Drain();
    buffer_.append(data);
  }

  void Drain() {
    if (stream_ != nullptr && !buffer_.empty()) {
      std::fwrite(buffer_.data(), 1, buffer_.size(), stream_);
    }
    buffer_.clear();
  }

  void Flush() {
    Drain();
    if (stream_ != nullptr) std::fflush(stream_);
  }

 private:
  std::FILE* stream_;
  std::string buffer_;
};

class File {
 public:
  File() = default;
  ~File() { Close(); }

  File(const File&) = delete;
  File& operator=(const File&) = delete;

  bool Open(const std::filesystem::path& path, bool truncate) {
    Close();
    std::error_code error;
    if (path.has_parent_path()) {
      std::filesystem::create_directories(path.parent_path(), error);
    }
    writer_.set_stream(
        std::fopen(path.string().c_str(), truncate ? "wb" : "ab"));
    if (!is_open()) return false;
    const auto size = std::filesystem::file_size(path, error);
    size_ = error ? 0 : size;
    return true;
  }

  void Write(std::string_view data) {
    if (!is_open()) return;
    writer_.Append(data);
    size_ += data.size();
  }

  void Commit() { writer_.Drain(); }
  void Flush() { writer_.Flush(); }

  void Close() {
    if (!is_open()) return;
    writer_.Drain();
    std::fclose(writer_.stream());
    writer_.set_stream(nullptr);
  }

  bool is_open() const { return writer_.stream() != nullptr; }
  uintmax_t size() const { return size_; }

 private:
  BufferedWriter writer_;
  uintmax_t size_ = 0;
};

}  // namespace internal

class ConsoleSink : public TextSink {
 public:
  enum class Stream { kStdout, kStderr };

  explicit ConsoleSink(Stream stream = Stream::kStdout, bool color = true,
                       std::unique_ptr<Formatter> formatter = nullptr)
      : TextSink(std::move(formatter)),
        writer_(stream == Stream::kStdout ? stdout : stderr),
        color_(color) {}

  ~ConsoleSink() override { writer_.Flush(); }

 protected:
  void Write(const Record& record, std::string_view line) override {
    if (!color_) {
      writer_.Append(line);
      return;
    }
    writer_.Append(ColorCode(record.level));
    writer_.Append(line.substr(0, line.size() - 1));
    writer_.Append("\x1b[0m\n");
  }

  void DoCommit() override { writer_.Drain(); }
  void DoFlush() override { writer_.Flush(); }

 private:
  static std::string_view ColorCode(Level level) {
    switch (level) {
      case Level::kTrace:
        return "\x1b[90m";
      case Level::kDebug:
        return "\x1b[36m";
      case Level::kInfo:
        return "\x1b[32m";
      case Level::kWarn:
        return "\x1b[33m";
      case Level::kError:
        return "\x1b[31m";
      case Level::kFatal:
        return "\x1b[1;41m";
      case Level::kOff:
        break;
    }
    return "";
  }

  internal::BufferedWriter writer_;
  bool color_;
};

class FileSink : public TextSink {
 public:
  explicit FileSink(const std::filesystem::path& path, bool truncate = false,
                    std::unique_ptr<Formatter> formatter = nullptr)
      : TextSink(std::move(formatter)) {
    file_.Open(path, truncate);
  }

  bool is_open() const { return file_.is_open(); }

 protected:
  void Write(const Record&, std::string_view line) override {
    file_.Write(line);
  }

  void DoCommit() override { file_.Commit(); }
  void DoFlush() override { file_.Flush(); }

 private:
  internal::File file_;
};

class RotatingFileSink : public TextSink {
 public:
  RotatingFileSink(std::filesystem::path path, uintmax_t max_bytes,
                   int max_files,
                   std::unique_ptr<Formatter> formatter = nullptr)
      : TextSink(std::move(formatter)),
        path_(std::move(path)),
        max_bytes_(max_bytes),
        max_files_(max_files) {
    file_.Open(path_, false);
  }

  bool is_open() const { return file_.is_open(); }

 protected:
  void Write(const Record&, std::string_view line) override {
    if (file_.size() > 0 && file_.size() + line.size() > max_bytes_) Rotate();
    file_.Write(line);
  }

  void DoCommit() override { file_.Commit(); }
  void DoFlush() override { file_.Flush(); }

 private:
  std::filesystem::path Backup(int index) const {
    std::filesystem::path backup = path_;
    backup += "." + std::to_string(index);
    return backup;
  }

  void Rotate() {
    file_.Close();
    std::error_code error;
    std::filesystem::remove(Backup(max_files_), error);
    for (int i = max_files_ - 1; i >= 1; --i) {
      std::filesystem::rename(Backup(i), Backup(i + 1), error);
    }
    if (max_files_ > 0) std::filesystem::rename(path_, Backup(1), error);
    file_.Open(path_, true);
  }

  std::filesystem::path path_;
  uintmax_t max_bytes_;
  int max_files_;
  internal::File file_;
};

class CallbackSink : public TextSink {
 public:
  using Callback = std::function<void(const Record&, std::string_view)>;

  explicit CallbackSink(Callback callback,
                        std::unique_ptr<Formatter> formatter = nullptr)
      : TextSink(std::move(formatter)), callback_(std::move(callback)) {}

 protected:
  void Write(const Record& record, std::string_view line) override {
    callback_(record, line);
  }

 private:
  Callback callback_;
};

class NullSink : public Sink {
 protected:
  void Process(const Record&) override {}
};

}  // namespace rocket

#endif  // ROCKET_SINKS_H_
