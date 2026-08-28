/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What a machine is, to core: a list of drivers and a run state.
 *
 * The list is the machine's own -- RP6502_MACH_DRIVERS in its drivers.h --
 * and this file is only the mechanism over it: the row shape, the walks, and
 * the latch that turns "stop the 6502" from an ask into a doing. Each machine
 * keeps the rest of its own loop in its own main.h. */

#ifndef _CORE_MACH_H_
#define _CORE_MACH_H_

#include <stdbool.h>
#include <stddef.h>


/* Cold boot: this machine's drivers, walked forward. Every machine answers
 * it, because every machine has drivers to bring up. */
void mach_init(void);

// This is true when the 6502 is running or there's a pending
// request to start it.
bool mach_active(void);

// Request to "start the 6502".
// It will safely do nothing if the 6502 is already running.
void mach_run(void);

// Request to "stop the 6502".
// It will safely do nothing if the 6502 is already stopped.
void mach_stop(void);

/* Perform a start or stop that was asked for. A machine calls this from its
 * loop, at a point where it can afford the fan-out. */
void mach_commit(void);

// Request to "break the system".
// A break is triggered by CTRL-ALT-DEL or UART breaks.
// If the 6502 is running, stop events will be called first.
// False when this platform has nowhere to break to, which is a machine
// with no monitor; the key that asked is then an ordinary key.
bool mach_break(void);

// Like mach_break, but keeps the launcher/exec chain so the launcher
// re-runs instead of dropping to the monitor. Triggered by Alt-F4.
// False when there is nowhere to go: from the launcher itself on any
// platform, and with none registered on a platform that has no monitor
// to fall back to. A RIA with none registered breaks to the monitor.
bool mach_break_to_launcher(void);

/* A machine lists its drivers once, as RP6502_MACH_DRIVERS in its own
 * drivers.h. A row is DRIVER(init, task, io_task, run, stop, break); a
 * driver names its own hooks in its own header, so a hook it has not got is
 * nul_* and costs nothing, and a driver a machine has not got is simply
 * absent from the list. Init and the task pumps walk forward; stop and break
 * walk backward, because a teardown reverses a bring-up.
 *
 * The two task columns carry the file IO contract: the task column must be
 * safe to call during blocking file IO -- it is what a host's blocking loops
 * re-enter while a transfer completes -- and a task that may itself perform
 * file IO goes in io_task, which is never re-entered.
 *
 * A walk picks one column out of every row: define DRIVER to the column you
 * want, expand the list, undefine it. That is the whole mechanism, and it is
 * spelled out at each walk rather than hidden behind a macro because the
 * preprocessor cannot define DRIVER from inside one.
 *
 * The arity boilerplate below is the price of reversing a list in the
 * preprocessor. 48 rows is the ceiling; add more by extending the runs. */

#define nul_init()
#define nul_task()
#define nul_run()
#define nul_stop()
#define nul_break()

#define LC_CAT(a, b) LC_CAT_(a, b)
#define LC_CAT_(a, b) a##b

