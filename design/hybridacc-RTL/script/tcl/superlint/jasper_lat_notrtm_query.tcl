proc hacc_detect_query_rtl_root {} {
    if {[info exists ::env(HACC_JG_PROJECT_ROOT)] && [string trim $::env(HACC_JG_PROJECT_ROOT)] ne ""} {
        return [file normalize [string trim $::env(HACC_JG_PROJECT_ROOT)]]
    }

    set search_dir [pwd]
    while {1} {
        foreach candidate [list $search_dir [file join $search_dir design hybridacc-RTL]] {
            if {[file exists [file join $candidate script tcl superlint jasper_superlint.tcl]] && [file exists [file join $candidate src hybridacc_utils_pkg.sv]]} {
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
set superlint_tcl_dir [file join $project_root script tcl superlint]
set repo_root [file normalize [file join $project_root .. ..]]

set ::HACC_JG_RUN_DIR [file join $repo_root output jasper_lat_notrtm_query run]
set ::HACC_JG_SKIP_PROVE 1
set ::HACC_JG_SKIP_REPORT 1

source [file join $superlint_tcl_dir jasper_superlint.tcl]

set ids [check_superlint -list -severity warning -tag [list LAT_NO_TRTM] -silent]
set srcs [check_superlint $ids -get source_location -silent]
set msgs [check_superlint $ids -get message -silent]
puts "LAT_NO_TRTM_COUNT=[llength $ids]"
for {set i 0} {$i < 30 && $i < [llength $ids]} {incr i} {
    puts "[lindex $srcs $i]\t[lindex $msgs $i]"
}

exit