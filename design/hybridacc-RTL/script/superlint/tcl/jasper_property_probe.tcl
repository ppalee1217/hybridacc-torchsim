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

set ::HACC_JG_RUN_DIR [file join $repo_root output jasper_probe_property run]
set ::HACC_JG_SKIP_PROVE 1
set ::HACC_JG_SKIP_REPORT 1
source [file join $rtl_script_dir jasper_superlint.tcl]
puts "PROP_MODULE=[get_design_info -property arithmetic_overflow_assignment_prop_52363 -list module]"
puts "PROP_INSTANCE=[get_design_info -property arithmetic_overflow_assignment_prop_52363 -list instance]"
puts "PROP_SIGNAL=[get_design_info -property arithmetic_overflow_assignment_prop_52363 -list signal]"
exit