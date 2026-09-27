#include "rocket/rocket.h"

int main() {
  rocket::Logger logger(rocket::Options{}, {});
  const char* format = "value {}";
  ROCKET_INFOF(logger, format, 1);
  return 0;
}
