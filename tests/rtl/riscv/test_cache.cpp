/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * cache.sv alone. The bench is the CPU's side of the bus, with the address
 * phase of an access in the clock that ends the access before it, and a line
 * memory that answers the line port after a random number of clocks.
 *
 * Three things judge the cache. ref is what the CPU must read back. mem is
 * the line memory, which must equal ref wherever the cache holds no dirty
 * line. policy is the placement the cache is specified to make, a tag for
 * every way and a pointer for every set, and it says for every access
 * whether it waits, and how many lines are read and written.
 *
 * The model is built with --x-initial unique, so the memories start with
 * random contents, and its memories return the complement of bytes that are
 * read on the edge that writes them.
 */

#define CACHE_STR(x) #x
#define CACHE_HDR(x) CACHE_STR(x.h)
#define CACHE_CAT(a, b) a##b
#define CACHE_ROOT(x) CACHE_CAT(x, ___024root)
#include CACHE_HDR(CACHE_MODEL)
#define CACHE_ROOT_HDR(x) CACHE_HDR(x)
#include CACHE_ROOT_HDR(CACHE_ROOT(CACHE_MODEL))

#include "utest.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

typedef CACHE_MODEL dut_t;

static const unsigned MEM_WORDS = 131072;
static const unsigned LINES = MEM_WORDS / 4;
static const unsigned TAGS = LINES / CACHE_SETS;

struct op
{
    uint32_t word;
    bool write;
    uint8_t strb;
    uint32_t data;
};

struct bench
{
    dut_t *d = nullptr;
    std::vector<uint32_t> ref, mem;
    uint64_t rng;
    int errors = 0;

    /* The access in its data phase. */
    bool cur_valid = false;
    op cur = {};
    bool cur_miss = false;
    bool cur_waited = false;
    /* An access that arrives while the cache walks waits for the walk. */
    bool settling = true;
    int bomb = -1;

    bool out_ready = true;
    uint32_t out_rdata = 0;

    /* The line memory. */
    int max_delay = 6;
    bool l_prev_pend = false;
    bool l_busy = false;
    bool l_done = false;
    int l_cnt = 0;
    int l_fall = 0;
    bool l_we = false;
    uint32_t l_addr = 0;
    uint32_t l_wdata[4] = {};
    uint32_t l_rdata[4] = {};
    long line_reads = 0, line_writes = 0;

    /* The placement. */
    struct entry
    {
        bool valid, dirty;
        uint32_t tag;
    };
    std::vector<entry> pol;
    std::vector<unsigned> ptr;
    long want_hits = 0, want_misses = 0, want_reads = 0, want_writes = 0;
    long base_hits = 0, base_misses = 0;

    explicit bench(uint64_t seed)
        : ref(MEM_WORDS), mem(MEM_WORDS), rng(seed * 0x9E3779B97F4A7C15ull + 1),
          pol(CACHE_SETS * CACHE_WAYS), ptr(CACHE_SETS)
    {
        Verilated::randSeed((int)(seed & 0x7fffffff) + 1);
        Verilated::randReset(2);
        d = new dut_t;
        for (auto &w : mem)
            w = r32();
        ref = mem;
        /* randReset reaches the inputs as well. */
        d->rst_n = 1;
        d->clk = 0;
        d->dph = 0;
        d->line_done = 0;
        d->eval();
    }
    ~bench()
    {
        d->final();
        delete d;
    }

    uint32_t r32()
    {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return (uint32_t)(rng >> 16);
    }
    uint32_t rnd(uint32_t n)
    {
        return r32() % n;
    }

    void fail(const char *what)
    {
        if (errors++ < 10)
            fprintf(stderr, "cache bench: %s\n", what);
    }

    long hits()
    {
        return (long)d->rootp->cache__DOT__hits - base_hits;
    }
    long misses()
    {
        return (long)d->rootp->cache__DOT__misses - base_misses;
    }

