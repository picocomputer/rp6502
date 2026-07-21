/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * DAP (Debug Adapter Protocol) server over stdio, built on google/cppdap. VS
 * Code (hosted by the stock lldb-dap extension) spawns us with --dap and drives
 * debugging over stdin/stdout. This is the MACHINE level: registers, memory,
 * disassembly, instruction stepping and instruction (address) breakpoints, plus
 * the launch/continue/pause lifecycle and program output. Source-line mapping
 * (DWARF) is layered on in a later phase.
 *
 * Threading: cppdap reads messages on its own thread, so handlers run there.
 * They MARSHAL state changes to the main (emulation) thread via a command queue
 * drained by dap_pump() — the only thread that advances the CPU — except
 * dbg_request_pause(), which is a lone atomic. Handlers that READ machine state
 * (registers/memory/stack) do so directly: VS Code only asks once we are stopped
 * (the CPU is then idle), so the reads are race-free.
 */

#include "dap/io.h"
#include "dap/protocol.h"
#include "dap/session.h"

extern "C"
{
#include "ria/api/oem.h"
#include "emu/api/pro.h"
#include "emu/dbg/dbg.h"
#include "emu/sys/com.h"
#include "emu/sys/cpu.h"
#include "emu/sys/mem.h"
#include "emu/main.h"
#include "emu/dbg/dap.h"
#include "emu/dbg/dwarf_line.h"
#include "emu/dbg/dwarf_info.h"
#include "emu/dbg/dwarf_frame.h"
#include "emu/dbg/cc65dbg.h"
}
#include "emu/chips/w65c02.h"
#include "emu/chips/w65c02dasm.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/* ---- a custom launch request carrying the RP6502 config VS Code passes ---- */
namespace dap
{
struct RP6502LaunchRequest : public LaunchRequest
{
    optional<string> program;     /* the .rp6502 to load + run */
    optional<array<string>> args; /* the program's argv[1..] */
    optional<string> elf;         /* companion ELF carrying the DWARF line table (llvm-mos) */
    optional<string> dbg;         /* companion cc65 .dbg file (cc65 has no DWARF) */
    optional<boolean> stopOnEntry;
    optional<boolean> stopOnExit; /* keep the session stopped (not terminated) on exit */
};
DAP_DECLARE_STRUCT_TYPEINFO(RP6502LaunchRequest);
DAP_IMPLEMENT_STRUCT_TYPEINFO_EXT(RP6502LaunchRequest, LaunchRequest, "launch",
                                  DAP_FIELD(program, "program"),
                                  DAP_FIELD(args, "args"),
                                  DAP_FIELD(elf, "elf"),
                                  DAP_FIELD(dbg, "dbg"),
                                  DAP_FIELD(stopOnEntry, "stopOnEntry"),
                                  DAP_FIELD(stopOnExit, "stopOnExit"));
} // namespace dap

namespace
{

std::unique_ptr<dap::Session> g_session;

std::mutex g_mtx;
std::vector<std::function<void()>> g_queue; /* main-thread work */
std::vector<uint16_t> g_instr_bps;          /* current instruction breakpoints (main thread) */
std::vector<uint16_t> g_func_bps;           /* current function breakpoints (main thread) */

/* The program's source map: DWARF (.debug_line + .debug_info, llvm-mos ELF) or
 * cc65 (.dbg). Exactly one toolchain is loaded per session; the helpers below
 * dispatch to whichever. g_dinfo is the variable/type half of the DWARF map.
 * g_src_mtx guards these three pointers and the memory they own: the launch lambda
 * frees+reloads them on the main thread while cppdap request handlers read them on
 * the reader thread. Main-thread readers (dbg.c stepping) don't take it — they
 * can't run concurrently with the same-thread writer, and read+read is safe. */
std::mutex g_src_mtx;
dwarf_line_t *g_dwarf = nullptr;
dwarf_info_t *g_dinfo = nullptr;
dwarf_frame_t *g_dframe = nullptr; /* .debug_frame CFI, for principled unwinding */
cc65dbg_t *g_cc65 = nullptr;
/* Optional per-breakpoint semantics (condition/hit-count/logpoint). Sparse: an
 * address with none of these never appears in g_bp_meta and always stops. */
struct BpMeta
{
    std::string condition, logMessage;
    enum HitOp { HIT_NONE, HIT_EQ, HIT_GE, HIT_GT, HIT_LT, HIT_LE, HIT_MULT } hitOp = HIT_NONE;
    long hitN = 0;
    unsigned long hits = 0;
    bool plain() const { return condition.empty() && logMessage.empty() && hitOp == HIT_NONE; }
};
struct SrcBp
{
    uint16_t addr;
    BpMeta meta;
};
std::map<std::string, std::vector<SrcBp>> g_src_bps; /* source breakpoints, per file */
std::map<uint16_t, BpMeta> g_bp_meta;                /* flattened; consulted by bp_filter */

/* Data breakpoints (watchpoints): a store/load into [addr, addr+width) trips a
 * stop. Owned by the main thread (SetDataBreakpoints post lambda + the bus hook). */
struct Watch
{
    uint16_t addr;
    int width;
    bool on_read, on_write;
};
std::vector<Watch> g_watches;

/* True if address a is still wanted by a breakpoint set other than the one being
 * replaced, so replace-semantics on one category (a source file or the instruction
 * set) never clears a coincident breakpoint another category set at the same
 * address. skip_src, when non-NULL, excludes the source file being replaced. */
static bool bp_referenced(uint16_t a, const std::string *skip_src, bool with_instr)
{
    if (with_instr)
        for (uint16_t x : g_instr_bps)
            if (x == a)
                return true;
    for (uint16_t x : g_func_bps) /* a function breakpoint always wants its entry */
        if (x == a)
            return true;
    for (auto &kv : g_src_bps)
    {
        if (skip_src && kv.first == *skip_src)
            continue;
        for (const SrcBp &sb : kv.second)
            if (sb.addr == a)
                return true;
    }
    return false;
}

/* Rebuild g_bp_meta from all source files' non-plain breakpoints (main thread). */
void rebuild_bp_meta()
{
    g_bp_meta.clear();
    for (auto &kv : g_src_bps)
        for (const SrcBp &sb : kv.second)
            if (!sb.meta.plain())
                g_bp_meta[sb.addr] = sb.meta;
}

/* Read a byte of 6502 memory — the callback the variable resolvers use to fetch
 * the live frame base (soft / C stack pointer). Only called while stopped. */
uint8_t dap_readmem(uint16_t a) { return ram[a]; }

bool src_addr_to_line(uint16_t addr, const char **file, int *line)
{
    if (g_dwarf)
        return dwarf_line_addr_to_src(g_dwarf, addr, file, line);
    if (g_cc65)
        return cc65dbg_addr_to_src(g_cc65, addr, file, line);
    return false;
}
bool src_line_to_addr(const char *file, int line, uint16_t *addr, int *bound)
{
    if (g_dwarf)
        return dwarf_line_src_to_addr(g_dwarf, file, line, addr, bound);
    if (g_cc65)
        return cc65dbg_src_to_addr(g_cc65, file, line, addr, bound);
    return false;
}
const char *src_addr_to_func(uint16_t addr)
{
    if (g_dwarf)
        return dwarf_line_addr_to_func(g_dwarf, addr);
    if (g_cc65)
        return cc65dbg_addr_to_func(g_cc65, addr);
    return nullptr;
}
bool src_func_addr(const char *name, uint16_t *addr)
{
    if (g_dwarf)
        return dwarf_line_func_addr(g_dwarf, name, addr);
    if (g_cc65)
        return cc65dbg_func_addr(g_cc65, name, addr);
    return false;
}
void src_free()
{
    if (g_dwarf)
    {
        dwarf_line_free(g_dwarf);
        g_dwarf = nullptr;
    }
    if (g_dinfo)
    {
        dwarf_info_free(g_dinfo);
        g_dinfo = nullptr;
    }
    if (g_dframe)
    {
        dwarf_frame_free(g_dframe);
        g_dframe = nullptr;
    }
    if (g_cc65)
    {
        cc65dbg_free(g_cc65);
        g_cc65 = nullptr;
    }
    dbg_set_segments(nullptr, 0); /* the memory map's segment layer goes empty */
}

/* Publish the loaded program's linker segments to dbg.c, where the ImGui memory
 * map reads them (llvm-mos = ELF sections, cc65 = .dbg seg records). */
void push_segments()
{
    dbg_segment_t segs[DBG_MAX_SEGMENTS];
    int n = 0;
    if (g_dwarf)
    {
        dwarf_section_t s[DBG_MAX_SEGMENTS];
        int m = dwarf_line_sections(g_dwarf, s, DBG_MAX_SEGMENTS);
        for (int i = 0; i < m; i++, n++)
        {
            std::snprintf(segs[n].name, sizeof segs[n].name, "%s", s[i].name);
            segs[n].addr = s[i].addr;
            segs[n].size = s[i].size;
        }
    }
    else if (g_cc65)
    {
        cc65seg_t s[DBG_MAX_SEGMENTS];
        int m = cc65dbg_segments(g_cc65, s, DBG_MAX_SEGMENTS);
        for (int i = 0; i < m; i++, n++)
        {
            std::snprintf(segs[n].name, sizeof segs[n].name, "%s", s[i].name);
            segs[n].addr = s[i].addr;
            segs[n].size = s[i].size;
        }
    }
    dbg_set_segments(segs, n);
}

/* addr -> source line, for dbg.c's source-level stepping (main thread). */
bool line_lookup(uint16_t addr, const char **file, int *line)
{
    return src_addr_to_line(addr, file, line);
}

std::string base_name(const std::string &p)
{
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::atomic<bool> g_configured{false};
std::atomic<bool> g_quit{false};
bool g_reached_entry = false;
bool g_launch_done = false;
bool g_stop_on_entry = false;
bool g_stop_on_exit = true; /* present program exit as a stop, not a terminate */
bool g_terminated = false;
bool g_term_sent = false; /* a TerminatedEvent has gone out; don't repeat it at teardown */
bool g_launch_requested = false; /* a launch reached pro_exec; used to detect a load that never started */
unsigned g_stop_gen = 0;         /* bumped on each client-visible stop */
unsigned g_varnodes_gen = ~0u;   /* the g_stop_gen g_varnodes were built for */
std::vector<std::string> g_default_args; /* ROM argv[1..] for launch requests that carry none */

void post(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_queue.push_back(std::move(fn));
}

w65c02_t *cpu() { return (w65c02_t *)cpu_chip(); }

const char *reason_str(int r)
{
    switch (r)
    {
    case DBG_REASON_BREAKPOINT:
        return "breakpoint";
    case DBG_REASON_STEP:
        return "step";
    case DBG_REASON_ENTRY:
        return "entry";
    case DBG_REASON_DATA:
        return "data breakpoint";
    default:
        return "pause";
    }
}

void send_stopped(int reason)
{
    g_stop_gen++; /* a new stop: expandable-variable refs from the last one are stale */
    if (!g_session)
        return;
    dap::StoppedEvent ev;
    ev.reason = reason_str(reason);
    ev.threadId = 1;
    ev.allThreadsStopped = true;
    g_session->send(ev);
}

/* dbg.c stopped callback — fires on the MAIN thread when execution halts. */
void on_stopped(int reason, uint16_t pc)
{
    (void)pc;
    /* The internal entry stop (used to hold the program during configuration) is
     * resolved by dap_pump once configurationDone has arrived. */
    if (reason == DBG_REASON_ENTRY && !g_launch_done)
    {
        g_reached_entry = true;
        return;
    }
    send_stopped(reason);
}

/* Program console output -> the Debug Console (also still shown in the window).
 * The bytes are in the active OEM code page; convert to UTF-8 so a high byte
 * (e.g. CP437 0x81 'ü') can't produce a malformed JSON OutputEvent. */
void stdout_tap(const char *buf, int len)
{
    if (!g_session)
        return;
    std::string utf8;
    utf8.reserve((size_t)len);
    for (int i = 0; i < len; i++)
    {
        char enc[3];
        utf8.append(enc, (size_t)oem_to_utf8_char((unsigned char)buf[i], enc));
    }
    dap::OutputEvent ev;
    ev.category = "stdout";
    ev.output = std::move(utf8);
    g_session->send(ev);
}

/* The inverse boundary: DAP JSON strings are UTF-8, the guest wants OEM. */
std::string oem_from_utf8_str(const std::string &u8)
{
    std::string oem;
    oem.reserve(u8.size());
    const char *p = u8.c_str();
    unsigned char b;
    while ((b = oem_from_utf8_next(&p)))
        oem.push_back((char)b);
    return oem;
}

uint16_t parse_addr(const std::string &s)
{
    return (uint16_t)strtoul(s.c_str(), nullptr, 0); /* "0x.." hex or decimal */
}

std::string hex16(uint16_t v)
{
    char b[8];
    snprintf(b, sizeof b, "0x%04X", v);
    return b;
}

std::string b64(const uint8_t *data, size_t n)
{
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3)
    {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < n)
            v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < n)
            v |= (uint32_t)data[i + 2];
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        out += (i + 2 < n) ? T[v & 63] : '=';
    }
    return out;
}

