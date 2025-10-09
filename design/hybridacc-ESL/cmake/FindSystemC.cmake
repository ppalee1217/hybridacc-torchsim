# FindSystemC.cmake

find_path(SYSTEMC_INCLUDE_DIR NAMES systemc.h PATHS /usr/local/systemc/include /usr/include/systemc)
find_library(SYSTEMC_LIBRARY NAMES systemc PATHS /usr/local/systemc/lib /usr/lib)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SystemC DEFAULT_MSG SYSTEMC_LIBRARY SYSTEMC_INCLUDE_DIR)

if(SYSTEMC_FOUND)
    set(SYSTEMC_INCLUDE_DIRS ${SYSTEMC_INCLUDE_DIR})
    set(SYSTEMC_LIBRARIES ${SYSTEMC_LIBRARY})
else()
    message(FATAL_ERROR "SystemC not found. Please set the SystemC_DIR environment variable or install SystemC.")
endif()