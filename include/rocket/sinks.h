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
    handle_ = std::fopen(path.string().c_str(), truncate ? "wb" : "ab");
    if (handle_ == nullptr) return false;
    const auto size = std::filesystem::file_size(path, error);
    size_ = error ? 0 : size;
    return true;
  }

  void Write(std::string_view data) {
    if (handle_ == nullptr) return;
    size_ += std::fwrite(data.data(), 1, data.size(), handle_);
  }

  void Flush() {
    if (handle_ != nullptr) std::fflush(handle_);
  }

  void Close() {
    if (handle_ != nullptr) std::fclose(handle_);
    handle_ = nullptr;
  }

  bool is_open() const { return handle_ != nullptr; }
  uintmax_t size() const { return size_; }

 private:
  std::FILE* handle_ = nullptr;
  uintmax_t size_ = 0;
};

}  // namespace internal

class ConsoleSink : public Sink {
 public:
  enum class Stream { kStdout, kStderr };

  explicit ConsoleSink(Stream stream = Stream::kStdout, bool color = true,
                       std::unique_ptr<Formatter> formatter = nullptr)
      : Sink(std::move(formatter)),
        stream_(stream == Stream::kStdout ? stdout : stderr),
        color_(color) {}

 protected:
  void Write(const Record& record, std::string_view line) override {
    if (!color_) {
      std::fwrite(line.data(), 1, line.size(), stream_);
      return;
    }
    colored_.assign(ColorCode(record.level));
    colored_.append(line.substr(0, line.size() - 1));
    colored_.append("\x1b[0m\n");
    std::fwrite(colored_.data(), 1, colored_.size(), stream_);
  }

  void DoFlush() override { std::fflush(stream_); }

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

  std::FILE* stream_;
  bool color_;
  std::string colored_;
};

class FileSink : public Sink {
 public:
  explicit FileSink(const std::filesystem::path& path, bool truncate = false,
                    std::unique_ptr<Formatter> formatter = nullptr)
      : Sink(std::move(formatter)) {
    file_.Open(path, truncate);
  }

  bool is_open() const { return file_.is_open(); }

 protected:
  void Write(const Record&, std::string_view line) override {
    file_.Write(line);
  }

  void DoFlush() override { file_.Flush(); }

 private:
  internal::File file_;
};

class RotatingFileSink : public Sink {
 public:
  RotatingFileSink(std::filesystem::path path, uintmax_t max_bytes,
                   int max_files,
                   std::unique_ptr<Formatter> formatter = nullptr)
      : Sink(std::move(formatter)),
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

class CallbackSink : public Sink {
 public:
  using Callback = std::function<void(const Record&, std::string_view)>;

  explicit CallbackSink(Callback callback,
                        std::unique_ptr<Formatter> formatter = nullptr)
      : Sink(std::move(formatter)), callback_(std::move(callback)) {}

 protected:
  void Write(const Record& record, std::string_view line) override {
    callback_(record, line);
  }

 private:
  Callback callback_;
};

class NullSink : public Sink {
 protected:
  void Write(const Record&, std::string_view) override {}
};

}  // namespace rocket

#endif  // ROCKET_SINKS_H_