/* One-instruction disassembly via w65c02dasm, capturing text + raw bytes. */
struct DasmCtx
{
    uint16_t pc;
    std::string text;
    std::string bytes;
};
uint8_t dasm_in(void *u)
{
    DasmCtx *c = (DasmCtx *)u;
    uint8_t b = ram[c->pc++];
    char h[4];
    snprintf(h, sizeof h, "%02X ", b);
    c->bytes += h;
    return b;
}
void dasm_out(char ch, void *u) { ((DasmCtx *)u)->text += ch; }

/* ---- variable inspection ----------------------------------------------------
 * Scopes: Locals (ref 2), Globals (ref 3), Registers (ref 1). Expandable
 * aggregates (arrays/structs/pointers) get a dynamic ref (1000+) into a per-stop
 * registry of {address, type} nodes, rebuilt each StackTrace. The DWARF path is
 * typed; the cc65 path is untyped (type == nullptr -> a raw 16-bit word). */

struct VarNode
{
    uint16_t addr;
    const dtype_t *type;
};
std::vector<VarNode> g_varnodes; /* index i -> variablesReference 1000+i */

int var_ref(uint16_t addr, const dtype_t *t)
{
    g_varnodes.push_back({addr, t});
    return 1000 + (int)g_varnodes.size() - 1;
}

/* A call-stack frame, rebuilt each StackTrace: src_pc is the address whose source
 * line/function names the frame (frame 0 = the stopped PC; a caller = its
 * call-site PC−1), ip is the machine instruction pointer to show. Frame id = the
 * index into g_frames; ScopesRequest.frameId selects it. */
struct Frame
{
    uint16_t src_pc;
    uint16_t ip;
    uint16_t base;  /* reconstructed soft/C stack frame base for locals */
    bool base_ok;   /* false -> frame-relative locals can't be resolved here */
};
std::vector<Frame> g_frames;

/* variablesReference scheme: 1 = Registers, 3 = Globals, 1000+ = expandable
 * aggregate nodes; a frame's Locals scope is LOCALS_REF_BASE + frameId (a band
 * clear of those, sized past the unwind's 64-frame cap). */
constexpr int64_t LOCALS_REF_BASE = 200;
constexpr int64_t LOCALS_REF_SPAN = 256;

/* Name-resolution context for one frame. have_base false => compute the base live
 * from pc (valid only for the innermost frame); true => use the base
 * compute_frame_bases() reconstructed for a caller frame. */
struct FrameCtx
{
    uint16_t pc = 0;
    uint16_t base = 0;
    bool base_ok = false;
    bool have_base = false;
};

/* Map a client frameId to its resolution context. Falls back to the innermost
 * frame (live base) when the stack hasn't been unwound or the id is out of range.
 * Caller holds g_src_mtx. */
FrameCtx frame_ctx(int64_t frameId)
{
    FrameCtx fc;
    if (frameId >= 0 && (size_t)frameId < g_frames.size())
    {
        fc.pc = g_frames[(size_t)frameId].src_pc;
        fc.base = g_frames[(size_t)frameId].base;
        fc.base_ok = g_frames[(size_t)frameId].base_ok;
        fc.have_base = true;
    }
    else
        fc.pc = dbg_stop_pc();
    return fc;
}

/* A 6502 stack slot holds a JSR return address when a JSR (0x20) sits at pushed−2
 * and its operand is EXACTLY a known function's entry. Requiring the entry (not
 * merely an in-range address) is what makes the scan below safe: register/data
 * bytes practically never form a JSR pointing at a real entry, so no frame is
 * fabricated. Caller holds g_src_mtx (the source map reads). */
bool stack_return_target(uint16_t pushed, uint16_t *target_out)
{
    uint16_t jsr = (uint16_t)(pushed - 2);
    if (ram[jsr] != 0x20)
        return false;
    uint16_t target = (uint16_t)(ram[(uint16_t)(jsr + 1)] | (ram[(uint16_t)(jsr + 2)] << 8));
    const char *fn = src_addr_to_func(target);
    uint16_t entry;
    if (!fn || !src_func_addr(fn, &entry) || entry != target)
        return false;
    if (target_out)
        *target_out = target;
    return true;
}

/* Walk the 6502 hardware stack (page 1) collecting JSR return frames, appending
 * to g_frames after frame 0. At each level scan forward for the next return slot:
 * llvm-mos saves callee registers on the hardware stack around calls (phx of the
 * imaginary registers), so return addresses are not adjacent; cc65 keeps its
 * frame on the soft stack, so its slots are. The scan skips those saved bytes but
 * truncates once no return slot appears within the window (under-report, never
 * fabricate). Caller must hold g_src_mtx. */
constexpr int UNWIND_SCAN_MAX = 16; /* bytes of register saves to step over */
void unwind_stack()
{
    uint8_t sp = w65c02_s(cpu());
    for (int depth = 0; depth < 64; depth++)
    {
        bool found = false;
        for (int skip = 0; skip <= UNWIND_SCAN_MAX; skip++)
        {
            uint8_t lo_s = (uint8_t)(sp + 1 + skip), hi_s = (uint8_t)(sp + 2 + skip);
            uint16_t pushed = (uint16_t)(ram[0x100 + lo_s] | (ram[0x100 + hi_s] << 8));
            if (!stack_return_target(pushed, nullptr))
                continue;
            g_frames.push_back({pushed, (uint16_t)(pushed + 1)});
            sp = (uint8_t)(sp + 2 + skip);
            found = true;
            break;
        }
        if (!found)
            break;
    }
}

/* Fill each frame's soft/C stack frame base for the heuristic (non-CFI) unwind.
 * llvm-mos resolves only the top frame's base directly — caller chaining comes
 * from CFI, so callers here stay base_ok=false. cc65 has no CFI, so a caller's
 * base is chained outward from stack-adjustment deltas (callee argument region +
 * caller frame size). Fail-closed: an unresolved delta leaves that frame and every
 * outer frame base_ok=false so frame-relative locals show as unavailable rather
 * than fabricated. Caller holds g_src_mtx. */
void compute_frame_bases()
{
    if (g_frames.empty())
        return;
    if (g_dinfo)
        g_frames[0].base_ok =
            dwarf_info_frame_base(g_dinfo, g_frames[0].src_pc, dap_readmem, &g_frames[0].base);
    else if (g_cc65)
    {
        g_frames[0].base_ok =
            cc65dbg_frame_base(g_cc65, g_frames[0].src_pc, dap_readmem, &g_frames[0].base);
        for (size_t k = 1; k < g_frames.size(); k++)
        {
            uint16_t argsz;
            if (g_frames[k - 1].base_ok &&
                cc65dbg_arg_size(g_cc65, g_frames[k - 1].src_pc, &argsz))
            {
                int32_t fs = cc65dbg_frame_size(g_cc65, g_frames[k].src_pc);
                g_frames[k].base = (uint16_t)((int32_t)g_frames[k - 1].base + argsz + fs);
                g_frames[k].base_ok = true;
            }
            else
                g_frames[k].base_ok = false;
        }
    }
}

/* CFI-driven unwind (llvm-mos .debug_frame): fills g_frames — src_pc, ip, and
 * soft-stack frame base — for every frame by evaluating the CIE/FDE rules. This
 * is exact where unwind_stack()+compute_frame_bases() were heuristic: the CFI
 * recovers each caller's PC and soft-stack pointer directly. A caller PC that
 * lands outside any known function stops the walk (never fabricate). Caller
 * holds g_src_mtx. */
void unwind_stack_cfi(uint16_t pc0)
{
    uint16_t pc = pc0;
    uint16_t s16 = (uint16_t)(0x0100 | w65c02_s(cpu()));
    uint16_t rs0 = 0;
    bool base_ok = g_dinfo && dwarf_info_frame_base(g_dinfo, pc, dap_readmem, &rs0);
    if (!base_ok)
        rs0 = (uint16_t)(dap_readmem(0) | (dap_readmem(1) << 8)); /* rp6502 soft SP at rc0=$00 */
    g_frames.push_back({pc, pc, rs0, base_ok});
    for (int depth = 1; depth < 64; depth++)
    {
        dwarf_unwind_t u = dwarf_frame_step(g_dframe, pc, s16, rs0, dap_readmem);
        if (!u.ok)
            break;
        pc = u.pc; /* the return-slot value: a call-site address (return - 1) */
        s16 = u.s16;
        rs0 = u.rs0;
        if (!src_addr_to_func(pc))
            break;
        g_frames.push_back({pc, (uint16_t)(pc + 1), rs0, true});
    }
}

