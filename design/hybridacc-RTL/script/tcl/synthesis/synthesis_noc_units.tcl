# ============================================================================
# NoC Unit Synthesis - Parameterized TCL Script
# Usage: dc_shell -f synthesis_noc_units.tcl -x "set MOD_NAME <name>"
#        or set MOD_NAME before sourcing this script
# ============================================================================

set script_dir [file dirname [file normalize [info script]]]
source [file join $script_dir syn_common.tcl]
hacc_init_run_config

if {![info exists MOD_NAME]} {
    puts "ERROR: MOD_NAME not set. Use: dc_shell -x \"set MOD_NAME <name>\" -f synthesis_noc_units.tcl"
    exit 1
}

set pkg_file [hacc_src hybridacc_utils_pkg.sv]
set networkonchip_srcs [list \
    [hacc_src FIFO.sv] \
    [hacc_src asyncFIFO.sv] \
    [hacc_src PE InstructionMemory.sv] \
    [hacc_src PE Decoder.sv] \
    [hacc_src PE LoopController.sv] \
    [hacc_src PE DataMemory.sv] \
    [hacc_src PE TransformRegFile.sv] \
    [hacc_src PE PsumRegFile.sv] \
    [hacc_src PE VMULU.sv] \
    [hacc_src PE VADDU.sv] \
    [hacc_src PE LDMA.sv] \
    [hacc_src PE SDMA.sv] \
    [hacc_src PE PErouter.sv] \
    [hacc_src PE IF_ID_Stage.sv] \
    [hacc_src PE EXE_M_Stage.sv] \
    [hacc_src PE EXE_A_Stage.sv] \
    [hacc_src PE ProcessElement.sv] \
    [hacc_src NoC MBUS.sv] \
    [hacc_src NoC NoCRouter.sv] \
    [hacc_src NetworkOnChip.sv]]

array set MOD_SRCS {
    MBUS              {}
    NoCRouter         {}
    NetworkOnChip     {}
}

set MOD_SRCS(MBUS) [list [hacc_src NoC MBUS.sv]]
set MOD_SRCS(NoCRouter) [list [hacc_src NoC NoCRouter.sv]]
set MOD_SRCS(NetworkOnChip) $networkonchip_srcs

set MOD_NEEDS_PKG {MBUS NoCRouter NetworkOnChip}
set MOD_HAS_SRAM {NetworkOnChip}
set MOD_COMBINATIONAL {}
set MOD_INTEGRATION {NetworkOnChip}

if {![info exists MOD_SRCS($MOD_NAME)]} {
    puts "ERROR: Unknown module '$MOD_NAME'. Available modules:"
    puts "  [lsort [array names MOD_SRCS]]"
    exit 1
}

puts "============================================================"
puts " Synthesizing NoC unit: $MOD_NAME"
hacc_print_run_context "NoC unit" $MOD_NAME
puts " Compile cfg : cores=$::SYN_MAX_CORES"
puts "============================================================"

if {[lsearch -exact $MOD_NEEDS_PKG $MOD_NAME] >= 0} {
    analyze -format sverilog $pkg_file
}
foreach src $MOD_SRCS($MOD_NAME) {
    analyze -format sverilog $src
}

elaborate $MOD_NAME
current_design $MOD_NAME
link

set_host_options -max_cores $::SYN_MAX_CORES
if {[lsearch -exact $MOD_COMBINATIONAL $MOD_NAME] >= 0} {
    puts "INFO: $MOD_NAME -- sdc/comb.sdc (virtual clock)"
    hacc_source_sdc comb.sdc
} elseif {[lsearch -exact $MOD_INTEGRATION $MOD_NAME] >= 0} {
    puts "INFO: $MOD_NAME -- sdc/seq_top.sdc (integration, 50% input budget)"
    hacc_source_sdc seq_top.sdc
} else {
    puts "INFO: $MOD_NAME -- sdc/seq_unit.sdc (unit, 30% input budget)"
    hacc_source_sdc seq_unit.sdc
}

check_design
uniquify
set_fix_multiple_port_nets -feedthroughs
set_fix_multiple_port_nets -all -buffer_constants [get_designs *]

set compile_implementation_selection true
set_critical_range 0.1 [current_design]

if {[lsearch -exact $MOD_INTEGRATION $MOD_NAME] >= 0} {
    puts "INFO: $MOD_NAME -- compile -map_effort high -area_effort high (integration)"
    compile -map_effort high -area_effort high
} elseif {[lsearch -exact $MOD_HAS_SRAM $MOD_NAME] >= 0} {
    puts "INFO: $MOD_NAME -- compile -map_effort high -area_effort high (SRAM unit)"
    compile -map_effort high -area_effort high
} else {
    puts "INFO: $MOD_NAME -- compile + compile_ultra -inc + optimize_netlist -area"
    compile -map_effort high -area_effort high
    compile_ultra -inc
    optimize_netlist -area
}

set output_dirs [hacc_prepare_output_dirs $MOD_NAME]
set rpt_dir [lindex $output_dirs 0]
set syn_dir [lindex $output_dirs 1]

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

write -hierarchy -format verilog -output $syn_dir/${MOD_NAME}_syn.v
write_sdf -version 3.0 -context verilog $syn_dir/${MOD_NAME}.sdf

puts "============================================================"
puts " Synthesis complete: $MOD_NAME"
puts " Reports: $rpt_dir/"
puts " Netlist: $syn_dir/${MOD_NAME}_syn.v"
puts "============================================================"

exit
