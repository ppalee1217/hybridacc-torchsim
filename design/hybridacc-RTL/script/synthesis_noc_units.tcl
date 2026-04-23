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

# Integration-level modules
set MOD_INTEGRATION {NetworkOnChip}

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
# Constraints
# ============================================================================
set_host_options -max_core 8

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

# # Exclude async reset from timing analysis (sequential modules only)
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

#     set hold_delay_cells [get_cells -quiet -hierarchical -filter "full_name =~ *u_dm_write_hold_inv*"]
#     set addr_delay_cells [get_cells -quiet -hierarchical -filter "full_name =~ *u_sram_addr_hold_inv*"]
#     if {[sizeof_collection $addr_delay_cells] > 0} {
#         set hold_delay_cells [add_to_collection $hold_delay_cells $addr_delay_cells]
#     }
#     if {[sizeof_collection $hold_delay_cells] > 0} {
#         puts "INFO: $MOD_NAME contains explicit SRAM hold-delay cells -- preserving them"
#         set_dont_touch $hold_delay_cells
#     }
# }

# ============================================================================
# Compile
# ============================================================================
set compile_implementation_selection true
set_critical_range 0.1 [current_design]


# Compile
# ============================================================================
set compile_implementation_selection true
set_critical_range 0.1 [current_design]

if {[lsearch -exact $MOD_INTEGRATION $MOD_NAME] >= 0} {
    puts "INFO: $MOD_NAME -- compile_ultra -gate_clock + 2x -inc"
    compile -map_effort high -area_effort high
    # Do NOT run optimize_netlist -area -- it removes hold-fix buffers.
} elseif {[lsearch -exact $MOD_HAS_SRAM $MOD_NAME] >= 0} {
    puts "INFO: $MOD_NAME -- compile_ultra + -inc (SRAM unit)"
    compile -map_effort high -area_effort high
} else {
    puts "INFO: $MOD_NAME -- compile"
    compile -map_effort high -area_effort high
    # try
    compile_ultra -inc
    optimize_netlist -area
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
