cmake_minimum_required(VERSION 3.10)
project(HybridAccSimulator)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Debug options
option(ENABLE_DEBUG_UTILS "Enable DEBUG_UTILS debug messages" OFF)
option(ENABLE_DEBUG_PE "Enable DEBUG_PE debug messages for PE modules" OFF)
option(ENABLE_DEBUG_NOC "Enable DEBUG_NOC debug messages for NoC modules" OFF)

# Find SystemC
find_package(PkgConfig REQUIRED)
set(SYSTEMC_ROOT "$ENV{SYSTEMC_LIB}")
message(STATUS "systemC Root: ${SYSTEMC_ROOT}")

# SystemC include directories
include_directories(${SYSTEMC_ROOT}/include)

# SystemC library directories
link_directories(${SYSTEMC_ROOT}/lib64 ${SYSTEMC_ROOT}/lib)

# Find SystemC library
find_library(SYSTEMC_LIBRARY
    NAMES systemc
    PATHS ${SYSTEMC_ROOT}/lib64 ${SYSTEMC_ROOT}/lib
    NO_DEFAULT_PATH
)

if(NOT SYSTEMC_LIBRARY)
    message(FATAL_ERROR "SystemC library not found in ${SYSTEMC_ROOT}/lib64 or ${SYSTEMC_ROOT}/lib")
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
    # DEBUG_LEVEL_MIN: numeric value matching DebugLevel enum in utils.hpp
    # 0=NONE 1=PE_COMPONENTS 2=PE_STAGE 3=PE_TOP 4=NOC_COMPONENTS
    # 5=NOC_TOP 6=CLUSTER_COMPONENTS 7=CLUSTER_TOP 8=CORE_COMPONENTS 9=CORE_TOP 10=ALL
    if(DEFINED DEBUG_LEVEL_MIN)
        target_compile_definitions(${PROJECT_NAME} PRIVATE "DEBUG_LEVEL_MIN=${DEBUG_LEVEL_MIN}")
        message(STATUS "DEBUG_LEVEL_MIN: ${DEBUG_LEVEL_MIN}")
    endif()
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

# Set output name and directory
set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME hybridacc-sim
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Optional: Add debug configuration
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # Note: Do NOT define -DDEBUG here — it conflicts with SectionKind::DEBUG enum in Types.hpp
    target_compile_options(${PROJECT_NAME} PRIVATE -g)
endif()

# Print configuration information
message(STATUS "SystemC Home: ${SYSTEMC_HOME}")
message(STATUS "SystemC Library: ${SYSTEMC_LIBRARY}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")