#define LC_FWD_1(a) a
#define LC_FWD_2(a, ...) a LC_FWD_1(__VA_ARGS__)
#define LC_FWD_3(a, ...) a LC_FWD_2(__VA_ARGS__)
#define LC_FWD_4(a, ...) a LC_FWD_3(__VA_ARGS__)
#define LC_FWD_5(a, ...) a LC_FWD_4(__VA_ARGS__)
#define LC_FWD_6(a, ...) a LC_FWD_5(__VA_ARGS__)
#define LC_FWD_7(a, ...) a LC_FWD_6(__VA_ARGS__)
#define LC_FWD_8(a, ...) a LC_FWD_7(__VA_ARGS__)
#define LC_FWD_9(a, ...) a LC_FWD_8(__VA_ARGS__)
#define LC_FWD_10(a, ...) a LC_FWD_9(__VA_ARGS__)
#define LC_FWD_11(a, ...) a LC_FWD_10(__VA_ARGS__)
#define LC_FWD_12(a, ...) a LC_FWD_11(__VA_ARGS__)
#define LC_FWD_13(a, ...) a LC_FWD_12(__VA_ARGS__)
#define LC_FWD_14(a, ...) a LC_FWD_13(__VA_ARGS__)
#define LC_FWD_15(a, ...) a LC_FWD_14(__VA_ARGS__)
#define LC_FWD_16(a, ...) a LC_FWD_15(__VA_ARGS__)
#define LC_FWD_17(a, ...) a LC_FWD_16(__VA_ARGS__)
#define LC_FWD_18(a, ...) a LC_FWD_17(__VA_ARGS__)
#define LC_FWD_19(a, ...) a LC_FWD_18(__VA_ARGS__)
#define LC_FWD_20(a, ...) a LC_FWD_19(__VA_ARGS__)
#define LC_FWD_21(a, ...) a LC_FWD_20(__VA_ARGS__)
#define LC_FWD_22(a, ...) a LC_FWD_21(__VA_ARGS__)
#define LC_FWD_23(a, ...) a LC_FWD_22(__VA_ARGS__)
#define LC_FWD_24(a, ...) a LC_FWD_23(__VA_ARGS__)
#define LC_FWD_25(a, ...) a LC_FWD_24(__VA_ARGS__)
#define LC_FWD_26(a, ...) a LC_FWD_25(__VA_ARGS__)
#define LC_FWD_27(a, ...) a LC_FWD_26(__VA_ARGS__)
#define LC_FWD_28(a, ...) a LC_FWD_27(__VA_ARGS__)
#define LC_FWD_29(a, ...) a LC_FWD_28(__VA_ARGS__)
#define LC_FWD_30(a, ...) a LC_FWD_29(__VA_ARGS__)
#define LC_FWD_31(a, ...) a LC_FWD_30(__VA_ARGS__)
#define LC_FWD_32(a, ...) a LC_FWD_31(__VA_ARGS__)
#define LC_FWD_33(a, ...) a LC_FWD_32(__VA_ARGS__)
#define LC_FWD_34(a, ...) a LC_FWD_33(__VA_ARGS__)
#define LC_FWD_35(a, ...) a LC_FWD_34(__VA_ARGS__)
#define LC_FWD_36(a, ...) a LC_FWD_35(__VA_ARGS__)
#define LC_FWD_37(a, ...) a LC_FWD_36(__VA_ARGS__)
#define LC_FWD_38(a, ...) a LC_FWD_37(__VA_ARGS__)
#define LC_FWD_39(a, ...) a LC_FWD_38(__VA_ARGS__)
#define LC_FWD_40(a, ...) a LC_FWD_39(__VA_ARGS__)
#define LC_FWD_41(a, ...) a LC_FWD_40(__VA_ARGS__)
#define LC_FWD_42(a, ...) a LC_FWD_41(__VA_ARGS__)
#define LC_FWD_43(a, ...) a LC_FWD_42(__VA_ARGS__)
#define LC_FWD_44(a, ...) a LC_FWD_43(__VA_ARGS__)
#define LC_FWD_45(a, ...) a LC_FWD_44(__VA_ARGS__)
#define LC_FWD_46(a, ...) a LC_FWD_45(__VA_ARGS__)
#define LC_FWD_47(a, ...) a LC_FWD_46(__VA_ARGS__)
#define LC_FWD_48(a, ...) a LC_FWD_47(__VA_ARGS__)

