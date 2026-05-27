# ============================================================================
# PE Unit Synthesis - Parameterized TCL Script
# Usage: dc_shell -f synthesis_pe_units.tcl -x "set MOD_NAME <name>"
#        or set MOD_NAME before sourcing this script
# ============================================================================

set script_dir [file dirname [file normalize [info script]]]
source [file join $script_dir syn_common.tcl]
hacc_init_run_config

if {![info exists MOD_NAME]} {
    puts "ERROR: MOD_NAME not set. Use: dc_shell -x \"set MOD_NAME <name>\" -f synthesis_pe_units.tcl"
    exit 1
}

set pkg_file [hacc_src hybridacc_utils_pkg.sv]
set datamemory_srcs [list [hacc_src PE DataMemory.sv]]
set exe_m_stage_srcs [list \
    [hacc_src PE TransformRegFile.sv] \
    [hacc_src PE VMULU.sv] \
    [hacc_src PE LDMA.sv] \
    [hacc_src PE SDMA.sv] \
    [hacc_src PE DataMemory.sv] \
    [hacc_src PE EXE_M_Stage.sv]]
set processelement_srcs [list \
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
    [hacc_src PE ProcessElement.sv]]

array set MOD_SRCS {
    DataMemory          {}
    Decoder             {}
    InstructionMemory   {}
    LoopController      {}
    PsumRegFile         {}
    TransformRegFile    {}
    VADDU               {}
    VMULU               {}
    LDMA                {}
    SDMA                {}
    IF_ID_Stage         {}
    EXE_M_Stage         {}
    EXE_A_Stage         {}
    PErouter            {}
    ProcessElement      {}
}

set MOD_SRCS(DataMemory) $datamemory_srcs
set MOD_SRCS(Decoder) [list [hacc_src PE Decoder.sv]]
set MOD_SRCS(InstructionMemory) [list [hacc_src PE InstructionMemory.sv]]
set MOD_SRCS(LoopController) [list [hacc_src PE LoopController.sv]]
set MOD_SRCS(PsumRegFile) [list [hacc_src PE PsumRegFile.sv]]
set MOD_SRCS(TransformRegFile) [list [hacc_src PE TransformRegFile.sv]]
set MOD_SRCS(VADDU) [list [hacc_src PE VADDU.sv]]
set MOD_SRCS(VMULU) [list [hacc_src PE VMULU.sv]]
set MOD_SRCS(LDMA) [list [hacc_src PE LDMA.sv]]
set MOD_SRCS(SDMA) [list [hacc_src PE SDMA.sv]]
set MOD_SRCS(IF_ID_Stage) [list \
    [hacc_src PE InstructionMemory.sv] \
    [hacc_src PE Decoder.sv] \
    [hacc_src PE LoopController.sv] \
    [hacc_src PE IF_ID_Stage.sv]]
set MOD_SRCS(EXE_M_Stage) $exe_m_stage_srcs
set MOD_SRCS(EXE_A_Stage) [list \
    [hacc_src PE VADDU.sv] \
    [hacc_src PE PsumRegFile.sv] \
    [hacc_src PE EXE_A_Stage.sv]]
set MOD_SRCS(PErouter) [list \
    [hacc_src FIFO.sv] \
    [hacc_src asyncFIFO.sv] \
    [hacc_src PE PErouter.sv]]
set MOD_SRCS(ProcessElement) $processelement_srcs

set MOD_NEEDS_PKG {Decoder EXE_A_Stage EXE_M_Stage IF_ID_Stage LDMA SDMA PErouter ProcessElement PsumRegFile TransformRegFile VADDU VMULU}
set MOD_COMBINATIONAL {Decoder VMULU VADDU}
set MOD_INTEGRATION {ProcessElement}

if {![info exists MOD_SRCS($MOD_NAME)]} {
    puts "ERROR: Unknown module '$MOD_NAME'. Available modules:"
    puts "  [lsort [array names MOD_SRCS]]"
    exit 1
}

puts "============================================================"
puts " Synthesizing PE unit: $MOD_NAME"
hacc_print_run_context "PE unit" $MOD_NAME
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
set_max_area 0

compile -map_effort high -area_effort high

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
