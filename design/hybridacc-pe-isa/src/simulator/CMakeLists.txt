add_library(hybridacc_sim
    simulator.cpp
    component.cpp
)

target_include_directories(hybridacc_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(hybridacc_sim PUBLIC hybridacc_asm)

add_executable(ha-sim main.cpp)

target_link_libraries(ha-sim PRIVATE hybridacc_sim)

install(TARGETS ha-sim RUNTIME DESTINATION bin)
