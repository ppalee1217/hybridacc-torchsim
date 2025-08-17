add_executable(test_assembler test_assembler.cpp)
target_link_libraries(test_assembler PRIVATE hybridacc_asm)
add_test(NAME assembler COMMAND test_assembler)

add_executable(test_valu_fp16 test_valu_fp16.cpp)
target_link_libraries(test_valu_fp16 PRIVATE hybridacc_sim)
add_test(NAME valu_fp16 COMMAND test_valu_fp16)