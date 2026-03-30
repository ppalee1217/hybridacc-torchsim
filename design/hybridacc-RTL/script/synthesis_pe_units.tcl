# ============================================================================
# PE Unit Synthesis - Parameterized TCL Script
# Usage: dc_shell -f synthesis_pe_units.tcl -x "set MOD_NAME <name>"
#        or set MOD_NAME before sourcing this script
# ============================================================================

if {![info exists MOD_NAME]} {
    puts "ERROR: MOD_NAME not set. Use: dc_shell -x \"set MOD_NAME <name>\" -f synthesis_pe_units.tcl"
    exit 1
}

# ============================================================================
# Module source file mapping
# ============================================================================
set pkg_file "../src/hybridacc_utils_pkg.sv"

# Source files required for each module (in dependency order)
array set MOD_SRCS {
    DataMemory          {../src/PE/DataMemory.sv}
    Decoder             {../src/PE/Decoder.sv}
    InstructionMemory   {../src/PE/InstructionMemory.sv}
    LoopController      {../src/PE/LoopController.sv}
    PsumRegFile         {../src/PE/PsumRegFile.sv}
    TransformRegFile    {../src/PE/TransformRegFile.sv}
    VADDU               {../src/PE/VADDU.sv}
    VMULU               {../src/PE/VMULU.sv}
    LDMA                {../src/PE/LDMA.sv}
    SDMA                {../src/PE/SDMA.sv}
    IF_ID_Stage         {../src/PE/InstructionMemory.sv ../src/PE/Decoder.sv ../src/PE/LoopController.sv ../src/PE/IF_ID_Stage.sv}
    EXE_M_Stage         {../src/PE/TransformRegFile.sv ../src/PE/VMULU.sv ../src/PE/LDMA.sv ../src/PE/SDMA.sv ../src/PE/DataMemory.sv ../src/PE/EXE_M_Stage.sv}
    EXE_A_Stage         {../src/PE/VADDU.sv ../src/PE/PsumRegFile.sv ../src/PE/EXE_A_Stage.sv}
    PErouter            {../src/FIFO.sv ../src/asyncFIFO.sv ../src/PE/PErouter.sv}
    ProcessElement      {../src/FIFO.sv ../src/asyncFIFO.sv ../src/PE/InstructionMemory.sv ../src/PE/Decoder.sv ../src/PE/LoopController.sv ../src/PE/DataMemory.sv ../src/PE/TransformRegFile.sv ../src/PE/PsumRegFile.sv ../src/PE/VMULU.sv ../src/PE/VADDU.sv ../src/PE/LDMA.sv ../src/PE/SDMA.sv ../src/PE/PErouter.sv ../src/PE/IF_ID_Stage.sv ../src/PE/EXE_M_Stage.sv ../src/PE/EXE_A_Stage.sv ../src/PE/ProcessElement.sv}
}

# Modules that instantiate SRAM hard macros (prevent DC from optimizing them)
set MOD_HAS_SRAM {DataMemory EXE_M_Stage ProcessElement}

# Modules that need the utility package
set MOD_NEEDS_PKG {Decoder EXE_A_Stage EXE_M_Stage IF_ID_Stage LDMA SDMA PErouter ProcessElement PsumRegFile TransformRegFile VADDU VMULU}

# Purely combinational modules (no clk port, no registers)
# → use virtual-clock SDC (DC_comb.sdc)
# All other modules are sequential → use physical-clock SDC (DC.sdc)
set MOD_COMBINATIONAL {Decoder VMULU VADDU}

# ============================================================================
# Validate module name
# ============================================================================
if {![info exists MOD_SRCS($MOD_NAME)]} {
    puts "ERROR: Unknown module '$MOD_NAME'. Available modules:"
    puts "  [lsort [array names MOD_SRCS]]"
    exit 1
}

puts "============================================================"
puts " Synthesizing PE unit: $MOD_NAME"
puts "============================================================"

# ============================================================================
# Read source files
#   Use analyze (parse only) + elaborate (build with actual parameters).
#   read_file would immediately elaborate with default parameters, causing
#   unresolved references for parameterized sub-modules (FIFO, asyncFIFO).
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

# Select SDC based on module type: combinational → virtual clock, sequential → physical clock
if {[lsearch -exact $MOD_COMBINATIONAL $MOD_NAME] >= 0} {
    puts "INFO: $MOD_NAME is COMBINATIONAL — using DC_comb.sdc (virtual clock)"
    source ../script/DC_comb.sdc
} else {
    puts "INFO: $MOD_NAME is SEQUENTIAL — using DC.sdc (physical clock on port clk)"
    source ../script/DC.sdc
}

check_design
uniquify
set_fix_multiple_port_nets -feedthroughs
set_fix_multiple_port_nets -all -buffer_constants [get_designs *]
set_max_area 0

# Protect SRAM hard macro instances from optimization
if {[lsearch -exact $MOD_HAS_SRAM $MOD_NAME] >= 0} {
    puts "INFO: $MOD_NAME contains SRAM macros — applying set_dont_touch"
    set_dont_touch [get_cells -hierarchical -filter "ref_name =~ TS1N16ADFP*"]
}

set compile_implementation_selection true
set compile_seqmap_propagate_constants false

compile_ultra -retime -no_seq_output_inversion -no_autoungroup -exact_map
compile_ultra -inc

# compile_ultra -timing_high_effort_script
# compile_ultra -timing_high_effort_script -inc

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
