# ============================================================================
# Top-level HybridAcc synthesis TCL script.
# This script synthesizes src/HybridAcc.sv with the default RTL parameter set.
# Clock period and output roots are inherited from syn_common.tcl.
# ============================================================================

set script_dir [file dirname [file normalize [info script]]]
set project_root [file normalize [file join $script_dir ..]]
set src_root [file join $project_root src]

source [file join $script_dir syn_common.tcl]
hacc_init_run_config

set TOP_NAME HybridAcc
set pkg_files [list \
    [file join $src_root hybridacc_utils_pkg.sv] \
    [file join $src_root Core core_pkg.sv] \
    [file join $src_root Cluster cluster_pkg.sv]]
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
        if {[lsearch -exact $source_files $src] >= 0} {
            continue
        }
        lappend source_files $src
    }
}

foreach src $source_files {
    analyze -format sverilog $src
}

elaborate $TOP_NAME
current_design $TOP_NAME
link

set_host_options -max_core 16
puts "INFO: $TOP_NAME -- sdc/seq_top.sdc (top-level integration)"
source [file join $script_dir sdc seq_top.sdc]

check_design
uniquify
set_fix_multiple_port_nets -feedthroughs
set_fix_multiple_port_nets -all -buffer_constants [get_designs *]

set compile_implementation_selection true
set_critical_range 0.1 [current_design]

compile_ultra

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
write_sdf -version 3.0 -context verilog $syn_dir/${TOP_NAME}.sdf

puts "============================================================"
puts " Synthesis complete: $TOP_NAME"
puts " Reports: $rpt_dir/"
puts " Netlist: $syn_dir/${TOP_NAME}_syn.v"
puts "============================================================"

exit