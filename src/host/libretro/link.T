/* The libretro ABI is the whole of what a core offers. Everything else the
 * machine is made of — sram, xram, main_run, the drivers — is this library's
 * business and nobody else's. */
{
    global:
        retro_*;
    local:
        *;
};
