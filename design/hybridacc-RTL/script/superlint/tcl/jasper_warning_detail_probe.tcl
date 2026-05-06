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

set ::HACC_JG_RUN_DIR [file join $repo_root output jasper_warning_detail_probe run]
set ::HACC_JG_SKIP_PROVE 1
set ::HACC_JG_SKIP_REPORT 1

source [file join $rtl_script_dir jasper_superlint.tcl]

puts "CHECK_SUPERLINT_HELP_BEGIN"
if {[catch {help check_superlint} help_status]} {
    puts "HELP_FAILED=$help_status"
}
puts "CHECK_SUPERLINT_HELP_END"

set probe_fields {tag severity source_location message module instance hierarchy hierarchical_name scope name object file line rule rule_name category domain}
foreach tag {MOD_NR_PRGD ARY_NR_SLRG PRT_NO_PRMS REG_NO_READ MAC_NO_USED IDN_NR_AMKW FNC_NO_USED INP_NO_USED EXP_NR_MXSU ENM_NR_TOST} {
    set issue_ids [check_superlint -list -severity warning -tag [list $tag] -silent]
    puts "ISSUE_TAG=$tag COUNT=[llength $issue_ids]"
    set sample_ids [lrange $issue_ids 0 2]
    foreach issue_id $sample_ids {
        puts "ISSUE_BEGIN $tag $issue_id"
        foreach field $probe_fields {
            if {[catch {set field_value [check_superlint [list $issue_id] -get $field -silent]} field_status]} {
                puts "FIELD_FAIL $field $field_status"
            } else {
                puts "FIELD $field=$field_value"
            }
        }
        puts "ISSUE_END $tag $issue_id"
    }
}

exit