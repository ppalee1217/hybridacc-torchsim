add_library(hybridacc_asm STATIC
    instruction.cpp
    utils.cpp
)

target_include_directories(hybridacc_asm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(ha-asm ha_asm.cpp)
target_link_libraries(ha-asm PRIVATE hybridacc_asm)

add_executable(ha-objdump ha_objdump.cpp)
target_link_libraries(ha-objdump PRIVATE hybridacc_asm)

install(TARGETS ha-asm ha-objdump RUNTIME DESTINATION bin)