uint64_t mem_le(uint16_t addr, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n && i < 8; i++)
        v |= (uint64_t)ram[(uint16_t)(addr + i)] << (8 * i);
    return v;
}

/* Sign-extend the low sz bytes of raw to a full int64_t (sz<8 guards the
 * shift-by-64 UB when sz==8). */
int64_t sign_extend(uint64_t raw, int sz)
{
    if (sz < 8 && (raw & ((uint64_t)1 << (sz * 8 - 1))))
        return (int64_t)(raw | (~(uint64_t)0 << (sz * 8)));
    return (int64_t)raw;
}

std::string char_repr(uint8_t c)
{
    char b[8];
    switch (c)
    {
    case '\n': return "'\\n'";
    case '\r': return "'\\r'";
    case '\t': return "'\\t'";
    case 0: return "'\\0'";
    }
    if (c == '\\' || c == '\'') { snprintf(b, sizeof b, "'\\%c'", c); return b; }
    if (c >= 0x20 && c < 0x7f) { snprintf(b, sizeof b, "'%c'", c); return b; }
    snprintf(b, sizeof b, "'\\x%02X'", c);
    return b;
}

/* A NUL-terminated string at addr (up to max bytes), quoted + escaped. */
std::string str_repr(uint16_t addr, int max)
{
    std::string s = "\"";
    for (int i = 0; i < max; i++)
    {
        uint8_t c = ram[(uint16_t)(addr + i)];
        if (!c) break;
        if (c == '"' || c == '\\') { s += '\\'; s += (char)c; }
        else if (c >= 0x20 && c < 0x7f) s += (char)c;
        else { char b[8]; snprintf(b, sizeof b, "\\x%02X", c); s += b; }
    }
    s += "\"";
    return s;
}

bool is_char_type(const dtype_t *t)
{
    return t && dwarf_type_kind(t) == DW_KIND_BASE && dwarf_type_size(t) == 1 &&
           (dwarf_type_encoding(t) == DW_ATE_signed_char ||
            dwarf_type_encoding(t) == DW_ATE_unsigned_char);
}

std::string base_value(uint16_t addr, const dtype_t *t)
{
    int enc = dwarf_type_encoding(t);
    int sz = (int)dwarf_type_size(t);
    if (sz <= 0) sz = 1;
    char b[48];
    if (enc == DW_ATE_float)
    {
        if (sz == 8) { uint64_t u = mem_le(addr, 8); double d; memcpy(&d, &u, 8); snprintf(b, sizeof b, "%g", d); }
        else { uint32_t u = (uint32_t)mem_le(addr, 4); float fv; memcpy(&fv, &u, 4); snprintf(b, sizeof b, "%g", (double)fv); }
        return b;
    }
    if (enc == DW_ATE_boolean)
        return mem_le(addr, sz) ? "true" : "false";
    uint64_t raw = mem_le(addr, sz);
    if (enc == DW_ATE_signed || enc == DW_ATE_signed_char)
    {
        int64_t s = sign_extend(raw, sz);
        if (enc == DW_ATE_signed_char)
        {
            snprintf(b, sizeof b, " (%lld)", (long long)s);
            return char_repr((uint8_t)raw) + b;
        }
        snprintf(b, sizeof b, "%lld", (long long)s);
        return b;
    }
    if (enc == DW_ATE_unsigned_char)
    {
        snprintf(b, sizeof b, " (%llu)", (unsigned long long)raw);
        return char_repr((uint8_t)raw) + b;
    }
    snprintf(b, sizeof b, "%llu", (unsigned long long)raw);
    return b;
}

bool type_expandable(const dtype_t *t)
{
    if (!t) return false;
    switch (dwarf_type_kind(t))
    {
    case DW_KIND_ARRAY: { uint32_t c; dwarf_type_element(t, &c); return c > 0; }
    case DW_KIND_STRUCT:
    case DW_KIND_UNION: return dwarf_type_member_count(t) > 0;
    case DW_KIND_POINTER:
    {
        const dtype_t *p = dwarf_type_pointee(t);
        return p && dwarf_type_kind(p) != DW_KIND_VOID;
    }
    default: return false;
    }
}

/* Build a DAP Variable for (name, addr, type). type == nullptr -> an untyped
 * hex word (the cc65 path): `width` bytes wide (1/2/4), defaulting to 2. */
dap::Variable make_var(const std::string &name, uint16_t addr, bool addr_ok, const dtype_t *t,
                       int width = 0)
{
    dap::Variable v;
    v.name = name;
    v.variablesReference = 0;
    if (t) v.type = dwarf_type_name(t);
    v.memoryReference = hex16(addr);
    if (!addr_ok) { v.value = "<optimized out>"; return v; }
    if (!t)
    {
        int w = (width == 1 || width == 2 || width == 4) ? width : 2;
        const char *fmt = w == 1 ? "0x%02X" : w == 4 ? "0x%08X" : "0x%04X";
        char b[24];
        snprintf(b, sizeof b, fmt, (unsigned)mem_le(addr, w));
        v.value = b;
        return v;
    }
    switch (dwarf_type_kind(t))
    {
    case DW_KIND_BASE: v.value = base_value(addr, t); break;
    case DW_KIND_ENUM:
    {
        int sz = (int)dwarf_type_size(t);
        if (sz <= 0) sz = 2;
        int64_t val = (int64_t)mem_le(addr, sz);
        const char *en = dwarf_type_enum_name(t, val);
        char b[48];
        if (en) { snprintf(b, sizeof b, " (%lld)", (long long)val); v.value = std::string(en) + b; }
        else { snprintf(b, sizeof b, "%lld", (long long)val); v.value = b; }
        break;
    }
    case DW_KIND_POINTER:
    {
        uint16_t tgt = (uint16_t)mem_le(addr, 2);
        char b[24];
        snprintf(b, sizeof b, "0x%04X", tgt);
        v.value = b;
        if (is_char_type(dwarf_type_pointee(t)) && tgt)
            v.value += " " + str_repr(tgt, 64);
        break;
    }
    case DW_KIND_ARRAY:
    {
        uint32_t c;
        const dtype_t *e = dwarf_type_element(t, &c);
        v.value = is_char_type(e) ? str_repr(addr, (int)c) : std::string(dwarf_type_name(t));
        break;
    }
    case DW_KIND_STRUCT:
    case DW_KIND_UNION: v.value = dwarf_type_name(t); break;
    default:
    {
        char b[24];
        snprintf(b, sizeof b, "0x%04X", (unsigned)mem_le(addr, 2));
        v.value = b;
        break;
    }
    }
    if (type_expandable(t))
        v.variablesReference = var_ref(addr, t);
    return v;
}

std::vector<dap::Variable> expand_node(VarNode n)
{
    std::vector<dap::Variable> out;
    const dtype_t *t = n.type;
    if (!t) return out;
    switch (dwarf_type_kind(t))
    {
    case DW_KIND_ARRAY:
    {
        uint32_t c;
        const dtype_t *e = dwarf_type_element(t, &c);
        uint32_t es = e ? dwarf_type_size(e) : 1;
        if (!es) es = 1;
        uint32_t lim = c > 256 ? 256 : c;
        for (uint32_t i = 0; i < lim; i++)
        {
            char nm[24];
            snprintf(nm, sizeof nm, "[%u]", i);
            out.push_back(make_var(nm, (uint16_t)(n.addr + i * es), true, e));
        }
        if (c > lim)
        {
            dap::Variable more;
            more.name = "...";
            char b[32];
            snprintf(b, sizeof b, "%u more", c - lim);
            more.value = b;
            more.variablesReference = 0;
            out.push_back(more);
        }
        break;
    }
    case DW_KIND_STRUCT:
    case DW_KIND_UNION:
    {
        int m = dwarf_type_member_count(t);
        for (int i = 0; i < m; i++)
        {
            const char *nm;
            uint32_t off;
            const dtype_t *mt;
            if (dwarf_type_member(t, i, &nm, &off, &mt))
                out.push_back(make_var(nm ? nm : "", (uint16_t)(n.addr + off), true, mt));
        }
        break;
    }
    case DW_KIND_POINTER:
    {
        uint16_t tgt = (uint16_t)mem_le(n.addr, 2);
        out.push_back(make_var("*", tgt, true, dwarf_type_pointee(t)));
        break;
    }
    default: break;
    }
    return out;
}

/* ---- expression evaluation --------------------------------------------------
 * A small C-ish evaluator shared by EvaluateRequest (watch/hover/repl),
 * SetVariable/SetExpression (target + RHS), and breakpoint conditions. Reuses the
 * DWARF/cc65 symbol resolvers, the dtype_t graph, and make_var() for formatting.
 * Reads ram[] + the source map, so reader-thread callers hold g_src_mtx; the
 * main-thread condition filter is the sole writer and needs no lock. cc65 has no
 * type graph, so member/typed-index there report an honest error. */

struct EvalResult
{
    bool ok = false;
    std::string err;
    bool lvalue = false;           /* addr/type/width describe a memory object */
    uint16_t addr = 0;
    bool addr_ok = true;           /* false -> register-resident / <optimized out> */
    const dtype_t *type = nullptr; /* null on the cc65 (untyped) path */
    int width = 2;                 /* byte width when type == null */
    int64_t ival = 0;              /* value of a computed rvalue */
    bool has_ival = false;         /* true -> rvalue, not a memory lvalue */
};

EvalResult ev_rvalue(int64_t v)
{
    EvalResult r;
    r.ok = true;
    r.has_ival = true;
    r.ival = v;
    r.width = 2;
    return r;
}

int64_t load_scalar(const EvalResult &e)
{
    if (e.has_ival) return e.ival;
    if (!e.addr_ok) return 0;
    int sz = e.type ? (int)dwarf_type_size(e.type) : e.width;
    if (sz <= 0 || sz > 8) sz = 2;
    uint64_t raw = mem_le(e.addr, sz);
    if (e.type && dwarf_type_kind(e.type) == DW_KIND_BASE)
    {
        int enc = dwarf_type_encoding(e.type);
        if (enc == DW_ATE_signed || enc == DW_ATE_signed_char)
            return sign_extend(raw, sz);
    }
    return (int64_t)raw;
}