    void line_model()
    {
        bool pend = d->cache_line_pend;
        if (pend)
        {
            if (!l_prev_pend)
            {
                if (d->line_done)
                    fail("line_pend rose while line_done was high");
                l_we = d->cache_line_we;
                l_addr = d->cache_line_addr;
                for (int i = 0; i < 4; i++)
                    l_wdata[i] = d->cache_line_wdata[i];
            }
            else
            {
                bool same = l_we == (bool)d->cache_line_we
                            && l_addr == d->cache_line_addr;
                for (int i = 0; l_we && i < 4; i++)
                    same = same && l_wdata[i] == d->cache_line_wdata[i];
                if (!same)
                    fail("the line request changed while pending");
            }
            if (!l_done && !l_busy)
            {
                /* The operation is taken, so the last line read is gone. */
                l_busy = true;
                l_cnt = (int)rnd((uint32_t)max_delay + 1);
                for (int i = 0; i < 4; i++)
                    l_rdata[i] = r32();
            }
        }
        else
        {
            l_busy = false;
            if (l_done)
            {
                if (l_fall == 0)
                    l_done = false;
                else
                    l_fall--;
            }
        }
        if (l_busy)
        {
            if (l_cnt == 0)
            {
                for (int i = 0; i < 4; i++)
                {
                    if (l_we)
                        mem[l_addr * 4 + i] = l_wdata[i];
                    else
                        l_rdata[i] = mem[l_addr * 4 + i];
                }
                if (l_we)
                    line_writes++;
                else
                    line_reads++;
                l_busy = false;
                l_done = true;
                l_fall = (int)rnd(3);
            }
            else
                l_cnt--;
        }
        l_prev_pend = pend;
    }

    void cycle()
    {
        d->clk = 0;
        d->eval();
        out_ready = d->cache_ready;
        out_rdata = d->cache_rdata;
        line_model();
        d->clk = 1;
        d->eval();
        d->line_done = l_done;
        for (int i = 0; i < 4; i++)
            d->line_rdata[i] = l_rdata[i];
    }

    /* What the placement says about an access. Returns true for a miss. */
    bool place(const op &o)
    {
        uint32_t line = o.word >> 2;
        uint32_t set = line % CACHE_SETS, tag = line / CACHE_SETS;
        entry *e = &pol[set * CACHE_WAYS];
        bool miss = true;
        unsigned w;
        for (w = 0; w < CACHE_WAYS; w++)
            if (e[w].valid && e[w].tag == tag)
            {
                miss = false;
                break;
            }
        if (miss)
        {
            w = ptr[set];
            ptr[set] = (w + 1) % CACHE_WAYS;
            if (e[w].valid && e[w].dirty)
                want_writes++;
            want_reads++;
            want_misses++;
            e[w] = {true, false, tag};
        }
        else
            want_hits++;
        if (o.write)
            e[w].dirty = true;
        return miss;
    }

    void forget()
    {
        for (auto &e : pol)
            e = {false, false, 0};
        for (auto &p : ptr)
            p = 0;
    }

    void rebase()
    {
        base_hits = (long)d->rootp->cache__DOT__hits;
        base_misses = (long)d->rootp->cache__DOT__misses;
        want_hits = want_misses = 0;
        want_reads = line_reads;
        want_writes = line_writes;
    }

    void reset()
    {
        d->rst_n = 0;
        d->dph = 0;
        cur_valid = false;
        int n = 1 + (int)rnd(3);
        for (int i = 0; i < n; i++)
            cycle();
        d->rst_n = 1;
        ref = mem;
        forget();
        rebase();
        settling = true;
        bomb = -1;
    }

    bool bombed()
    {
        if (bomb < 0 || bomb-- > 0)
            return false;
        reset();
        return true;
    }

