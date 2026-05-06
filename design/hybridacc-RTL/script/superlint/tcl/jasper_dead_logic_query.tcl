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

set ::HACC_JG_RUN_DIR [file join $repo_root output jasper_dead_logic_query run]
set ::HACC_JG_SKIP_PROVE 1
set ::HACC_JG_SKIP_REPORT 1

source [file join $rtl_script_dir jasper_superlint.tcl]

proc emit_issue_details {tag severity limit} {
    set issue_ids [check_superlint -list -severity $severity -tag [list $tag] -silent]
    puts "${tag}_COUNT=[llength $issue_ids]"
    puts "${tag}_DETAILS_BEGIN"
    if {[llength $issue_ids] > 0} {
        set sources [check_superlint $issue_ids -get source_location -silent]
        set messages [check_superlint $issue_ids -get message -silent]
        set emit_count [expr {[llength $issue_ids] < $limit ? [llength $issue_ids] : $limit}]
        for {set i 0} {$i < $emit_count} {incr i} {
            puts "$tag\t[lindex $sources $i]\t[lindex $messages $i]"
        }
    }
    puts "${tag}_DETAILS_END"
}

emit_issue_details NET_NO_LOAD warning 240
emit_issue_details NET_NO_LDDR warning 200
emit_issue_details REG_NO_LOAD warning 160

exit