include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(rocket_test rocket_test.cc)
target_link_libraries(rocket_test PRIVATE rocket::rocket GTest::gtest_main)
target_compile_options(
  rocket_test PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic
                      -Werror> $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>)

include(GoogleTest)
gtest_discover_tests(rocket_test)

foreach(case placeholder_count runtime_format)
  add_executable(compile_fail_${case} EXCLUDE_FROM_ALL
                                      compile_fail/${case}.cc)
  target_link_libraries(compile_fail_${case} PRIVATE rocket::rocket)
  add_test(NAME CompileFail.${case}
           COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target
                   compile_fail_${case})
endforeach()
set_tests_properties(
  CompileFail.placeholder_count
  PROPERTIES PASS_REGULAR_EXPRESSION
             "placeholders must match the arguments")
set_tests_properties(CompileFail.runtime_format PROPERTIES WILL_FAIL TRUE)
