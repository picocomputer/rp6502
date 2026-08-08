# The harness itself. Only reached by the RTL tree, which is the one that
# builds the oracle.

# The reference oracle links and runs headless before anything is compared
# against it, so a broken oracle fails here rather than as a mysterious
# mismatch in whichever comparison test happens to run first.
rp6502_add_test(oracle
    SOURCES test_oracle.cpp
    LIBS fpga_oracle FIXTURE adventure.rp6502)
