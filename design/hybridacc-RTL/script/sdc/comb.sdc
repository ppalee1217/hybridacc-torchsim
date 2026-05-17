#=====================================================================
# SDC for Purely Combinational Modules (Decoder, VMULU, VADDU)
#
# These modules have NO clock port. A virtual clock is created as a
# timing reference so that input-to-output propagation delay is
# constrained to fit within one clock period of the target system.
#=====================================================================

#=====================================================================
# Setting Clock freq & some parameter
#=====================================================================

if {[info exists ::clk_period]} {
    set clk_period $::clk_period
} elseif {[info exists ::CLOCK_PERIOD_NS] && [string is double -strict $::CLOCK_PERIOD_NS] && ($::CLOCK_PERIOD_NS > 0.0)} {
    set clk_period $::CLOCK_PERIOD_NS
} else {
    set clk_period 1.0
}
set input_max   [expr {double(round(1000*$clk_period * 0.6))/1000}]
set input_min   [expr {double(round(1000*$clk_period * 0.0))/1000}]
set output_max  [expr {double(round(1000*$clk_period * 0.1))/1000}]
set output_min  [expr {double(round(1000*$clk_period * 0.0))/1000}]

#=====================================================================
# Virtual Clock (not bound to any physical port)
#=====================================================================
create_clock -name vclk -period $clk_period

# NOTE: No set_dont_touch_network / set_fix_hold / set_ideal_network
#       because virtual clock has no physical network to protect.
# NOTE: No clock uncertainty / latency — the virtual clock is ideal.

set_input_transition 0.05 [all_inputs]

#=====================================================================
# Setting Design Environment
#=====================================================================
# 1. 修正 Operating Conditions
set_operating_conditions -min_library N16ADFP_StdCellff0p88v125c -min ff0p88v125c \
                         -max_library N16ADFP_StdCellss0p72vm40c -max ss0p72vm40c
set_min_library N16ADFP_StdCellss0p72vm40c.db -min_version N16ADFP_StdCellff0p88v125c.db
set_min_library N16ADFP_SRAM_ss0p72v0p72vm40c_100a.db -min_version N16ADFP_SRAM_ff0p88v0p88v125c_100a.db

# 2. 驅動能力設定 — combinational modules have no clk port
set_driving_cell -library N16ADFP_StdCellss0p72vm40c -lib_cell DFQD1BWP16P90LVT -pin {Q} \
                 [all_inputs]

# 3. 輸出負載設定 (強烈建議開啟，模擬真實電路負載)
set_load [load_of "N16ADFP_StdCellss0p72vm40c/DFQD1BWP16P90LVT/D"] [all_outputs]

set_input_delay  -clock vclk  -max $input_max   [all_inputs]
set_input_delay  -clock vclk  -min $input_min   [all_inputs]

set_output_delay -clock vclk  -max $output_max  [all_outputs]
set_output_delay -clock vclk  -min $output_min  [all_outputs]

# set_wire_load_model -name ZeroWireload -library N16ADFP_StdCellss0p72vm40c
set_wire_load_mode segmented

#=====================================================================
# Setting DRC Constraints
#=====================================================================
set_max_area          0
set_max_fanout       10 [all_inputs]
set_max_transition  0.1 [all_inputs]
set_max_capacitance 0.1 [all_inputs]

set bus_inference_style {%s[%d]}
set bus_naming_style {%s[%d]}
set hdlout_internal_busses true
change_names -hierarchy -rule verilog
define_name_rules name_rule -allowed "A-Z a-z 0-9 _" -max_length 255 -type cell
define_name_rules name_rule -allowed "A-Z a-z 0-9 _[]" -max_length 255 -type net
define_name_rules name_rule -map {{"\\*cell\\*" "cell"}}
define_name_rules name_rule -case_insensitive
change_names -hierarchy -rules name_rule

set compile_ultra_ungroup_dw true