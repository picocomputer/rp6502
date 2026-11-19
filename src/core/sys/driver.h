/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_SYS_DRIVER_H_
#define _CORE_SYS_DRIVER_H_

/* A machine's drivers are one list, RP6502_MACH_DRIVERS in its drivers.h,
 * and its stdio table is a second, RP6502_STD_DRIVERS, which names drivers
 * in the order open() tries their STD rows. Each list has its own order. A
 * row names only the hooks its driver has:
 *
 *   #define STD_DRIVER DRIVER(         \
 *       INIT(std_init)                 \
 *       TASK_PUMPED(std_task)          \
 *       STOP(std_stop))
 *
 *   INIT(f)         at boot, in list order
 *   TASK_FATFS(f)   from the main loop only: it calls FatFs, which pumps
 *   TASK_PUMPED(f)  from the main loop, and from sys_pump while FatFs waits
 *   TASK_ALWAYS(f)  as TASK_PUMPED, and also while flashing, so it and all
 *                   it reaches then must be in RAM
 *   RUN(f)          when a program starts, in list order
 *   STOP(f)         when a program stops, in reverse order: also when it
 *                   asks for an exec, so what it hands the next program
 *                   stays
 *   END(f)          when an exec chain ends, and before a launcher starts
 *                   again, after the stop, in reverse order: what a program
 *                   could hand on to an exec is put away here
 *   BREAK(f)        on a break, in reverse order
 *   FLASHING(f)     once, when the RIA starts rewriting its own flash, in
 *                   reverse order
 *   CONFIG_INT/STR/RAW/HIDDEN/SAVE(...) and SST(...), any number of each
 *   STD(...)        the driver's stdio row: the fields of a std_driver_t of
 *                   core/api/std.h, other than its sst
 *   STD_SST(...)    the fields of that row's sst, which a machine that
 *                   makes no savestate leaves out
 *
 * The tasks of a row run in the order the row lists them. A walk defines the
 * labels it uses, then opens with core/sys/driver_walk.h, which makes every
 * other label expand to nothing, and closes with core/sys/driver_walk_end.h.
 * A row's expansion must have no top-level comma, because the list macros
 * below split on them. */

/* The check and apply of a setting that has none. */
#define nul_check(...) true
#define nul_apply(...)

#define DRIVERS_CAT(a, b) DRIVERS_CAT_(a, b)
#define DRIVERS_CAT_(a, b) a##b

