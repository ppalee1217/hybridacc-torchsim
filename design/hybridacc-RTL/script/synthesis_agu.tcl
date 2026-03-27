#   Read in top module
# read_file -autoread -top top {../src/ ../include} , if your top module named top
read_file -format sverilog {../src/AGU.sv}

# SET POWER INTENT and ENVIRONMENT ###################################
current_design AGU
link

#   Set Design Environment
set_host_options -max_core 8
source ../script/DC.sdc
check_design
uniquify
set_fix_multiple_port_nets -all -buffer_constants [get_designs *]
set_max_area 0


#   Synthesize circuit
compile_ultra -timing_high_effort_script
compile_ultra -timing_high_effort_script -inc
# compile -map_effort high -area_effort high
#compile -map_effort high -area_effort high -inc

file mkdir ../report/agu
file mkdir ../syn/agu

#   Create Report
#timing report(setup time)
report_timing -path full -delay max -nworst 1 -max_paths 1 -significant_digits 4 -sort_by group > ../report/agu/timing_max_rpt_agu.txt
#timing report(hold time)
report_timing -path full -delay min -nworst 1 -max_paths 1 -significant_digits 4 -sort_by group > ../report/agu/timing_min_rpt_agu.txt
#area report
report_area -designware -nosplit > ../report/agu/area_rpt_agu.txt
#report power
report_power -analysis_effort low > ../report/agu/power_rpt_agu.txt

report_cell > ../report/agu/cell.log

#   Save syntheized file
#                                   " change here for your own filename "
write -hierarchy -format verilog -output {../syn/agu/agu_syn.v}
write_sdf -version 3.0 -context verilog {../syn/agu/agu.sdf}

exit