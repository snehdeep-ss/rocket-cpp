add_executable(rocket_decode rocket_decode.cc)
target_link_libraries(rocket_decode PRIVATE rocket::rocket)
target_compile_options(
  rocket_decode PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic
                        -Werror> $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>)
install(TARGETS rocket_decode)