/* Write `sz` little-endian bytes of value into 6502 memory (regs alias ram). */
void store_scalar(uint16_t addr, int sz, int64_t value)
{
    if (sz <= 0 || sz > 8) sz = 2;
    for (int i = 0; i < sz; i++)
        ram[(uint16_t)(addr + i)] = (uint8_t)((uint64_t)value >> (8 * i));
}

bool resolve_ident(const std::string &name, const FrameCtx &fc, EvalResult &r)
{
    if (g_dinfo)
    {
        dwarf_var_t buf[512];
        uint16_t fb = fc.base;
        bool fb_ok = fc.have_base ? fc.base_ok
                                  : dwarf_info_frame_base(g_dinfo, fc.pc, dap_readmem, &fb);
        int n = dwarf_info_locals(g_dinfo, fc.pc, fb, fb_ok, buf, 512);
        for (int i = 0; i < n; i++)
            if (name == buf[i].name)
            {
                r.ok = r.lvalue = true;
                r.addr = buf[i].addr; r.addr_ok = buf[i].addr_ok; r.type = buf[i].type;
                return true;
            }
        int m = dwarf_info_globals(g_dinfo, buf, 512);
        for (int i = 0; i < m; i++)
            if (name == buf[i].name)
            {
                r.ok = r.lvalue = true;
                r.addr = buf[i].addr; r.addr_ok = buf[i].addr_ok; r.type = buf[i].type;
                return true;
            }
    }
    else if (g_cc65)
    {
        cc65var_t buf[512];
        uint16_t fb = fc.base;
        bool fb_ok = fc.have_base ? fc.base_ok
                                  : cc65dbg_frame_base(g_cc65, fc.pc, dap_readmem, &fb);
        int n = cc65dbg_locals(g_cc65, fc.pc, fb, fb_ok, buf, 512);
        for (int i = 0; i < n; i++)
            if (name == buf[i].name)
            {
                r.ok = r.lvalue = true;
                r.addr = buf[i].addr; r.addr_ok = buf[i].addr_ok;
                r.width = buf[i].size ? buf[i].size : 2;
                return true;
            }
        int m = cc65dbg_globals(g_cc65, buf, 512);
        for (int i = 0; i < m; i++)
            if (name == buf[i].name)
            {
                r.ok = r.lvalue = true;
                r.addr = buf[i].addr; r.addr_ok = buf[i].addr_ok;
                r.width = buf[i].size ? buf[i].size : 2;
                return true;
            }
    }
    r.err = "unknown identifier '" + name + "'";
    return false;
}

struct Eval
{
    const char *p;
    FrameCtx fc;
    int depth = 0;
    bool err = false;
    std::string errmsg;

    /* Bound native recursion: the descent re-enters through expr() (parentheses,
     * '[' index) and unary() (prefix operators), so a pathological expression like
     * "((((...))))" or "----x" would otherwise overflow the C stack. */
    static constexpr int MAX_DEPTH = 256;
    struct Depth
    {
        Eval &e;
        Depth(Eval &ev) : e(ev) { if (++e.depth > MAX_DEPTH) e.fail("expression nesting too deep"); }
        ~Depth() { --e.depth; }
    };

    static bool is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
    static bool is_dig(char c) { return c >= '0' && c <= '9'; }
    void skip() { while (*p == ' ' || *p == '\t') p++; }
    void fail(const std::string &m) { if (!err) { err = true; errmsg = m; } }

    std::string ident()
    {
        skip();
        std::string s;
        while (is_alpha(*p) || is_dig(*p)) s += *p++;
        return s;
    }

    EvalResult expr() { Depth d(*this); if (err) return EvalResult{}; return lor(); }

    EvalResult lor()
    {
        EvalResult a = land();
        for (;;)
        {
            skip();
            if (!(p[0] == '|' && p[1] == '|')) break;
            p += 2;
            EvalResult b = land();
            if (err) return a;
            a = ev_rvalue((load_scalar(a) != 0 || load_scalar(b) != 0) ? 1 : 0);
        }
        return a;
    }
    EvalResult land()
    {
        EvalResult a = equality();
        for (;;)
        {
            skip();
            if (!(p[0] == '&' && p[1] == '&')) break;
            p += 2;
            EvalResult b = equality();
            if (err) return a;
            a = ev_rvalue((load_scalar(a) != 0 && load_scalar(b) != 0) ? 1 : 0);
        }
        return a;
    }
    EvalResult equality()
    {
        EvalResult a = relational();
        for (;;)
        {
            skip();
            bool eq = (p[0] == '=' && p[1] == '=');
            bool ne = (p[0] == '!' && p[1] == '=');
            if (!eq && !ne) break;
            p += 2;
            EvalResult b = relational();
            if (err) return a;
            bool r = load_scalar(a) == load_scalar(b);
            a = ev_rvalue((eq ? r : !r) ? 1 : 0);
        }
        return a;
    }
    EvalResult relational()
    {
        EvalResult a = add();
        for (;;)
        {
            skip();
            char c0 = p[0], c1 = p[1];
            int op = 0; /* 1:<  2:>  3:<=  4:>= */
            if (c0 == '<' && c1 == '=') { op = 3; p += 2; }
            else if (c0 == '>' && c1 == '=') { op = 4; p += 2; }
            else if (c0 == '<') { op = 1; p++; }
            else if (c0 == '>') { op = 2; p++; }
            else break;
            EvalResult b = add();
            if (err) return a;
            int64_t x = load_scalar(a), y = load_scalar(b);
            bool r = op == 1 ? x < y : op == 2 ? x > y : op == 3 ? x <= y : x >= y;
            a = ev_rvalue(r ? 1 : 0);
        }
        return a;
    }

    EvalResult add()
    {
        EvalResult a = mul();
        for (;;)
        {
            skip();
            char op = *p;
            if (op != '+' && op != '-') break;
            p++;
            EvalResult b = mul();
            if (err) return a;
            uint64_t x = (uint64_t)load_scalar(a), y = (uint64_t)load_scalar(b);
            a = ev_rvalue((int64_t)(op == '+' ? x + y : x - y));
        }
        return a;
    }
    EvalResult mul()
    {
        EvalResult a = unary();
        for (;;)
        {
            skip();
            char op = *p;
            if (op != '*' && op != '/' && op != '%') break; /* prefix deref is in unary */
            p++;
            EvalResult b = unary();
            if (err) return a;
            int64_t x = load_scalar(a), y = load_scalar(b);
            int64_t res;
            if (op == '*')
                res = (int64_t)((uint64_t)x * (uint64_t)y);
            else if (y == 0)
                res = 0;                                  /* keep the existing "div by zero -> 0" */
            else if (x == INT64_MIN && y == -1)
                res = (op == '/') ? INT64_MIN : 0;        /* the one signed-overflow div/mod case */
            else
                res = (op == '/') ? x / y : x % y;
            a = ev_rvalue(res);
        }
        return a;
    }
    EvalResult unary()
    {
        Depth d(*this);
        if (err) return EvalResult{};
        skip();
        char c = *p;
        if (c == '-') { p++; return ev_rvalue((int64_t)(0 - (uint64_t)load_scalar(unary()))); }
        if (c == '+') { p++; return unary(); }
        if (c == '*') { p++; return deref(unary()); }
        if (c == '&')
        {
            p++;
            EvalResult v = unary();
            if (!v.lvalue) { fail("'&' needs an lvalue"); return v; }
            return ev_rvalue(v.addr);
        }
        return postfix();
    }
    EvalResult deref(EvalResult v)
    {
        if (err) return v;
        if (v.type)
        {
            dw_kind_t k = dwarf_type_kind(v.type);
            if (k == DW_KIND_POINTER)
            {
                EvalResult r; r.ok = r.lvalue = true;
                r.addr = (uint16_t)mem_le(v.addr, 2);
                r.type = dwarf_type_pointee(v.type);
                return r;
            }
            if (k == DW_KIND_ARRAY)
            {
                uint32_t c;
                EvalResult r; r.ok = r.lvalue = true; r.addr = v.addr;
                r.type = dwarf_type_element(v.type, &c);
                return r;
            }
            fail("cannot dereference"); return v;
        }
        EvalResult r; r.ok = r.lvalue = true; /* cc65: raw 16-bit pointer */
        r.addr = (uint16_t)mem_le(v.addr, 2); r.width = 2;
        return r;
    }
    EvalResult postfix()
    {
        EvalResult v = primary();
        for (;;)
        {
            skip();
            if (*p == '.') { p++; v = member(v, false); }
            else if (p[0] == '-' && p[1] == '>') { p += 2; v = member(v, true); }
            else if (*p == '[')
            {
                p++;
                EvalResult idx = expr();
                skip();
                if (*p == ']') p++; else fail("expected ']'");
                v = index(v, load_scalar(idx));
            }
            else break;
            if (err) break;
        }
        return v;
    }
    EvalResult member(EvalResult v, bool arrow)
    {
        std::string nm = ident();
        if (nm.empty()) { fail("expected member name"); return v; }
        if (!v.type) { fail("no type info (cc65 build)"); return v; }
        uint16_t base = v.addr;
        const dtype_t *st = v.type;
        if (arrow)
        {
            if (dwarf_type_kind(st) != DW_KIND_POINTER) { fail("'->' needs a pointer"); return v; }
            base = (uint16_t)mem_le(v.addr, 2);
            st = dwarf_type_pointee(st);
        }
        if (!st || (dwarf_type_kind(st) != DW_KIND_STRUCT && dwarf_type_kind(st) != DW_KIND_UNION))
        { fail("not a struct/union"); return v; }
        int mc = dwarf_type_member_count(st);
        for (int i = 0; i < mc; i++)
        {
            const char *mn; uint32_t off; const dtype_t *mt;
            if (dwarf_type_member(st, i, &mn, &off, &mt) && mn && nm == mn)
            {
                EvalResult r; r.ok = r.lvalue = true;
                r.addr = (uint16_t)(base + off); r.type = mt;
                return r;
            }
        }
        fail("no member '" + nm + "'"); return v;
    }
    EvalResult index(EvalResult v, int64_t i)
    {
        if (!v.type) { fail("no type info (cc65 build)"); return v; }
        dw_kind_t k = dwarf_type_kind(v.type);
        if (k == DW_KIND_ARRAY)
        {
            uint32_t c;
            const dtype_t *e = dwarf_type_element(v.type, &c);
            int es = e ? (int)dwarf_type_size(e) : 1; if (es <= 0) es = 1;
            EvalResult r; r.ok = r.lvalue = true;
            r.addr = (uint16_t)(v.addr + i * es); r.type = e;
            return r;
        }
        if (k == DW_KIND_POINTER)
        {
            const dtype_t *e = dwarf_type_pointee(v.type);
            int es = e ? (int)dwarf_type_size(e) : 1; if (es <= 0) es = 1;
            EvalResult r; r.ok = r.lvalue = true;
            r.addr = (uint16_t)((uint16_t)mem_le(v.addr, 2) + i * es); r.type = e;
            return r;
        }
        fail("not indexable"); return v;
    }
    EvalResult reg()
    {
        std::string nm = ident();
        w65c02_t *c = cpu();
        if (nm == "A") return ev_rvalue(w65c02_a(c));
        if (nm == "X") return ev_rvalue(w65c02_x(c));
        if (nm == "Y") return ev_rvalue(w65c02_y(c));
        if (nm == "S" || nm == "SP") return ev_rvalue(w65c02_s(c));
        if (nm == "P") return ev_rvalue(w65c02_p(c));
        if (nm == "PC") return ev_rvalue(w65c02_pc(c));
        fail("unknown register '$" + nm + "'");
        return EvalResult{};
    }
    EvalResult number()
    {
        char *end = nullptr;
        long long v = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
                          ? strtoll(p, &end, 16) : strtoll(p, &end, 10);
        if (end) p = end;
        return ev_rvalue(v);
    }
    EvalResult primary()
    {
        skip();
        char c = *p;
        if (c == '(')
        {
            p++;
            EvalResult v = expr();
            skip();
            if (*p == ')') p++; else fail("expected ')'");
            return v;
        }
        if (c == '$') { p++; return reg(); }
        if (is_dig(c)) return number();
        if (is_alpha(c))
        {
            std::string nm = ident();
            EvalResult r;
            if (!resolve_ident(nm, fc, r)) fail(r.err);
            return r;
        }
        fail(std::string("unexpected '") + (c ? c : '?') + "'");
        return EvalResult{};
    }
};

