cmake_minimum_required(VERSION 3.21)

project(rocket-cpp VERSION 1.0.0 LANGUAGES CXX)

option(ROCKET_BUILD_TESTS "Build rocket-cpp tests" ${PROJECT_IS_TOP_LEVEL})
option(ROCKET_BUILD_EXAMPLES "Build rocket-cpp examples"
       ${PROJECT_IS_TOP_LEVEL})
option(ROCKET_BUILD_TOOLS "Build the rocket_decode tool" ${PROJECT_IS_TOP_LEVEL})

find_package(Threads REQUIRED)

add_library(rocket INTERFACE)
add_library(rocket::rocket ALIAS rocket)
target_include_directories(
  rocket INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                   $<INSTALL_INTERFACE:include>)
target_compile_features(rocket INTERFACE cxx_std_17)
target_link_libraries(rocket INTERFACE Threads::Threads)

include(GNUInstallDirs)
install(TARGETS rocket EXPORT rocketTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
  EXPORT rocketTargets
  NAMESPACE rocket::
  FILE rocketConfig.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rocket)

if(ROCKET_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(ROCKET_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(ROCKET_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