#define DRIVERS_FWD_1(a) a
#define DRIVERS_FWD_2(a, ...) a DRIVERS_FWD_1(__VA_ARGS__)
#define DRIVERS_FWD_3(a, ...) a DRIVERS_FWD_2(__VA_ARGS__)
#define DRIVERS_FWD_4(a, ...) a DRIVERS_FWD_3(__VA_ARGS__)
#define DRIVERS_FWD_5(a, ...) a DRIVERS_FWD_4(__VA_ARGS__)
#define DRIVERS_FWD_6(a, ...) a DRIVERS_FWD_5(__VA_ARGS__)
#define DRIVERS_FWD_7(a, ...) a DRIVERS_FWD_6(__VA_ARGS__)
#define DRIVERS_FWD_8(a, ...) a DRIVERS_FWD_7(__VA_ARGS__)
#define DRIVERS_FWD_9(a, ...) a DRIVERS_FWD_8(__VA_ARGS__)
#define DRIVERS_FWD_10(a, ...) a DRIVERS_FWD_9(__VA_ARGS__)
#define DRIVERS_FWD_11(a, ...) a DRIVERS_FWD_10(__VA_ARGS__)
#define DRIVERS_FWD_12(a, ...) a DRIVERS_FWD_11(__VA_ARGS__)
#define DRIVERS_FWD_13(a, ...) a DRIVERS_FWD_12(__VA_ARGS__)
#define DRIVERS_FWD_14(a, ...) a DRIVERS_FWD_13(__VA_ARGS__)
#define DRIVERS_FWD_15(a, ...) a DRIVERS_FWD_14(__VA_ARGS__)
#define DRIVERS_FWD_16(a, ...) a DRIVERS_FWD_15(__VA_ARGS__)
#define DRIVERS_FWD_17(a, ...) a DRIVERS_FWD_16(__VA_ARGS__)
#define DRIVERS_FWD_18(a, ...) a DRIVERS_FWD_17(__VA_ARGS__)
#define DRIVERS_FWD_19(a, ...) a DRIVERS_FWD_18(__VA_ARGS__)
#define DRIVERS_FWD_20(a, ...) a DRIVERS_FWD_19(__VA_ARGS__)
#define DRIVERS_FWD_21(a, ...) a DRIVERS_FWD_20(__VA_ARGS__)
#define DRIVERS_FWD_22(a, ...) a DRIVERS_FWD_21(__VA_ARGS__)
#define DRIVERS_FWD_23(a, ...) a DRIVERS_FWD_22(__VA_ARGS__)
#define DRIVERS_FWD_24(a, ...) a DRIVERS_FWD_23(__VA_ARGS__)
#define DRIVERS_FWD_25(a, ...) a DRIVERS_FWD_24(__VA_ARGS__)
#define DRIVERS_FWD_26(a, ...) a DRIVERS_FWD_25(__VA_ARGS__)
#define DRIVERS_FWD_27(a, ...) a DRIVERS_FWD_26(__VA_ARGS__)
#define DRIVERS_FWD_28(a, ...) a DRIVERS_FWD_27(__VA_ARGS__)
#define DRIVERS_FWD_29(a, ...) a DRIVERS_FWD_28(__VA_ARGS__)
#define DRIVERS_FWD_30(a, ...) a DRIVERS_FWD_29(__VA_ARGS__)
#define DRIVERS_FWD_31(a, ...) a DRIVERS_FWD_30(__VA_ARGS__)
#define DRIVERS_FWD_32(a, ...) a DRIVERS_FWD_31(__VA_ARGS__)
#define DRIVERS_FWD_33(a, ...) a DRIVERS_FWD_32(__VA_ARGS__)
#define DRIVERS_FWD_34(a, ...) a DRIVERS_FWD_33(__VA_ARGS__)
#define DRIVERS_FWD_35(a, ...) a DRIVERS_FWD_34(__VA_ARGS__)
#define DRIVERS_FWD_36(a, ...) a DRIVERS_FWD_35(__VA_ARGS__)
#define DRIVERS_FWD_37(a, ...) a DRIVERS_FWD_36(__VA_ARGS__)
#define DRIVERS_FWD_38(a, ...) a DRIVERS_FWD_37(__VA_ARGS__)
#define DRIVERS_FWD_39(a, ...) a DRIVERS_FWD_38(__VA_ARGS__)
#define DRIVERS_FWD_40(a, ...) a DRIVERS_FWD_39(__VA_ARGS__)
#define DRIVERS_FWD_41(a, ...) a DRIVERS_FWD_40(__VA_ARGS__)
#define DRIVERS_FWD_42(a, ...) a DRIVERS_FWD_41(__VA_ARGS__)
#define DRIVERS_FWD_43(a, ...) a DRIVERS_FWD_42(__VA_ARGS__)
#define DRIVERS_FWD_44(a, ...) a DRIVERS_FWD_43(__VA_ARGS__)
#define DRIVERS_FWD_45(a, ...) a DRIVERS_FWD_44(__VA_ARGS__)
#define DRIVERS_FWD_46(a, ...) a DRIVERS_FWD_45(__VA_ARGS__)
#define DRIVERS_FWD_47(a, ...) a DRIVERS_FWD_46(__VA_ARGS__)
#define DRIVERS_FWD_48(a, ...) a DRIVERS_FWD_47(__VA_ARGS__)
#define DRIVERS_FWD_49(a, ...) a DRIVERS_FWD_48(__VA_ARGS__)
#define DRIVERS_FWD_50(a, ...) a DRIVERS_FWD_49(__VA_ARGS__)
#define DRIVERS_FWD_51(a, ...) a DRIVERS_FWD_50(__VA_ARGS__)
#define DRIVERS_FWD_52(a, ...) a DRIVERS_FWD_51(__VA_ARGS__)
#define DRIVERS_FWD_53(a, ...) a DRIVERS_FWD_52(__VA_ARGS__)
#define DRIVERS_FWD_54(a, ...) a DRIVERS_FWD_53(__VA_ARGS__)
#define DRIVERS_FWD_55(a, ...) a DRIVERS_FWD_54(__VA_ARGS__)
#define DRIVERS_FWD_56(a, ...) a DRIVERS_FWD_55(__VA_ARGS__)
#define DRIVERS_FWD_57(a, ...) a DRIVERS_FWD_56(__VA_ARGS__)
#define DRIVERS_FWD_58(a, ...) a DRIVERS_FWD_57(__VA_ARGS__)
#define DRIVERS_FWD_59(a, ...) a DRIVERS_FWD_58(__VA_ARGS__)
#define DRIVERS_FWD_60(a, ...) a DRIVERS_FWD_59(__VA_ARGS__)
#define DRIVERS_FWD_61(a, ...) a DRIVERS_FWD_60(__VA_ARGS__)
#define DRIVERS_FWD_62(a, ...) a DRIVERS_FWD_61(__VA_ARGS__)
#define DRIVERS_FWD_63(a, ...) a DRIVERS_FWD_62(__VA_ARGS__)
#define DRIVERS_FWD_64(a, ...) a DRIVERS_FWD_63(__VA_ARGS__)

