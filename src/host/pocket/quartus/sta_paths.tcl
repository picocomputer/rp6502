# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Name the paths, every fit. The signoff report carries only worst-case
# slack numbers, so a gate failure in CI was an anonymous number on a
# machine whose placement cannot be reproduced here — the pair that
# actually failed was gone before anyone could ask. This writes the
# few worst paths per check per corner beside the report the gate
# reads, and the gate prints them when it says no.

project_open rp6502
create_timing_netlist
read_sdc
update_timing_netlist

set out output_files/rp6502.paths.rpt
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
