/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_BENCH_TB_SNAP_H_
#define _TESTS_BENCH_TB_SNAP_H_

/* Snapshots of a whole simulated machine, so that a test starts from a
 * machine that has already booted instead of booting it again.
 *
 * A snapshot is the Verilated model's state, which needs a model Verilated
 * with --savable, and then the state the bench holds in C++: its clocks, its
 * card, what it has seen. A bench keeps that state in one struct, which a
 * fresh configuration starts from as it is made and whose io() lists every
 * field. A field io() leaves out comes back stale from a snapshot, so a new
 * field of the bench goes into io() too.
 *
 * Snapshots kept between runs are files, so every test process can start from one another
 * process made. A file is named for what made it: the test executable,
 * which holds the model and the bench, and a key the test makes of what it
 * fed the machine, such as the firmware, the ROM and the card. What runs up
 * to a snapshot must depend on nothing the key leaves out. A test process
 * that finds no file boots the machine itself and writes one, so a test
 * run alone needs none. A new executable makes new files, and the first it
 * writes removes those of the executables before it.
 *
 * A snapshot also forks a session in memory: test_psleep.cpp runs its
 * reference from one, and holds it tick for tick to the machine that ran
 * on, so a restored machine that is not the one it was taken from fails
 * there. */

#include "verilated_save.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <map>
#include <set>
#include <string>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <vector>

/* Writes into bytes. */
class tb_snap_out final : public VerilatedSerialize
{
    std::vector<uint8_t> &m_bytes;

public:
    explicit tb_snap_out(std::vector<uint8_t> &bytes) : m_bytes(bytes)
    {
        m_bytes.clear();
        m_isOpen = true;
        header();
    }
    ~tb_snap_out() override
    {
        trailer();
        flush();
    }
    void flush() override
    {
        m_bytes.insert(m_bytes.end(), m_bufp, m_cp);
        m_cp = m_bufp;
    }
};

/* Reads from bytes. */
class tb_snap_in final : public VerilatedDeserialize
{
    const std::vector<uint8_t> &m_bytes;
    size_t m_at = 0;

public:
    explicit tb_snap_in(const std::vector<uint8_t> &bytes) : m_bytes(bytes)
    {
        m_isOpen = true;
        m_cp = m_endp = m_bufp;
        header();
    }
    ~tb_snap_in() override { trailer(); }
    void fill() override
    {
        size_t left = (size_t)(m_endp - m_cp);
        memmove(m_bufp, m_cp, left);
        m_cp = m_bufp;
        m_endp = m_bufp + left;
        size_t n = bufferSize() - left;
        if (n > m_bytes.size() - m_at)
            n = m_bytes.size() - m_at;
        memcpy(m_endp, m_bytes.data() + m_at, n);
        m_endp += n;
        m_at += n;
    }
};

/* tb_snap_io writes a field of the bench to a VerilatedSerialize, and
 * reads it from a VerilatedDeserialize, so one io() function does both. */
template <typename T>
static void tb_snap_io(VerilatedSerialize &os, const T &v)
{
    static_assert(std::is_trivially_copyable<T>::value, "plain data only");
    os.write(&v, sizeof v);
}
template <typename T>
static void tb_snap_io(VerilatedDeserialize &is, T &v)
{
    static_assert(std::is_trivially_copyable<T>::value, "plain data only");
    is.read(&v, sizeof v);
}
static void tb_snap_io(VerilatedSerialize &os, const std::string &v)
{
    os << v;
}
static void tb_snap_io(VerilatedDeserialize &is, std::string &v)
{
    is >> v;
}
template <typename T>
static void tb_snap_io(VerilatedSerialize &os, const std::vector<T> &v)
{
    tb_snap_io(os, (uint64_t)v.size());
    for (const T &e : v)
        tb_snap_io(os, e);
}
template <typename T>
static void tb_snap_io(VerilatedDeserialize &is, std::vector<T> &v)
{
    uint64_t n;
    tb_snap_io(is, n);
    v.resize(n);
    for (T &e : v)
        tb_snap_io(is, e);
}
template <typename T, size_t N>
static void tb_snap_io(VerilatedSerialize &os, const T (&v)[N])
{
    for (const T &e : v)
        tb_snap_io(os, e);
}
template <typename T, size_t N>
static void tb_snap_io(VerilatedDeserialize &is, T (&v)[N])
{
    for (T &e : v)
        tb_snap_io(is, e);
}
template <typename T>
static void tb_snap_io(VerilatedSerialize &os, const std::set<T> &v)
{
    tb_snap_io(os, (uint64_t)v.size());
    for (const T &e : v)
        tb_snap_io(os, e);
}
template <typename T>
static void tb_snap_io(VerilatedDeserialize &is, std::set<T> &v)
{
    uint64_t n;
    tb_snap_io(is, n);
    v.clear();
    while (n--)
    {
        T e;
        tb_snap_io(is, e);
        v.insert(e);
    }
}
template <typename K, typename V>
static void tb_snap_io(VerilatedSerialize &os, const std::map<K, V> &v)
{
    tb_snap_io(os, (uint64_t)v.size());
    for (const auto &e : v)
    {
        tb_snap_io(os, e.first);
        tb_snap_io(os, e.second);
    }
}
template <typename K, typename V>
static void tb_snap_io(VerilatedDeserialize &is, std::map<K, V> &v)
{
    uint64_t n;
    tb_snap_io(is, n);
    v.clear();
    while (n--)
    {
        K k;
        tb_snap_io(is, k);
        tb_snap_io(is, v[k]);
    }
}

