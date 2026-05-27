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

set ::HACC_JG_RUN_DIR [file join $repo_root output jasper_exe_m_extract run]
set ::HACC_JG_SKIP_PROVE 1
set ::HACC_JG_SKIP_REPORT 1
set ::HACC_JG_SKIP_EXTRACT 1

source [file join $superlint_tcl_dir jasper_superlint.tcl]

set exe_m_instances [get_design_info -module EXE_M_Stage -list instance -silent]
set exe_m_instance [lindex $exe_m_instances 0]
puts "EXE_M_INSTANCE=$exe_m_instance"

check_superlint -extract -instances [list $exe_m_instance]

set comb_loop_tags {MOD_IS_CMBL MOD_IS_FCMB}
puts "EXE_M_COMB_LOOP_DETAILS_BEGIN"
foreach tag $comb_loop_tags {
    set comb_ids [check_superlint -list -severity error -instance $exe_m_instance -tag [list $tag] -silent]
    puts "${tag}_COUNT=[llength $comb_ids]"
    if {[llength $comb_ids] > 0} {
        set comb_sources [check_superlint $comb_ids -get source_location -silent]
        set comb_messages [check_superlint $comb_ids -get message -silent]
        for {set i 0} {$i < [llength $comb_ids]} {incr i} {
            puts "$tag\t[lindex $comb_sources $i]\t[lindex $comb_messages $i]"
        }
    }
}
puts "EXE_M_COMB_LOOP_DETAILS_END"

exit