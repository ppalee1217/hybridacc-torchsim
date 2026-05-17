# ============================================================================
# Cluster Unit Synthesis - Parameterized TCL Script
# Usage: dc_shell -f synthesis_cluster_units.tcl -x "set MOD_NAME <name>"
#        or set MOD_NAME before sourcing this script
# ============================================================================

set script_dir [file dirname [file normalize [info script]]]
source [file join $script_dir syn_common.tcl]
hacc_init_run_config

if {![info exists MOD_NAME]} {
    puts "ERROR: MOD_NAME not set. Use: dc_shell -x \"set MOD_NAME <name>\" -f synthesis_cluster_units.tcl"
    exit 1
}

# ============================================================================
# Module source file mapping
# ============================================================================
set pkg_file "../src/hybridacc_utils_pkg.sv"
set scratchpadmemory_srcs [list ../src/Cluster/AddressGenerateUnit.sv ../src/Cluster/ScratchpadMemoryBank.sv ../src/Cluster/ScratchpadMemory.sv]
set computecluster_srcs [list ../src/FIFO.sv ../src/asyncFIFO.sv ../src/Cluster/AddressGenerateUnit.sv ../src/Cluster/ScratchpadMemoryBank.sv ../src/Cluster/ScratchpadMemory.sv ../src/Cluster/HybridDataDeliverUnit.sv ../src/PE/InstructionMemory.sv ../src/PE/Decoder.sv ../src/PE/LoopController.sv ../src/PE/DataMemory.sv ../src/PE/TransformRegFile.sv ../src/PE/PsumRegFile.sv ../src/PE/VMULU.sv ../src/PE/VADDU.sv ../src/PE/LDMA.sv ../src/PE/SDMA.sv ../src/PE/PErouter.sv ../src/PE/IF_ID_Stage.sv ../src/PE/EXE_M_Stage.sv ../src/PE/EXE_A_Stage.sv ../src/PE/ProcessElement.sv ../src/NoC/MBUS.sv ../src/NoC/NoCRouter.sv ../src/NetworkOnChip.sv ../src/Cluster/ComputeCluster.sv]

# Source files required for each module (in dependency order)
array set MOD_SRCS {
    AddressGenerateUnit     {../src/Cluster/AddressGenerateUnit.sv}
    ScratchpadMemory        {}
    HybridDataDeliverUnit   {../src/FIFO.sv ../src/Cluster/AddressGenerateUnit.sv ../src/Cluster/HybridDataDeliverUnit.sv}
    ComputeCluster          {}
}

set MOD_SRCS(ScratchpadMemory) $scratchpadmemory_srcs
set MOD_SRCS(ComputeCluster) $computecluster_srcs

# Modules that need the utility package
set MOD_NEEDS_PKG {AddressGenerateUnit ScratchpadMemory HybridDataDeliverUnit ComputeCluster}

# Modules that instantiate SRAM hard macros
set MOD_HAS_SRAM {ScratchpadMemory ComputeCluster}

# Purely combinational modules
set MOD_COMBINATIONAL {AddressGenerateUnit}

# Integration-level modules
set MOD_INTEGRATION {ComputeCluster}

# ============================================================================
# Validate module name
# ============================================================================
if {![info exists MOD_SRCS($MOD_NAME)]} {
    puts "ERROR: Unknown module '$MOD_NAME'. Available modules:"
    puts "  [lsort [array names MOD_SRCS]]"
    exit 1
}

puts "============================================================"
puts " Synthesizing Cluster unit: $MOD_NAME"
hacc_print_run_context "Cluster unit" $MOD_NAME
puts " Compile cfg : cores=$::SYN_MAX_CORES"
puts "============================================================"

# ============================================================================
# Read source files
# ============================================================================
set output_dirs [hacc_prepare_output_dirs $MOD_NAME]
set rpt_dir [lindex $output_dirs 0]
set syn_dir [lindex $output_dirs 1]
foreach src $MOD_SRCS($MOD_NAME) {
    analyze -format sverilog $src
}

