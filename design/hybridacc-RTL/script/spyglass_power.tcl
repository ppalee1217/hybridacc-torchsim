# ============================================================================
# SpyGlass RTL power-estimation flow for HybridAcc.
#
# List-valued variables below use Tcl list syntax.
#
# Optional environment variables:
#   CLOCK_PERIOD_NS             Target clock period in ns. Default: 1.0
#   RUN_TAG                     Logical run label. Default: default
#   HACC_SG_PROJECT_NAME        SpyGlass project name. Default: HybridAcc_power_${RUN_TAG}
#   HACC_SG_PROJECT_WDIR        Project workspace root. Default: <rtl_root>/spyglass
#   HACC_SG_GOAL                SpyGlass goal. Default: power/power_est_average
#   HACC_SG_EXTRA_HDL           Extra HDL files for external RTL models (for example DW models)
#   HACC_SG_EXTRA_GATESLIB      Extra .lib/.db gateslib files
#   HACC_SG_EXTRA_INCDIR        Extra include directories
#   HACC_SG_EXTRA_SGDC          Extra SGDC files applied after the generated base SGDC
#   HACC_SG_ACTIVITY_FILE       Optional activity file (.vcd/.vcd.gz/.fsdb/.saif)
#   HACC_SG_ACTIVITY_FORMAT     Optional override: VCD, FSDB, or SAIF
#   HACC_SG_ACTIVITY_START      Optional start time, for example 0ns
#   HACC_SG_ACTIVITY_END        Optional end time, for example 20us
#   HACC_SG_ACTIVITY_WEIGHT     Optional integer weight for multi-window activity input
#   HACC_SG_ACTIVITY_SIM_TOP    Optional simulation top name. Default: HybridAcc
#   HACC_SG_ACTIVITY_INST       Optional instance name for horizontal activity mapping
#   HACC_SG_ACTIVITY_RTL_DESIGN_NL
#                               Optional 0/1. Use 1 when RTL activity is mapped onto a
#                               gate-level design. Default: 0
#   HACC_SG_DEBUG_INSTS         Optional leaf-instance list for show_power_calc_details
# ============================================================================

proc hacc_fail {message} {
	puts "ERROR: $message"
	exit -f 1
}

proc hacc_init_var {name default} {
	if {[info exists ::$name] && [string trim [set ::$name]] ne ""} {
		return [set ::$name]
	}
	if {[info exists ::env($name)] && [string trim $::env($name)] ne ""} {
		set ::$name [string trim $::env($name)]
		return [set ::$name]
	}
	set ::$name $default
	return $default
}

proc hacc_parse_bool {name value} {
	set normalized [string tolower [string trim $value]]
	switch -- $normalized {
		"" - "0" - "false" - "no" - "off" {
			return 0
		}
		"1" - "true" - "yes" - "on" {
			return 1
		}
		default {
			hacc_fail "$name must be one of 0/1/true/false/yes/no/on/off, got '$value'"
		}
	}
}

proc hacc_sanitize_project_name {name} {
	set sanitized [string trim $name]
	if {$sanitized eq ""} {
		hacc_fail "HACC_SG_PROJECT_NAME resolved to an empty string"
	}
	if {[string match *.prj $sanitized]} {
		set sanitized [string range $sanitized 0 end-4]
	}
	regsub -all {[^A-Za-z0-9._-]} $sanitized _ sanitized
	return $sanitized
}

proc hacc_resolve_path {base_dir raw_path} {
	set trimmed [string trim $raw_path]
	if {$trimmed eq ""} {
		return ""
	}
	if {[file pathtype $trimmed] eq "absolute"} {
		return [file normalize $trimmed]
	}
	return [file normalize [file join $base_dir $trimmed]]
}

proc hacc_resolve_path_list {base_dir raw_list} {
	set resolved {}
	foreach item $raw_list {
		lappend resolved [hacc_resolve_path $base_dir $item]
	}
	return $resolved
}

proc hacc_unique_append {list_name value} {
	upvar 1 $list_name values
	if {[lsearch -exact $values $value] < 0} {
		lappend values $value
	}
}

proc hacc_require_readable_file {path description} {
	if {![file exists $path]} {
		hacc_fail "$description does not exist: $path"
	}
	if {[file isdirectory $path]} {
		hacc_fail "$description is a directory, expected a file: $path"
	}
	if {![file readable $path]} {
		hacc_fail "$description is not readable: $path"
	}
}

proc hacc_require_directory {path description} {
	if {![file exists $path]} {
		hacc_fail "$description does not exist: $path"
	}
	if {![file isdirectory $path]} {
		hacc_fail "$description is not a directory: $path"
	}
}

