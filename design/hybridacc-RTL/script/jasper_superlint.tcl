# ============================================================================
# JasperGold Superlint RTL flow for HybridAcc.
#
# Run with:
#   jg -superlint -allow_unsupported_OS design/hybridacc-RTL/script/jasper_superlint.tcl
# Run with no GUI:
#   jg -superlint -allow_unsupported_OS -batch design/hybridacc-RTL/script/jasper_superlint.tcl
#
# Optional environment variables:
#   RUN_TAG                  Logical run label. Default: default
#   HACC_JG_PROJECT_ROOT     RTL project root. Default: auto-detect from script path or cwd
#   HACC_JG_TOP              Top module. Default: HybridAcc
#   HACC_JG_RUN_DIR          Run directory. Default: <rtl_root>/jasper/superlint_<RUN_TAG>
#   HACC_JG_CLOCK            Clock signal name, or NONE. Default: clk
#   HACC_JG_RESET_EXPR       Reset expression, or NONE. Default: {reset_n == 1'b0}
#   HACC_JG_BBOX_A           Elaborate -bbox_a value. Default: 1024
#   HACC_JG_USE_DW_STUBS     Optional 0/1. Default: 1. Compile Jasper-only DW_fp_add/DW_fp_mult stubs
#   HACC_JG_BBOX_MODULES     Tcl list of vendor/IP modules to blackbox. Default: {} when DW stubs are on,
#                           otherwise {DW_fp_mult DW_fp_add}
#   HACC_JG_BBOX_MUL_THRESHOLD Optional non-negative integer. Default: 1.
#                           Passes elaborate -bbox_mul <threshold> to avoid WNL018 noise
#   HACC_JG_DISABLE_DOMAINS  Tcl list of rule domains to disable, for example {DFT}
#   HACC_JG_ENABLE_TAGS      Tcl list of rule tags to enable. If set, script first disables ALL
#   HACC_JG_DISABLE_TAGS     Tcl list of rule tags to disable after load
#   HACC_JG_WAIVER_TCL       Optional Tcl file sourced after elaborate/reset and before extract.
#                           Default: <rtl_root>/script/jasper_superlint_waivers.tcl when present.
#                           Set to NONE to disable project-local waivers.
#   HACC_JG_EXTRA_HDL        Tcl list of extra HDL files
#   HACC_JG_EXTRA_INCDIR     Tcl list of extra include directories
#   HACC_JG_ANALYZE_OPTS     Tcl list of extra analyze options, for example {+define+FOO}
#   HACC_JG_PROVE_TIME_LIMIT Optional Jasper prove time limit, for example 30m
#   HACC_JG_MAX_TRACE_LENGTH Optional max trace length before prove. Default: 50
#   HACC_JG_SKIP_PROVE       Optional 0/1. Default: 0
#   HACC_JG_SKIP_EXTRACT     Optional 0/1. Default: 0
#   HACC_JG_SKIP_REPORT      Optional 0/1. Default: 0
# ============================================================================

proc hacc_fail {message} {
    puts "ERROR: $message"
    exit 1
}

proc hacc_init_var {name default} {
    if {[info exists ::$name] && [string trim [set ::$name]] ne ""} {
        return [set ::$name]
    }
    if {[info exists ::env($name)] && [string trim $::env($name)] ne ""} {
        set ::$name [string trim $::env($name)]
        return [set ::$name]
    }
    set ::$name $default
    return $default
}

proc hacc_parse_bool {name value} {
    set normalized [string tolower [string trim $value]]
    switch -- $normalized {
        "" - "0" - "false" - "no" - "off" {
            return 0
        }
        "1" - "true" - "yes" - "on" {
            return 1
        }
        default {
            hacc_fail "$name must be one of 0/1/true/false/yes/no/on/off, got '$value'"
        }
    }
}

proc hacc_resolve_path {base_dir raw_path} {
    set trimmed [string trim $raw_path]
    if {$trimmed eq ""} {
        return ""
    }
    if {[file pathtype $trimmed] eq "absolute"} {
        return [file normalize $trimmed]
    }
    return [file normalize [file join $base_dir $trimmed]]
}

