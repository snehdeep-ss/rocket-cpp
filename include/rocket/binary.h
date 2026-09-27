#ifndef ROCKET_BINARY_H_
#define ROCKET_BINARY_H_

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rocket/level.h"
#include "rocket/record.h"
#include "rocket/sink.h"
#include "rocket/sinks.h"

namespace rocket {
namespace internal {

constexpr std::string_view kBinaryMagic = "ROCKETB";
constexpr uint8_t kBinaryVersion = 1;
constexpr uint64_t kMaxBinaryField = uint64_t{1} << 30;

enum class BinaryTag : uint8_t { kSession = 0, kString = 1, kRecord = 2 };

inline void PutVarint(std::string& out, uint64_t value) {
  while (value >= 0x80) {
    out.push_back(static_cast<char>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<char>(value));
}

inline uint64_t ZigZagEncode(int64_t value) {
  return (static_cast<uint64_t>(value) << 1) ^
         static_cast<uint64_t>(value >> 63);
}

inline int64_t ZigZagDecode(uint64_t value) {
  return static_cast<int64_t>(value >> 1) ^ -static_cast<int64_t>(value & 1);
}

inline int64_t ToNanoseconds(std::chrono::system_clock::time_point time) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             time.time_since_epoch())
      .count();
}

inline std::chrono::system_clock::time_point FromNanoseconds(int64_t nanos) {
  return std::chrono::system_clock::time_point(
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          std::chrono::nanoseconds(nanos)));
}

}  // namespace internal

class BinaryFileSink : public Sink {
 public:
  explicit BinaryFileSink(const std::filesystem::path& path,
                          bool truncate = false) {
    if (!file_.Open(path, truncate)) return;
    buffer_.push_back(static_cast<char>(internal::BinaryTag::kSession));
    buffer_.append(internal::kBinaryMagic);
    buffer_.push_back(static_cast<char>(internal::kBinaryVersion));
    file_.Write(buffer_);
  }

  bool is_open() const { return file_.is_open(); }

 protected:
  void Process(const Record& record) override {
    if (!file_.is_open()) return;
    buffer_.clear();
    const uint64_t logger = Intern(record.logger_name);
    const uint64_t file = Intern(record.location.file);
    const int64_t nanos = internal::ToNanoseconds(record.time);
    buffer_.push_back(static_cast<char>(internal::BinaryTag::kRecord));
    buffer_.push_back(static_cast<char>(record.level));
    internal::PutVarint(buffer_, internal::ZigZagEncode(nanos - last_nanos_));
    internal::PutVarint(buffer_, record.thread_id);
    internal::PutVarint(buffer_, logger);
    internal::PutVarint(buffer_, file);
    internal::PutVarint(buffer_, static_cast<uint32_t>(record.location.line));
    internal::PutVarint(buffer_, record.message.size());
    buffer_.append(record.message);
    file_.Write(buffer_);
    last_nanos_ = nanos;
  }

  void DoCommit() override { file_.Commit(); }
  void DoFlush() override { file_.Flush(); }

 private:
  uint64_t Intern(std::string_view value) {
    const auto found = ids_.find(value);
    if (found != ids_.end()) return found->second;
    const uint64_t id = strings_.size();
    const std::string& stored = strings_.emplace_back(value);
    ids_.emplace(stored, id);
    buffer_.push_back(static_cast<char>(internal::BinaryTag::kString));
    internal::PutVarint(buffer_, id);
    internal::PutVarint(buffer_, stored.size());
    buffer_.append(stored);
    return id;
  }

  internal::File file_;
  std::string buffer_;
  std::deque<std::string> strings_;
  std::unordered_map<std::string_view, uint64_t> ids_;
  int64_t last_nanos_ = 0;
};

class BinaryReader {
 public:
  enum class Status { kOk, kTruncated, kCorrupt };

  explicit BinaryReader(const std::filesystem::path& path)
      : file_(std::fopen(path.string().c_str(), "rb")), buffer_(kBufferSize) {}

  ~BinaryReader() {
    if (file_ != nullptr) std::fclose(file_);
  }

  BinaryReader(const BinaryReader&) = delete;
  BinaryReader& operator=(const BinaryReader&) = delete;

  bool is_open() const { return file_ != nullptr; }
  Status status() const { return status_; }