/* Evaluate expr in the given frame context. On success .ok is set; else .err
 * holds a message. */
EvalResult eval_expr_at(const char *expr, const FrameCtx &fc)
{
    Eval e;
    e.p = expr;
    e.fc = fc;
    EvalResult r = e.expr();
    e.skip();
    if (!e.err && *e.p) e.fail("trailing input");
    if (e.err) { EvalResult bad; bad.ok = false; bad.err = e.errmsg; return bad; }
    return r;
}

/* Evaluate expr at the innermost frame's pc (live base). */
EvalResult eval_expr(const char *expr, uint16_t pc)
{
    FrameCtx fc;
    fc.pc = pc;
    return eval_expr_at(expr, fc);
}

/* Resolve an expanded aggregate child (a g_varnodes entry + a DAP child name like
 * "[3]", "member", or "*") to its lvalue, so SetVariable can write it. */
EvalResult resolve_child(const VarNode &pn, const std::string &name)
{
    EvalResult base;
    base.ok = base.lvalue = true;
    base.addr = pn.addr;
    base.type = pn.type;
    Eval e;
    e.p = "";
    e.fc.pc = dbg_stop_pc();
    if (!name.empty() && name[0] == '[')
        return e.index(base, strtoll(name.c_str() + 1, nullptr, 10));
    if (name == "*")
        return e.deref(base);
    e.p = name.c_str();
    return e.member(base, false);
}

std::vector<uint8_t> b64decode(const std::string &s)
{
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    unsigned buf = 0, bits = 0;
    for (char c : s)
    {
        int v = val(c);
        if (v < 0) continue;
        buf = ((buf << 6) | (unsigned)v) & 0xFFFFu; /* keep only the pending bits */
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)((buf >> bits) & 0xFF)); }
    }
    return out;
}

/* Parse a DAP hitCondition ("> 5", ">=5", "==5", "%3", or bare "5" => >=) into m. */
void parse_hit(const std::string &s, BpMeta &m)
{
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= s.size()) { m.hitOp = BpMeta::HIT_NONE; return; }
    BpMeta::HitOp op = BpMeta::HIT_GE; /* bare N => >= */
    if (s.compare(i, 2, ">=") == 0) { op = BpMeta::HIT_GE; i += 2; }
    else if (s.compare(i, 2, "<=") == 0) { op = BpMeta::HIT_LE; i += 2; }
    else if (s.compare(i, 2, "==") == 0) { op = BpMeta::HIT_EQ; i += 2; }
    else if (s[i] == '>') { op = BpMeta::HIT_GT; i++; }
    else if (s[i] == '<') { op = BpMeta::HIT_LT; i++; }
    else if (s[i] == '=') { op = BpMeta::HIT_EQ; i++; }
    else if (s[i] == '%') { op = BpMeta::HIT_MULT; i++; }
    m.hitOp = op;
    m.hitN = strtol(s.c_str() + i, nullptr, 0);
}

/* Hit-count test, after the condition held and m.hits was bumped. */
bool hit_satisfied(const BpMeta &m)
{
    long h = (long)m.hits, n = m.hitN;
    switch (m.hitOp)
    {
    case BpMeta::HIT_EQ:   return h == n;
    case BpMeta::HIT_GE:   return h >= n;
    case BpMeta::HIT_GT:   return h > n;
    case BpMeta::HIT_LT:   return h < n;
    case BpMeta::HIT_LE:   return h <= n;
    case BpMeta::HIT_MULT: return n > 0 && (h % n) == 0;
    default:               return true;
    }
}

/* Splice {expr} occurrences in a logpoint message via the evaluator. */
std::string interp_log(const std::string &msg, uint16_t pc)
{
    std::string out;
    for (size_t i = 0; i < msg.size();)
    {
        if (msg[i] != '{') { out += msg[i++]; continue; }
        size_t e = msg.find('}', i);
        if (e == std::string::npos) { out += msg.substr(i); break; }
        std::string ex = msg.substr(i + 1, e - i - 1);
        EvalResult r = eval_expr(ex.c_str(), pc);
        if (!r.ok) out += "{" + ex + "?}";
        else if (r.lvalue) out += make_var("", r.addr, r.addr_ok, r.type, r.width).value;
        else out += std::to_string(r.ival);
        i = e + 1;
    }
    return out;
}

/* Main-thread gate registered via dbg_set_break_filter: a breakpoint's bitmap bit
 * matched — honor its condition, hit-count, and logpoint. Consulted only for
 * addresses carrying metadata (g_bp_meta is sparse), so plain breakpoints stay
 * O(1). Runs while the CPU is live at an instruction boundary, so eval reads the
 * about-to-execute frame/registers directly (no lock: main thread is sole writer). */
bool bp_filter(uint16_t pc)
{
    auto it = g_bp_meta.find(pc);
    if (it == g_bp_meta.end())
        return true; /* plain breakpoint */
    BpMeta &m = it->second;
    if (!m.condition.empty())
    {
        EvalResult e = eval_expr(m.condition.c_str(), pc);
        if (!e.ok || load_scalar(e) == 0)
            return false; /* condition unmet/erroring -> don't count the hit, keep running */
    }
    m.hits++;
    if (!hit_satisfied(m))
        return false;
    if (!m.logMessage.empty())
    {
        if (g_session)
        {
            dap::OutputEvent ev;
            ev.category = "console";
            ev.output = interp_log(m.logMessage, pc) + "\n";
            g_session->send(ev);
        }
        return false; /* logpoint: logged, keep running */
    }
    return true;
}

/* Runs on the main thread mid-cycle; the store has already landed, so we latch a
 * stop for the next instruction boundary (standard watchpoint semantics). */
void dbg_watch_on_access(uint16_t addr, uint8_t val, bool is_write)
{
    (void)val;
    for (const Watch &w : g_watches)
    {
        if (is_write ? !w.on_write : !w.on_read) continue;
        if (addr < w.addr || addr >= (uint32_t)w.addr + w.width) continue;
        dbg_note_data_stop(addr);
        return;
    }
}

} // namespace

extern "C" void dap_set_default_args(int argc, char **argv)
{
    g_default_args.assign(argv, argv + argc);
}

extern "C" bool dap_is_active(void)
{
    return g_session != nullptr; /* only dap_start() creates it; plain --debug never does */
}

