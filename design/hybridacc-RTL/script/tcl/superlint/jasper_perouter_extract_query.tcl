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

set ::HACC_JG_RUN_DIR [file join $repo_root output jasper_perouter_extract_v1 run]
set ::HACC_JG_SKIP_PROVE 1
set ::HACC_JG_SKIP_REPORT 1
set ::HACC_JG_SKIP_EXTRACT 1

source [file join $superlint_tcl_dir jasper_superlint.tcl]

set pe_instances [get_design_info -module PErouter -list instance -silent]
set pe_instance [lindex $pe_instances 0]
puts "PE_INSTANCE=$pe_instance"

check_superlint -extract -instances [list $pe_instance]

set warning_ids [check_superlint -list -severity warning -instance $pe_instance -silent]
set warning_tags {}
set warning_sources {}
set warning_messages {}
if {[llength $warning_ids] > 0} {
    set warning_tags [check_superlint $warning_ids -get tag -silent]
    set warning_sources [check_superlint $warning_ids -get source_location -silent]
    set warning_messages [check_superlint $warning_ids -get message -silent]
}

puts "PE_WARNING_COUNT=[llength $warning_tags]"
puts "PE_WARNING_DETAILS_BEGIN"
set detail_count [llength $warning_tags]
for {set i 0} {$i < $detail_count} {incr i} {
    puts "[lindex $warning_tags $i]\t[lindex $warning_sources $i]\t[lindex $warning_messages $i]"
}
puts "PE_WARNING_DETAILS_END"

exit