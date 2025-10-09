cmake_minimum_required(VERSION 3.15)
project(hybridacc_ESL CXX)

set(CMAKE_CXX_STANDARD 17)

# 增加自訂 CMake 模組搜尋路徑 (含 FindSystemC.cmake)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

find_package(SystemC QUIET)

# 若未找到，嘗試使用本地 libs/systemc (設計根目錄為 design/ 的上層)
if (NOT SystemC_FOUND)
    message(WARNING "SystemC package not found via find_package; falling back to local libs/systemc")
    set(_LOCAL_SYSTEMC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/systemc)
    if (EXISTS ${_LOCAL_SYSTEMC_ROOT}/include/systemc AND EXISTS ${_LOCAL_SYSTEMC_ROOT}/lib-linux64/libsystemc.so)
        add_library(SystemC::systemc SHARED IMPORTED)
        set_target_properties(SystemC::systemc PROPERTIES
            IMPORTED_LOCATION ${_LOCAL_SYSTEMC_ROOT}/lib-linux64/libsystemc.so
            INTERFACE_INCLUDE_DIRECTORIES ${_LOCAL_SYSTEMC_ROOT}/include
            INTERFACE_LINK_LIBRARIES pthread)
        set(SystemC_FOUND TRUE)
        message(STATUS "Using local SystemC at ${_LOCAL_SYSTEMC_ROOT}")
    else()
        message(FATAL_ERROR "Local SystemC not found at ${_LOCAL_SYSTEMC_ROOT}; run scripts/setup_systemc.sh 或設定 SystemC_DIR")
    endif()
endif()

add_library(hybridacc
    src/pe.cpp
    src/DMA.cpp
    src/array.cpp
    src/DLB.cpp
    src/subcontroller.cpp
    src/topcontroller.cpp
    src/noc.cpp
    src/xbus.cpp
    src/ybus.cpp
    src/controller.cpp
    src/DataLoader.cpp
    src/hybridacc.cpp
)

target_include_directories(hybridacc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if (TARGET SystemC::systemc)
    target_link_libraries(hybridacc PUBLIC SystemC::systemc)
else()
    # 舊變數相容 (若 FindSystemC.cmake 提供)
    target_link_libraries(hybridacc PUBLIC ${SYSTEMC_LIBRARIES})
endif()

# Stub top executable (sc_main) 若未來需要系統層實例化可擴充
add_executable(top src/top.cpp)

if (TARGET SystemC::systemc)
    target_link_libraries(top PRIVATE hybridacc SystemC::systemc)
else()
    target_link_libraries(top PRIVATE hybridacc)
endif()

# 測試 (存在才建立)
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tb/main_tb.cpp)
    add_executable(tb_main tb/main_tb.cpp)
    if (TARGET SystemC::systemc)
        target_link_libraries(tb_main PRIVATE hybridacc SystemC::systemc)
    else()
        target_link_libraries(tb_main PRIVATE hybridacc)
    endif()
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tb/utils_tb.cpp)
    add_executable(tb_utils tb/utils_tb.cpp)
    if (TARGET SystemC::systemc)
        target_link_libraries(tb_utils PRIVATE hybridacc SystemC::systemc)
    else()
        target_link_libraries(tb_utils PRIVATE hybridacc)
    endif()
endif()

# 新增 DataLoader 單元測試 target
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tb/dataloader_tb.cpp)
    add_executable(tb_dataloader tb/dataloader_tb.cpp)
    if (TARGET SystemC::systemc)
        target_link_libraries(tb_dataloader PRIVATE hybridacc SystemC::systemc)
    else()
        target_link_libraries(tb_dataloader PRIVATE hybridacc)
    endif()
endif()