# ============================================================================
# Elaborate and link
# ============================================================================
elaborate $MOD_NAME
current_design $MOD_NAME
link

# ============================================================================
# Constraints
# ============================================================================
set_host_options -max_cores $::SYN_MAX_CORES

# SDC selection: combinational / unit / integration
if {[lsearch -exact $MOD_COMBINATIONAL $MOD_NAME] >= 0} {
    puts "INFO: $MOD_NAME -- sdc/comb.sdc (virtual clock)"
    source ../script/sdc/comb.sdc
} elseif {[lsearch -exact $MOD_INTEGRATION $MOD_NAME] >= 0} {
    puts "INFO: $MOD_NAME -- sdc/seq_top.sdc (integration, 50% input budget)"
    source ../script/sdc/seq_top.sdc
} else {
    puts "INFO: $MOD_NAME -- sdc/seq_unit.sdc (unit, 30% input budget)"
    source ../script/sdc/seq_unit.sdc
}

# Exclude async reset from timing analysis (sequential modules only)
# if {[lsearch -exact $MOD_COMBINATIONAL $MOD_NAME] < 0} {
#     if {[sizeof_collection [get_ports reset_n]] > 0} {
#         puts "INFO: Applying false path from reset_n"
#         set_false_path -from [get_ports reset_n]
#     }
# }

check_design
uniquify
set_fix_multiple_port_nets -feedthroughs
set_fix_multiple_port_nets -all -buffer_constants [get_designs *]

# Protect SRAM hard macro instances from optimization
# if {[lsearch -exact $MOD_HAS_SRAM $MOD_NAME] >= 0} {
#     puts "INFO: $MOD_NAME contains SRAM macros -- applying set_dont_touch"
#     set_dont_touch [get_cells -hierarchical -filter "ref_name =~ TS1N16ADFP*"]
# }

# ============================================================================
# Compile
# ============================================================================
set compile_implementation_selection true
set_critical_range 0.1 [current_design]

# if {[lsearch -exact $MOD_INTEGRATION $MOD_NAME] >= 0} {
#     puts "INFO: $MOD_NAME -- compile_ultra -gate_clock + 2x -inc (integration)"
#     compile_ultra -gate_clock
#     compile_ultra -inc
#     compile_ultra -inc
# } else {
#     puts "INFO: $MOD_NAME -- compile_ultra"
#     compile_ultra
# }

compile_ultra -gate_clock
optimize_netlist -area

# ============================================================================
# Reports
# ============================================================================
set rpt_dir ../report/$MOD_NAME
set syn_dir ../syn/$MOD_NAME
file mkdir $rpt_dir
file mkdir $syn_dir

report_timing -path full -delay max -nworst 1 -max_paths 1 -significant_digits 4 -sort_by group \
    > $rpt_dir/timing_max_rpt_${MOD_NAME}.txt

report_timing -path full -delay min -nworst 1 -max_paths 1 -significant_digits 4 -sort_by group \
    > $rpt_dir/timing_min_rpt_${MOD_NAME}.txt

report_area -designware -nosplit \
    > $rpt_dir/area_rpt_${MOD_NAME}.txt

report_power -analysis_effort low \
    > $rpt_dir/power_rpt_${MOD_NAME}.txt

report_cell \
    > $rpt_dir/cell_rpt_${MOD_NAME}.txt

# ============================================================================
# Write outputs
# ============================================================================
write -hierarchy -format verilog -output $syn_dir/${MOD_NAME}_syn.v
write_sdf -version 3.0 -context verilog $syn_dir/${MOD_NAME}.sdf

puts "============================================================"
puts " Synthesis complete: $MOD_NAME"
puts " Reports: $rpt_dir/"
puts " Netlist: $syn_dir/${MOD_NAME}_syn.v"
puts "============================================================"

exit