proc hacc_collect_sources {pkg_files preferred_sources source_patterns extra_hdl} {
	set ordered {}
	foreach pkg $pkg_files {
		hacc_unique_append ordered $pkg
	}
	foreach src $preferred_sources {
		hacc_unique_append ordered $src
	}
	foreach pattern $source_patterns {
		foreach src [lsort [glob -nocomplain $pattern]] {
			if {[lsearch -exact $pkg_files $src] >= 0} {
				continue
			}
			if {[lsearch -exact $preferred_sources $src] >= 0} {
				continue
			}
			hacc_unique_append ordered $src
		}
	}
	foreach src $extra_hdl {
		hacc_unique_append ordered $src
	}
	return $ordered
}

proc hacc_infer_activity_format {activity_file explicit_format} {
	if {[string trim $explicit_format] ne ""} {
		set fmt [string toupper [string trim $explicit_format]]
		if {$fmt ni {VCD FSDB SAIF}} {
			hacc_fail "HACC_SG_ACTIVITY_FORMAT must be one of VCD/FSDB/SAIF, got '$explicit_format'"
		}
		return $fmt
	}

	set lower_name [string tolower [file tail $activity_file]]
	if {[string match *.vcd $lower_name] || [string match *.vcd.gz $lower_name]} {
		return VCD
	}
	if {[string match *.fsdb $lower_name]} {
		return FSDB
	}
	if {[string match *.saif $lower_name] || [string match *.saif.gz $lower_name]} {
		return SAIF
	}

	hacc_fail "Cannot infer activity format from '$activity_file'. Set HACC_SG_ACTIVITY_FORMAT explicitly."
}

proc hacc_write_power_sgdc {
	sgdc_path
	top_name
	clock_port
	reset_port
	clock_period
	activity_file
	activity_format
	activity_start
	activity_end
	activity_weight
	activity_sim_top
	activity_inst
	activity_rtl_design_nl
	debug_insts
} {
	set input_max  [expr {double(round(1000.0 * $clock_period * 0.6)) / 1000.0}]
	set input_min  0.0
	set output_max [expr {double(round(1000.0 * $clock_period * 0.1)) / 1000.0}]
	set output_min 0.0

	file mkdir [file dirname $sgdc_path]
	set fh [open $sgdc_path w]

	puts $fh "# Auto-generated by script/spyglass_power.tcl"
	puts $fh "# HybridAcc RTL power constraints for SpyGlass power_est."
	puts $fh "current_design $top_name"
	puts $fh ""
	puts $fh [format {create_clock -name %s -period %.3f [get_ports %s]} $clock_port $clock_period $clock_port]
	puts $fh [format {reset -name %s -value 0} $reset_port]
	puts $fh {set_clock_uncertainty 0.01 [all_clocks]}
	puts $fh {set_clock_latency 0.2 [all_clocks]}
	puts $fh {set_clock_latency -source 0 [all_clocks]}
	puts $fh {set_input_transition 0.2 [all_inputs]}
	puts $fh {set_clock_transition 0.1 [all_clocks]}
	puts $fh {set_load 0.005 [all_outputs]}
	puts $fh {set_operating_conditions -min_library N16ADFP_StdCellff0p88v125c -min ff0p88v125c -max_library N16ADFP_StdCellss0p72vm40c -max ss0p72vm40c}
	puts $fh [format {set_driving_cell -library N16ADFP_StdCellss0p72vm40c -lib_cell BUFFD4BWP16P90LVT -pin {Z} [get_ports %s]} $clock_port]
	puts $fh [format {set_driving_cell -library N16ADFP_StdCellss0p72vm40c -lib_cell DFQD1BWP16P90LVT -pin {Q} [remove_from_collection [all_inputs] [get_ports %s]]} $clock_port]
	puts $fh [format {set_input_delay -clock %s -max %.3f [remove_from_collection [all_inputs] [get_ports %s]]} $clock_port $input_max $clock_port]
	puts $fh [format {set_input_delay -clock %s -min %.3f [remove_from_collection [all_inputs] [get_ports %s]]} $clock_port $input_min $clock_port]
	puts $fh [format {set_output_delay -clock %s -max %.3f [all_outputs]} $clock_port $output_max]
	puts $fh [format {set_output_delay -clock %s -min %.3f [all_outputs]} $clock_port $output_min]

	if {$activity_file ne ""} {
		puts $fh ""
		puts $fh "# Activity annotation for power_est."
		set cmd [list activity_data -format $activity_format -file $activity_file]
		if {[string trim $activity_start] ne ""} {
			lappend cmd -starttime $activity_start
		}
		if {[string trim $activity_end] ne ""} {
			lappend cmd -endtime $activity_end
		}
		if {[string trim $activity_weight] ne ""} {
			lappend cmd -weight $activity_weight
		}
		lappend cmd -sim_topname $activity_sim_top
		if {[string trim $activity_inst] ne ""} {
			lappend cmd -instname $activity_inst
		}
		if {$activity_rtl_design_nl} {
			lappend cmd -sim_rtl_design_nl
		}
		puts $fh $cmd
	} else {
		puts $fh ""
		puts $fh "# No activity_data provided: SpyGlass will fall back to vectorless activity inference."
	}

	if {[llength $debug_insts] > 0} {
		puts $fh ""
		puts $fh "# Optional power calculation detail dump for selected leaf instances."
		puts $fh [concat [list show_power_calc_details -instname] $debug_insts]
	}

	close $fh
}

