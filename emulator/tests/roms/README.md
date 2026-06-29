# Third-party CPU test images

These two 64KB binaries are **test fixtures only** — they are not part of the
emulator and are never linked into or distributed with `rp6502-emu`. They are
included here so the CPU conformance tests run offline / in CI.

Source: Klaus Dormann's 6502/65C02 functional test suite
<https://github.com/Klaus2m5/6502_65C02_functional_tests>

License: **GPL-3.0** (see the upstream `license.txt`). This is mere aggregation
with the BSD-3-licensed emulator — keeping the GPL test data isolated in this
directory and out of the build product.

| File | Loads at | Start | Success trap (`jmp *`) |
|---|---|---|---|
| `6502_functional_test.bin` | `$0000` (full 64K) | `$0400` | `$3469` |
| `65C02_extended_opcodes_test.bin` | `$0000` (full 64K) | `$0400` | `$24F1` |

The harness (`tests/test_cpu_conformance.c`) loads the image, points the reset
vector at `$0400`, runs until the CPU self-traps, and passes iff the trap
address equals the success address above (any other self-trap is a failing
subtest — cross-reference the upstream `.lst`).

To refresh: re-fetch from upstream and re-confirm the success addresses from
the matching `.lst` files (search for `jmp *  ;test passed, no errors`).
