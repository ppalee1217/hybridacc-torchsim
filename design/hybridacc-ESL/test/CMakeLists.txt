cmake_minimum_required(VERSION 3.16)
project(HybridAcc_PE_Tests)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization flags for Release build
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Debug options
option(ENABLE_DEBUG_UTILS "Enable DEBUG_UTILS debug messages" OFF)
set(DEBUG_LEVEL_MIN "0" CACHE STRING "Minimum debug level")
set(DEBUG_LEVEL_MAX "6" CACHE STRING "Maximum debug level")

# Find SystemC
find_package(PkgConfig REQUIRED)
set(SYSTEMC_ROOT "$ENV{SYSTEMC_LIB}")
message(STATUS "systemC Root: ${SYSTEMC_ROOT}")

# SystemC include directories
include_directories(${SYSTEMC_ROOT}/include)

# SystemC library directories
link_directories(${SYSTEMC_ROOT}/lib64)

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
    target_compile_definitions(pe_simulator PRIVATE DEBUG_LEVEL_MIN=${DEBUG_LEVEL_MIN})
    target_compile_definitions(pe_simulator PRIVATE DEBUG_LEVEL_MAX=${DEBUG_LEVEL_MAX})
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

# Test Bench Utils (Header Only)
add_library(tb_utils INTERFACE)
target_include_directories(tb_utils INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# PE Unit Tests
add_executable(test_pe_unit
    test_pe_unit.cpp
)

# Conditional compile definitions for test_pe_unit
if(ENABLE_DEBUG_UTILS)
    target_compile_definitions(test_pe_unit PRIVATE DEBUG_UTILS)
    target_compile_definitions(test_pe_unit PRIVATE DEBUG_LEVEL_MIN=${DEBUG_LEVEL_MIN})
    target_compile_definitions(test_pe_unit PRIVATE DEBUG_LEVEL_MAX=${DEBUG_LEVEL_MAX})
endif()

target_link_libraries(test_pe_unit
    pe_simulator
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
    target_compile_definitions(test_pe_sim PRIVATE DEBUG_LEVEL_MIN=${DEBUG_LEVEL_MIN})
    target_compile_definitions(test_pe_sim PRIVATE DEBUG_LEVEL_MAX=${DEBUG_LEVEL_MAX})
endif()

target_link_libraries(test_pe_sim
    pe_simulator
    systemc
    pthread
    tb_utils
)

# NoC Unit Tests
add_executable(test_noc_unit
    test_noc_unit.cpp
)

# Conditional compile definitions for test_noc_unit
if(ENABLE_DEBUG_UTILS)
    target_compile_definitions(test_noc_unit PRIVATE DEBUG_UTILS)
    target_compile_definitions(test_noc_unit PRIVATE DEBUG_LEVEL_MIN=${DEBUG_LEVEL_MIN})
    target_compile_definitions(test_noc_unit PRIVATE DEBUG_LEVEL_MAX=${DEBUG_LEVEL_MAX})
endif()

target_link_libraries(test_noc_unit
    pe_simulator
    systemc
    pthread
)

# SRAM Unit Tests
add_executable(test_sram_unit
    test_sram_unit.cpp
)

# Conditional compile definitions for test_sram_unit
if(ENABLE_DEBUG_UTILS)
    target_compile_definitions(test_sram_unit PRIVATE DEBUG_UTILS)
    target_compile_definitions(test_sram_unit PRIVATE DEBUG_LEVEL_MIN=${DEBUG_LEVEL_MIN})
    target_compile_definitions(test_sram_unit PRIVATE DEBUG_LEVEL_MAX=${DEBUG_LEVEL_MAX})
endif()

target_link_libraries(test_sram_unit
    systemc
    pthread
)

# Test runner for SRAM
add_custom_target(run_sram_test
    COMMAND ./test_sram_unit
    DEPENDS test_sram_unit
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running SRAM unit test"
)

add_test(NAME SRAM_Unit_Tests COMMAND test_sram_unit)

# ScratchpadMemory Unit Tests
add_executable(test_spm_unit
    test_spm_unit.cpp
)

if(ENABLE_DEBUG_UTILS)
    target_compile_definitions(test_spm_unit PRIVATE DEBUG_UTILS)
    target_compile_definitions(test_spm_unit PRIVATE DEBUG_LEVEL_MIN=${DEBUG_LEVEL_MIN})
    target_compile_definitions(test_spm_unit PRIVATE DEBUG_LEVEL_MAX=${DEBUG_LEVEL_MAX})
endif()

target_link_libraries(test_spm_unit
    systemc
    pthread
)

add_custom_target(run_spm_test
    COMMAND ./test_spm_unit
    DEPENDS test_spm_unit
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running ScratchpadMemory unit test"
)

add_test(NAME ScratchpadMemory_Unit_Tests COMMAND test_spm_unit)

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

# Test runner for NoC
add_custom_target(run_noc_test
    COMMAND ./test_noc_unit
    DEPENDS test_noc_unit
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running NetworkOnChip unit tests"
)

# Run all tests
add_custom_target(run_all_tests
    COMMAND ./test_pe_unit && ./test_noc_unit && ./test_sram_unit && ./test_spm_unit
    DEPENDS test_pe_unit test_noc_unit test_sram_unit test_spm_unit
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running all unit tests"
)

# Installation
install(TARGETS test_pe_unit test_pe_sim test_noc_unit
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

# Add test discovery for CTest
enable_testing()
add_test(NAME PE_Unit_Tests COMMAND test_pe_unit)
add_test(NAME PE_Simulation_Conv3x3 COMMAND test_pe_sim)
add_test(NAME NoC_Unit_Tests COMMAND test_noc_unit)
# NoC Simulation Test
add_executable(test_noc_sim
    test_noc_sim.cpp
)

# Conditional compile definitions for test_noc_sim
if(ENABLE_DEBUG_UTILS)
    target_compile_definitions(test_noc_sim PRIVATE DEBUG_UTILS)
    target_compile_definitions(test_noc_sim PRIVATE DEBUG_LEVEL_MIN=${DEBUG_LEVEL_MIN})
    target_compile_definitions(test_noc_sim PRIVATE DEBUG_LEVEL_MAX=${DEBUG_LEVEL_MAX})
endif()

target_link_libraries(test_noc_sim
    pe_simulator
    systemc
    pthread
    tb_utils
)

# Test runner for NoC Simulation
add_custom_target(run_noc_sim
    COMMAND ./test_noc_sim
    DEPENDS test_noc_sim
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running NoC simulation test"
)

add_test(NAME NoC_Simulation COMMAND test_noc_sim)

# AGU Unit Tests
add_executable(test_agu_unit
    test_agu_unit.cpp
)

if(ENABLE_DEBUG_UTILS)
    target_compile_definitions(test_agu_unit PRIVATE DEBUG_UTILS)
    target_compile_definitions(test_agu_unit PRIVATE DEBUG_LEVEL_MIN=${DEBUG_LEVEL_MIN})
    target_compile_definitions(test_agu_unit PRIVATE DEBUG_LEVEL_MAX=${DEBUG_LEVEL_MAX})
endif()

target_link_libraries(test_agu_unit
    systemc
    pthread
)

add_custom_target(run_agu_test
    COMMAND ./test_agu_unit
    DEPENDS test_agu_unit
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running AGU unit test"
)

add_test(NAME AGU_Unit_Tests COMMAND test_agu_unit)

# HDDU Unit Tests
add_executable(test_hddu_unit
    test_hddu_unit.cpp
)

if(ENABLE_DEBUG_UTILS)
    target_compile_definitions(test_hddu_unit PRIVATE DEBUG_UTILS)
    target_compile_definitions(test_hddu_unit PRIVATE DEBUG_LEVEL_MIN=${DEBUG_LEVEL_MIN})
    target_compile_definitions(test_hddu_unit PRIVATE DEBUG_LEVEL_MAX=${DEBUG_LEVEL_MAX})
endif()

target_link_libraries(test_hddu_unit
    systemc
    pthread
)

add_custom_target(run_hddu_test
    COMMAND ./test_hddu_unit
    DEPENDS test_hddu_unit
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running HDDU unit test"
)

add_test(NAME HDDU_Unit_Tests COMMAND test_hddu_unit)

# ComputeCluster Unit Tests
add_executable(test_cluster_unit
    test_cluster_unit.cpp
)

if(ENABLE_DEBUG_UTILS)
    target_compile_definitions(test_cluster_unit PRIVATE DEBUG_UTILS)
    target_compile_definitions(test_cluster_unit PRIVATE DEBUG_LEVEL_MIN=${DEBUG_LEVEL_MIN})
    target_compile_definitions(test_cluster_unit PRIVATE DEBUG_LEVEL_MAX=${DEBUG_LEVEL_MAX})
endif()

target_link_libraries(test_cluster_unit
    pe_simulator
    systemc
    pthread
)

add_custom_target(run_cluster_unit_test
    COMMAND ./test_cluster_unit
    DEPENDS test_cluster_unit
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running ComputeCluster unit test"
)

add_test(NAME ComputeCluster_Unit_Tests COMMAND test_cluster_unit)

# ComputeCluster Simulation
add_executable(test_cluster_sim
    test_cluster_sim.cpp
    mvp_compiler.hpp
)

# Core Controller Unit Tests
add_executable(test_core_unit
    core_unit_tests/test_core_unit.cpp
)

add_executable(test_core_controller_integration
    core_unit_tests/test_core_controller_integration.cpp
)

add_executable(test_cluster_sim_advanced
    test_cluster_sim_advanced.cpp
)

if(ENABLE_DEBUG_UTILS)
    target_compile_definitions(test_cluster_sim PRIVATE DEBUG_UTILS)
    target_compile_definitions(test_cluster_sim PRIVATE DEBUG_LEVEL_MIN=${DEBUG_LEVEL_MIN})
    target_compile_definitions(test_cluster_sim PRIVATE DEBUG_LEVEL_MAX=${DEBUG_LEVEL_MAX})
    target_compile_definitions(test_cluster_sim_advanced PRIVATE DEBUG_UTILS)
    target_compile_definitions(test_cluster_sim_advanced PRIVATE DEBUG_LEVEL_MIN=${DEBUG_LEVEL_MIN})
    target_compile_definitions(test_cluster_sim_advanced PRIVATE DEBUG_LEVEL_MAX=${DEBUG_LEVEL_MAX})
    target_compile_definitions(test_core_unit PRIVATE DEBUG_UTILS)
    target_compile_definitions(test_core_unit PRIVATE DEBUG_LEVEL_MIN=${DEBUG_LEVEL_MIN})
    target_compile_definitions(test_core_unit PRIVATE DEBUG_LEVEL_MAX=${DEBUG_LEVEL_MAX})
    target_compile_definitions(test_core_controller_integration PRIVATE DEBUG_UTILS)
    target_compile_definitions(test_core_controller_integration PRIVATE DEBUG_LEVEL_MIN=${DEBUG_LEVEL_MIN})
    target_compile_definitions(test_core_controller_integration PRIVATE DEBUG_LEVEL_MAX=${DEBUG_LEVEL_MAX})
endif()

target_link_libraries(test_core_unit
    systemc
    pthread
)

target_link_libraries(test_core_controller_integration
    pe_simulator
    systemc
    pthread
)

target_link_libraries(test_cluster_sim
    pe_simulator
    systemc
    pthread
    tb_utils
    stdc++fs
)

target_link_libraries(test_cluster_sim_advanced
    pe_simulator
    systemc
    pthread
    tb_utils
    stdc++fs
)

add_custom_target(run_cluster_sim
    COMMAND ./test_cluster_sim
    DEPENDS test_cluster_sim
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running ComputeCluster simulation"
)

add_custom_target(run_cluster_sim_advanced
    COMMAND ./test_cluster_sim_advanced
    DEPENDS test_cluster_sim_advanced
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running advanced ComputeCluster simulation"
)

add_custom_target(run_core_unit_test
    COMMAND ./test_core_unit
    DEPENDS test_core_unit
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running HACC core controller unit test"
)

add_custom_target(run_core_controller_integration
    COMMAND ./test_core_controller_integration
    DEPENDS test_core_controller_integration
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running HACC core controller integration test"
)

add_test(NAME ComputeCluster_Simulation COMMAND test_cluster_sim)
add_test(NAME ComputeCluster_Simulation_Advanced COMMAND test_cluster_sim_advanced)
add_test(NAME HACC_Core_Unit_Tests COMMAND test_core_unit)
add_test(NAME HACC_Core_Controller_Integration COMMAND test_core_controller_integration)

# Core DMA Simulation (DMA DRAM ↔ Cluster SPM tests)
add_executable(test_core_sim
    test_core_sim.cpp
)

if(ENABLE_DEBUG_UTILS)
    target_compile_definitions(test_core_sim PRIVATE DEBUG_UTILS)
    target_compile_definitions(test_core_sim PRIVATE DEBUG_LEVEL_MIN=${DEBUG_LEVEL_MIN})
    target_compile_definitions(test_core_sim PRIVATE DEBUG_LEVEL_MAX=${DEBUG_LEVEL_MAX})
endif()

target_link_libraries(test_core_sim
    pe_simulator
    systemc
    pthread
    tb_utils
    stdc++fs
)

add_custom_target(run_core_sim
    COMMAND ./test_core_sim
    DEPENDS test_core_sim
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running Core DMA simulation"
)

add_test(NAME HACC_Core_DMA_Simulation COMMAND test_core_sim)
