
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
# 2. Setting Clock Constraints
#=====================================================================
create_clock -name clk -period $clk_period [get_ports clk]

# 在 16nm，Uncertainty 是關鍵 (包含 Jitter 與 OCV 預估)
set_clock_uncertainty  0.01  [all_clocks]
set_clock_latency      0.2   [all_clocks]
set_clock_latency -source 0  [all_clocks]

# 合成階段建議設定為 Ideal，Latency 留給後端 CTS 實際計算
set_ideal_network            [all_clocks]
set_dont_touch_network       [all_clocks]

# 16nm 的訊號轉換速度非常快
set_input_transition   0.2   [all_inputs]
set_clock_transition   0.1   [all_clocks]

# 模擬輸出負載，0.005pF 是 16nm 常見的典型負載
set_load 0.005 [all_outputs]

# 讓工具在合成階段就稍微修正 Hold，但主要大修留給後端
set_fix_hold                 [all_clocks]

#=====================================================================
# Setting Design Environment
#=====================================================================
set_operating_conditions -min_library N16ADFP_StdCellff0p88v125c -min ff0p88v125c \
                         -max_library N16ADFP_StdCellss0p72vm40c -max ss0p72vm40c

set_driving_cell -library N16ADFP_StdCellss0p72vm40c -lib_cell BUFFD4BWP16P90LVT -pin {Z} [get_ports clk]
set_driving_cell -library N16ADFP_StdCellss0p72vm40c -lib_cell DFQD1BWP16P90LVT  -pin {Q} [remove_from_collection [all_inputs] [get_ports clk]]
#set_load [load_of "N16ADFP_StdCellss0p72vm40c/DFQD1BWP16P90LVT/D"] [all_outputs]

set_input_delay  -clock clk -max $input_max  [remove_from_collection [all_inputs] [get_ports clk]]
set_input_delay  -clock clk -min $input_min  [remove_from_collection [all_inputs] [get_ports clk]]
set_output_delay -clock clk -max $output_max [all_outputs]
set_output_delay -clock clk -min $output_min [all_outputs]

set_wire_load_model -name ZeroWireload -library N16ADFP_StdCellss0p72vm40c

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