    /* One address phase: next, or no access for a null pointer. It lasts
     * until the access in its data phase completes. Returns false if the
     * bomb went off and reset the cache. */
    bool phase(const op *next)
    {
        d->req_word = next ? next->word : r32() % MEM_WORDS;
        for (int waits = 0;; waits++)
        {
            cycle();
            bool ready = out_ready;
            if (bombed())
                return false;
            if (ready)
                break;
            if (!cur_valid)
                fail("cache_ready is low with no access");
            cur_waited = true;
            if (waits > 100000)
            {
                fail("the access never completed");
                return false;
            }
        }
        if (cur_valid)
        {
            uint32_t mask = 0;
            for (int b = 0; b < 4; b++)
                if (cur.strb & (1 << b))
                    mask |= 0xffu << (8 * b);
            if (cur.write)
                ref[cur.word] = (ref[cur.word] & ~mask) | (cur.data & mask);
            else if (out_rdata != ref[cur.word])
            {
                fprintf(stderr, "word %05x read %08x, wanted %08x\n",
                        cur.word, out_rdata, ref[cur.word]);
                fail("a load returned the wrong word");
            }
            if (!settling && cur_waited != cur_miss)
                fail(cur_miss ? "a miss did not wait" : "a hit waited");
            settling = false;
        }
        cur_valid = next != nullptr;
        d->dph = cur_valid;
        if (next)
        {
            cur = *next;
            cur_miss = place(cur);
            cur_waited = false;
            d->dph_write = cur.write;
            d->dph_word = cur.word;
            d->dph_strb = cur.strb;
            d->wdata = cur.data;
        }
        else
        {
            /* The bus carries another target's access. */
            d->dph_write = rnd(2);
            d->dph_word = r32() % MEM_WORDS;
            d->dph_strb = rnd(16);
            d->wdata = r32();
        }
        return true;
    }

    bool drain()
    {
        return !cur_valid || phase(nullptr);
    }

    /* Loads every word of every line the cache holds a newer copy of,
     * which phase checks against ref. */
    void read_dirty()
    {
        std::vector<uint32_t> words;
        for (unsigned s = 0; s < CACHE_SETS; s++)
            for (unsigned w = 0; w < CACHE_WAYS; w++)
            {
                const entry &e = pol[s * CACHE_WAYS + w];
                if (e.valid && e.dirty)
                    for (uint32_t off = 0; off < 4; off++)
                        words.push_back(word_of(e.tag, s, off));
            }
        for (uint32_t word : words)
            load(word);
        drain();
    }

    /* mem equals ref in every line that is not dirty in the cache, and
     * the counters are what the placement says. */
    void check()
    {
        if (!drain())
            return;
        std::vector<bool> dirty(LINES);
        for (unsigned s = 0; s < CACHE_SETS; s++)
            for (unsigned w = 0; w < CACHE_WAYS; w++)
            {
                const entry &e = pol[s * CACHE_WAYS + w];
                if (e.valid && e.dirty)
                    dirty[e.tag * CACHE_SETS + s] = true;
            }
        int bad = 0;
        for (unsigned i = 0; i < MEM_WORDS; i++)
            if (!dirty[i / 4] && mem[i] != ref[i])
                bad++;
        if (bad)
        {
            fprintf(stderr, "%d words differ\n", bad);
            fail("the line memory differs from the CPU's memory");
        }
        if (hits() != want_hits || misses() != want_misses
            || line_reads != want_reads || line_writes != want_writes)
        {
            fprintf(stderr,
                    "hits %ld/%ld misses %ld/%ld reads %ld/%ld "
                    "writes %ld/%ld\n",
                    hits(), want_hits, misses(), want_misses, line_reads,
                    want_reads, line_writes, want_writes);
            fail("the counters differ from the placement");
        }
    }

    op random_op(uint32_t word)
    {
        static const uint8_t strbs[] = {1, 2, 4, 8, 3, 12, 15};
        op o;
        o.word = word;
        o.write = rnd(2);
        o.strb = strbs[rnd(7)];
        o.data = r32();
        return o;
    }

    static uint32_t word_of(uint32_t tag, uint32_t set, uint32_t off)
    {
        return (tag * CACHE_SETS + set) * 4 + off;
    }

