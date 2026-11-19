cmake_minimum_required(VERSION 3.25)

project(Silex VERSION 0.1.0 DESCRIPTION "Open-source parametric 3D CAD" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(SILEX_BUILD_APP   "Build the Qt6 desktop application" ON)
option(SILEX_BUILD_TESTS "Build the geometric regression tests" ON)

if(MSVC)
  add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /MP /bigobj)
  add_compile_definitions(NOMINMAX _USE_MATH_DEFINES WIN32_LEAN_AND_MEAN)
endif()

add_subdirectory(third_party/planegcs)
add_subdirectory(src/model)
add_subdirectory(src/sketch)
add_subdirectory(src/kernel)

if(SILEX_BUILD_APP)
  add_subdirectory(src/app)
endif()

if(SILEX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
