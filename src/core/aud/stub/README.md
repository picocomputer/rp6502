Silent stand-ins for the three audio engines, port for port.

They exist for one reason: the Pocket fit sits at 83% of the device and
closes its worst path on the fitter's luck, so two unrelated four-line
edits to pocket_file.sv both came back failing setup on a path in
ria_regs' console FIFO that neither of them touched. A diagnostic
bitstream cannot be built while that is true, and the question it has to
answer -- why the host's Get File response is invisible to the fabric
after the first one -- needs a bitstream to answer it.

The OPL2 is the largest single thing in the machine and none of it is
involved in the question. Swapping these in frees that area so the
fitter has room to place the parts that matter, and so a failed fit
means the change broke something rather than that the dice came up bad.

Diagnostic only. A core built with these makes no sound.
