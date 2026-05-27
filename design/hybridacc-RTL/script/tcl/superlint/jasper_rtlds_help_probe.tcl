puts "CONFIG_RTLDS_HELP_BEGIN"
if {[catch {help config_rtlds} help_status]} {
    puts "HELP_FAILED=$help_status"
}
puts "CONFIG_RTLDS_HELP_END"
exit