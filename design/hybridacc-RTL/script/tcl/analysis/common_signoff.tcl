proc hacc_signoff_fail {message} {
    puts "ERROR: $message"
    exit 1
}

proc hacc_signoff_init_var {name default} {
    if {[info exists ::$name] && [string trim [set ::$name]] ne ""} {
        return
    }
    if {[info exists ::env($name)] && [string trim $::env($name)] ne ""} {
        set ::$name [string trim $::env($name)]
        return
    }
    set ::$name $default
}

proc hacc_signoff_truthy {value} {
    set lowered [string tolower [string trim $value]]
    return [expr {$lowered in {1 y yes true on}}]
}

proc hacc_signoff_find_project_root {start_dir} {
    set current [file normalize $start_dir]
    while {1} {
        if {[file exists [file join $current src hybridacc_utils_pkg.sv]] &&
            [file exists [file join $current script sdc seq_top.sdc]]} {
            return $current
        }
        set parent [file dirname $current]
        if {$parent eq $current} {
            hacc_signoff_fail "Unable to locate HybridAcc RTL project root from $start_dir"
        }
        set current $parent
    }
}

proc hacc_signoff_init_paths {} {
    if {[info exists ::HACC_PROJECT_ROOT] && [string trim $::HACC_PROJECT_ROOT] ne ""} {
        return
    }

    set ::HACC_ANALYSIS_SCRIPT_DIR [file dirname [file normalize [info script]]]
    set ::HACC_PROJECT_ROOT [hacc_signoff_find_project_root $::HACC_ANALYSIS_SCRIPT_DIR]
    set ::HACC_SCRIPT_ROOT [file join $::HACC_PROJECT_ROOT script]
    set ::HACC_SDC_ROOT [file join $::HACC_SCRIPT_ROOT sdc]
}

proc hacc_signoff_setup_libraries {} {
    set_app_var search_path [list \
        /cad/process/ADFP/Executable_Package/Collaterals/IP/stdcell/N16ADFP_StdCell/NLDM \
        /cad/process/ADFP/Executable_Package/Collaterals/IP/sram/N16ADFP_SRAM/NLDM \
        /usr/cad/synopsys/synthesis/2024.09-sp2/libraries/syn]

    set_app_var link_path [list \
        * \
        N16ADFP_StdCellss0p72vm40c.db \
        N16ADFP_StdCellff0p88v125c.db \
        N16ADFP_SRAM_ss0p72v0p72vm40c_100a.db \
        N16ADFP_SRAM_ff0p88v0p88v125c_100a.db \
        dw_foundation.sldb]
}

proc hacc_signoff_finish {} {
    hacc_signoff_init_var GUI_MODE 0
    if {[hacc_signoff_truthy $::GUI_MODE]} {
        if {[llength [info commands start_gui]] > 0} {
            catch {start_gui} start_gui_result
            if {[string trim $start_gui_result] ne ""} {
                puts "INFO: start_gui returned: $start_gui_result"
            }
        }
        puts "INFO: GUI mode requested; leaving shell open."
        return
    }
    quit
}
