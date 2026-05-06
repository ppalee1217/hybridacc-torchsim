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

set ::HACC_JG_RUN_DIR [file join $repo_root output jasper_error_hotspot run]
set ::HACC_JG_SKIP_PROVE 1
set ::HACC_JG_SKIP_REPORT 1

source [file join $rtl_script_dir jasper_superlint.tcl]

proc emit_error_hotspots {tag max_rows} {
    set issue_ids [check_superlint -list -severity error -tag [list $tag] -silent]
    set source_locations {}
    if {[llength $issue_ids] > 0} {
        set source_locations [check_superlint $issue_ids -get source_location -silent]
    }

    array set source_counts {}
    foreach source_location $source_locations {
        if {![info exists source_counts($source_location)]} {
            set source_counts($source_location) 0
        }
        incr source_counts($source_location)
    }

    set sorted_sources {}
    foreach source_location [array names source_counts] {
        lappend sorted_sources [list $source_counts($source_location) $source_location]
    }
    set sorted_sources [lsort -decreasing -integer -index 0 $sorted_sources]

    puts "${tag}_COUNT=[llength $issue_ids]"
    puts "${tag}_TOP_SOURCES_BEGIN"
    set row_count 0
    foreach entry $sorted_sources {
        if {$row_count >= $max_rows} {
            break
        }
        lassign $entry count source_location
        puts "$count\t$source_location"
        incr row_count
    }
    puts "${tag}_TOP_SOURCES_END"
}

foreach tag {BLK_NO_RCHB ASG_AR_OVFL ARY_IS_OOBI EXP_AR_OVFL CAS_NO_UNIQ ASG_IS_OVFL FLP_NO_SCAN} {
    emit_error_hotspots $tag 40
}

exit