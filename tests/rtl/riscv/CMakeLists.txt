rp6502_add_machine_test(mtime TIMEOUT 120)

rp6502_add_machine_test(ram TIMEOUT 120)

rp6502_add_machine_test(hold TIMEOUT 600)

# cache.sv alone, at the size the machine uses, with two ways, and with so
# few sets that nearly every miss replaces a line.
foreach(_cache IN ITEMS "cache 128 4" "cache128x2 128 2"
        "cache4x4 4 4" "cache2x2 2 2")
    separate_arguments(_cache)
    list(GET _cache 0 _name)
    list(GET _cache 1 _sets)
    list(GET _cache 2 _ways)
    rp6502_add_module_test(${_name}
        TOP cache
        PREFIX V${_name}
        RTL ${RP6502_SRC}/core/riscv/cache.sv
        SOURCES test_cache.cpp
        ARGS -GSETS=${_sets} -GWAYS=${_ways} --x-initial unique
        DEFS CACHE_MODEL=V${_name} CACHE_SETS=${_sets} CACHE_WAYS=${_ways}
        TIMEOUT 300)
endforeach()
