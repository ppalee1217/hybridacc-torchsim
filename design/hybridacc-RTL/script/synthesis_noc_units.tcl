# ============================================================================
# NoC Unit Synthesis - Parameterized TCL Script
# Usage: dc_shell -f synthesis_noc_units.tcl -x "set MOD_NAME <name>"
#        or set MOD_NAME before sourcing this script
# ============================================================================

if {![info exists MOD_NAME]} {
    puts "ERROR: MOD_NAME not set. Use: dc_shell -x \"set MOD_NAME <name>\" -f synthesis_noc_units.tcl"
    exit 1
}

# ============================================================================
# Module source file mapping
# ============================================================================
set pkg_file "../src/hybridacc_utils_pkg.sv"

# Source files required for each module (in dependency order)
array set MOD_SRCS {
    MBUS              {../src/NoC/MBUS.sv}
    NoCRouter         {../src/NoC/NoCRouter.sv}
    NetworkOnChip     {../src/FIFO.sv ../src/asyncFIFO.sv ../src/PE/InstructionMemory.sv ../src/PE/Decoder.sv ../src/PE/LoopController.sv ../src/PE/DataMemory.sv ../src/PE/TransformRegFile.sv ../src/PE/PsumRegFile.sv ../src/PE/VMULU.sv ../src/PE/VADDU.sv ../src/PE/LDMA.sv ../src/PE/SDMA.sv ../src/PE/PErouter.sv ../src/PE/IF_ID_Stage.sv ../src/PE/EXE_M_Stage.sv ../src/PE/EXE_A_Stage.sv ../src/PE/ProcessElement.sv ../src/NoC/MBUS.sv ../src/NoC/NoCRouter.sv ../src/NetworkOnChip.sv}
}

# All NoC modules need the utility package
set MOD_NEEDS_PKG {MBUS NoCRouter NetworkOnChip}

# Modules that instantiate SRAM hard macros
set MOD_HAS_SRAM {NetworkOnChip}

# Purely combinational modules
set MOD_COMBINATIONAL {}

# ============================================================================
# Validate module name
# ============================================================================
if {![info exists MOD_SRCS($MOD_NAME)]} {
    puts "ERROR: Unknown module '$MOD_NAME'. Available modules:"
    puts "  [lsort [array names MOD_SRCS]]"
    exit 1
}

puts "============================================================"
puts " Synthesizing NoC unit: $MOD_NAME"
puts "============================================================"

# ============================================================================
# Read source files
# ============================================================================
if {[lsearch -exact $MOD_NEEDS_PKG $MOD_NAME] >= 0} {
    analyze -format sverilog $pkg_file
}

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
# Compile
# ============================================================================
set_host_options -max_core 8

if {[lsearch -exact $MOD_COMBINATIONAL $MOD_NAME] >= 0} {
    puts "INFO: $MOD_NAME is COMBINATIONAL - using DC_comb.sdc (virtual clock)"
    source ../script/DC_comb.sdc
} else {
    puts "INFO: $MOD_NAME is SEQUENTIAL - using DC.sdc (physical clock on port clk)"
    source ../script/DC.sdc
}

check_design
uniquify
set_fix_multiple_port_nets -feedthroughs
set_fix_multiple_port_nets -all -buffer_constants [get_designs *]
set_max_area 0

# Protect SRAM hard macro instances from optimization
if {[lsearch -exact $MOD_HAS_SRAM $MOD_NAME] >= 0} {
    puts "INFO: $MOD_NAME contains SRAM macros - applying set_dont_touch"
    set_dont_touch [get_cells -hierarchical -filter "ref_name =~ TS1N16ADFP*"]
}

if {$MOD_NAME == "NetworkOnChip"} {
    set compile_seqmap_propagate_high_effort true
    set compile_seqmap_propagate_constants true
    set compile_timing_high_effort true
    set compile_ultra_ungroup_dw true
    set compile_ultra_ungroup_small_hierarchies true
    compile_ultra -retime -no_seq_output_inversion -no_autoungroup -exact_map
} else {
    set compile_implementation_selection true
    set compile_seqmap_propagate_constants false
    compile_ultra -retime -no_seq_output_inversion -no_autoungroup -exact_map
    compile_ultra -inc
}

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