/* The model and the bench as they are. */
template <typename Model, typename Bench>
static std::vector<uint8_t> tb_snap_take(Model &model, Bench &bench)
{
    std::vector<uint8_t> v;
    {
        tb_snap_out os(v);
        os << model;
        bench.io(os);
    }
    return v;
}

/* The model and the bench put back as tb_snap_take found them. The model
 * may be one that has run since, or a new one. */
template <typename Model, typename Bench>
static void tb_snap_load(Model &model, const std::vector<uint8_t> &v,
                         Bench &bench)
{
    tb_snap_in is(v);
    is >> model;
    bench.io(is);
}

/* A new model, and the bench, as tb_snap_take found them. */
template <typename Model, typename Bench>
static Model *tb_snap_make(const std::vector<uint8_t> &v, Bench &bench)
{
    Model *model = new Model;
    tb_snap_load(*model, v, bench);
    return model;
}

/* A 64-bit FNV-1a hash, which the keys are made of. */
static uint64_t tb_snap_hash(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--)
        h = (h ^ *b++) * 0x100000001B3ull;
    return h;
}
#define TB_SNAP_HASH0 0xCBF29CE484222325ull

/* A key over the bench's state, made with its io() function. */
class tb_snap_key final : public VerilatedSerialize
{
public:
    uint64_t h = TB_SNAP_HASH0;
    tb_snap_key() { m_isOpen = true; }
    ~tb_snap_key() override { flush(); }
    void flush() override
    {
        h = tb_snap_hash(h, m_bufp, (size_t)(m_cp - m_bufp));
        m_cp = m_bufp;
    }
};

/* The hash of the running test executable. */
static uint64_t tb_snap_exe()
{
    static uint64_t h;
    if (h)
        return h;
    h = TB_SNAP_HASH0;
    FILE *f = fopen("/proc/self/exe", "rb");
    if (!f)
        return h;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        h = tb_snap_hash(h, buf, n);
    fclose(f);
    return h;
}

/* The folder of the snapshots, made on first use. dir is the test's build
 * folder and name the test executable's. */
static std::string tb_snap_dir(const char *dir, const char *name)
{
    std::string d = std::string(dir) + "/snap";
    mkdir(d.c_str(), 0777);
    d += std::string("/") + name;
    mkdir(d.c_str(), 0777);
    return d;
}

static std::string tb_snap_path(const std::string &dir, uint64_t key)
{
    char s[64];
    snprintf(s, sizeof s, "/%016llx-%016llx.snap",
             (unsigned long long)tb_snap_exe(), (unsigned long long)key);
    return dir + s;
}

/* The model's memories are mostly zeros, so a file holds runs: the length
 * of some bytes as they are, those bytes, then the length of the zeros that
 * follow them. */
static std::vector<uint8_t> tb_snap_pack(const std::vector<uint8_t> &v)
{
    std::vector<uint8_t> out;
    auto len = [&](uint64_t n) {
        out.insert(out.end(), (const uint8_t *)&n, (const uint8_t *)&n + 8);
    };
    size_t i = 0;
    while (i < v.size())
    {
        size_t lit = i;
        /* A run of zeros shorter than 64 bytes stays with the bytes. */
        while (lit < v.size())
        {
            size_t z = lit;
            while (z < v.size() && !v[z] && z - lit < 64)
                z++;
            if (z - lit == 64 || z == v.size())
                break;
            lit = z + 1;
        }
        size_t zeros = lit;
        while (zeros < v.size() && !v[zeros])
            zeros++;
        len(lit - i);
        out.insert(out.end(), v.begin() + (long)i, v.begin() + (long)lit);
        len(zeros - lit);
        i = zeros;
    }
    return out;
}

static bool tb_snap_unpack(const std::vector<uint8_t> &in,
                           std::vector<uint8_t> &v)
{
    v.clear();
    size_t i = 0;
    auto len = [&](uint64_t &n) {
        if (in.size() - i < 8)
            return false;
        memcpy(&n, in.data() + i, 8);
        i += 8;
        return true;
    };
    while (i < in.size())
    {
        uint64_t lit, zeros;
        if (!len(lit) || in.size() - i < lit)
            return false;
        v.insert(v.end(), in.begin() + (long)i, in.begin() + (long)(i + lit));
        i += lit;
        if (!len(zeros))
            return false;
        v.resize(v.size() + zeros, 0);
    }
    return true;
}

/* Reads a snapshot. Returns false when there is none. */
static bool tb_snap_read(const std::string &path, std::vector<uint8_t> &v)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::vector<uint8_t> in;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        in.insert(in.end(), buf, buf + n);
    fclose(f);
    return tb_snap_unpack(in, v);
}

/* Writes a snapshot under a name of its own and then renames it, so a
 * process that reads it at the same time finds all of it or none. The first
 * write of an executable removes the snapshots of the ones before it. */
static void tb_snap_write(const std::string &path, const std::vector<uint8_t> &v)
{
    static bool swept;
    std::string dir = path.substr(0, path.rfind('/'));
    if (!swept)
    {
        swept = true;
        std::string mine = path.substr(dir.size() + 1, 16);
        if (DIR *d = opendir(dir.c_str()))
        {
            while (dirent *e = readdir(d))
                if (e->d_name[0] != '.' && strncmp(e->d_name, mine.c_str(), 16))
                    unlink((dir + "/" + e->d_name).c_str());
            closedir(d);
        }
    }
    std::string tmp = path + "." + std::to_string((long)getpid());
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f)
        return;
    std::vector<uint8_t> out = tb_snap_pack(v);
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp.c_str(), path.c_str()))
        unlink(tmp.c_str());
}

#endif /* _TESTS_BENCH_TB_SNAP_H_ */
