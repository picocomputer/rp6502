# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

# quartus_sta -t sta_paths.tcl <project> [sdc ...] writes two reports beside
# the fit's:
#
# - output_files/<project>.paths.rpt, the worst paths of every corner;
# - output_files/<project>.names.rpt, what the netlist matches for each
#   register collection in the SDC files named, and for each target of the
#   project's instance assignments. Quartus applies a constraint or an
#   assignment to whatever its name matches, and when that is nothing it
#   mostly says nothing, so sta_gate.py and assignment_gate.py judge these
#   counts.
set project [lindex $quartus(args) 0]
set sdcs [lrange $quartus(args) 1 end]
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

# A name matches a register, a port or a pin, or, as a hierarchy path, the
# cells under it. A bus's name matches its bits.
proc matches {name} {
    expr {[get_collection_size [get_keepers -nowarn $name]]
        + [get_collection_size [get_keepers -nowarn "$name\[*\]"]]
        + [get_collection_size [get_cells -nowarn "$name|*"]]}
}

set f [open output_files/$project.names.rpt w]
foreach sdc $sdcs {
    set in [open $sdc]
    set text [read $in]
    close $in
    set seen {}
    foreach {- pattern} [regexp -all -inline \
            {get_registers[\s\\]+\{([^\}]*)\}} $text] {
        if {[lsearch -exact $seen $pattern] >= 0} continue
        lappend seen $pattern
        # The fitter's copies of a register, named ~DUPLICATE, come and go
        # with placement, so only the design's own registers are counted.
        set n 0
        foreach_in_collection r [get_registers -nowarn $pattern] {
            if {![string match *~DUPLICATE* [get_node_info -name $r]]} {
                incr n
            }
        }
        puts $f "collection;$n;$pattern"
    }
}
set top [get_global_assignment -name TOP_LEVEL_ENTITY]
foreach_in_collection a [get_all_instance_assignments -name *] {
    lassign $a - from to name - entity
    # An assignment to another entity names paths inside each of its
    # instances; Quartus warns itself when that entity is not in the design.
    if {$entity ne "" && $entity ne $top} continue
    foreach target [list $from $to] {
        if {$target ne ""} {
            puts $f "assignment;[matches $target];$name;$target"
        }
    }
}
close $f

project_close
