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

set ::MODULE_NAME HybridDataDeliverUnit
set ::HACC_JG_RUN_DIR [file join $repo_root output jasper_hddu_error_query run]
set ::HACC_JG_SKIP_PROVE 1
set ::HACC_JG_SKIP_REPORT 1
set ::HACC_JG_SKIP_EXTRACT 1

source [file join $rtl_script_dir jasper_superlint.tcl]

set module_instances [get_design_info -module $::MODULE_NAME -list instance -silent]
set module_instance [lindex $module_instances 0]
check_superlint -extract -instances [list $module_instance]

set error_ids [check_superlint -list -severity error -instance $module_instance -silent]
set error_tags {}
set error_sources {}
set error_messages {}
if {[llength $error_ids] > 0} {
    set error_tags [check_superlint $error_ids -get tag -silent]
    set error_sources [check_superlint $error_ids -get source_location -silent]
    set error_messages [check_superlint $error_ids -get message -silent]
}

puts "MODULE_NAME=$::MODULE_NAME"
puts "MODULE_INSTANCE=$module_instance"
puts "MODULE_ERROR_COUNT=[llength $error_tags]"
puts "MODULE_ERROR_DETAILS_BEGIN"
for {set i 0} {$i < [llength $error_tags]} {incr i} {
    puts "[lindex $error_tags $i]\t[lindex $error_sources $i]\t[lindex $error_messages $i]"
}
puts "MODULE_ERROR_DETAILS_END"

exit