proc hacc_resolve_path_list {base_dir raw_list} {
    set resolved {}
    foreach item $raw_list {
        lappend resolved [hacc_resolve_path $base_dir $item]
    }
    return $resolved
}

proc hacc_detect_script_path {} {
    if {[info exists ::env(HACC_JG_SCRIPT_PATH)] && [string trim $::env(HACC_JG_SCRIPT_PATH)] ne ""} {
        return [file normalize [string trim $::env(HACC_JG_SCRIPT_PATH)]]
    }

    set candidates {}
    if {![catch {info script} info_script]} {
        lappend candidates $info_script
    }
    if {[info exists ::argv0]} {
        lappend candidates $::argv0
    }
    if {[info exists ::argv]} {
        foreach arg $::argv {
            if {[string match *.tcl $arg]} {
                lappend candidates $arg
            }
        }
    }

    foreach candidate $candidates {
        set trimmed [string trim $candidate]
        if {$trimmed eq "" || $trimmed eq "."} {
            continue
        }
        if {[file pathtype $trimmed] eq "absolute"} {
            set resolved [file normalize $trimmed]
        } else {
            set resolved [file normalize [file join [pwd] $trimmed]]
        }
        if {[file exists $resolved] && ![file isdirectory $resolved]} {
            return $resolved
        }
    }

    hacc_fail "Unable to determine jasper_superlint.tcl path. Set HACC_JG_SCRIPT_PATH to the absolute script path."
}

proc hacc_is_project_root {path} {
    set normalized [file normalize $path]
    return [expr {
        [file exists [file join $normalized src hybridacc_utils_pkg.sv]] &&
        [file exists [file join $normalized src Core core_pkg.sv]] &&
        [file exists [file join $normalized src Cluster cluster_pkg.sv]]
    }]
}

proc hacc_detect_project_root {script_path} {
    if {[info exists ::env(HACC_JG_PROJECT_ROOT)] && [string trim $::env(HACC_JG_PROJECT_ROOT)] ne ""} {
        set env_root [file normalize [string trim $::env(HACC_JG_PROJECT_ROOT)]]
        if {![hacc_is_project_root $env_root]} {
            hacc_fail "HACC_JG_PROJECT_ROOT is not a valid HybridAcc RTL root: $env_root"
        }
        return $env_root
    }

    if {[file tail $script_path] eq "jasper_superlint.tcl"} {
        set script_root [file normalize [file join [file dirname $script_path] ..]]
        if {[hacc_is_project_root $script_root]} {
            return $script_root
        }
    }

    set search_dir [pwd]
    while {1} {
        foreach candidate [list $search_dir [file join $search_dir design hybridacc-RTL]] {
            if {[hacc_is_project_root $candidate]} {
                return [file normalize $candidate]
            }
        }
        set parent [file dirname $search_dir]
        if {$parent eq $search_dir} {
            break
        }
        set search_dir $parent
    }

    hacc_fail "Unable to determine HybridAcc RTL root. Set HACC_JG_PROJECT_ROOT to the design/hybridacc-RTL path."
}

proc hacc_unique_append {list_name value} {
    upvar 1 $list_name values
    if {[lsearch -exact $values $value] < 0} {
        lappend values $value
    }
}

proc hacc_require_readable_file {path description} {
    if {![file exists $path]} {
        hacc_fail "$description does not exist: $path"
    }
    if {[file isdirectory $path]} {
        hacc_fail "$description is a directory, expected a file: $path"
    }
    if {![file readable $path]} {
        hacc_fail "$description is not readable: $path"
    }
}

proc hacc_require_directory {path description} {
    if {![file exists $path]} {
        hacc_fail "$description does not exist: $path"
    }
    if {![file isdirectory $path]} {
        hacc_fail "$description is not a directory: $path"
    }
}

proc hacc_collect_sources {pkg_files source_patterns extra_hdl} {
    set ordered {}
    foreach pkg $pkg_files {
        hacc_unique_append ordered $pkg
    }
    foreach pattern $source_patterns {
        foreach src [lsort [glob -nocomplain $pattern]] {
            if {[lsearch -exact $pkg_files $src] >= 0} {
                continue
            }
            hacc_unique_append ordered $src
        }
    }
    foreach src $extra_hdl {
        hacc_unique_append ordered $src
    }
    return $ordered
}

