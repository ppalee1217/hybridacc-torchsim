# Read in top module
read_file -format sverilog {../src/Cluster/AddressGenerateUnit.sv}

current_design AddressGenerateUnit
link

set_host_options -max_core 8
source ../script/DC.sdc
check_design
uniquify
set_fix_multiple_port_nets -all -buffer_constants [get_designs *]
set_max_area 0

compile_ultra -timing_high_effort_script
compile_ultra -timing_high_effort_script -inc

file mkdir ../report/AddressGenerateUnit
file mkdir ../syn/AddressGenerateUnit

report_timing -path full -delay max -nworst 1 -max_paths 1 -significant_digits 4 -sort_by group > ../report/AddressGenerateUnit/timing_max_rpt_AddressGenerateUnit.txt
report_timing -path full -delay min -nworst 1 -max_paths 1 -significant_digits 4 -sort_by group > ../report/AddressGenerateUnit/timing_min_rpt_AddressGenerateUnit.txt
report_area -designware -nosplit > ../report/AddressGenerateUnit/area_rpt_AddressGenerateUnit.txt
report_power -analysis_effort low > ../report/AddressGenerateUnit/power_rpt_AddressGenerateUnit.txt
report_cell > ../report/AddressGenerateUnit/cell.log

write -hierarchy -format verilog -output {../syn/AddressGenerateUnit/AddressGenerateUnit_syn.v}
write_sdf -version 3.0 -context verilog {../syn/AddressGenerateUnit/AddressGenerateUnit.sdf}

exit