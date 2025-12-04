add_library(hybridacc_packager STATIC
    package.cpp
)

target_include_directories(hybridacc_packager PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hybridacc_packager PUBLIC hybridacc_asm)

add_executable(ha-package ha-package.cpp)
target_link_libraries(ha-package PRIVATE hybridacc_packager hybridacc_asm)

install(TARGETS ha-package RUNTIME DESTINATION bin)
