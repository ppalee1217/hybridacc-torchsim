cmake_minimum_required(VERSION 3.16)
project(HybridAcc_PE_Tests)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Debug options
option(ENABLE_DEBUG_UTILS "Enable DEBUG_UTILS debug messages" OFF)

# Find SystemC
find_package(PkgConfig REQUIRED)
set(SYSTEMC_ROOT ${CMAKE_SOURCE_DIR}/../../../libs/systemc)

# SystemC include directories
include_directories(${SYSTEMC_ROOT}/include)

# SystemC library directories
link_directories(${SYSTEMC_ROOT}/lib-linux64)

# Include PE simulator headers
include_directories(../simulator/include)

# Create PE simulator library
file(GLOB_RECURSE PE_SIMULATOR_SOURCES "../simulator/src/*.cpp" "../simulator/src/*.cc")
# Remove main.cpp from the library since we only want utility functions
list(FILTER PE_SIMULATOR_SOURCES EXCLUDE REGEX ".*main\\.cpp$")

add_library(pe_simulator STATIC ${PE_SIMULATOR_SOURCES})

# Conditional compile definitions for pe_simulator
if(ENABLE_DEBUG_UTILS)
    target_compile_definitions(pe_simulator PRIVATE DEBUG_UTILS)
    message(STATUS "DEBUG_UTILS: Enabled (PE Simulator)")
else()
    message(STATUS "DEBUG_UTILS: Disabled (PE Simulator)")
endif()

target_include_directories(pe_simulator PUBLIC
    ../simulator/include
    ${SYSTEMC_ROOT}/include
)

target_link_libraries(pe_simulator
    systemc
    pthread
)

# PE Wrapper Library
add_library(pe_wrapper
    pe_wrapper.cpp
)

# Conditional compile definitions for pe_wrapper
if(ENABLE_DEBUG_UTILS)
    target_compile_definitions(pe_wrapper PRIVATE DEBUG_UTILS)
endif()

target_include_directories(pe_wrapper PUBLIC
    .
    ../simulator/include
    ${SYSTEMC_ROOT}/include
)

target_link_libraries(pe_wrapper
    pe_simulator  # 連結 PE simulator 庫
    systemc
    pthread
)

# PE Unit Tests
add_executable(test_pe_unit
    test_pe_unit.cpp
)

# Conditional compile definitions for test_pe_unit
if(ENABLE_DEBUG_UTILS)
    target_compile_definitions(test_pe_unit PRIVATE DEBUG_UTILS)
endif()

target_link_libraries(test_pe_unit
    pe_wrapper
    systemc
    pthread
)

# PE Simulation Test (Conv3x3)
add_executable(test_pe_sim
    test_pe_sim.cpp
)

# Conditional compile definitions for test_pe_sim
if(ENABLE_DEBUG_UTILS)
    target_compile_definitions(test_pe_sim PRIVATE DEBUG_UTILS)
endif()

target_link_libraries(test_pe_sim
    pe_wrapper
    systemc
    pthread
)

# Test runner script
add_custom_target(run_tests
    COMMAND ./test_pe_unit
    DEPENDS test_pe_unit
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running PE unit tests"
)

# Test runner for simulation
add_custom_target(run_sim
    COMMAND ./test_pe_sim
    DEPENDS test_pe_sim
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running PE simulation test (Conv3x3)"
)

# Installation
install(TARGETS pe_wrapper test_pe_unit test_pe_sim
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

# Add test discovery for CTest
enable_testing()
add_test(NAME PE_Unit_Tests COMMAND test_pe_unit)
add_test(NAME PE_Simulation_Conv3x3 COMMAND test_pe_sim)