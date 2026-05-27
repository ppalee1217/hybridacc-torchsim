if {![info exists clk_period]} {
    set clk_period 1.25
}

set data_inputs [remove_from_collection [all_inputs] [get_ports {clk reset_n}]]
set async_reset_port [get_ports reset_n]
set async_reset_pins [all_registers -async_pins]

set input_max  [expr {double(round(1000.0 * $clk_period * 0.6)) / 1000.0}]
set input_min  0.0
set output_max [expr {double(round(1000.0 * $clk_period * 0.1)) / 1000.0}]
set output_min 0.0

create_clock -name clk -period $clk_period [get_ports clk]

set_clock_uncertainty 0.01 [all_clocks]
set_clock_latency 0.2 [all_clocks]
set_clock_latency -source 0 [all_clocks]

set_input_transition 0.2 $data_inputs
set_clock_transition 0.1 [all_clocks]
set_load 0.005 [all_outputs]

set_operating_conditions \
    -min_library N16ADFP_StdCellff0p88v125c -min ff0p88v125c \
    -max_library N16ADFP_StdCellss0p72vm40c -max ss0p72vm40c

set_min_library N16ADFP_StdCellss0p72vm40c.db \
    -min_version N16ADFP_StdCellff0p88v125c.db
set_min_library N16ADFP_SRAM_ss0p72v0p72vm40c_100a.db \
    -min_version N16ADFP_SRAM_ff0p88v0p88v125c_100a.db

set_driving_cell -library N16ADFP_StdCellss0p72vm40c \
    -lib_cell BUFFD4BWP16P90LVT -pin Z [get_ports clk]
set_driving_cell -library N16ADFP_StdCellss0p72vm40c \
    -lib_cell DFQD1BWP16P90LVT -pin Q \
    $data_inputs

set_input_delay -clock clk -max $input_max $data_inputs
set_input_delay -clock clk -min $input_min $data_inputs
set_output_delay -clock clk -max $output_max [all_outputs]
set_output_delay -clock clk -min $output_min [all_outputs]

# reset_n is an asynchronous reset source, not a synchronous timed data input.
set_ideal_network $async_reset_port
set_false_path -from $async_reset_port -to $async_reset_pins