set script_dir [file dirname [file normalize [info script]]]
set project_root [file normalize [file join $script_dir ..]]
set src_root [file join $project_root src]

set top_name HybridAcc
set clock_port clk
set reset_port reset_n

set lib_files [list \
	/cad/process/ADFP/Executable_Package/Collaterals/IP/stdcell/N16ADFP_StdCell/NLDM/N16ADFP_StdCellss0p72vm40c.db \
	/cad/process/ADFP/Executable_Package/Collaterals/IP/stdcell/N16ADFP_StdCell/NLDM/N16ADFP_StdCellff0p88v125c.db \
	/cad/process/ADFP/Executable_Package/Collaterals/IP/stdio/N16ADFP_StdIO/NLDM/N16ADFP_StdIOss0p72v1p62v125c.db \
	/cad/process/ADFP/Executable_Package/Collaterals/IP/stdio/N16ADFP_StdIO/NLDM/N16ADFP_StdIOff0p88v1p98vm40c.db \
	/cad/process/ADFP/Executable_Package/Collaterals/IP/sram/N16ADFP_SRAM/NLDM/N16ADFP_SRAM_ss0p72v0p72vm40c_100a.db \
	/cad/process/ADFP/Executable_Package/Collaterals/IP/sram/N16ADFP_SRAM/NLDM/N16ADFP_SRAM_ff0p88v0p88v125c_100a.db]

set pkg_files [list \
	[file join $src_root hybridacc_utils_pkg.sv] \
	[file join $src_root Core core_pkg.sv] \
	[file join $src_root Cluster cluster_pkg.sv]]

set ordered_source_files [list \
	[file join $src_root Cluster ScratchpadMemoryBank.sv] \
	[file join $src_root Cluster ScratchpadMemory.sv]]

set source_patterns [list \
	[file join $src_root *.sv] \
	[file join $src_root utils *.sv] \
	[file join $src_root Core *.sv] \
	[file join $src_root Cluster *.sv] \
	[file join $src_root NoC *.sv] \
	[file join $src_root PE *.sv]]

set include_dirs [list \
	$src_root \
	[file join $src_root utils] \
	[file join $src_root Core] \
	[file join $src_root Cluster] \
	[file join $src_root NoC] \
	[file join $src_root PE]]

set clock_period [hacc_init_var CLOCK_PERIOD_NS 1.0]
if {![string is double -strict $clock_period] || $clock_period <= 0.0} {
	hacc_fail "CLOCK_PERIOD_NS must be a positive number, got '$clock_period'"
}

set run_tag [hacc_init_var RUN_TAG default]
set project_name [hacc_sanitize_project_name [hacc_init_var HACC_SG_PROJECT_NAME "${top_name}_power_${run_tag}"]]
set project_wdir [hacc_resolve_path $project_root [hacc_init_var HACC_SG_PROJECT_WDIR [file join $project_root spyglass]]]
set goal [hacc_init_var HACC_SG_GOAL power/power_est_average]

set extra_hdl [hacc_resolve_path_list $project_root [hacc_init_var HACC_SG_EXTRA_HDL {}]]
set extra_gateslib [hacc_resolve_path_list $project_root [hacc_init_var HACC_SG_EXTRA_GATESLIB {}]]
set extra_incdir [hacc_resolve_path_list $project_root [hacc_init_var HACC_SG_EXTRA_INCDIR {}]]
set extra_sgdc [hacc_resolve_path_list $project_root [hacc_init_var HACC_SG_EXTRA_SGDC {}]]

