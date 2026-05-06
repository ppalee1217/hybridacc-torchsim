# ============================================================================
# Shared helpers for parameterized HybridAcc synthesis runs.
# Variables may be provided either via dc_shell -x or inherited environment:
#   CLOCK_PERIOD_NS  Target clock period in ns
#   RUN_TAG          Logical label for the synthesis run
#   SYN_OUT_ROOT     Root directory for netlists/SDF
#   REPORT_OUT_ROOT  Root directory for logs/reports
# ============================================================================

proc hacc_fail {message} {
    puts "ERROR: $message"
    exit 1
}

proc hacc_init_var {name default} {
    if {[info exists ::$name] && [string trim [set ::$name]] ne ""} {
        return
    }
    if {[info exists ::env($name)] && [string trim $::env($name)] ne ""} {
        set ::$name [string trim $::env($name)]
        return
    }
    set ::$name $default
}

proc hacc_require_var {name usage} {
    hacc_init_var $name ""
    if {[string trim [set ::$name]] eq ""} {
        hacc_fail "$name not set. $usage"
    }
}

proc hacc_init_run_config {} {
    hacc_init_var CLOCK_PERIOD_NS 1.0
    if {![string is double -strict $::CLOCK_PERIOD_NS]} {
        hacc_fail "CLOCK_PERIOD_NS must be a positive number, got '$::CLOCK_PERIOD_NS'"
    }
    if {$::CLOCK_PERIOD_NS <= 0.0} {
        hacc_fail "CLOCK_PERIOD_NS must be greater than 0, got '$::CLOCK_PERIOD_NS'"
    }
    set ::clk_period $::CLOCK_PERIOD_NS

    hacc_init_var RUN_TAG default
    hacc_init_var SYN_OUT_ROOT "../syn"
    hacc_init_var REPORT_OUT_ROOT "../report"

    set ::SYN_OUT_ROOT [file normalize $::SYN_OUT_ROOT]
    set ::REPORT_OUT_ROOT [file normalize $::REPORT_OUT_ROOT]
    file mkdir $::SYN_OUT_ROOT
    file mkdir $::REPORT_OUT_ROOT
}

proc hacc_prepare_output_dirs {design_name} {
    set rpt_dir [file join $::REPORT_OUT_ROOT $design_name]
    set syn_dir [file join $::SYN_OUT_ROOT $design_name]
    file mkdir $rpt_dir
    file mkdir $syn_dir
    return [list $rpt_dir $syn_dir]
}

proc hacc_print_run_context {design_kind design_name} {
    puts " Design kind : $design_kind"
    puts " Design name : $design_name"
    puts " Run tag     : $::RUN_TAG"
    puts " Clock (ns)  : $::clk_period"
    puts " Syn root    : $::SYN_OUT_ROOT"
    puts " Report root : $::REPORT_OUT_ROOT"
}