# CMake generated Testfile for 
# Source directory: /home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test
# Build directory: /home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/build-core-rv32i
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(SRAM_Unit_Tests "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/build-core-rv32i/test_sram_unit")
set_tests_properties(SRAM_Unit_Tests PROPERTIES  _BACKTRACE_TRIPLES "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;141;add_test;/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;0;")
add_test(ScratchpadMemory_Unit_Tests "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/build-core-rv32i/test_spm_unit")
set_tests_properties(ScratchpadMemory_Unit_Tests PROPERTIES  _BACKTRACE_TRIPLES "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;166;add_test;/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;0;")
add_test(PE_Unit_Tests "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/build-core-rv32i/test_pe_unit")
set_tests_properties(PE_Unit_Tests PROPERTIES  _BACKTRACE_TRIPLES "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;209;add_test;/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;0;")
add_test(PE_Simulation_Conv3x3 "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/build-core-rv32i/test_pe_sim")
set_tests_properties(PE_Simulation_Conv3x3 PROPERTIES  _BACKTRACE_TRIPLES "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;210;add_test;/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;0;")
add_test(NoC_Unit_Tests "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/build-core-rv32i/test_noc_unit")
set_tests_properties(NoC_Unit_Tests PROPERTIES  _BACKTRACE_TRIPLES "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;211;add_test;/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;0;")
add_test(NoC_Simulation "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/build-core-rv32i/test_noc_sim")
set_tests_properties(NoC_Simulation PROPERTIES  _BACKTRACE_TRIPLES "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;239;add_test;/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;0;")
add_test(AGU_Unit_Tests "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/build-core-rv32i/test_agu_unit")
set_tests_properties(AGU_Unit_Tests PROPERTIES  _BACKTRACE_TRIPLES "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;264;add_test;/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;0;")
add_test(HDDU_Unit_Tests "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/build-core-rv32i/test_hddu_unit")
set_tests_properties(HDDU_Unit_Tests PROPERTIES  _BACKTRACE_TRIPLES "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;289;add_test;/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;0;")
add_test(ComputeCluster_Unit_Tests "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/build-core-rv32i/test_cluster_unit")
set_tests_properties(ComputeCluster_Unit_Tests PROPERTIES  _BACKTRACE_TRIPLES "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;315;add_test;/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;0;")
add_test(ComputeCluster_Simulation "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/build-core-rv32i/test_cluster_sim")
set_tests_properties(ComputeCluster_Simulation PROPERTIES  _BACKTRACE_TRIPLES "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;406;add_test;/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;0;")
add_test(ComputeCluster_Simulation_Advanced "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/build-core-rv32i/test_cluster_sim_advanced")
set_tests_properties(ComputeCluster_Simulation_Advanced PROPERTIES  _BACKTRACE_TRIPLES "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;407;add_test;/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;0;")
add_test(HACC_Core_Unit_Tests "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/build-core-rv32i/test_core_unit")
set_tests_properties(HACC_Core_Unit_Tests PROPERTIES  _BACKTRACE_TRIPLES "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;408;add_test;/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;0;")
add_test(HACC_Core_Controller_Integration "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/build-core-rv32i/test_core_controller_integration")
set_tests_properties(HACC_Core_Controller_Integration PROPERTIES  _BACKTRACE_TRIPLES "/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;409;add_test;/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/test/CMakeLists.txt;0;")
