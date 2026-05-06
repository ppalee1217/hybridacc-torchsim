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

set ::HACC_JG_RUN_DIR [file join $repo_root output jasper_report_query run]
set ::HACC_JG_SKIP_PROVE 1
set ::HACC_JG_SKIP_REPORT 1

source [file join $rtl_script_dir jasper_superlint.tcl]

proc emit_tag_counts {label severity} {
    set issue_ids [check_superlint -list -severity $severity -silent]
    set issue_tags {}
    if {[llength $issue_ids] > 0} {
        set issue_tags [check_superlint $issue_ids -get tag -silent]
    }

    array set tag_counts {}
    foreach tag $issue_tags {
        if {![info exists tag_counts($tag)]} {
            set tag_counts($tag) 0
        }
        incr tag_counts($tag)
    }

    set sorted_tags {}
    foreach tag [array names tag_counts] {
        lappend sorted_tags [list $tag_counts($tag) $tag]
    }
    set sorted_tags [lsort -decreasing -integer -index 0 $sorted_tags]

    puts "${label}_COUNT=[llength $issue_tags]"
    puts "${label}_TAG_COUNTS_BEGIN"
    foreach entry $sorted_tags {
        lassign $entry count tag
        puts "$tag\t$count"
    }
    puts "${label}_TAG_COUNTS_END"
}

emit_tag_counts WARNING warning
emit_tag_counts ERROR error

set comb_loop_tags {MOD_IS_CMBL MOD_IS_FCMB}
puts "COMB_LOOP_DETAILS_BEGIN"
foreach tag $comb_loop_tags {
    set comb_ids [check_superlint -list -severity error -tag [list $tag] -silent]
    puts "${tag}_COUNT=[llength $comb_ids]"
    if {[llength $comb_ids] > 0} {
        set comb_sources [check_superlint $comb_ids -get source_location -silent]
        set comb_messages [check_superlint $comb_ids -get message -silent]
        for {set i 0} {$i < [llength $comb_ids]} {incr i} {
            puts "$tag\t[lindex $comb_sources $i]\t[lindex $comb_messages $i]"
        }
    }
}
puts "COMB_LOOP_DETAILS_END"

exit