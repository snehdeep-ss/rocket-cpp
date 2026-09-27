#include "rocket/rocket.h"

int main() {
  rocket::Logger logger(rocket::Options{}, {});
  ROCKET_INFOF(logger, "two {} placeholders {}", 1);
  return 0;
}
