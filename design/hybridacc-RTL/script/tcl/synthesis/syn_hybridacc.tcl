# ============================================================================
# Top-level HybridAcc synthesis TCL script.
# This script synthesizes src/HybridAcc.sv with the default RTL parameter set.
# Clock period and output roots are inherited from syn_common.tcl.
# ============================================================================

set script_dir [file dirname [file normalize [info script]]]
source [file join $script_dir syn_common.tcl]
hacc_init_run_config
hacc_init_var TOP_SYN_MAX_CORES $::SYN_MAX_CORES
hacc_init_var TOP_SYN_MAP_EFFORT high
hacc_init_var TOP_SYN_AREA_EFFORT high
hacc_init_var TOP_SYN_ENABLE_IMPL_SELECTION 0
hacc_init_var TOP_SYN_CRITICAL_RANGE ""
hacc_init_var TOP_SYN_MAX_AREA ""

set TOP_NAME HybridAcc
set src_root $::HACC_SRC_ROOT
set pkg_files [list \
    [hacc_src hybridacc_utils_pkg.sv] \
    [hacc_src Core core_pkg.sv] \
    [hacc_src Cluster cluster_pkg.sv]]
set ordered_source_files [list \
    [hacc_src Cluster ScratchpadMemoryBank.sv] \
    [hacc_src Cluster ScratchpadMemory.sv]]
set synth_excluded_files [list \
    [hacc_src utils SRAM_Wrapper.sv]]
set source_patterns [list \
    [file join $src_root *.sv] \
    [file join $src_root utils *.sv] \
    [file join $src_root Core *.sv] \
    [file join $src_root Cluster *.sv] \
    [file join $src_root NoC *.sv] \
    [file join $src_root PE *.sv]]

puts "============================================================"
puts " Synthesizing top-level design: $TOP_NAME"
hacc_print_run_context "Top-level" $TOP_NAME
puts " Compile cfg : cores=$::TOP_SYN_MAX_CORES map=$::TOP_SYN_MAP_EFFORT area=$::TOP_SYN_AREA_EFFORT impl_sel=$::TOP_SYN_ENABLE_IMPL_SELECTION critical_range=$::TOP_SYN_CRITICAL_RANGE max_area=$::TOP_SYN_MAX_AREA"
puts "============================================================"

foreach pkg $pkg_files {
    analyze -format sverilog $pkg
}

set source_files {}
foreach pattern $source_patterns {
    foreach src [lsort [glob -nocomplain $pattern]] {
        if {[lsearch -exact $pkg_files $src] >= 0} {
            continue
        }
        if {[lsearch -exact $ordered_source_files $src] >= 0} {
            continue
        }
        if {[lsearch -exact $synth_excluded_files $src] >= 0} {
            continue
        }
        if {[lsearch -exact $source_files $src] >= 0} {
            continue
        }
        lappend source_files $src
    }
}

foreach src $ordered_source_files {
    analyze -format sverilog $src
}

foreach src $source_files {
    analyze -format sverilog $src
}

elaborate $TOP_NAME
current_design $TOP_NAME
link

set_host_options -max_cores $::TOP_SYN_MAX_CORES
puts "INFO: $TOP_NAME -- sdc/seq_top.sdc (top-level integration)"
hacc_source_sdc seq_top.sdc

check_design
uniquify
set_fix_multiple_port_nets -feedthroughs
set_fix_multiple_port_nets -all -buffer_constants [get_designs *]
if {[string trim $::TOP_SYN_MAX_AREA] ne ""} {
    set_max_area $::TOP_SYN_MAX_AREA
} else {
    puts "INFO: $TOP_NAME -- conservative compile skips set_max_area"
}

set compile_implementation_selection [expr {
    [string trim $::TOP_SYN_ENABLE_IMPL_SELECTION] eq "1" ? "true" : "false"
}]
if {[string trim $::TOP_SYN_CRITICAL_RANGE] ne ""} {
    set_critical_range $::TOP_SYN_CRITICAL_RANGE [current_design]
} else {
    puts "INFO: $TOP_NAME -- conservative compile skips set_critical_range"
}

compile -map_effort $::TOP_SYN_MAP_EFFORT -area_effort $::TOP_SYN_AREA_EFFORT

set output_dirs [hacc_prepare_output_dirs $TOP_NAME]
set rpt_dir [lindex $output_dirs 0]
set syn_dir [lindex $output_dirs 1]

report_timing -path full -delay max -nworst 1 -max_paths 1 -significant_digits 4 -sort_by group \
    > $rpt_dir/timing_max_rpt_${TOP_NAME}.txt

report_timing -path full -delay min -nworst 1 -max_paths 1 -significant_digits 4 -sort_by group \
    > $rpt_dir/timing_min_rpt_${TOP_NAME}.txt

report_area -designware -nosplit \
    > $rpt_dir/area_rpt_${TOP_NAME}.txt

report_power -analysis_effort low \
    > $rpt_dir/power_rpt_${TOP_NAME}.txt

report_cell \
    > $rpt_dir/cell_rpt_${TOP_NAME}.txt

write -hierarchy -format verilog -output $syn_dir/${TOP_NAME}_syn.v
write -format ddc -hierarchy -output $syn_dir/${TOP_NAME}.ddc
write_sdf -version 3.0 -context verilog $syn_dir/${TOP_NAME}.sdf

puts "============================================================"
puts " Synthesis complete: $TOP_NAME"
puts " Reports: $rpt_dir/"
puts " Netlist: $syn_dir/${TOP_NAME}_syn.v"
puts " DDC    : $syn_dir/${TOP_NAME}.ddc"
puts "============================================================"

exit
