# ============================================================================
# Shared helpers for parameterized HybridAcc synthesis runs.
# Variables may be provided either via dc_shell -x or inherited environment:
#   CLOCK_PERIOD_NS  Target clock period in ns
#   RUN_TAG          Logical label for the synthesis run
#   SYN_OUT_ROOT     Root directory for netlists/SDF
#   REPORT_OUT_ROOT  Root directory for logs/reports
#   SYN_MAX_CORES    Max DC parallel cores
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

proc hacc_find_project_root {start_dir} {
    set current [file normalize $start_dir]
    while {1} {
        if {[file exists [file join $current src hybridacc_utils_pkg.sv]] &&
            [file exists [file join $current script sdc seq_top.sdc]]} {
            return $current
        }
        set parent [file dirname $current]
        if {$parent eq $current} {
            hacc_fail "Unable to locate HybridAcc RTL project root from $start_dir"
        }
        set current $parent
    }
}

proc hacc_init_paths {} {
    if {[info exists ::HACC_PROJECT_ROOT] && [string trim $::HACC_PROJECT_ROOT] ne ""} {
        return
    }

    set ::HACC_SYN_SCRIPT_DIR [file dirname [file normalize [info script]]]
    set ::HACC_PROJECT_ROOT [hacc_find_project_root $::HACC_SYN_SCRIPT_DIR]
    set ::HACC_SCRIPT_ROOT [file join $::HACC_PROJECT_ROOT script]
    set ::HACC_SDC_ROOT [file join $::HACC_SCRIPT_ROOT sdc]
    set ::HACC_SRC_ROOT [file join $::HACC_PROJECT_ROOT src]
}

proc hacc_src {args} {
    hacc_init_paths
    return [eval file join [list $::HACC_SRC_ROOT] $args]
}

proc hacc_sdc {name} {
    hacc_init_paths
    return [file join $::HACC_SDC_ROOT $name]
}

proc hacc_source_sdc {name} {
    set sdc_file [hacc_sdc $name]
    if {![file exists $sdc_file]} {
        hacc_fail "SDC file not found: $sdc_file"
    }
    source $sdc_file
}

proc hacc_init_run_config {} {
    hacc_init_paths

    hacc_init_var CLOCK_PERIOD_NS 1.0
    if {![string is double -strict $::CLOCK_PERIOD_NS]} {
        hacc_fail "CLOCK_PERIOD_NS must be a positive number, got '$::CLOCK_PERIOD_NS'"
    }
    if {$::CLOCK_PERIOD_NS <= 0.0} {
        hacc_fail "CLOCK_PERIOD_NS must be greater than 0, got '$::CLOCK_PERIOD_NS'"
    }
    set ::clk_period $::CLOCK_PERIOD_NS

    hacc_init_var RUN_TAG default
    hacc_init_var SYN_OUT_ROOT [file join $::HACC_PROJECT_ROOT syn]
    hacc_init_var REPORT_OUT_ROOT [file join $::HACC_PROJECT_ROOT report]
    hacc_init_var SYN_MAX_CORES 8

    if {![string is integer -strict $::SYN_MAX_CORES] || $::SYN_MAX_CORES < 1} {
        hacc_fail "SYN_MAX_CORES must be a positive integer, got '$::SYN_MAX_CORES'"
    }

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
    puts " Max cores   : $::SYN_MAX_CORES"
    puts " Src root    : $::HACC_SRC_ROOT"
    puts " Syn root    : $::SYN_OUT_ROOT"
    puts " Report root : $::REPORT_OUT_ROOT"
}