  bool Next(Record& record) {
    if (file_ == nullptr || status_ != Status::kOk) return false;
    uint8_t tag = 0;
    while (ReadByte(tag)) {
      switch (static_cast<internal::BinaryTag>(tag)) {
        case internal::BinaryTag::kSession:
          if (!ReadSession()) return false;
          break;
        case internal::BinaryTag::kString:
          if (!ReadString()) return false;
          break;
        case internal::BinaryTag::kRecord:
          return ReadRecord(record);
        default:
          return Fail(Status::kCorrupt);
      }
    }
    return false;
  }

 private:
  static constexpr size_t kBufferSize = 64 * 1024;

  bool Fail(Status status) {
    status_ = status;
    return false;
  }

  bool Refill() {
    size_ = std::fread(buffer_.data(), 1, buffer_.size(), file_);
    position_ = 0;
    return size_ > 0;
  }

  bool ReadByte(uint8_t& byte) {
    if (position_ == size_ && !Refill()) return false;
    byte = static_cast<uint8_t>(buffer_[position_++]);
    return true;
  }

  bool ReadBytes(std::string& out, uint64_t count) {
    out.clear();
    while (count > 0) {
      if (position_ == size_ && !Refill()) return false;
      const size_t take =
          static_cast<size_t>(std::min<uint64_t>(count, size_ - position_));
      out.append(buffer_.data() + position_, take);
      position_ += take;
      count -= take;
    }
    return true;
  }

  bool ReadVarint(uint64_t& value) {
    value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      uint8_t byte = 0;
      if (!ReadByte(byte)) return Fail(Status::kTruncated);
      value |= static_cast<uint64_t>(byte & 0x7f) << shift;
      if ((byte & 0x80) == 0) return true;
    }
    return Fail(Status::kCorrupt);
  }

  bool ReadSession() {
    std::string magic;
    if (!ReadBytes(magic, internal::kBinaryMagic.size() + 1)) {
      return Fail(Status::kTruncated);
    }
    if (std::string_view(magic).substr(0, internal::kBinaryMagic.size()) !=
            internal::kBinaryMagic ||
        static_cast<uint8_t>(magic.back()) != internal::kBinaryVersion) {
      return Fail(Status::kCorrupt);
    }
    strings_.clear();
    last_nanos_ = 0;
    in_session_ = true;
    return true;
  }

  bool ReadString() {
    uint64_t id = 0;
    uint64_t length = 0;
    if (!ReadVarint(id) || !ReadVarint(length)) return false;
    if (!in_session_ || id != strings_.size() ||
        length > internal::kMaxBinaryField) {
      return Fail(Status::kCorrupt);
    }
    std::string& value = strings_.emplace_back();
    return ReadBytes(value, length) || Fail(Status::kTruncated);
  }

  bool ReadRecord(Record& record) {
    uint8_t level = 0;
    uint64_t delta = 0;
    uint64_t thread = 0;
    uint64_t logger = 0;
    uint64_t file = 0;
    uint64_t line = 0;
    uint64_t length = 0;
    if (!ReadByte(level)) return Fail(Status::kTruncated);
    if (!ReadVarint(delta) || !ReadVarint(thread) || !ReadVarint(logger) ||
        !ReadVarint(file) || !ReadVarint(line) || !ReadVarint(length)) {
      return false;
    }
    if (!in_session_ || level > static_cast<uint8_t>(Level::kOff) ||
        logger >= strings_.size() || file >= strings_.size() ||
        length > internal::kMaxBinaryField) {
      return Fail(Status::kCorrupt);
    }
    if (!ReadBytes(message_, length)) return Fail(Status::kTruncated);
    last_nanos_ += internal::ZigZagDecode(delta);
    record.level = static_cast<Level>(level);
    record.time = internal::FromNanoseconds(last_nanos_);
    record.thread_id = static_cast<uint32_t>(thread);
    record.logger_name = strings_[logger];
    record.location = {strings_[file].c_str(), static_cast<int>(line)};
    record.message = message_;
    return true;
  }

  std::FILE* file_;
  std::vector<char> buffer_;
  size_t position_ = 0;
  size_t size_ = 0;
  Status status_ = Status::kOk;
  bool in_session_ = false;
  std::deque<std::string> strings_;
  std::string message_;
  int64_t last_nanos_ = 0;
};

}  // namespace rocket

#endif  // ROCKET_BINARY_H_