extern "C" void dap_start(void)
{
    dbg_set_stopped_cb(on_stopped);
    dbg_set_line_lookup(line_lookup);
    dbg_set_break_filter(bp_filter);
    dbg_set_watch_cb(dbg_watch_on_access);
    com_set_tx_tap(stdout_tap);

    g_session = dap::Session::create();

    g_session->registerHandler([](const dap::InitializeRequest &) {
        dap::InitializeResponse r;
        r.supportsConfigurationDoneRequest = true;
        r.supportsReadMemoryRequest = true;
        r.supportsDisassembleRequest = true;
        r.supportsInstructionBreakpoints = true;
        r.supportsSteppingGranularity = true;
        r.supportsDelayedStackTraceLoading = true;
        r.supportsEvaluateForHovers = true;
        r.supportsSetVariable = true;
        r.supportsSetExpression = true;
        r.supportsWriteMemoryRequest = true;
        r.supportsConditionalBreakpoints = true;
        r.supportsHitConditionalBreakpoints = true;
        r.supportsLogPoints = true;
        r.supportsDataBreakpoints = true;
        r.supportsFunctionBreakpoints = true;
        /* Variable.type + Variable.memoryReference are gated on CLIENT caps
         * (supportsVariableType / supportsMemoryReferences), which VS Code sends;
         * the adapter need not advertise anything for them. */
        return r;
    });
    g_session->registerSentHandler(
        [](const dap::ResponseOrError<dap::InitializeResponse> &) {
            g_session->send(dap::InitializedEvent());
        });

    g_session->registerHandler([](const dap::RP6502LaunchRequest &req) {
        std::string program = req.program.value("");
        /* Request args are UTF-8 JSON and convert here; the --dap command-line
         * defaults arrived already OEM (main.c converted its argv), so they
         * pass through — converting them again would mangle high bytes. */
        std::vector<std::string> args;
        for (const std::string &a : req.args.value({}))
            args.push_back(oem_from_utf8_str(a));
        if (args.empty())
            args = g_default_args;
        std::string elf = req.elf.value("");
        std::string dbg = req.dbg.value("");
        bool soe = req.stopOnEntry.value(false);
        bool sox = req.stopOnExit.value(true);

        post([program, args, soe, sox, elf, dbg]() {
            /* Debug-info (g_cc65/g_dwarf/g_dinfo/g_segments) and CPU state are owned
             * by this main/emulation thread; the loads and segment push run here,
             * never on the cppdap reader thread that delivered the request. g_src_mtx
             * fences the free+reload so a reader-thread handler can't observe a
             * half-freed/half-loaded map. */
            {
                std::lock_guard<std::mutex> lk(g_src_mtx);
                src_free();
                std::string base = program; /* program path with a trailing ".rp6502" removed */
                const std::string ext = ".rp6502";
                if (base.size() > ext.size() &&
                    base.compare(base.size() - ext.size(), ext.size(), ext) == 0)
                    base = base.substr(0, base.size() - ext.size());
                if (!dbg.empty())
                {
                    /* cc65 toolchain: an ld65 --dbgfile (cc65 emits no DWARF). */
                    g_cc65 = cc65dbg_load(dbg.c_str());
                }
                else if (!elf.empty())
                {
                    g_dwarf = dwarf_line_load(elf.c_str()); /* NULL -> source mapping off */
                    g_dinfo = dwarf_info_load(elf.c_str()); /* NULL -> variables off */
                    g_dframe = dwarf_frame_load(elf.c_str()); /* NULL -> heuristic unwind */
                }
                else
                {
                    /* Neither field given: prefer a cc65 ".dbg" companion beside the
                     * program (the ld65 --dbgfile the cc65 template ships), else the
                     * llvm-mos ELF's DWARF at the same base. */
                    std::string cand = base + ".dbg";
                    g_cc65 = cc65dbg_load(cand.c_str());
                    if (!g_cc65)
                    {
                        g_dwarf = dwarf_line_load(base.c_str());
                        g_dinfo = dwarf_info_load(base.c_str());
                        g_dframe = dwarf_frame_load(base.c_str());
                    }
                }
                push_segments(); /* feed the memory map the program's segment sizes */
            }

            g_stop_on_entry = soe;
            g_stop_on_exit = sox;
            g_reached_entry = false;
            g_launch_done = false;
            g_terminated = false;
            dbg_clear_breakpoints();
            g_instr_bps.clear();
            g_src_bps.clear();
            g_bp_meta.clear();
            g_func_bps.clear();
            g_watches.clear();
            dbg_watch_armed = 0;
            dbg_stop_at_entry();   /* hold at the first instruction for config */
            if (!program.empty())
            {
                /* The .dbg/ELF companions above open host-side (UTF-8 paths);
                 * program crosses into the guest here, so it goes OEM (args
                 * were converted at the request boundary). */
                std::string prog_oem = oem_from_utf8_str(program);
                std::vector<char *> argv;
                for (const std::string &a : args)
                    argv.push_back(const_cast<char *>(a.c_str()));
                if (!pro_set_argv(prog_oem.c_str(), (int)argv.size(), argv.data()))
                {
                    /* Args over the 512-byte argv buffer. The response already
                     * went out, so run anyway — but with argv[0] intact (the
                     * re-exec invariant) and the failure in the Debug Console. */
                    pro_set_argv(prog_oem.c_str(), 0, NULL);
                    dap::OutputEvent ev;
                    ev.category = "console";
                    ev.output = "rp6502-emu: ROM argv overflow; launch args dropped\n";
                    g_session->send(ev);
                }
                pro_exec(prog_oem.c_str());
            }
            g_launch_requested = true; /* dap_pump can now detect a load that never started */
        });
        return dap::LaunchResponse();
    });

    g_session->registerHandler([](const dap::SetBreakpointsRequest &req,
                                  std::function<void(dap::SetBreakpointsResponse)> respond) {
        std::string path = req.source.path.value("");
        auto sbs = req.breakpoints.value({});
        /* Resolve on the main thread: the post queue is FIFO, so a launch
         * queued ahead of us has loaded the source map before the lookup runs.
         * cppdap frees req when this handler returns (hence the copies);
         * respond() is thread-safe and may fire after we return. */
        post([path, sbs, respond]() {
            dap::SetBreakpointsResponse r;
            std::vector<SrcBp> bps;
            for (auto &sb : sbs)
            {
                dap::Breakpoint ob;
                uint16_t addr = 0;
                int bound = 0;
                if (src_line_to_addr(path.c_str(), (int)sb.line, &addr, &bound))
                {
                    ob.verified = true;
                    ob.line = bound;
                    ob.instructionReference = hex16(addr);
                    SrcBp bp;
                    bp.addr = addr;
                    bp.meta.condition = sb.condition.value("");
                    bp.meta.logMessage = sb.logMessage.value("");
                    parse_hit(sb.hitCondition.value(""), bp.meta);
                    bps.push_back(bp);
                }
                else
                {
                    ob.verified = false;
                    ob.line = sb.line;
                }
                r.breakpoints.push_back(ob);
            }
            /* DAP replace-semantics, per source file: swap this file's address set. */
            auto it = g_src_bps.find(path);
            if (it != g_src_bps.end())
                for (const SrcBp &sb : it->second)
                    if (!bp_referenced(sb.addr, &path, true)) /* keep if instr/another file wants it */
                        dbg_remove_breakpoint(sb.addr);
            g_src_bps[path] = bps;
            for (const SrcBp &sb : bps)
                dbg_add_breakpoint(sb.addr);
            rebuild_bp_meta();
            respond(r);
        });
    });

    g_session->registerHandler([](const dap::ConfigurationDoneRequest &) {
        g_configured.store(true);
        return dap::ConfigurationDoneResponse();
    });

    g_session->registerHandler([](const dap::DisconnectRequest &) {
        post([]() { dbg_continue(); });
        g_quit.store(true); /* the window loop closes the app (see dap_quit_requested) */
        return dap::DisconnectResponse();
    });

    g_session->registerHandler([](const dap::ThreadsRequest &) {
        dap::ThreadsResponse r;
        dap::Thread t;
        t.id = 1;
        t.name = "6502";
        r.threads.push_back(t);
        return r;
    });

    g_session->registerHandler([](const dap::StackTraceRequest &req) {
        dap::StackTraceResponse r;
        /* Invalidate expandable-variable refs only on a genuinely new stop — VS Code
         * issues several StackTrace requests per stop when paging the call stack
         * (supportsDelayedStackTraceLoading), and refs already handed to the client
         * must keep resolving across those pages. g_frames is always rebuilt (cheap,
         * deterministic). */
        if (g_varnodes_gen != g_stop_gen)
        {
            g_varnodes.clear();
            g_varnodes_gen = g_stop_gen;
        }
        g_frames.clear();
        {
            /* file/fn/unwind all read the source map; hold g_src_mtx so a
             * concurrent (re)load on the main thread can't free it mid-use. */
            std::lock_guard<std::mutex> lk(g_src_mtx);
            uint16_t pc0 = dbg_stop_pc();
            if (g_dframe && dwarf_frame_has(g_dframe, pc0))
                unwind_stack_cfi(pc0); /* exact CFI unwind (llvm-mos DWARF5) */
            else
            {
                /* cc65, no .debug_frame, or a pc no FDE covers: heuristic scan. */
                g_frames.push_back({pc0, pc0});
                unwind_stack();
                compute_frame_bases();
            }

            /* Return the requested window; g_frames holds all frames so Scopes
             * can select any frameId. */
            int64_t start = req.startFrame.value(0);
            if (start < 0) start = 0;
            int64_t levels = req.levels.value(0);
            int64_t end = (levels > 0) ? start + levels : (int64_t)g_frames.size();
            if (end > (int64_t)g_frames.size()) end = (int64_t)g_frames.size();
            for (int64_t i = start; i < end; i++)
            {
                dap::StackFrame f;
                f.id = i;
                f.instructionPointerReference = hex16(g_frames[(size_t)i].ip);
                f.line = 0;
                f.column = 0;
                const char *file = nullptr;
                int line = 0;
                if (src_addr_to_line(g_frames[(size_t)i].src_pc, &file, &line))
                {
                    dap::Source src;
                    src.path = file;
                    src.name = base_name(file);
                    f.source = src;
                    f.line = line;
                    f.column = 1;
                }
                const char *fn = src_addr_to_func(g_frames[(size_t)i].src_pc);
                if (fn)
                    f.name = fn;
                else
                {
                    char name[16];
                    snprintf(name, sizeof name, "$%04X", g_frames[(size_t)i].ip);
                    f.name = name;
                }
                r.stackFrames.push_back(f);
            }
        }
        r.totalFrames = (int)g_frames.size();
        return r;
    });

    g_session->registerHandler([](const dap::ScopesRequest &req) {
        dap::ScopesResponse r;
        bool have_map;
        {
            std::lock_guard<std::mutex> lk(g_src_mtx);
            have_map = (g_dinfo || g_cc65);
        }
        /* Locals apply to any frame (the Variables handler resolves them against
         * that frame's reconstructed base, marking unresolvable ones unavailable).
         * Globals are frame-independent (fixed addresses). Registers are the live
         * top frame only — a caller's registers aren't recoverable without CFI. */
        bool valid_frame = req.frameId >= 0 && (size_t)req.frameId < g_frames.size();
        if (have_map && valid_frame)
        {
            dap::Scope loc;
            loc.name = "Locals";
            loc.variablesReference = LOCALS_REF_BASE + req.frameId;
            loc.expensive = false;
            r.scopes.push_back(loc);
        }
        if (have_map)
        {
            dap::Scope glb;
            glb.name = "Globals";
            glb.variablesReference = 3;
            glb.expensive = false;
            r.scopes.push_back(glb);
        }
        if (req.frameId == 0)
        {
            dap::Scope reg;
            reg.name = "Registers";
            reg.presentationHint = "registers";
            reg.variablesReference = 1;
            reg.expensive = false;
            r.scopes.push_back(reg);
        }
        return r;
    });

    g_session->registerHandler([](const dap::VariablesRequest &req) {
        dap::VariablesResponse r;
        int64_t ref = req.variablesReference;
        if (ref == 1)
        {
            /* Registers. */
            w65c02_t *c = cpu();
            auto add = [&](const char *nm, unsigned val, int width) {
                dap::Variable v;
                v.name = nm;
                char b[16];
                snprintf(b, sizeof b, width == 2 ? "$%04X" : "$%02X", val);
                v.value = b;
                v.variablesReference = 0;
                r.variables.push_back(v);
            };
            add("A", w65c02_a(c), 1);
            add("X", w65c02_x(c), 1);
            add("Y", w65c02_y(c), 1);
            add("SP", w65c02_s(c), 1);
            add("PC", w65c02_pc(c), 2);
            add("P", w65c02_p(c), 1);
        }
        else if (ref >= LOCALS_REF_BASE && ref < LOCALS_REF_BASE + LOCALS_REF_SPAN)
        {
            /* Locals of the selected frame, resolved at that frame's call-site PC
             * against its reconstructed base. The resolvers read the source/type
             * map and return name pointers into it; fence against a concurrent
             * (re)load on the main thread. */
            size_t fi = (size_t)(ref - LOCALS_REF_BASE);
            std::lock_guard<std::mutex> lk(g_src_mtx);
            if (fi < g_frames.size())
            {
                uint16_t pc = g_frames[fi].src_pc;
                uint16_t base = g_frames[fi].base;
                bool base_ok = g_frames[fi].base_ok;
                if (g_dinfo)
                {
                    dwarf_var_t buf[256];
                    int n = dwarf_info_locals(g_dinfo, pc, base, base_ok, buf, 256);
                    for (int i = 0; i < n; i++)
                        r.variables.push_back(make_var(buf[i].name, buf[i].addr, buf[i].addr_ok, buf[i].type));
                }
                else if (g_cc65)
                {
                    cc65var_t buf[256];
                    int n = cc65dbg_locals(g_cc65, pc, base, base_ok, buf, 256);
                    for (int i = 0; i < n; i++)
                        r.variables.push_back(make_var(buf[i].name, buf[i].addr, buf[i].addr_ok, nullptr, buf[i].size));
                }
            }
        }
        else if (ref == 3)
        {
            /* Global / file-static variables. */
            std::lock_guard<std::mutex> lk(g_src_mtx);
            if (g_dinfo)
            {
                dwarf_var_t buf[512];
                int n = dwarf_info_globals(g_dinfo, buf, 512);
                for (int i = 0; i < n; i++)
                    r.variables.push_back(make_var(buf[i].name, buf[i].addr, buf[i].addr_ok, buf[i].type));
            }
            else if (g_cc65)
            {
                cc65var_t buf[512];
                int n = cc65dbg_globals(g_cc65, buf, 512);
                for (int i = 0; i < n; i++)
                    r.variables.push_back(make_var(buf[i].name, buf[i].addr, buf[i].addr_ok, nullptr, buf[i].size));
            }
        }
        else if (ref >= 1000)
        {
            /* Expand a previously emitted aggregate/pointer node. The node holds a
             * dtype_t* into g_dinfo, so fence against a concurrent (re)load that
             * frees the type graph — like the ref==2/3 branches above. */
            std::lock_guard<std::mutex> lk(g_src_mtx);
            size_t idx = (size_t)(ref - 1000);
            if (g_dinfo && idx < g_varnodes.size())
                r.variables = expand_node(g_varnodes[idx]);
        }
        return r;
    });

    g_session->registerHandler([](const dap::ContinueRequest &) {
        post([]() { dbg_continue(); });
        dap::ContinueResponse r;
        r.allThreadsContinued = true;
        return r;
    });
    g_session->registerHandler([](const dap::PauseRequest &) {
        dbg_request_pause(); /* atomic; safe from the reader thread */
        return dap::PauseResponse();
    });
    /* Stepping granularity: "instruction" (the Disassembly view) -> one machine
     * instruction; otherwise source-line over/into/out. */
    g_session->registerHandler([](const dap::NextRequest &req) {
        bool insn = req.granularity.value("") == "instruction";
        post([insn]() { dbg_step(insn ? DBG_STEP_INSTR : DBG_STEP_LINE_OVER); });
        return dap::NextResponse();
    });
    g_session->registerHandler([](const dap::StepInRequest &req) {
        bool insn = req.granularity.value("") == "instruction";
        post([insn]() { dbg_step(insn ? DBG_STEP_INSTR : DBG_STEP_LINE_INTO); });
        return dap::StepInResponse();
    });
    g_session->registerHandler([](const dap::StepOutRequest &) {
        post([]() { dbg_step(DBG_STEP_LINE_OUT); });
        return dap::StepOutResponse();
    });

    g_session->registerHandler([](const dap::SetInstructionBreakpointsRequest &req) {
        dap::SetInstructionBreakpointsResponse r;
        /* DAP replace-semantics: this request carries the full instruction-bp set. */
        auto addrs = std::make_shared<std::vector<uint16_t>>();
        for (auto &bp : req.breakpoints)
        {
            long a = (long)parse_addr(bp.instructionReference);
            if (bp.offset.has_value())
                a += bp.offset.value();
            uint16_t addr = (uint16_t)(a & 0xFFFF);
            addrs->push_back(addr);
            dap::Breakpoint ob;
            ob.verified = true;
            ob.instructionReference = hex16(addr);
            r.breakpoints.push_back(ob);
        }
        post([addrs]() {
            for (uint16_t a : g_instr_bps)
                if (!bp_referenced(a, nullptr, false)) /* keep if a source file wants it */
                    dbg_remove_breakpoint(a);
            g_instr_bps = *addrs;
            for (uint16_t a : g_instr_bps)
                dbg_add_breakpoint(a);
        });
        return r;
    });

    g_session->registerHandler([](const dap::ReadMemoryRequest &req) {
        dap::ReadMemoryResponse r;
        long base = (long)parse_addr(req.memoryReference);
        if (req.offset.has_value())
            base += req.offset.value();
        long count = req.count;
        if (count < 0)
            count = 0;
        std::vector<uint8_t> buf;
        buf.reserve((size_t)count);
        for (long i = 0; i < count; i++)
        {
            long a = base + i;
            buf.push_back((a >= 0 && a <= 0xFFFF) ? ram[a] : 0);
        }
        r.address = hex16((uint16_t)(base & 0xFFFF));
        if (!buf.empty())
            r.data = b64(buf.data(), buf.size());
        return r;
    });

    g_session->registerHandler([](const dap::DisassembleRequest &req) {
        dap::DisassembleResponse r;
        long base = (long)parse_addr(req.memoryReference);
        if (req.offset.has_value())
            base += req.offset.value();
        long want = req.instructionCount;
        if (want < 0)
            want = 0;
        uint16_t pc = (uint16_t)(base & 0xFFFF);
        long ioff = req.instructionOffset.has_value() ? (long)req.instructionOffset.value() : 0;
        long lead_pad = 0; /* placeholder rows before base when the back-decode can't align */
        if (ioff < 0)
        {
            // No backward decode on the variable-length 65C02: scan candidate
            // starts back from base and keep the one whose forward decode lands
            // on base after exactly n instructions.
            long n = -ioff;
            uint16_t tgt = (uint16_t)(base & 0xFFFF);
            uint16_t best = tgt;
            bool aligned = false;
            for (long back = n * 3; back >= 1; back--)
            {
                uint16_t p = (uint16_t)((base - back) & 0xFFFF);
                long k = 0;
                while (k < n && p != tgt)
                {
                    DasmCtx c;
                    c.pc = p;
                    p = w65c02dasm_op(p, dasm_in, dasm_out, &c);
                    k++;
                }
                if (p == tgt && k == n)
                {
                    best = (uint16_t)((base - back) & 0xFFFF);
                    aligned = true;
                    break;
                }
            }
            pc = best;
            // Couldn't decode n instructions before base: keep base at result
            // index n by padding the leading slots with placeholder rows, as the
            // DAP spec requires (rather than shifting every row up by n).
            if (!aligned)
                lead_pad = n;
        }
        else if (ioff > 0)
        {
            for (long k = 0; k < ioff; k++)
            {
                DasmCtx c;
                c.pc = pc;
                pc = w65c02dasm_op(pc, dasm_in, dasm_out, &c);
            }
        }
        for (long i = 0; i < lead_pad && (long)r.instructions.size() < want; i++)
        {
            uint16_t a = (uint16_t)((base - (lead_pad - i)) & 0xFFFF);
            dap::DisassembledInstruction di;
            di.address = hex16(a);
            di.instruction = "(unknown)";
            di.presentationHint = "invalid";
            r.instructions.push_back(di);
        }
        while ((long)r.instructions.size() < want)
        {
            DasmCtx ctx;
            ctx.pc = pc;
            uint16_t next = w65c02dasm_op(pc, dasm_in, dasm_out, &ctx);
            dap::DisassembledInstruction di;
            di.address = hex16(pc);
            di.instruction = ctx.text;
            di.instructionBytes = ctx.bytes;
            r.instructions.push_back(di);
            pc = next;
        }
        return r;
    });

    /* Expression evaluation (watch/hover/repl). Read-only; runs on the reader
     * thread under g_src_mtx like VariablesRequest — the CPU is idle while
     * stopped, so ram[]/registers are stable. */
    g_session->registerHandler(
        [](const dap::EvaluateRequest &req) -> dap::ResponseOrError<dap::EvaluateResponse> {
            std::lock_guard<std::mutex> lk(g_src_mtx);
            if (!g_dinfo && !g_cc65)
                return dap::Error("no debug info loaded");
            /* Evaluate in the client-selected frame (watch/hover on a caller frame),
             * not always the innermost one. */
            EvalResult e = eval_expr_at(req.expression.c_str(), frame_ctx(req.frameId.value(0)));
            if (!e.ok)
                return dap::Error(e.err);
            dap::EvaluateResponse r;
            if (e.lvalue)
            {
                dap::Variable v = make_var("", e.addr, e.addr_ok, e.type, e.width);
                r.result = v.value;
                r.type = v.type;
                r.variablesReference = v.variablesReference;
                r.memoryReference = v.memoryReference;
            }
            else
            {
                char b[48];
                snprintf(b, sizeof b, "%lld (0x%llX)", (long long)e.ival,
                         (unsigned long long)(uint64_t)e.ival);
                r.result = b;
                r.variablesReference = 0;
            }
            return r;
        });

    /* SetVariable / SetExpression / WriteMemory: writes to ram[]/registers. Safe
     * on the reader thread while stopped (CPU idle, no concurrent writer) — the
     * same invariant the reads rely on; take g_src_mtx for the resolvers. */
    g_session->registerHandler(
        [](const dap::SetVariableRequest &req) -> dap::ResponseOrError<dap::SetVariableResponse> {
            std::lock_guard<std::mutex> lk(g_src_mtx);
            int64_t ref = req.variablesReference;
            /* The Locals scope ref encodes the frame (LOCALS_REF_BASE + frameId);
             * resolve both the value expression and the target in that frame. */
            int64_t fid = (ref >= LOCALS_REF_BASE && ref < LOCALS_REF_BASE + LOCALS_REF_SPAN)
                              ? ref - LOCALS_REF_BASE : -1;
            FrameCtx fc = frame_ctx(fid);
            EvalResult rhs = eval_expr_at(req.value.c_str(), fc);
            if (!rhs.ok)
                return dap::Error(rhs.err);
            int64_t val = load_scalar(rhs);
            dap::SetVariableResponse r;
            if (ref == 1) /* Registers */
            {
                w65c02_t *c = cpu();
                const std::string &nm = req.name;
                if (nm == "A") w65c02_set_a(c, (uint8_t)val);
                else if (nm == "X") w65c02_set_x(c, (uint8_t)val);
                else if (nm == "Y") w65c02_set_y(c, (uint8_t)val);
                else if (nm == "SP" || nm == "S") w65c02_set_s(c, (uint8_t)val);
                else if (nm == "P") w65c02_set_p(c, (uint8_t)val);
                else if (nm == "PC") w65c02_set_pc(c, (uint16_t)val);
                else return dap::Error("unknown register '%s'", nm.c_str());
                char b[16];
                snprintf(b, sizeof b, nm == "PC" ? "$%04X" : "$%02X", (unsigned)val);
                r.value = b;
                return r;
            }
            EvalResult tgt;
            if (ref >= 1000)
            {
                size_t idx = (size_t)(ref - 1000);
                if (idx >= g_varnodes.size())
                    return dap::Error("stale variable reference");
                tgt = resolve_child(g_varnodes[idx], req.name);
            }
            else /* Locals (LOCALS_REF_BASE + frameId) / Globals (3) */
                resolve_ident(req.name, fc, tgt);
            if (!tgt.ok || !tgt.lvalue)
                return dap::Error(tgt.ok ? "not assignable" : tgt.err);
            if (!tgt.addr_ok)
                return dap::Error("value is not in memory");
            int w = tgt.type ? (int)dwarf_type_size(tgt.type) : tgt.width;
            store_scalar(tgt.addr, w, val);
            dap::Variable nv = make_var("", tgt.addr, true, tgt.type, tgt.width);
            r.value = nv.value;
            r.type = nv.type;
            r.variablesReference = nv.variablesReference;
            r.memoryReference = nv.memoryReference;
            return r;
        });

    g_session->registerHandler(
        [](const dap::SetExpressionRequest &req) -> dap::ResponseOrError<dap::SetExpressionResponse> {
            std::lock_guard<std::mutex> lk(g_src_mtx);
            FrameCtx fc = frame_ctx(req.frameId.value(0));
            EvalResult tgt = eval_expr_at(req.expression.c_str(), fc);
            if (!tgt.ok || !tgt.lvalue)
                return dap::Error(tgt.ok ? "expression is not assignable" : tgt.err);
            if (!tgt.addr_ok)
                return dap::Error("value is not in memory");
            EvalResult rhs = eval_expr_at(req.value.c_str(), fc);
            if (!rhs.ok)
                return dap::Error(rhs.err);
            int w = tgt.type ? (int)dwarf_type_size(tgt.type) : tgt.width;
            store_scalar(tgt.addr, w, load_scalar(rhs));
            dap::Variable nv = make_var("", tgt.addr, true, tgt.type, tgt.width);
            dap::SetExpressionResponse r;
            r.value = nv.value;
            r.type = nv.type;
            r.variablesReference = nv.variablesReference;
            r.memoryReference = nv.memoryReference;
            return r;
        });

    g_session->registerHandler([](const dap::WriteMemoryRequest &req) {
        dap::WriteMemoryResponse r;
        long base = (long)parse_addr(req.memoryReference);
        if (req.offset.has_value())
            base += req.offset.value();
        std::vector<uint8_t> bytes = b64decode(req.data);
        int written = 0;
        for (size_t i = 0; i < bytes.size(); i++)
        {
            long a = base + (long)i;
            if (a < 0 || a > 0xFFFF) break;
            ram[a] = bytes[i];
            written++;
        }
        r.bytesWritten = written;
        r.offset = 0;
        return r;
    });

    /* Data breakpoints (watchpoints). Info: resolve an expression/variable to an
     * address + width and mint a dataId. Read-only, reader thread under g_src_mtx. */
    g_session->registerHandler(
        [](const dap::DataBreakpointInfoRequest &req)
            -> dap::ResponseOrError<dap::DataBreakpointInfoResponse> {
            std::lock_guard<std::mutex> lk(g_src_mtx);
            dap::DataBreakpointInfoResponse r;
            EvalResult e;
            if (req.variablesReference.has_value() && req.variablesReference.value() >= 1000)
            {
                size_t idx = (size_t)(req.variablesReference.value() - 1000);
                if (idx < g_varnodes.size())
                    e = resolve_child(g_varnodes[idx], req.name);
            }
            else
            {
                /* The variable's Locals scope ref (if any) names its frame. */
                int64_t ref = req.variablesReference.value(-1);
                int64_t fid = (ref >= LOCALS_REF_BASE && ref < LOCALS_REF_BASE + LOCALS_REF_SPAN)
                                  ? ref - LOCALS_REF_BASE : -1;
                e = eval_expr_at(req.name.c_str(), frame_ctx(fid));
            }
            if (!e.ok || !e.lvalue || !e.addr_ok)
            {
                r.dataId = dap::null();
                r.description = e.ok ? "not a memory location" : e.err;
                return r;
            }
            int w = e.type ? (int)dwarf_type_size(e.type) : e.width;
            if (w <= 0 || w > 256) w = 1;
            char id[16];
            snprintf(id, sizeof id, "%04X:%d", e.addr, w);
            r.dataId = std::string(id);
            r.description = req.name;
            r.accessTypes = {std::string("read"), std::string("write"), std::string("readWrite")};
            r.canPersist = false;
            return r;
        });

    g_session->registerHandler([](const dap::SetDataBreakpointsRequest &req) {
        dap::SetDataBreakpointsResponse r;
        auto watches = std::make_shared<std::vector<Watch>>();
        for (auto &db : req.breakpoints)
        {
            dap::Breakpoint ob;
            unsigned addr = 0, width = 1;
            if (sscanf(db.dataId.c_str(), "%x:%u", &addr, &width) == 2)
            {
                Watch w;
                w.addr = (uint16_t)addr;
                w.width = (int)(width ? width : 1);
                std::string acc = db.accessType.value("write");
                w.on_write = (acc == "write" || acc == "readWrite");
                w.on_read = (acc == "read" || acc == "readWrite");
                watches->push_back(w);
                ob.verified = true;
            }
            else
                ob.verified = false;
            r.breakpoints.push_back(ob);
        }
        post([watches]() {
            g_watches = *watches;
            dbg_watch_armed = (int)g_watches.size();
        });
        return r;
    });

    /* Function breakpoints: resolve each name to its entry on the main thread
     * (FIFO after the launch that loads the map), then respond — same ordering as
     * SetBreakpoints, since these arrive during configuration before the map exists. */
    g_session->registerHandler(
        [](const dap::SetFunctionBreakpointsRequest &req,
           std::function<void(dap::SetFunctionBreakpointsResponse)> respond) {
            auto names = std::make_shared<std::vector<std::string>>();
            for (auto &fb : req.breakpoints)
                names->push_back(fb.name);
            post([names, respond]() {
                dap::SetFunctionBreakpointsResponse r;
                std::vector<uint16_t> addrs;
                for (const std::string &nm : *names)
                {
                    dap::Breakpoint ob;
                    uint16_t addr = 0;
                    if (src_func_addr(nm.c_str(), &addr))
                    {
                        ob.verified = true;
                        ob.instructionReference = hex16(addr);
                        addrs.push_back(addr);
                    }
                    else
                        ob.verified = false;
                    r.breakpoints.push_back(ob);
                }
                std::vector<uint16_t> old = g_func_bps;
                g_func_bps.clear(); /* clear first so bp_referenced excludes this category */
                for (uint16_t a : old)
                    if (!bp_referenced(a, nullptr, true)) /* keep if instr/source wants it */
                        dbg_remove_breakpoint(a);
                g_func_bps = addrs;
                for (uint16_t a : g_func_bps)
                    dbg_add_breakpoint(a);
                respond(r);
            });
        });

    /* Bind to the stdio DAP channel; cppdap spawns the reader thread. */
#ifdef _WIN32
    /* CRLF translation would corrupt the Content-Length framing. */
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    auto in = dap::file(stdin, false);
    auto out = dap::file(stdout, false);
    g_session->bind(in, out);
}

