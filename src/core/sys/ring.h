/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Ring buffers after Linux's kfifo, with the size fixed at compile time.
 *
 * RING(type, size) holds the entries and two counters, wr and rd, that only
 * count up and wrap with uint32_t. A counter's slot is the counter masked by
 * size - 1, so the size is a power of two, every slot holds an entry and
 * wr - rd is the count. The writer moves only wr and the reader only rd.
 * Each end loads the other's counter with acquire and stores its own with
 * release, so the ends may run on two cores, two threads, or a main loop and
 * an interrupt.
 *
 *   RING_LEN(r)          entries waiting
 *   RING_AVAIL(r)        free slots
 *   RING_PUT(r, val)     writes one entry, 0 when full
 *   RING_GET(r, &val)    reads one entry, 0 when empty
 *   RING_IN(r, src, n)   writes what fits of n and returns the count
 *   RING_OUT(r, dst, n)  reads up to n and returns the count
 *   RING_AT(r, i)        the i-th oldest entry, read in place
 *   RING_LINEAR(r)       the count from RING_AT(r, 0) to the end of the array
 *   RING_SKIP(r, n)      drops the n oldest
 *   RING_RESET(r)        empties the ring while neither end runs
 */

#ifndef _CORE_SYS_RING_H_
#define _CORE_SYS_RING_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C++"
{
#include <atomic>
using std::atomic_load_explicit;
using std::atomic_store_explicit;
using std::memory_order_acquire;
using std::memory_order_relaxed;
using std::memory_order_release;
}
typedef struct
{
    std::atomic<uint32_t> wr, rd;
} ring_t;
#else
#include <stdatomic.h>
typedef struct
{
    _Atomic uint32_t wr, rd;
} ring_t;
#endif

/* A size that is not a power of two makes the array size negative. */
#define RING(type, size)                               \
    struct                                             \
    {                                                  \
        ring_t idx;                                    \
        type buf[(size) & ((size) - 1) ? -1 : (size)]; \
    }

#define RING_SIZE(r) ((uint32_t)(sizeof (r)->buf / sizeof (r)->buf[0]))

/* rd first, so wr - rd never goes below zero. */
static inline __attribute__((always_inline)) uint32_t
ring_len(ring_t *i)
{
    const uint32_t rd = atomic_load_explicit(&i->rd, memory_order_acquire);
    return atomic_load_explicit(&i->wr, memory_order_acquire) - rd;
}

static inline __attribute__((always_inline)) uint32_t
ring_wr_slot(ring_t *i, uint32_t size)
{
    return atomic_load_explicit(&i->wr, memory_order_relaxed) & (size - 1);
}

static inline __attribute__((always_inline)) uint32_t
ring_rd_slot(ring_t *i, uint32_t n, uint32_t size)
{
    return (atomic_load_explicit(&i->rd, memory_order_relaxed) + n) & (size - 1);
}

static inline __attribute__((always_inline)) void
ring_wr_add(ring_t *i, uint32_t n)
{
    atomic_store_explicit(&i->wr, atomic_load_explicit(&i->wr, memory_order_relaxed) + n,
                          memory_order_release);
}

static inline __attribute__((always_inline)) void
ring_rd_add(ring_t *i, uint32_t n)
{
    atomic_store_explicit(&i->rd, atomic_load_explicit(&i->rd, memory_order_relaxed) + n,
                          memory_order_release);
}

static inline __attribute__((always_inline)) uint32_t
ring_linear(ring_t *i, uint32_t size)
{
    const uint32_t len = ring_len(i);
    const uint32_t end = size - ring_rd_slot(i, 0, size);
    return len < end ? len : end;
}

static inline __attribute__((always_inline)) void
ring_reset(ring_t *i)
{
    atomic_store_explicit(&i->wr, (uint32_t)0, memory_order_release);
    atomic_store_explicit(&i->rd, (uint32_t)0, memory_order_release);
}

#ifdef __cplusplus
extern "C"
{
#endif
uint32_t ring_in(ring_t *i, void *buf, uint32_t size, size_t esize,
                 const void *src, uint32_t n);
uint32_t ring_out(ring_t *i, const void *buf, uint32_t size, size_t esize,
                  void *dst, uint32_t n);
#ifdef __cplusplus
}
#endif

#define RING_LEN(r) ring_len(&(r)->idx)
#define RING_AVAIL(r) (RING_SIZE(r) - RING_LEN(r))
#define RING_PUT(r, val)                                             \
    (RING_AVAIL(r)                                                   \
         ? ((r)->buf[ring_wr_slot(&(r)->idx, RING_SIZE(r))] = (val), \
            ring_wr_add(&(r)->idx, 1), 1)                            \
         : 0)
#define RING_GET(r, p)                                                  \
    (RING_LEN(r)                                                        \
         ? (*(p) = (r)->buf[ring_rd_slot(&(r)->idx, 0, RING_SIZE(r))], \
            ring_rd_add(&(r)->idx, 1), 1)                               \
         : 0)
#define RING_IN(r, src, n) \
    ring_in(&(r)->idx, (r)->buf, RING_SIZE(r), sizeof (r)->buf[0], (src), (n))
#define RING_OUT(r, dst, n) \
    ring_out(&(r)->idx, (r)->buf, RING_SIZE(r), sizeof (r)->buf[0], (dst), (n))
#define RING_AT(r, i) (&(r)->buf[ring_rd_slot(&(r)->idx, (i), RING_SIZE(r))])
#define RING_LINEAR(r) ring_linear(&(r)->idx, RING_SIZE(r))
#define RING_SKIP(r, n) ring_rd_add(&(r)->idx, (n))
#define RING_RESET(r) ring_reset(&(r)->idx)

#endif /* _CORE_SYS_RING_H_ */