set activity_file [hacc_resolve_path $project_root [hacc_init_var HACC_SG_ACTIVITY_FILE ""]]
set activity_format [hacc_init_var HACC_SG_ACTIVITY_FORMAT ""]
set activity_start [hacc_init_var HACC_SG_ACTIVITY_START ""]
set activity_end [hacc_init_var HACC_SG_ACTIVITY_END ""]
set activity_weight [hacc_init_var HACC_SG_ACTIVITY_WEIGHT ""]
set activity_sim_top [hacc_init_var HACC_SG_ACTIVITY_SIM_TOP $top_name]
set activity_inst [hacc_init_var HACC_SG_ACTIVITY_INST ""]
set activity_rtl_design_nl [hacc_parse_bool HACC_SG_ACTIVITY_RTL_DESIGN_NL [hacc_init_var HACC_SG_ACTIVITY_RTL_DESIGN_NL 0]]
set debug_insts [hacc_init_var HACC_SG_DEBUG_INSTS {}]

foreach lib_file [concat $lib_files $extra_gateslib] {
	hacc_require_readable_file $lib_file "gates library"
}
foreach src_file [concat $pkg_files $extra_hdl] {
	hacc_require_readable_file $src_file "HDL file"
}
foreach inc_dir [concat $include_dirs $extra_incdir] {
	hacc_require_directory $inc_dir "include directory"
}
foreach sgdc_file $extra_sgdc {
	hacc_require_readable_file $sgdc_file "extra SGDC file"
}

if {$activity_file ne ""} {
	hacc_require_readable_file $activity_file "activity file"
	set activity_format [hacc_infer_activity_format $activity_file $activity_format]
}

set ordered_hdl [hacc_collect_sources $pkg_files $ordered_source_files $source_patterns $extra_hdl]
if {[llength $ordered_hdl] == 0} {
	hacc_fail "No RTL source files were collected for SpyGlass"
}
foreach src_file $ordered_hdl {
	hacc_require_readable_file $src_file "RTL source"
}

set all_incdirs {}
foreach inc_dir [concat $include_dirs $extra_incdir] {
	hacc_unique_append all_incdirs $inc_dir
}

file mkdir $project_wdir
set generated_dir [file join $project_wdir generated $project_name]
file mkdir $generated_dir
set base_sgdc [file join $generated_dir ${top_name}_power.sgdc]

hacc_write_power_sgdc \
	$base_sgdc \
	$top_name \
	$clock_port \
	$reset_port \
	$clock_period \
	$activity_file \
	$activity_format \
	$activity_start \
	$activity_end \
	$activity_weight \
	$activity_sim_top \
	$activity_inst \
	$activity_rtl_design_nl \
	$debug_insts

set sgdc_files [concat [list $base_sgdc] $extra_sgdc]
set gateslib_files [concat $lib_files $extra_gateslib]

set report_dir [file join $project_wdir $project_name $top_name $goal spyglass_reports power_est]

puts "============================================================"
puts " SpyGlass RTL power estimation"
puts " Project name : $project_name"
puts " Project wdir : $project_wdir"
puts " Top         : $top_name"
puts " Goal        : $goal"
puts " Clock (ns)  : $clock_period"
puts " Base SGDC   : $base_sgdc"
if {$activity_file ne ""} {
	puts " Activity    : $activity_file ($activity_format)"
} else {
	puts " Activity    : vectorless (no activity_data)"
}
puts " Reports     : $report_dir"
puts "============================================================"

set sg_run_result {7 FATAL}
if {[catch {
	new_project -f $project_name -projectwdir $project_wdir

	set_option top $top_name
	set_option language_mode verilog
	set_option enableSV 1
	set_option enableSV09 1
	set_option incdir [join $all_incdirs " "]
	set_option enable_gateslib_autocompile yes
	set_option enable_pass_exit_codes 1
	set_option enable_power_platform_flow true
	set_option enable_save_restore true

	read_file -type gateslib $gateslib_files
	read_file -type hdl $ordered_hdl

	current_goal $goal
	read_file -type sgdc $sgdc_files

	set_parameter pe_calc_sw yes
	set_parameter pe_calc_lk yes
	set_parameter pe_infer_clock_net_bufs yes
	set_parameter pe_infer_high_fanout_net_bufs yes
	set_parameter pe_report_memory_power yes
	set_parameter pe_report_leaf yes

	set sg_run_result [run_goal]
	current_goal none
	close_project -f
} err]} {
	catch {show_error}
	catch {close_project -f}
	puts "FATAL: $err"
	puts "SpyGlass Exit Code 7 (FATAL)"
	exit -f 7
}

set exit_code [lindex $sg_run_result 0]
set exit_status [lindex $sg_run_result 1]
if {$exit_code eq ""} {
	set exit_code 0
}
if {$exit_status eq ""} {
	set exit_status OK
}

puts "SpyGlass Exit Code $exit_code ($exit_status)"
exit -f $exit_code