    bool load(uint32_t word)
    {
        op o = {word, false, 15, 0};
        return phase(&o);
    }
    bool store(uint32_t word, uint32_t data)
    {
        op o = {word, true, 15, data};
        return phase(&o);
    }
};

static uint64_t seed_of(uint64_t n)
{
    const char *s = getenv("CACHE_SEED");
    return (s ? strtoull(s, nullptr, 0) : 0x6502) + n * 1000003;
}

/* Words from a few sets and more tags than there are ways, so that most
 * misses replace a line. */
static uint32_t dense_word(bench &b, unsigned sets, unsigned tags)
{
    return bench::word_of(b.rnd(tags) * (TAGS / tags), b.rnd(sets), b.rnd(4));
}

UTEST(cache, random_over_the_whole_ram)
{
    bench b(seed_of(1));
    uint32_t hot = b.rnd(MEM_WORDS - 4096);
    for (int i = 0; i < 300000; i++)
    {
        op o = b.random_op(b.rnd(3) ? hot + b.rnd(4096) : b.rnd(MEM_WORDS));
        b.phase(&o);
        while (b.rnd(4) == 0)
            b.phase(nullptr);
        if (i % 50000 == 49999)
            b.check();
    }
    b.read_dirty();
    b.check();
    ASSERT_EQ(0, b.errors);
    ASSERT_GT(b.hits(), 0);
    ASSERT_GT(b.misses(), 0);
}

UTEST(cache, back_to_back_with_overlapping_phases)
{
    bench b(seed_of(2));
    b.max_delay = 1;
    uint32_t last = 0;
    for (int i = 0; i < 300000; i++)
    {
        uint32_t word;
        switch (b.rnd(4))
        {
            case 0: word = last; break;
            case 1: word = last ^ b.rnd(4); break;
            /* The same set and word of the line in another way. */
            case 2: word = last ^ (CACHE_SETS * 4 << b.rnd(2)); break;
            default: word = dense_word(b, 2, CACHE_WAYS); break;
        }
        op o = b.random_op(word);
        b.phase(&o);
        last = word;
    }
    b.read_dirty();
    b.check();
    ASSERT_EQ(0, b.errors);
}

UTEST(cache, store_then_load_of_the_same_word)
{
    bench b(seed_of(3));
    for (int i = 0; i < 20000; i++)
    {
        uint32_t word = dense_word(b, 4, CACHE_WAYS * 2);
        op s = b.random_op(word);
        s.write = true;
        b.phase(&s);
        b.load(word);
        if (b.rnd(2))
            b.phase(nullptr);
    }
    b.check();
    ASSERT_EQ(0, b.errors);
}

UTEST(cache, dense_evictions)
{
    bench b(seed_of(4));
    for (int i = 0; i < 300000; i++)
    {
        op o = b.random_op(dense_word(b, 3, CACHE_WAYS * 2 + 1));
        b.phase(&o);
        while (b.rnd(8) == 0)
            b.phase(nullptr);
        if (i % 30000 == 29999)
            b.check();
    }
    b.read_dirty();
    b.check();
    ASSERT_EQ(0, b.errors);
    ASSERT_GT(b.line_writes, 1000);
}

UTEST(cache, every_way_fills_and_the_oldest_goes_first)
{
    bench b(seed_of(5));
    const uint32_t set = CACHE_SETS - 1;
    for (uint32_t t = 0; t < CACHE_WAYS; t++)
        b.load(bench::word_of(t, set, t & 3));
    b.drain();
    ASSERT_EQ((long)CACHE_WAYS, b.misses());
    for (uint32_t t = 0; t < CACHE_WAYS; t++)
        b.load(bench::word_of(t, set, 3 - (t & 3)));
    b.drain();
    ASSERT_EQ((long)CACHE_WAYS, b.misses());
    ASSERT_EQ((long)CACHE_WAYS, b.hits());
    /* One more tag takes the place of the oldest, twice around the
     * ways. The tags that stay all hit. */
    for (uint32_t t = CACHE_WAYS; t < CACHE_WAYS * 3; t++)
    {
        long misses = b.misses();
        b.load(bench::word_of(t, set, 0));
        for (uint32_t k = t - CACHE_WAYS + 1; k <= t; k++)
            b.load(bench::word_of(k, set, 1));
        b.drain();
        ASSERT_EQ(misses + 1, b.misses());
    }
    long misses = b.misses();
    b.load(bench::word_of(CACHE_WAYS * 2 - 1, set, 0));
    b.drain();
    ASSERT_EQ(misses + 1, b.misses());
    b.check();
    ASSERT_EQ(0, b.errors);
    ASSERT_EQ(0L, b.line_writes);
}

