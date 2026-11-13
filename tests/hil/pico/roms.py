# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""The ROMs of the hardware tests, in the assembler of the test suite."""

from rp6502_asm import (API_A, API_ERRNO, API_X, OP_ATTR_GET, OP_ATTR_SET,
                        OP_CLOSE, OP_EXIT, OP_OPEN, OP_READ_XRAM, OP_REMOVE,
                        OP_WRITE_XRAM, O_CREAT, O_RDONLY, O_TRUNC, O_WRONLY,
                        RIA_READY, RIA_RX, RIA_TX, RW0_ADDR, RW0_DATA,
                        RW0_STEP, RW1_ADDR, RW1_DATA, RW1_STEP, XSTACK, Asm,
                        puthex, putnib, putc)

API_SREG = 0xFFF8
ATTR_ERRNO_OPT = 0x00
ATTR_CLK_RUN_MS = 0x10
ERRNO_OPT_CC65 = 1

# Zero page.
FD, PCM_FD, STEP = 0x10, 0x11, 0x12
BS, N, COUNT, RET = 0x14, 0x16, 0x18, 0x1A
T0, T, SUM, WMS = 0x20, 0x24, 0x28, 0x2C
LOOPS, ERRS, SEED, ACC = 0x30, 0x33, 0x35, 0x36
NUM, DEN, REM, TMP = 0x38, 0x3C, 0x40, 0x44
NDIG, WIDTH, DIGITS = 0x47, 0x48, 0x49

# The cc65 errno numbers.
ERRNO_NAMES = ("ENOENT", "ENOMEM", "EACCES", "ENODEV", "EMFILE", "EBUSY",
               "EINVAL", "ENOSPC", "EEXIST", "EAGAIN", "EIO", "EINTR",
               "ENOSYS", "ESPIPE", "ERANGE", "EBADF", "ENOEXEC", "EUNKNOWN")


def audio_start(p, psg_at, opl_at):
    """Maps the PSG and the OPL2 and plays every voice: the eight PSG
    voices, two of them noise, and the nine OPL2 channels, both operators
    loud. Opens PCM:48000,16,2 into PCM_FD."""
    p.xreg(0, 1, 0, psg_at)
    for v in range(8):
        f = 600 + 350 * v
        wave = 4 if v in (3, 6) else v % 4
        for at, val in ((0, f & 0xFF), (1, f >> 8), (2, 0x80), (3, 0x00),
                        (4, 0x00), (5, wave << 4),
                        (6, ((v * 30 - 105) * 2 & 0xFE) | 1)):
            p.poke(psg_at + 8 * v + at, val)
    p.xreg(0, 1, 1, opl_at)
    regs = [(0x01, 0x20), (0xBD, 0xC0)]
    mod = (0, 1, 2, 8, 9, 10, 16, 17, 18)
    for c in range(9):
        for s in (mod[c], mod[c] + 3):
            regs += [(0x20 + s, 0xE1), (0x40 + s, 0x00), (0x60 + s, 0xF0),
                     (0x80 + s, 0x0F), (0xE0 + s, (c + s) % 4)]
        fnum = 0x150 + 0x18 * c
        regs += [(0xC0 + c, 0x0F), (0xA0 + c, fnum & 0xFF),
                 (0xB0 + c, 0x20 | (4 << 2) | (fnum >> 8))]
    for reg, val in regs:
        p.poke(opl_at + reg, val)
    p.push_str("PCM:48000,16,2")
    p.call_a(OP_OPEN, O_WRONLY)
    p.sta_zp(PCM_FD)


