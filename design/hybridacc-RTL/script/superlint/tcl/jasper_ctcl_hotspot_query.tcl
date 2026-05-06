proc hacc_detect_query_rtl_root {} {
    if {[info exists ::env(HACC_JG_PROJECT_ROOT)] && [string trim $::env(HACC_JG_PROJECT_ROOT)] ne ""} {
        return [file normalize [string trim $::env(HACC_JG_PROJECT_ROOT)]]
    }

    set search_dir [pwd]
    while {1} {
        foreach candidate [list $search_dir [file join $search_dir design hybridacc-RTL]] {
            if {[file exists [file join $candidate script jasper_superlint.tcl]] && [file exists [file join $candidate src hybridacc_utils_pkg.sv]]} {
                return [file normalize $candidate]
            }
        }
        set parent [file dirname $search_dir]
        if {$parent eq $search_dir} {
            break
        }
        set search_dir $parent
    }

    puts "ERROR: Unable to determine HybridAcc RTL root for query script. Set HACC_JG_PROJECT_ROOT."
    exit 1
}

set project_root [hacc_detect_query_rtl_root]
set rtl_script_dir [file join $project_root script]
set repo_root [file normalize [file join $project_root .. ..]]

set ::HACC_JG_RUN_DIR [file join $repo_root output jasper_ctcl_hotspot run]
set ::HACC_JG_SKIP_PROVE 1
set ::HACC_JG_SKIP_REPORT 1

source [file join $rtl_script_dir jasper_superlint.tcl]

proc emit_error_details {tag} {
    set issue_ids [check_superlint -list -severity error -tag [list $tag] -silent]
    puts "${tag}_COUNT=[llength $issue_ids]"
    if {[llength $issue_ids] == 0} {
        return
    }

    puts "${tag}_SOURCE_LOCATIONS_BEGIN"
    foreach source_location [check_superlint $issue_ids -get source_location -silent] {
        puts $source_location
    }
    puts "${tag}_SOURCE_LOCATIONS_END"

    puts "${tag}_INSTANCES_BEGIN"
    foreach instance_name [check_superlint $issue_ids -get instance -silent] {
        puts $instance_name
    }
    puts "${tag}_INSTANCES_END"

}

emit_error_details FLP_NO_CTCL

exit
