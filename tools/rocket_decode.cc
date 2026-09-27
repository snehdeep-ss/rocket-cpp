#include <cstdio>
#include <string>
#include <string_view>

#include "rocket/binary.h"
#include "rocket/formatter.h"
#include "rocket/record.h"

namespace {

constexpr std::string_view kUsage =
    "usage: rocket_decode [--pattern PATTERN] [--utc] INPUT.blog [OUTPUT]\n";

int Usage() {
  std::fwrite(kUsage.data(), 1, kUsage.size(), stderr);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::string_view pattern = rocket::PatternFormatter::kDefaultPattern;
  rocket::PatternFormatter::Clock clock =
      rocket::PatternFormatter::Clock::kLocal;
  const char* input = nullptr;
  const char* output = nullptr;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--pattern" && i + 1 < argc) {
      pattern = argv[++i];
    } else if (argument == "--utc") {
      clock = rocket::PatternFormatter::Clock::kUtc;
    } else if (argument.substr(0, 2) == "--") {
      return Usage();
    } else if (input == nullptr) {
      input = argv[i];
    } else if (output == nullptr) {
      output = argv[i];
    } else {
      return Usage();
    }
  }
  if (input == nullptr) return Usage();

  rocket::BinaryReader reader(input);
  if (!reader.is_open()) {
    std::fprintf(stderr, "rocket_decode: cannot open %s\n", input);
    return 1;
  }
  std::FILE* sink = output == nullptr ? stdout : std::fopen(output, "wb");
  if (sink == nullptr) {
    std::fprintf(stderr, "rocket_decode: cannot create %s\n", output);
    return 1;
  }

  const rocket::PatternFormatter formatter(pattern, clock);
  rocket::Record record;
  std::string line;
  uint64_t count = 0;
  while (reader.Next(record)) {
    line.clear();
    formatter.Format(record, line);
    line.push_back('\n');
    std::fwrite(line.data(), 1, line.size(), sink);
    ++count;
  }
  if (sink != stdout) std::fclose(sink);

  switch (reader.status()) {
    case rocket::BinaryReader::Status::kOk:
      return 0;
    case rocket::BinaryReader::Status::kTruncated:
      std::fprintf(stderr,
                   "rocket_decode: %s is truncated after %llu records\n", input,
                   static_cast<unsigned long long>(count));
      return 2;
    case rocket::BinaryReader::Status::kCorrupt:
      std::fprintf(stderr, "rocket_decode: %s is corrupt after %llu records\n",
                   input, static_cast<unsigned long long>(count));
      return 2;
  }
  return 2;
}
