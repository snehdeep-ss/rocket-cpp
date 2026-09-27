#include <memory>
#include <thread>
#include <vector>

#include "rocket/rocket.h"

int main() {
  rocket::Options options;
  options.name = "orders";
  options.writers = rocket::Writers::kShared;
  rocket::Logger logger(
      options,
      {std::make_shared<rocket::BinaryFileSink>("logs/orders.blog", true),
       std::make_shared<rocket::FileSink>(
           "logs/orders.log", true,
           std::make_unique<rocket::PatternFormatter>(
               "{time} [{level}] [{thread}] {logger} {file}:{line} "
               "{message}"))});

  std::vector<std::thread> workers;
  for (int id = 0; id < 4; ++id) {
    workers.emplace_back([&logger, id] {
      for (int order = 0; order < 25000; ++order) {
        ROCKET_INFO(logger, "worker ", id, " filled order ", order, " at ",
                    101.25 + order * 0.01);
      }
    });
  }
  for (std::thread& worker : workers) worker.join();
  ROCKET_ERROR(logger, "settlement feed disconnected");
  return 0;
}
