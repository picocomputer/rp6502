# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

# quartus_sta -t sta_paths.tcl <project> writes
# output_files/<project>.paths.rpt, the worst paths of every corner.
set project [lindex $quartus(args) 0]
project_open $project
create_timing_netlist
read_sdc
update_timing_netlist

set out output_files/$project.paths.rpt
file delete $out

foreach_in_collection oc [get_available_operating_conditions] {
    set_operating_conditions $oc
    update_timing_netlist
    report_timing -hold -npaths 5 -append -file $out
    report_timing -setup -npaths 3 -append -file $out
    report_timing -recovery -npaths 2 -append -file $out
    report_timing -removal -npaths 2 -append -file $out
}

project_close
