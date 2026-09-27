foreach(example basic benchmark binary)
  add_executable(${example} ${example}.cc)
  target_link_libraries(${example} PRIVATE rocket::rocket)
  target_compile_options(
    ${example} PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic
                       -Werror> $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>)
endforeach()
