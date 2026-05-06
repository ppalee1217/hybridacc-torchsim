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

set ::HACC_JG_RUN_DIR [file join $repo_root output jasper_extract_query_v1 run]
set ::HACC_JG_SKIP_PROVE 1
set ::HACC_JG_SKIP_REPORT 1

source [file join $rtl_script_dir jasper_superlint.tcl]

set warning_ids [check_superlint -list -severity warning -silent]
set warning_tags {}
set warning_sources {}
set warning_messages {}
if {[llength $warning_ids] > 0} {
    set warning_tags [check_superlint $warning_ids -get tag -silent]
    set warning_sources [check_superlint $warning_ids -get source_location -silent]
    set warning_messages [check_superlint $warning_ids -get message -silent]
}

array set tag_counts {}
foreach tag $warning_tags {
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

puts "WARNING_COUNT=[llength $warning_tags]"
puts "TOP_WARNING_TAGS_BEGIN"
set top_limit 20
set top_count [expr {[llength $sorted_tags] < $top_limit ? [llength $sorted_tags] : $top_limit}]
for {set i 0} {$i < $top_count} {incr i} {
    lassign [lindex $sorted_tags $i] count tag
    puts "$tag\t$count"
}
puts "TOP_WARNING_TAGS_END"

if {$top_count > 0} {
    set top_tag [lindex [lindex $sorted_tags 0] 1]
    puts "TOP_TAG=$top_tag"
    set top_ids [check_superlint -list -severity warning -tag [list $top_tag] -silent]
    set top_sources {}
    set top_messages {}
    if {[llength $top_ids] > 0} {
        set top_sources [check_superlint $top_ids -get source_location -silent]
        set top_messages [check_superlint $top_ids -get message -silent]
    }
    puts "TOP_TAG_DETAILS_BEGIN"
    set detail_limit 30
    set detail_count [expr {[llength $top_sources] < $detail_limit ? [llength $top_sources] : $detail_limit}]
    for {set i 0} {$i < $detail_count} {incr i} {
        puts "[lindex $top_sources $i]\t[lindex $top_messages $i]"
    }
    puts "TOP_TAG_DETAILS_END"
}

exit