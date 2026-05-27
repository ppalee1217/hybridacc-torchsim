if {![info exists ::MODULE_NAME] && [info exists ::env(MODULE_NAME)] && [string trim $::env(MODULE_NAME)] ne ""} {
    set ::MODULE_NAME [string trim $::env(MODULE_NAME)]
}

if {![info exists ::MODULE_NAME] || [string trim $::MODULE_NAME] eq ""} {
    puts "ERROR: MODULE_NAME must be set before sourcing this script"
    exit 1
}

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

set ::HACC_JG_RUN_DIR [file join $repo_root output jasper_module_extract_query run]
set ::HACC_JG_SKIP_PROVE 1
set ::HACC_JG_SKIP_REPORT 1
set ::HACC_JG_SKIP_EXTRACT 1

source [file join $superlint_tcl_dir jasper_superlint.tcl]

set module_instances [get_design_info -module $::MODULE_NAME -list instance -silent]
set module_instance [lindex $module_instances 0]
puts "MODULE_NAME=$::MODULE_NAME"
puts "MODULE_INSTANCE=$module_instance"

check_superlint -extract -instances [list $module_instance]

set warning_ids [check_superlint -list -severity warning -instance $module_instance -silent]
set warning_tags {}
set warning_sources {}
set warning_messages {}
if {[llength $warning_ids] > 0} {
    set warning_tags [check_superlint $warning_ids -get tag -silent]
    set warning_sources [check_superlint $warning_ids -get source_location -silent]
    set warning_messages [check_superlint $warning_ids -get message -silent]
}

puts "MODULE_WARNING_COUNT=[llength $warning_tags]"
puts "MODULE_WARNING_DETAILS_BEGIN"
for {set i 0} {$i < [llength $warning_tags]} {incr i} {
    puts "[lindex $warning_tags $i]\t[lindex $warning_sources $i]\t[lindex $warning_messages $i]"
}
puts "MODULE_WARNING_DETAILS_END"

exit