set script_path [hacc_detect_script_path]
set script_dir [file dirname $script_path]
set project_root [hacc_detect_project_root $script_path]
set src_root [file join $project_root src]

set top_name [hacc_init_var HACC_JG_TOP HybridAcc]
set run_tag [hacc_init_var RUN_TAG default]
set run_dir [hacc_resolve_path $project_root [hacc_init_var HACC_JG_RUN_DIR [file join $project_root jasper superlint_$run_tag]]]
set clock_name [hacc_init_var HACC_JG_CLOCK clk]
set reset_expr [hacc_init_var HACC_JG_RESET_EXPR {reset_n == 1'b0}]
set bbox_a [hacc_init_var HACC_JG_BBOX_A 1024]
set use_dw_stubs [hacc_parse_bool HACC_JG_USE_DW_STUBS [hacc_init_var HACC_JG_USE_DW_STUBS 1]]
if {$use_dw_stubs} {
    set bbox_modules_default {}
} else {
    set bbox_modules_default {DW_fp_mult DW_fp_add}
}
set bbox_modules [hacc_init_var HACC_JG_BBOX_MODULES $bbox_modules_default]
set bbox_mul_threshold [hacc_init_var HACC_JG_BBOX_MUL_THRESHOLD 1000]
set disable_domains [hacc_init_var HACC_JG_DISABLE_DOMAINS {}]
set enable_tags [hacc_init_var HACC_JG_ENABLE_TAGS {}]
set disable_tags [hacc_init_var HACC_JG_DISABLE_TAGS {}]

set waiver_tcl ""
set default_waiver_tcl [file join $project_root script jasper_superlint_waivers.tcl]
if {[info exists ::env(HACC_JG_WAIVER_TCL)] && [string trim $::env(HACC_JG_WAIVER_TCL)] ne ""} {
    set waiver_tcl_raw [string trim $::env(HACC_JG_WAIVER_TCL)]
    if {![string equal -nocase $waiver_tcl_raw NONE]} {
        set waiver_tcl [hacc_resolve_path $project_root $waiver_tcl_raw]
    }
} elseif {[file exists $default_waiver_tcl] && ![file isdirectory $default_waiver_tcl]} {
    set waiver_tcl [file normalize $default_waiver_tcl]
}

if {[string trim $waiver_tcl] ne ""} {
    hacc_require_readable_file $waiver_tcl "Waiver Tcl file"
}

set extra_hdl [hacc_resolve_path_list $project_root [hacc_init_var HACC_JG_EXTRA_HDL {}]]
set extra_incdir [hacc_resolve_path_list $project_root [hacc_init_var HACC_JG_EXTRA_INCDIR {}]]
set analyze_opts [hacc_init_var HACC_JG_ANALYZE_OPTS {}]
set prove_time_limit [hacc_init_var HACC_JG_PROVE_TIME_LIMIT ""]
set max_trace_length [hacc_init_var HACC_JG_MAX_TRACE_LENGTH 50]
set skip_prove [hacc_parse_bool HACC_JG_SKIP_PROVE [hacc_init_var HACC_JG_SKIP_PROVE 0]]
set skip_extract [hacc_parse_bool HACC_JG_SKIP_EXTRACT [hacc_init_var HACC_JG_SKIP_EXTRACT 0]]
set skip_report [hacc_parse_bool HACC_JG_SKIP_REPORT [hacc_init_var HACC_JG_SKIP_REPORT 0]]

set default_jasper_hdl {}
if {$use_dw_stubs} {
    lappend default_jasper_hdl [file join $project_root script jasper_dw_fp_stubs.sv]
}
foreach stub_file $default_jasper_hdl {
    hacc_require_readable_file $stub_file "Jasper helper HDL"
}

if {$use_dw_stubs && [lsearch -exact $analyze_opts +define+HACC_JASPER_DW_STUBS] < 0} {
    lappend analyze_opts +define+HACC_JASPER_DW_STUBS
}

if {![string is integer -strict $bbox_a] || $bbox_a < 0} {
    hacc_fail "HACC_JG_BBOX_A must be a non-negative integer, got '$bbox_a'"
}
if {![string is integer -strict $bbox_mul_threshold] || $bbox_mul_threshold < 0} {
    hacc_fail "HACC_JG_BBOX_MUL_THRESHOLD must be a non-negative integer, got '$bbox_mul_threshold'"
}
if {![string is integer -strict $max_trace_length] || $max_trace_length <= 0} {
    hacc_fail "HACC_JG_MAX_TRACE_LENGTH must be a positive integer, got '$max_trace_length'"
}

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

set include_dirs [list \
    $src_root \
    [file join $src_root utils] \
    [file join $src_root Core] \
    [file join $src_root Cluster] \
    [file join $src_root NoC] \
    [file join $src_root PE]]

foreach inc_dir [concat $include_dirs $extra_incdir] {
    hacc_require_directory $inc_dir "include directory"
}
foreach src_file [concat $pkg_files $default_jasper_hdl $extra_hdl] {
    hacc_require_readable_file $src_file "HDL file"
}

set ordered_hdl [hacc_collect_sources [concat $pkg_files $default_jasper_hdl] $source_patterns $extra_hdl]
if {[llength $ordered_hdl] == 0} {
    hacc_fail "No RTL source files were collected for JasperGold Superlint"
}
foreach src_file $ordered_hdl {
    hacc_require_readable_file $src_file "RTL source"
}

set incdir_args {}
set all_incdirs {}
foreach inc_dir [concat $include_dirs $extra_incdir] {
    hacc_unique_append all_incdirs $inc_dir
}
foreach inc_dir $all_incdirs {
    lappend incdir_args "+incdir+$inc_dir"
}

file mkdir $run_dir

puts "============================================================"
puts " JasperGold Superlint RTL run"
puts " Top        : $top_name"
puts " Run dir    : $run_dir"
puts " Clock      : $clock_name"
puts " Reset expr : $reset_expr"
puts " DW stubs   : $use_dw_stubs"
if {[llength $bbox_modules] > 0} {
    puts " BBox mods  : $bbox_modules"
}
puts " BBox mul   : $bbox_mul_threshold"
if {[llength $enable_tags] > 0} {
    puts " Enable tags: $enable_tags"
} else {
    puts " Disable dom: $disable_domains"
}
if {[llength $disable_tags] > 0} {
    puts " Disable tags: $disable_tags"
}
if {[string trim $waiver_tcl] ne ""} {
    puts " Waiver Tcl : $waiver_tcl"
}
puts "============================================================"

cd $run_dir

check_superlint -init
config_rtlds -rule -load [get_install_dir]/etc/res/rtlds/rules/superlint.def

if {[llength $enable_tags] > 0} {
    config_rtlds -rule -disable -domain {ALL}
    foreach tag $enable_tags {
        config_rtlds -rule -enable -tag $tag
    }
} else {
    foreach domain $disable_domains {
        config_rtlds -rule -disable -domain $domain
    }
}

foreach tag $disable_tags {
    config_rtlds -rule -disable -tag $tag
}

analyze -clear
foreach src_file $ordered_hdl {
    set analyze_cmd [concat [list analyze -sv09] $analyze_opts $incdir_args [list $src_file]]
    eval $analyze_cmd
}

set elaborate_cmd [list elaborate -top $top_name]
if {$bbox_a > 0} {
    lappend elaborate_cmd -bbox_a $bbox_a
}
if {$bbox_mul_threshold > 0} {
    lappend elaborate_cmd -bbox_mul $bbox_mul_threshold
}
foreach bbox_module $bbox_modules {
    lappend elaborate_cmd -bbox_m $bbox_module
}
eval $elaborate_cmd

if {[string equal -nocase $clock_name NONE]} {
    clock -none
} else {
    clock $clock_name
}

if {[string trim $reset_expr] eq "" || [string equal -nocase $reset_expr NONE]} {
    reset -none
} else {
    reset $reset_expr
}

if {[string trim $waiver_tcl] ne ""} {
    source $waiver_tcl
}

if {!$skip_extract} {
    check_superlint -extract
}

if {!$skip_extract && !$skip_prove} {
    if {[string trim $prove_time_limit] ne ""} {
        set_prove_time_limit $prove_time_limit
    }
    set_max_trace_length $max_trace_length
    check_superlint -prove
}

if {!$skip_report} {
    report
}