#define DRIVERS_REV_1(a) a
#define DRIVERS_REV_2(a, ...) DRIVERS_REV_1(__VA_ARGS__) a
#define DRIVERS_REV_3(a, ...) DRIVERS_REV_2(__VA_ARGS__) a
#define DRIVERS_REV_4(a, ...) DRIVERS_REV_3(__VA_ARGS__) a
#define DRIVERS_REV_5(a, ...) DRIVERS_REV_4(__VA_ARGS__) a
#define DRIVERS_REV_6(a, ...) DRIVERS_REV_5(__VA_ARGS__) a
#define DRIVERS_REV_7(a, ...) DRIVERS_REV_6(__VA_ARGS__) a
#define DRIVERS_REV_8(a, ...) DRIVERS_REV_7(__VA_ARGS__) a
#define DRIVERS_REV_9(a, ...) DRIVERS_REV_8(__VA_ARGS__) a
#define DRIVERS_REV_10(a, ...) DRIVERS_REV_9(__VA_ARGS__) a
#define DRIVERS_REV_11(a, ...) DRIVERS_REV_10(__VA_ARGS__) a
#define DRIVERS_REV_12(a, ...) DRIVERS_REV_11(__VA_ARGS__) a
#define DRIVERS_REV_13(a, ...) DRIVERS_REV_12(__VA_ARGS__) a
#define DRIVERS_REV_14(a, ...) DRIVERS_REV_13(__VA_ARGS__) a
#define DRIVERS_REV_15(a, ...) DRIVERS_REV_14(__VA_ARGS__) a
#define DRIVERS_REV_16(a, ...) DRIVERS_REV_15(__VA_ARGS__) a
#define DRIVERS_REV_17(a, ...) DRIVERS_REV_16(__VA_ARGS__) a
#define DRIVERS_REV_18(a, ...) DRIVERS_REV_17(__VA_ARGS__) a
#define DRIVERS_REV_19(a, ...) DRIVERS_REV_18(__VA_ARGS__) a
#define DRIVERS_REV_20(a, ...) DRIVERS_REV_19(__VA_ARGS__) a
#define DRIVERS_REV_21(a, ...) DRIVERS_REV_20(__VA_ARGS__) a
#define DRIVERS_REV_22(a, ...) DRIVERS_REV_21(__VA_ARGS__) a
#define DRIVERS_REV_23(a, ...) DRIVERS_REV_22(__VA_ARGS__) a
#define DRIVERS_REV_24(a, ...) DRIVERS_REV_23(__VA_ARGS__) a
#define DRIVERS_REV_25(a, ...) DRIVERS_REV_24(__VA_ARGS__) a
#define DRIVERS_REV_26(a, ...) DRIVERS_REV_25(__VA_ARGS__) a
#define DRIVERS_REV_27(a, ...) DRIVERS_REV_26(__VA_ARGS__) a
#define DRIVERS_REV_28(a, ...) DRIVERS_REV_27(__VA_ARGS__) a
#define DRIVERS_REV_29(a, ...) DRIVERS_REV_28(__VA_ARGS__) a
#define DRIVERS_REV_30(a, ...) DRIVERS_REV_29(__VA_ARGS__) a
#define DRIVERS_REV_31(a, ...) DRIVERS_REV_30(__VA_ARGS__) a
#define DRIVERS_REV_32(a, ...) DRIVERS_REV_31(__VA_ARGS__) a
#define DRIVERS_REV_33(a, ...) DRIVERS_REV_32(__VA_ARGS__) a
#define DRIVERS_REV_34(a, ...) DRIVERS_REV_33(__VA_ARGS__) a
#define DRIVERS_REV_35(a, ...) DRIVERS_REV_34(__VA_ARGS__) a
#define DRIVERS_REV_36(a, ...) DRIVERS_REV_35(__VA_ARGS__) a
#define DRIVERS_REV_37(a, ...) DRIVERS_REV_36(__VA_ARGS__) a
#define DRIVERS_REV_38(a, ...) DRIVERS_REV_37(__VA_ARGS__) a
#define DRIVERS_REV_39(a, ...) DRIVERS_REV_38(__VA_ARGS__) a
#define DRIVERS_REV_40(a, ...) DRIVERS_REV_39(__VA_ARGS__) a
#define DRIVERS_REV_41(a, ...) DRIVERS_REV_40(__VA_ARGS__) a
#define DRIVERS_REV_42(a, ...) DRIVERS_REV_41(__VA_ARGS__) a
#define DRIVERS_REV_43(a, ...) DRIVERS_REV_42(__VA_ARGS__) a
#define DRIVERS_REV_44(a, ...) DRIVERS_REV_43(__VA_ARGS__) a
#define DRIVERS_REV_45(a, ...) DRIVERS_REV_44(__VA_ARGS__) a
#define DRIVERS_REV_46(a, ...) DRIVERS_REV_45(__VA_ARGS__) a
#define DRIVERS_REV_47(a, ...) DRIVERS_REV_46(__VA_ARGS__) a
#define DRIVERS_REV_48(a, ...) DRIVERS_REV_47(__VA_ARGS__) a
#define DRIVERS_REV_49(a, ...) DRIVERS_REV_48(__VA_ARGS__) a
#define DRIVERS_REV_50(a, ...) DRIVERS_REV_49(__VA_ARGS__) a
#define DRIVERS_REV_51(a, ...) DRIVERS_REV_50(__VA_ARGS__) a
#define DRIVERS_REV_52(a, ...) DRIVERS_REV_51(__VA_ARGS__) a
#define DRIVERS_REV_53(a, ...) DRIVERS_REV_52(__VA_ARGS__) a
#define DRIVERS_REV_54(a, ...) DRIVERS_REV_53(__VA_ARGS__) a
#define DRIVERS_REV_55(a, ...) DRIVERS_REV_54(__VA_ARGS__) a
#define DRIVERS_REV_56(a, ...) DRIVERS_REV_55(__VA_ARGS__) a
#define DRIVERS_REV_57(a, ...) DRIVERS_REV_56(__VA_ARGS__) a
#define DRIVERS_REV_58(a, ...) DRIVERS_REV_57(__VA_ARGS__) a
#define DRIVERS_REV_59(a, ...) DRIVERS_REV_58(__VA_ARGS__) a
#define DRIVERS_REV_60(a, ...) DRIVERS_REV_59(__VA_ARGS__) a
#define DRIVERS_REV_61(a, ...) DRIVERS_REV_60(__VA_ARGS__) a
#define DRIVERS_REV_62(a, ...) DRIVERS_REV_61(__VA_ARGS__) a
#define DRIVERS_REV_63(a, ...) DRIVERS_REV_62(__VA_ARGS__) a
#define DRIVERS_REV_64(a, ...) DRIVERS_REV_63(__VA_ARGS__) a

#define DRIVERS_COUNT(...) DRIVERS_COUNT_(__VA_ARGS__, 64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1)
#define DRIVERS_COUNT_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, _33, _34, _35, _36, _37, _38, _39, _40, _41, _42, _43, _44, _45, _46, _47, _48, _49, _50, _51, _52, _53, _54, _55, _56, _57, _58, _59, _60, _61, _62, _63, _64, N, ...) N

#define DRIVERS_FORWARD(...) DRIVERS_CAT(DRIVERS_FWD_, DRIVERS_COUNT(__VA_ARGS__))(__VA_ARGS__)
#define DRIVERS_REVERSE(...) DRIVERS_CAT(DRIVERS_REV_, DRIVERS_COUNT(__VA_ARGS__))(__VA_ARGS__)

#endif /* _CORE_SYS_DRIVER_H_ */