def zero_xram(p, at, size):
    """Zeroes `size` bytes of XRAM, a multiple of 256, from `at`."""
    p.store(RW0_STEP, 1)
    p.store(RW0_ADDR, at & 0xFF)
    p.store(RW0_ADDR + 1, at >> 8)
    p.lda_imm(0)
    p.ldy_imm(size // 256)
    page = p.symbol(p.local("zero"))
    p.ldx_imm(0)
    byte = p.symbol(p.local("zero"))
    p.sta_abs(RW0_DATA)
    p.inx()
    p.bne(byte)
    p.dey()
    p.bne(page)


def exit_with(p, code):
    p.store(API_A, code)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.stp()


def call(p, op, step):
    """An API call that leaves the index of `step` in TPUT_STEPS in STEP,
    for fail."""
    p.store(STEP, TPUT_STEPS.index(step))
    p.call(op)


def fail_on_error(p):
    """Goes to fail when the call returned -1, which sets N."""
    with p.branch("bpl"):
        p.jmp_abs("fail")


def fail_unless_ax(p, at):
    """Goes to fail unless A and X hold the 16 bits at `at`."""
    p.cmp_zp(at)
    with p.branch("beq"):
        p.jmp_abs("fail")
    p.cpx_zp(at + 1)
    with p.branch("beq"):
        p.jmp_abs("fail")


def push_zp16(p, at):
    p.lda_zp(at + 1)
    p.sta_abs(XSTACK)
    p.lda_zp(at)
    p.sta_abs(XSTACK)


def copy(p, dst, src, size):
    for i in range(size):
        p.lda_zp(src + i)
        p.sta_zp(dst + i)


def add32(p, dst, a, b):
    p.clc()
    for i in range(4):
        p.lda_zp(a + i)
        p.adc_zp(b + i)
        p.sta_zp(dst + i)


def sub32(p, dst, a, b):
    p.sec()
    for i in range(4):
        p.lda_zp(a + i)
        p.sbc_zp(b + i)
        p.sta_zp(dst + i)


def count_down(p, loop):
    """Decrements the 16 bits of COUNT and goes to `loop` until they are
    zero."""
    p.lda_zp(COUNT)
    with p.branch("bne"):
        p.dec_zp(COUNT + 1)
    p.dec_zp(COUNT)
    p.lda_zp(COUNT)
    p.ora_zp(COUNT + 1)
    with p.branch("beq"):
        p.jmp_abs(loop)


def div32(p):
    """NUM becomes NUM / DEN, and REM the remainder, all 32 bits."""
    for i in range(4):
        p.stz_zp(REM + i)
    p.ldx_imm(32)
    bit = p.symbol("div32.bit")
    p.asl_zp(NUM)
    for i in range(1, 4):
        p.rol_zp(NUM + i)
    for i in range(4):
        p.rol_zp(REM + i)
    p.sec()
    for i in range(3):
        p.lda_zp(REM + i)
        p.sbc_zp(DEN + i)
        p.sta_zp(TMP + i)
    p.lda_zp(REM + 3)
    p.sbc_zp(DEN + 3)
    with p.branch("bcc"):
        p.sta_zp(REM + 3)
        for i in range(3):
            p.lda_zp(TMP + i)
            p.sta_zp(REM + i)
        p.inc_zp(NUM)
    p.dex()
    p.bne(bit)
    p.rts()


def putdec(p):
    """Prints NUM in decimal, right-aligned in WIDTH columns."""
    p.stz_zp(NDIG)
    p.store(DEN, 10)
    for i in range(1, 4):
        p.stz_zp(DEN + i)
    digit = p.symbol("putdec.digit")
    p.jsr_abs("div32")
    p.ldx_zp(NDIG)
    p.lda_zp(REM)
    p.ora_imm(ord("0"))
    p.sta_zpx(DIGITS)
    p.inc_zp(NDIG)
    p.lda_zp(NUM)
    for i in range(1, 4):
        p.ora_zp(NUM + i)
    p.bne(digit)
    pad = p.symbol("putdec.pad")
    p.lda_zp(NDIG)
    p.cmp_zp(WIDTH)
    with p.branch("bcs"):
        p.lda_imm(ord(" "))
        p.jsr_abs("putc")
        p.dec_zp(WIDTH)
        p.bra(pad)
    p.ldx_zp(NDIG)
    out = p.symbol("putdec.out")
    p.dex()
    p.lda_zpx(DIGITS)
    p.jsr_abs("putc")
    p.cpx_imm(0)
    p.bne(out)
    p.rts()


def print_dec(p, at, size, width=0):
    """Prints the `size` bytes at `at` in decimal."""
    for i in range(4):
        if i < size:
            p.lda_zp(at + i)
            p.sta_zp(NUM + i)
        else:
            p.stz_zp(NUM + i)
    p.store(WIDTH, width)
    p.jsr_abs("putdec")


def print_kbps(p, ms, width):
    """Prints TPUT_TOTAL bytes in the milliseconds at `ms` as KB/s, and 0
    for no time at all."""
    total = TPUT_TOTAL * 1000 // 1024
    for i in range(4):
        p.store(NUM + i, total >> (8 * i) & 0xFF)
    copy(p, DEN, ms, 4)
    p.lda_zp(DEN)
    for i in range(1, 4):
        p.ora_zp(DEN + i)
    some, out = p.local("kbps"), p.local("kbps")
    p.bne(some)
    for i in range(4):
        p.stz_zp(NUM + i)
    p.bra(out)
    p.symbol(some)
    p.jsr_abs("div32")
    p.symbol(out)
    p.store(WIDTH, width)
    p.jsr_abs("putdec")


TPUT_FILE = "hil-tput.tmp"
TPUT_TOTAL = 512 * 1024
TPUT_SIZES = (512, 1024, 2048, 4096, 8192, 16384)
TPUT_PSG, TPUT_OPL = 0xFE00, 0xFF00
TPUT_PCM_AT, TPUT_PCM_LEN = 0x8000, 4096
TPUT_STEPS = ("open PCM:48000,16,2", f"open {TPUT_FILE}", "write_xram",
              "read_xram", "close")


def tput():
    """XRAM transfer speed with every sound engine playing. For each block
    size, write_xram writes TPUT_TOTAL bytes to TPUT_FILE in the current
    directory, timed from the open to the close. Then read_xram reads them
    back, and only the reads are timed. After each read, write_xram gives
    PCM 4 KB of silence. Each block size prints a row of the table: the
    block size, the KB/s of the writes and of the reads, and their
    milliseconds. A failed call prints a line that starts with "?"."""
    p = Asm()
    p.push(ERRNO_OPT_CC65)
    p.call_a(OP_ATTR_SET, ATTR_ERRNO_OPT)
    zero_xram(p, TPUT_PCM_AT, TPUT_PCM_LEN)
    p.store(STEP, TPUT_STEPS.index("open PCM:48000,16,2"))
    audio_start(p, TPUT_PSG, TPUT_OPL)
    fail_on_error(p)  # the open of PCM: ends audio_start
    p.say(f"{TPUT_FILE}, {TPUT_TOTAL // 1024} KB per block size, "
          "PSG, OPL2, and PCM playing\r\n")
    p.say("block   write KB/s  read KB/s\r\n")
    for bs in TPUT_SIZES:
        for at, v in ((BS, bs), (N, TPUT_TOTAL // bs)):
            p.store(at, v & 0xFF)
            p.store(at + 1, v >> 8)
        p.jsr_abs("run")
    p.push_str(TPUT_FILE)
    p.call(OP_REMOVE)
    p.say("done\r\n")
    exit_with(p, 0)

    p.symbol("run")
    p.push_str(TPUT_FILE)
    p.store(API_A, O_WRONLY | O_CREAT | O_TRUNC)
    call(p, OP_OPEN, f"open {TPUT_FILE}")
    fail_on_error(p)
    p.sta_zp(FD)
    p.jsr_abs("clock")
    copy(p, T0, T, 4)
    copy(p, COUNT, N, 2)
    loop = p.symbol("write")
    p.pushw(0)
    push_zp16(p, BS)
    p.lda_zp(FD)
    p.sta_abs(API_A)
    call(p, OP_WRITE_XRAM, "write_xram")
    fail_unless_ax(p, BS)
    count_down(p, loop)
    p.lda_zp(FD)
    p.sta_abs(API_A)
    call(p, OP_CLOSE, "close")
    fail_on_error(p)
    p.jsr_abs("clock")
    sub32(p, WMS, T, T0)

    p.push_str(TPUT_FILE)
    p.store(API_A, O_RDONLY)
    call(p, OP_OPEN, f"open {TPUT_FILE}")
    fail_on_error(p)
    p.sta_zp(FD)
    for i in range(4):
        p.stz_zp(SUM + i)
    copy(p, COUNT, N, 2)
    loop = p.symbol("read")
    p.jsr_abs("clock")
    copy(p, T0, T, 4)
    p.pushw(0)
    push_zp16(p, BS)
    p.lda_zp(FD)
    p.sta_abs(API_A)
    call(p, OP_READ_XRAM, "read_xram")
    p.sta_zp(RET)
    p.stx_zp(RET + 1)
    p.jsr_abs("clock")
    add32(p, SUM, SUM, T)
    sub32(p, SUM, SUM, T0)
    p.lda_zp(RET)
    p.ldx_zp(RET + 1)
    fail_unless_ax(p, BS)
    p.pushw(TPUT_PCM_AT)
    p.pushw(TPUT_PCM_LEN)
    p.lda_zp(PCM_FD)
    p.sta_abs(API_A)
    p.call(OP_WRITE_XRAM)
    count_down(p, loop)
    p.lda_zp(FD)
    p.sta_abs(API_A)
    call(p, OP_CLOSE, "close")
    fail_on_error(p)
    print_dec(p, BS, 2, 5)
    p.say("   ")
    print_kbps(p, WMS, 10)
    p.say(" ")
    print_kbps(p, SUM, 10)
    p.say("   (")
    print_dec(p, WMS, 4)
    p.say(" ms, ")
    print_dec(p, SUM, 4)
    p.say(" ms)\r\n")
    p.rts()

    # T is the run time of the 6502 in milliseconds.
    p.symbol("clock")
    p.call_a(OP_ATTR_GET, ATTR_CLK_RUN_MS)
    p.sta_zp(T)
    p.stx_zp(T + 1)
    p.lda_abs(API_SREG)
    p.sta_zp(T + 2)
    p.lda_abs(API_SREG + 1)
    p.sta_zp(T + 3)
    p.rts()

    # Prints "?<step> failed: <errno>" for a call that returned -1, or
    # "?<step> returned <n> of <block size>", and exits with 1.
    p.symbol("fail")
    p.sta_zp(RET)
    p.stx_zp(RET + 1)
    p.say("?")
    for i, step in enumerate(TPUT_STEPS):
        p.lda_zp(STEP)
        p.cmp_imm(i)
        with p.branch("bne"):
            p.say(step)
            p.jmp_abs("fail.step")
    p.symbol("fail.step")
    p.lda_zp(RET)
    p.and_zp(RET + 1)
    p.cmp_imm(0xFF)
    with p.branch("beq"):
        p.say(" returned ")
        print_dec(p, RET, 2)
        p.say(" of ")
        print_dec(p, BS, 2)
        p.jmp_abs("fail.end")
    p.say(" failed: ")
    for i, name in enumerate(ERRNO_NAMES, 1):
        p.lda_abs(API_ERRNO)
        p.cmp_imm(i)
        with p.branch("bne"):
            p.say(name)
            p.jmp_abs("fail.end")
    p.say("errno ")
    p.lda_abs(API_ERRNO)
    p.sta_zp(RET)
    p.lda_abs(API_ERRNO + 1)
    p.sta_zp(RET + 1)
    print_dec(p, RET, 2)
    p.symbol("fail.end")
    p.say("\r\n")
    exit_with(p, 1)

    p.symbol("div32")
    div32(p)
    p.symbol("putdec")
    putdec(p)
    p.use(putc)
    return p


AUDIO_PSG, AUDIO_OPL = 0xC000, 0xC100
AUDIO_LOOPS = 0xFF00


def audio():
    """Every sound engine at once, with the bus busy. The loop rings the
    bell with a BEL, then four times gives PCM 4 KB with write_xram and
    writes XRAM 2048 times at the full speed of the bus. The XRAM byte at
    AUDIO_LOOPS counts the loops."""
    p = Asm()
    audio_start(p, AUDIO_PSG, AUDIO_OPL)
    p.store(RW1_STEP, 0)
    p.store(RW1_ADDR, AUDIO_LOOPS & 0xFF)
    p.store(RW1_ADDR + 1, AUDIO_LOOPS >> 8)
    p.store(RW0_STEP, 1)
    loop = p.symbol("loop")
    p.lda_abs(RW1_DATA)
    p.clc()
    p.adc_imm(1)
    p.sta_abs(RW1_DATA)
    p.store(RIA_TX, 0x07)
    for k in range(4):
        p.pushw(0x1000 * k)
        p.pushw(4096)
        p.lda_zp(PCM_FD)
        p.sta_abs(API_A)
        p.call(OP_WRITE_XRAM)
        p.ldx_imm(32)
        flood = p.symbol(p.local("flood"))
        p.store(RW0_ADDR, 0x00)
        p.store(RW0_ADDR + 1, 0x40)
        for _ in range(64):
            p.sta_abs(RW0_DATA)
        p.dex()
        with p.branch("beq"):
            p.jmp_abs(flood)
    p.jmp_abs(loop)
    return p


BUS_FIXED, BUS_SEEDED, BUS_COUNTERS = 0x2000, 0x1000, 0xFF00


def bus():
    """The 6502's accesses of the RIA registers, checked. Each loop writes
    256 seeded bytes through RW0 and reads them back through RW1, reads 256
    fixed bytes back to back through RW1, and pushes and pulls 16 bytes
    through the XSTACK. LOOPS, 24 bits, and ERRS, the mismatches in 16 bits,
    go to XRAM at BUS_COUNTERS after each loop."""
    p = Asm()
    p.lda_imm(1)
    p.sta_abs(RW0_STEP)
    p.sta_abs(RW1_STEP)
    for z in (SEED, LOOPS, LOOPS + 1, LOOPS + 2, ERRS, ERRS + 1):
        p.stz_zp(z)
    p.store(RW0_ADDR, BUS_FIXED & 0xFF)
    p.store(RW0_ADDR + 1, BUS_FIXED >> 8)
    p.ldx_imm(0)
    fill = p.symbol("fill")
    p.stx_abs(RW0_DATA)
    p.inx()
    p.bne(fill)

    def count_error():
        with p.branch("beq"):
            p.inc_zp(ERRS)
            with p.branch("bne"):
                p.inc_zp(ERRS + 1)

    main = p.symbol("main")
    p.store(RW0_ADDR, BUS_SEEDED & 0xFF)
    p.store(RW0_ADDR + 1, BUS_SEEDED >> 8)
    p.ldx_imm(0)
    write = p.symbol("write")
    p.txa()
    p.eor_zp(SEED)
    p.sta_abs(RW0_DATA)
    p.inx()
    p.bne(write)
    p.store(RW1_ADDR, BUS_SEEDED & 0xFF)
    p.store(RW1_ADDR + 1, BUS_SEEDED >> 8)
    p.ldx_imm(0)
    read = p.symbol("read")
    p.txa()
    p.eor_zp(SEED)
    p.cmp_abs(RW1_DATA)
    count_error()
    p.inx()
    p.bne(read)

    p.store(RW1_ADDR, BUS_FIXED & 0xFF)
    p.store(RW1_ADDR + 1, BUS_FIXED >> 8)
    p.stz_zp(ACC)
    for k in range(256):
        p.lda_abs(RW1_DATA)
        p.eor_imm(k)
        p.ora_zp(ACC)
        p.sta_zp(ACC)
    p.lda_zp(ACC)
    count_error()

    p.ldx_imm(0)
    push = p.symbol("push")
    p.txa()
    p.eor_zp(SEED)
    p.sta_abs(XSTACK)
    p.inx()
    p.cpx_imm(16)
    p.bne(push)
    p.ldx_imm(15)
    pull = p.symbol("pull")
    p.txa()
    p.eor_zp(SEED)
    p.cmp_abs(XSTACK)
    count_error()
    p.dex()
    p.bpl(pull)

    p.inc_zp(SEED)
    p.inc_zp(LOOPS)
    with p.branch("bne"):
        p.inc_zp(LOOPS + 1)
        with p.branch("bne"):
            p.inc_zp(LOOPS + 2)
    p.store(RW0_ADDR, BUS_COUNTERS & 0xFF)
    p.store(RW0_ADDR + 1, BUS_COUNTERS >> 8)
    for z in (LOOPS, LOOPS + 1, LOOPS + 2, ERRS, ERRS + 1):
        p.lda_zp(z)
        p.sta_abs(RW0_DATA)
    p.jmp_abs(main)
    return p


# The slowest things the console's terminal does, each followed at once by a
# probe: the cursor homed to row 2, 200 x's, and a cursor position request.
# The UART keeps sending while the terminal works on the op, so the probe's
# bytes arrive then, and a byte the VGA's UART lost puts the cursor somewhere
# else than row 4, column 41.
TERM_OPS = (
    b"x\33[65535b",
    b"\33[4h" + b"y" * 80 + b"\33[4l",
    b"\33[H" + b"\33[80@" * 10 + b"\33[80P" * 10,
    b"\33[1;31;42m\33[2J\33[0m\33[3J",
    b"\33[H" + b"\33[30L" * 5 + b"\33[30M" * 5 + b"\33[30S\33[30T",
    b"\r\n" * 60,
    b"\33]4;1;#ff0000\7" * 10 + b"\33]104\7\33]10;#123456;#000000\7\33]110\7",
    b"\33c",
    b"\33[?1049h\33[?1049l" * 10,
    b"\33[5;25r" + b"\n" * 40 + b"\33[r",
    b"\33[" + b"1;" * 300 + b"m",
    b"\33[38;2;1;2;3;48;5;100;1;4;5m" * 20 + b"\33[0m",
    b"\33[11m\1\2\3\4\5\6\33[10m\33(0lqqk\33(B" * 10,
)
TERM_PROBE = b"\33[2;1H" + b"x" * 200 + b"\33[6n"
TERM_ANSWER = b"\33[4;41R"
TERM_LOOPS = 4

# Zero page of the terminal ROM.
TPTR, TOP, TLOOP, TWAIT, TBUF = 0x50, 0x52, 0x53, 0x54, 0x60


def term():
    """Each op of TERM_OPS and its probe, TERM_LOOPS times. A probe that
    gets another answer, or none in about a second, prints "?op" and the op's
    index in hex. The last line is "done"."""
    # Built twice: the first time finds where the ops land.
    ops_at = term_build(0).address("ops")
    return term_build(ops_at)


def term_build(ops_at):
    p = Asm()
    p.store(TLOOP, TERM_LOOPS)
    p.symbol("outer")
    p.store(TPTR, ops_at & 0xFF)
    p.store(TPTR + 1, ops_at >> 8)
    p.stz_zp(TOP)
    p.symbol("op")
    p.lda_izp(TPTR)
    with p.branch("bne"):
        p.jmp_abs("next_loop")
    p.symbol("send")
    p.lda_izp(TPTR)
    with p.branch("bne"):
        p.jmp_abs("sent")
    p.jsr_abs("putc")
    p.inc_zp(TPTR)
    with p.branch("bne"):
        p.inc_zp(TPTR + 1)
    p.jmp_abs("send")
    p.symbol("sent")
    p.inc_zp(TPTR)
    with p.branch("bne"):
        p.inc_zp(TPTR + 1)
    # The probe.
    p.ldx_imm(0)
    p.symbol("probe")
    p.lda_abx("probe_text")
    with p.branch("bne"):
        p.jmp_abs("probe_sent")
    p.jsr_abs("putc")
    p.inx()
    p.jmp_abs("probe")
    p.symbol("probe_sent")
    # The answer, up to its R, into TBUF; Y counts it.
    p.ldy_imm(0)
    p.symbol("rd")
    p.stz_zp(TWAIT)
    p.stz_zp(TWAIT + 1)
    p.ldx_imm(0)
    p.symbol("wait")
    p.bit_abs(RIA_READY)
    with p.branch("bvc"):
        p.jmp_abs("got")
    p.dex()
    p.bne("wait")
    p.inc_zp(TWAIT)
    p.bne("wait")
    p.inc_zp(TWAIT + 1)
    p.lda_zp(TWAIT + 1)
    p.cmp_imm(8)
    p.bne("wait")
    p.jmp_abs("bad")
    p.symbol("got")
    p.lda_abs(RIA_RX)
    p.sta_aby(TBUF)
    p.iny()
    p.cmp_imm(ord("R"))
    with p.branch("bne"):
        p.jmp_abs("check")
    p.cpy_imm(15)
    p.bne("rd")
    p.jmp_abs("bad")
    p.symbol("check")
    p.cpy_imm(len(TERM_ANSWER))
    p.bne("bad_near")
    p.ldx_imm(0)
    p.symbol("cmp")
    p.lda_zpx(TBUF)
    p.cmp_abx("answer")
    p.bne("bad_near")
    p.inx()
    p.cpx_imm(len(TERM_ANSWER))
    p.bne("cmp")
    p.jmp_abs("good")
    p.symbol("bad_near")
    p.symbol("bad")
    p.say("\r\n?op ")
    p.lda_zp(TOP)
    p.jsr_abs("puthex")
    p.say("\r\n")
    p.symbol("good")
    p.inc_zp(TOP)
    p.jmp_abs("op")
    p.symbol("next_loop")
    p.dec_zp(TLOOP)
    with p.branch("bne"):
        p.jmp_abs("finish")
    p.jmp_abs("outer")
    p.symbol("finish")
    p.say("\33c\r\ndone\r\n")
    exit_with(p, 0)
    p.use(putc, puthex, putnib)
    p.symbol("probe_text")
    p.data(TERM_PROBE + b"\0")
    p.symbol("answer")
    p.data(TERM_ANSWER)
    p.symbol("ops")
    for o in TERM_OPS:
        p.data(o + b"\0")
    p.data(b"\0")
    return p


ROMS = {"hil-tput": tput, "hil-audio": audio, "hil-bus": bus, "hil-term": term}