UTEST(cache, a_dirty_line_is_written_before_it_is_replaced)
{
    bench b(seed_of(6));
    const uint32_t set = 1 % CACHE_SETS;
    for (uint32_t t = 0; t < CACHE_WAYS; t++)
        b.store(bench::word_of(t, set, t & 3), 0xD1D10000 + t);
    b.drain();
    ASSERT_EQ(0L, b.line_writes);
    for (uint32_t t = 0; t < CACHE_WAYS; t++)
    {
        /* A store miss replaces a dirty line and makes another. */
        b.store(bench::word_of(CACHE_WAYS + t, set, 0), 0xD2D20000 + t);
        b.drain();
        ASSERT_EQ((long)t + 1, b.line_writes);
        ASSERT_EQ(0xD1D10000 + t, b.mem[bench::word_of(t, set, t & 3)]);
    }
    /* Loads replace those, and clean lines after that write nothing. */
    for (uint32_t t = 0; t < CACHE_WAYS * 2; t++)
        b.load(bench::word_of(CACHE_WAYS * 2 + t, set, 0));
    b.drain();
    ASSERT_EQ((long)CACHE_WAYS * 2, b.line_writes);
    ASSERT_TRUE(b.mem == b.ref);
    b.check();
    ASSERT_EQ(0, b.errors);
}

UTEST(cache, reset_in_the_middle_of_work)
{
    bench b(seed_of(9));
    int resets = 0;
    for (int i = 0; i < 400000; i++)
    {
        if (b.bomb < 0 && b.rnd(64) == 0)
            b.bomb = (int)b.rnd(60);
        op o = b.random_op(b.rnd(4) ? dense_word(b, 3, CACHE_WAYS * 2)
                                    : b.rnd(MEM_WORDS));
        bool ok = b.phase(&o);
        if (ok && b.rnd(4) == 0)
            ok = b.phase(nullptr);
        if (!ok)
        {
            resets++;
            /* The first access can come in any clock after the reset. */
            for (int k = (int)b.rnd(4) * (int)b.rnd(CACHE_SETS); k > 0; k--)
                b.phase(nullptr);
        }
        if (i % 20000 == 19999)
        {
            b.bomb = -1;
            b.check();
        }
    }
    b.bomb = -1;
    b.read_dirty();
    b.check();
    ASSERT_EQ(0, b.errors);
    ASSERT_GT(resets, 1000);
}

UTEST(cache, power_up_with_no_reset)
{
    for (int k = 0; k < 20; k++)
    {
        bench b(seed_of(100 + k));
        for (int i = (int)b.rnd(3); i > 0; i--)
            b.phase(nullptr);
        for (int i = 0; i < 5000; i++)
        {
            op o = b.random_op(b.rnd(MEM_WORDS));
            b.phase(&o);
        }
        b.read_dirty();
        b.check();
        ASSERT_EQ(0, b.errors);
    }
}

UTEST(cache, slow_and_instant_line_memory)
{
    for (int delay : {0, 40})
    {
        bench b(seed_of(11));
        b.max_delay = delay;
        for (int i = 0; i < 50000; i++)
        {
            op o = b.random_op(dense_word(b, 3, CACHE_WAYS * 2));
            b.phase(&o);
        }
        b.read_dirty();
        b.check();
        ASSERT_EQ(0, b.errors);
    }
}

UTEST_MAIN()