extern "C" void dap_pump(void)
{
    std::vector<std::function<void()>> work;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        work.swap(g_queue);
    }
    for (auto &fn : work)
        fn();

    /* Resolve the launch hold once the program has reached its entry point and
     * the client has finished configuring (breakpoints set): either present the
     * entry stop, or silently continue. */
    if (!g_launch_done && g_configured.load() && g_reached_entry)
    {
        g_launch_done = true;
        if (g_stop_on_entry)
            send_stopped(DBG_REASON_ENTRY);
        else
            dbg_continue();
    }

    /* A launched program that never reached its entry point: rom_load failed at the
     * frame commit (CPU left halted, entry stop unconsumed) or an empty program was
     * launched. Without this the session hangs — both the entry-resolve branch above
     * and the exit branch below wait on g_reached_entry / g_launch_done. Resolve the
     * launch so the exit branch announces/terminates. pro_exec_pending() excludes the
     * window between pro_exec() and its commit. */
    if (!g_launch_done && g_launch_requested && g_configured.load() &&
        !g_reached_entry && !pro_exec_pending() && cpu_halted() && !dbg_is_stopped())
    {
        g_launch_done = true;
        if (g_session)
        {
            dap::OutputEvent ev;
            ev.category = "console";
            ev.output = "rp6502-emu: program failed to start\n";
            g_session->send(ev);
        }
    }

    /* Program exit (once): either keep the session alive in a stopped state so
     * the final screen + machine state stay inspectable until the client
     * disconnects (stopOnExit, the default), or terminate the session. */
    if (!g_terminated && g_launch_done && cpu_halted() && !dbg_is_stopped())
    {
        g_terminated = true;
        if (!g_session)
            ; /* no client */
        else if (g_stop_on_exit)
        {
            g_stop_gen++; /* a new client-visible stop: stale last stop's var refs */
            dbg_note_stop(w65c02_pc(cpu())); /* present halt as a stop */
            dap::StoppedEvent ev;
            ev.reason = "exited";
            ev.description = "Program exited (code " + std::to_string(main_exit_code()) +
                             ") — press Stop to close";
            ev.threadId = 1;
            ev.allThreadsStopped = true;
            g_session->send(ev);
        }
        else
        {
            g_session->send(dap::TerminatedEvent());
            g_term_sent = true;
        }
    }
}

extern "C" bool dap_quit_requested(void) { return g_quit.load(); }

extern "C" void dap_stop(void)
{
    /* The window is going away with the client still attached (the user hit [X],
     * not VS Code's Stop): announce it so the session ends cleanly instead of the
     * client seeing a dropped pipe. send() flushes (cppdap's File::write). Skip
     * when the client itself asked to disconnect (g_quit) or a TerminatedEvent
     * already went out. */
    if (g_session && !g_quit.load() && !g_term_sent)
    {
        g_session->send(dap::TerminatedEvent());
        g_term_sent = true;
    }
    /* Do NOT destroy the session: its reader thread is parked in a blocking,
     * non-interruptible read on stdin (the pipe stays open while VS Code holds its
     * end), so cppdap's destructor join would hang teardown forever. Leak it — we
     * are exiting and the OS reclaims the threads and memory. */
    (void)g_session.release();
}
