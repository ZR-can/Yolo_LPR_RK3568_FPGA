# Source-only PDS batch flow for the MES2L100H PCIe DMA/PIO wrapper.
# Run from this directory with:
#   D:/PDS_2022.2-SP6.4/bin/pds_shell.exe -file impl.tcl -work_dir .

set project_dir [file normalize [file dirname [info script]]]
set source_list [file join $project_dir scripts pds_sources.f]
set constraint_file [file join $project_dir fdc pcie_dma_test_100h_top.fdc]

set_arch -family Logos2 -device PG2L100H -speedgrade -6 -package FBG484

set fp [open $source_list r]
while {[gets $fp line] >= 0} {
    set line [string trim $line]
    if {$line eq ""} {
        continue
    }
    if {[string match "#*" $line]} {
        continue
    }

    set rel_path [string trim [lindex [split $line "#"] 0]]
    add_design [file normalize [file join $project_dir $rel_path]]
}
close $fp

add_constraint [file normalize $constraint_file]

compile -top_module pcie_dma_test_100h_top
synthesize -ads -selected_syn_tool_opt 2
dev_map
pnr
report_timing
gen_bit_stream -compress_bitstream true -master_configuration_clock_frequency {40M}
