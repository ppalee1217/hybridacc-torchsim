set script_dir [file dirname [file normalize [info script]]]
source [file normalize [file join $script_dir .. common_signoff.tcl]]

hacc_signoff_init_paths
hacc_signoff_init_var RUN_TAG clk_1p25ns
hacc_signoff_init_var CLOCK_PERIOD_NS 1.25
hacc_signoff_init_var TOP_NAME HybridAcc
hacc_signoff_init_var NETLIST_FILE [file join $::HACC_PROJECT_ROOT syn $::RUN_TAG $::TOP_NAME ${TOP_NAME}_syn.v]
hacc_signoff_init_var REPORT_DIR [file join $::HACC_PROJECT_ROOT report primetime $::RUN_TAG]
hacc_signoff_init_var SPEF_FILE ""
hacc_signoff_init_var GUI_MODE 0

set clk_period $::CLOCK_PERIOD_NS
set report_dir [file normalize $::REPORT_DIR]
set netlist [file normalize $::NETLIST_FILE]
file mkdir $report_dir

if {![file exists $netlist]} {
    hacc_signoff_fail "Netlist not found: $netlist"
}

puts "============================================================"
puts " PrimeTime standalone STA"
puts " Run tag    : $::RUN_TAG"
puts " Clock (ns) : $clk_period"
puts " Netlist    : $netlist"
puts " Report dir : $report_dir"
puts "============================================================"

hacc_signoff_setup_libraries
read_verilog $netlist
current_design $::TOP_NAME
link_design $::TOP_NAME
source [file join $script_dir pt_constraints_hybridacc.tcl]

if {[string trim $::SPEF_FILE] ne ""} {
    set spef_file [file normalize $::SPEF_FILE]
    if {![file exists $spef_file]} {
        hacc_signoff_fail "SPEF file not found: $spef_file"
    }
    read_parasitics -format spef $spef_file
    set_propagated_clock [all_clocks]
}

update_timing

check_timing > [file join $report_dir check_timing.rpt]
report_analysis_coverage > [file join $report_dir analysis_coverage.rpt]
report_constraint -all_violators > [file join $report_dir constraint_violators.rpt]

report_timing -delay_type max -path_type full_clock_expanded \
    -max_paths 10 -nworst 3 > [file join $report_dir timing_max.rpt]

report_timing -delay_type min -path_type full_clock_expanded \
    -max_paths 10 -nworst 3 > [file join $report_dir timing_min.rpt]

report_clock > [file join $report_dir clocks.rpt]

puts "============================================================"
puts " PrimeTime reports written to $report_dir"
puts "============================================================"

hacc_signoff_finish
