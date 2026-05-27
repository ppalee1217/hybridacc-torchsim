set script_dir [file dirname [file normalize [info script]]]
source [file normalize [file join $script_dir .. common_signoff.tcl]]

hacc_signoff_init_paths
hacc_signoff_init_var RUN_TAG clk_1p25ns
hacc_signoff_init_var CLOCK_PERIOD_NS 1.25
hacc_signoff_init_var TOP_NAME HybridAcc
hacc_signoff_init_var NETLIST_FILE [file join $::HACC_PROJECT_ROOT syn $::RUN_TAG $::TOP_NAME ${TOP_NAME}_syn.v]
hacc_signoff_init_var REPORT_DIR [file join $::HACC_PROJECT_ROOT report primepower $::RUN_TAG]
hacc_signoff_init_var SPEF_FILE ""
hacc_signoff_init_var ACTIVITY_SCOPE tb_hybridacc_sim/dut
hacc_signoff_init_var ACTIVITY_FILE ""
hacc_signoff_init_var ACTIVITY_FORMAT vcd
hacc_signoff_init_var HIERARCHY_LEVELS 4
hacc_signoff_init_var GUI_MODE 0

if {[string trim $::ACTIVITY_FILE] eq ""} {
    if {[info exists ::env(VCD_FILE)] && [string trim $::env(VCD_FILE)] ne ""} {
        set ::ACTIVITY_FILE [string trim $::env(VCD_FILE)]
        set ::ACTIVITY_FORMAT vcd
    } elseif {[info exists ::env(SAIF_FILE)] && [string trim $::env(SAIF_FILE)] ne ""} {
        set ::ACTIVITY_FILE [string trim $::env(SAIF_FILE)]
        set ::ACTIVITY_FORMAT saif
    }
}

set clk_period $::CLOCK_PERIOD_NS
set report_dir [file normalize $::REPORT_DIR]
set netlist [file normalize $::NETLIST_FILE]
set hierarchy_levels [string trim $::HIERARCHY_LEVELS]
file mkdir $report_dir

if {![file exists $netlist]} {
    hacc_signoff_fail "Netlist not found: $netlist"
}

puts "============================================================"
puts " PrimePower gate-level power analysis"
puts " Run tag       : $::RUN_TAG"
puts " Clock (ns)    : $clk_period"
puts " Netlist       : $netlist"
puts " Report dir    : $report_dir"
puts " Activity fmt  : $::ACTIVITY_FORMAT"
puts " Activity file : $::ACTIVITY_FILE"
puts " Activity scope: $::ACTIVITY_SCOPE"
puts " Hier levels   : $hierarchy_levels"
puts "============================================================"

hacc_signoff_setup_libraries
read_verilog $netlist
current_design $::TOP_NAME
link_design $::TOP_NAME
set_app_var power_enable_analysis true
source [file normalize [file join $script_dir .. primetime pt_constraints_hybridacc.tcl]]

if {[string trim $::SPEF_FILE] ne ""} {
    set spef_file [file normalize $::SPEF_FILE]
    if {![file exists $spef_file]} {
        hacc_signoff_fail "SPEF file not found: $spef_file"
    }
    read_parasitics -format spef $spef_file
    set_propagated_clock [all_clocks]
    update_timing
}

if {[string trim $::ACTIVITY_FILE] ne ""} {
    set activity_file [file normalize $::ACTIVITY_FILE]
    if {![file exists $activity_file]} {
        hacc_signoff_fail "Activity file not found: $activity_file"
    }
    set activity_format [string tolower [string trim $::ACTIVITY_FORMAT]]
    switch -- $activity_format {
        vcd {
            read_vcd -strip_path $::ACTIVITY_SCOPE $activity_file
        }
        saif {
            read_saif -input $activity_file -instance_name $::ACTIVITY_SCOPE
        }
        default {
            hacc_signoff_fail "Unsupported ACTIVITY_FORMAT '$::ACTIVITY_FORMAT'. Use vcd or saif."
        }
    }
} else {
    puts "INFO: no ACTIVITY_FILE provided; falling back to vectorless activity"
}

check_power > [file join $report_dir check_power.rpt]
update_power

report_power > [file join $report_dir power_summary.rpt]
report_power -hierarchy -levels $hierarchy_levels > [file join $report_dir power_hierarchy.rpt]
report_switching_activity -list_not_annotated > [file join $report_dir unannotated_activity.rpt]
report_clock_gate_savings > [file join $report_dir clock_gate_savings.rpt]

puts "============================================================"
puts " PrimePower reports written to $report_dir"
puts "============================================================"

hacc_signoff_finish
