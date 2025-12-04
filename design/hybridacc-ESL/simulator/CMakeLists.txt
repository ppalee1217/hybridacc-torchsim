cmake_minimum_required(VERSION 3.10)
project(HybridAccSimulator)

# Set C++ standard
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Debug options
option(ENABLE_DEBUG_UTILS "Enable DEBUG_UTILS debug messages" OFF)
option(ENABLE_DEBUG_PE "Enable DEBUG_PE debug messages for PE modules" OFF)
option(ENABLE_DEBUG_NOC "Enable DEBUG_NOC debug messages for NoC modules" OFF)

# SystemC library path
set(SYSTEMC_HOME "/home/yoyo/work/MasterResearch/HybridAcc/libs/systemc")

# SystemC include directories
include_directories(${SYSTEMC_HOME}/include)

# SystemC library directory
link_directories(${SYSTEMC_HOME}/lib-linux64)

# Find SystemC library
find_library(SYSTEMC_LIBRARY
    NAMES systemc
    PATHS ${SYSTEMC_HOME}/lib-linux64
    NO_DEFAULT_PATH
)

if(NOT SYSTEMC_LIBRARY)
    message(FATAL_ERROR "SystemC library not found in ${SYSTEMC_HOME}/lib-linux64")
endif()

# Include directories for the project
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Collect all source files
file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.cc")
file(GLOB_RECURSE HEADERS "include/*.h" "include/*.hpp")

# Create executable
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

# Conditional compile definitions
if(ENABLE_DEBUG_UTILS)
    target_compile_definitions(${PROJECT_NAME} PRIVATE DEBUG_UTILS)
    message(STATUS "DEBUG_UTILS: Enabled")
else()
    message(STATUS "DEBUG_UTILS: Disabled")
endif()

if(ENABLE_DEBUG_PE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE DEBUG_PE)
    message(STATUS "DEBUG_PE: Enabled")
else()
    message(STATUS "DEBUG_PE: Disabled")
endif()

if(ENABLE_DEBUG_NOC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE DEBUG_NOC)
    message(STATUS "DEBUG_NOC: Enabled")
else()
    message(STATUS "DEBUG_NOC: Disabled")
endif()

# Link SystemC library
target_link_libraries(${PROJECT_NAME} ${SYSTEMC_LIBRARY})

# Compiler flags
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -O2)

# Add pthread library (required by SystemC)
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} Threads::Threads)

# Set output directory
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Optional: Add debug configuration
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(${PROJECT_NAME} PRIVATE DEBUG)
    target_compile_options(${PROJECT_NAME} PRIVATE -g)
endif()

# Print configuration information
message(STATUS "SystemC Home: ${SYSTEMC_HOME}")
message(STATUS "SystemC Library: ${SYSTEMC_LIBRARY}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")