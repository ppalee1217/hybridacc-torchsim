cmake_minimum_required(VERSION 3.10)
project(CounterSystemC)

# Set C++ standard to match SystemC compilation
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set SystemC library path
set(SYSTEMC_HOME "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/systemc")

# Use static library instead of shared library
set(SYSTEMC_LIBRARY "${SYSTEMC_HOME}/lib-linux64/libsystemc.a")

if(NOT EXISTS ${SYSTEMC_LIBRARY})
    message(FATAL_ERROR "SystemC library not found at ${SYSTEMC_LIBRARY}")
endif()

# Find pybind11
find_package(pybind11 CONFIG)
if(NOT pybind11_FOUND)
    # Try to find pybind11 using uv environment
    execute_process(
        COMMAND bash -c "cd /home/yoyo/work/MasterResearch/HybridAcc && uv run python -c 'import pybind11; print(pybind11.get_cmake_dir())'"
        OUTPUT_VARIABLE pybind11_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(pybind11_DIR AND NOT pybind11_DIR STREQUAL "")
        find_package(pybind11 PATHS ${pybind11_DIR})
    else()
        # Try alternative method with pip show
        execute_process(
            COMMAND bash -c "cd /home/yoyo/work/MasterResearch/HybridAcc && uv run python -m pybind11 --cmakedir"
            OUTPUT_VARIABLE pybind11_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(pybind11_DIR AND NOT pybind11_DIR STREQUAL "")
            find_package(pybind11 PATHS ${pybind11_DIR})
        endif()
    endif()
endif()

if(NOT pybind11_FOUND)
    message(FATAL_ERROR "pybind11 not found. Please ensure it's installed with: uv add pybind11")
endif()

# Set include directories
include_directories(${SYSTEMC_HOME}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Add compiler flags for SystemC compatibility
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -g -Wno-unused-parameter -DSC_INCLUDE_DYNAMIC_PROCESSES -fPIC")

# Define source files for standalone executable
set(STANDALONE_SOURCES
    counter_test.cpp
)

# Define source files for Python module
set(PYTHON_MODULE_SOURCES
    counter_wrapper.cpp
)

# Create standalone executable (original)
add_executable(counter_test ${STANDALONE_SOURCES})
target_link_libraries(counter_test ${SYSTEMC_LIBRARY} m pthread dl)

# Create Python module
pybind11_add_module(counter_systemc ${PYTHON_MODULE_SOURCES})
target_link_libraries(counter_systemc PRIVATE ${SYSTEMC_LIBRARY} m pthread dl)

# Set properties for Python module
set_target_properties(counter_systemc PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    VISIBILITY_INLINES_HIDDEN ON
)

# Custom targets
add_custom_target(run
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/counter_test
    DEPENDS counter_test
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running counter simulation"
)

add_custom_target(python-install
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:counter_systemc> ${CMAKE_CURRENT_SOURCE_DIR}/
    DEPENDS counter_systemc
    COMMENT "Copy Python module to source directory"
)

add_custom_target(usage
    COMMAND ${CMAKE_COMMAND} -E echo "Available targets:"
    COMMAND ${CMAKE_COMMAND} -E echo "  all           - Build both standalone and Python module"
    COMMAND ${CMAKE_COMMAND} -E echo "  counter_test  - Build standalone executable"
    COMMAND ${CMAKE_COMMAND} -E echo "  counter_systemc - Build Python module"
    COMMAND ${CMAKE_COMMAND} -E echo "  run           - Build and run standalone simulation"
    COMMAND ${CMAKE_COMMAND} -E echo "  python-install - Copy Python module to source directory"
    COMMAND ${CMAKE_COMMAND} -E echo "  clean         - Remove build files"
    COMMAND ${CMAKE_COMMAND} -E echo "  usage         - Show this usage message"
    COMMENT "Showing available targets"
)

# Print configuration info
message(STATUS "SystemC Home: ${SYSTEMC_HOME}")
message(STATUS "SystemC Library: ${SYSTEMC_LIBRARY}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "pybind11 version: ${pybind11_VERSION}")
message(STATUS "Python version: ${PYTHON_VERSION_STRING}")