#define LC_REV_1(a) a
#define LC_REV_2(a, ...) LC_REV_1(__VA_ARGS__) a
#define LC_REV_3(a, ...) LC_REV_2(__VA_ARGS__) a
#define LC_REV_4(a, ...) LC_REV_3(__VA_ARGS__) a
#define LC_REV_5(a, ...) LC_REV_4(__VA_ARGS__) a
#define LC_REV_6(a, ...) LC_REV_5(__VA_ARGS__) a
#define LC_REV_7(a, ...) LC_REV_6(__VA_ARGS__) a
#define LC_REV_8(a, ...) LC_REV_7(__VA_ARGS__) a
#define LC_REV_9(a, ...) LC_REV_8(__VA_ARGS__) a
#define LC_REV_10(a, ...) LC_REV_9(__VA_ARGS__) a
#define LC_REV_11(a, ...) LC_REV_10(__VA_ARGS__) a
#define LC_REV_12(a, ...) LC_REV_11(__VA_ARGS__) a
#define LC_REV_13(a, ...) LC_REV_12(__VA_ARGS__) a
#define LC_REV_14(a, ...) LC_REV_13(__VA_ARGS__) a
#define LC_REV_15(a, ...) LC_REV_14(__VA_ARGS__) a
#define LC_REV_16(a, ...) LC_REV_15(__VA_ARGS__) a
#define LC_REV_17(a, ...) LC_REV_16(__VA_ARGS__) a
#define LC_REV_18(a, ...) LC_REV_17(__VA_ARGS__) a
#define LC_REV_19(a, ...) LC_REV_18(__VA_ARGS__) a
#define LC_REV_20(a, ...) LC_REV_19(__VA_ARGS__) a
#define LC_REV_21(a, ...) LC_REV_20(__VA_ARGS__) a
#define LC_REV_22(a, ...) LC_REV_21(__VA_ARGS__) a
#define LC_REV_23(a, ...) LC_REV_22(__VA_ARGS__) a
#define LC_REV_24(a, ...) LC_REV_23(__VA_ARGS__) a
#define LC_REV_25(a, ...) LC_REV_24(__VA_ARGS__) a
#define LC_REV_26(a, ...) LC_REV_25(__VA_ARGS__) a
#define LC_REV_27(a, ...) LC_REV_26(__VA_ARGS__) a
#define LC_REV_28(a, ...) LC_REV_27(__VA_ARGS__) a
#define LC_REV_29(a, ...) LC_REV_28(__VA_ARGS__) a
#define LC_REV_30(a, ...) LC_REV_29(__VA_ARGS__) a
#define LC_REV_31(a, ...) LC_REV_30(__VA_ARGS__) a
#define LC_REV_32(a, ...) LC_REV_31(__VA_ARGS__) a
#define LC_REV_33(a, ...) LC_REV_32(__VA_ARGS__) a
#define LC_REV_34(a, ...) LC_REV_33(__VA_ARGS__) a
#define LC_REV_35(a, ...) LC_REV_34(__VA_ARGS__) a
#define LC_REV_36(a, ...) LC_REV_35(__VA_ARGS__) a
#define LC_REV_37(a, ...) LC_REV_36(__VA_ARGS__) a
#define LC_REV_38(a, ...) LC_REV_37(__VA_ARGS__) a
#define LC_REV_39(a, ...) LC_REV_38(__VA_ARGS__) a
#define LC_REV_40(a, ...) LC_REV_39(__VA_ARGS__) a
#define LC_REV_41(a, ...) LC_REV_40(__VA_ARGS__) a
#define LC_REV_42(a, ...) LC_REV_41(__VA_ARGS__) a
#define LC_REV_43(a, ...) LC_REV_42(__VA_ARGS__) a
#define LC_REV_44(a, ...) LC_REV_43(__VA_ARGS__) a
#define LC_REV_45(a, ...) LC_REV_44(__VA_ARGS__) a
#define LC_REV_46(a, ...) LC_REV_45(__VA_ARGS__) a
#define LC_REV_47(a, ...) LC_REV_46(__VA_ARGS__) a
#define LC_REV_48(a, ...) LC_REV_47(__VA_ARGS__) a

#define LC_COUNT(...) LC_COUNT_(__VA_ARGS__, 48, 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1)
#define LC_COUNT_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, _33, _34, _35, _36, _37, _38, _39, _40, _41, _42, _43, _44, _45, _46, _47, _48, N, ...) N

#define MACH_FORWARD(...) LC_CAT(LC_FWD_, LC_COUNT(__VA_ARGS__))(__VA_ARGS__)
#define MACH_REVERSE(...) LC_CAT(LC_REV_, LC_COUNT(__VA_ARGS__))(__VA_ARGS__)

#endif /* _CORE_MACH_H_ */
