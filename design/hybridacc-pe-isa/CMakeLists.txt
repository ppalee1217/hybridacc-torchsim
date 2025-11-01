# CMake build for HybridAcc ISA tools
cmake_minimum_required(VERSION 3.15)
project(HybridAccISA CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Build tests" ON)

add_subdirectory(src/assambler)

# 啟用 PE 模擬器 (原本被註解)
add_subdirectory(src/simulator)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
