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
#include "emu/dbg/dbg.h"
#include "emu/host/host.h"
#include "emu/sys/mem.h"
#include "emu/sys/sys.h"
#include "emu/dbg/dap.h"
#include "emu/dbg/dwarf_line.h"
#include "emu/dbg/dwarf_info.h"
#include "emu/dbg/cc65dbg.h"
}
#include "emu/sys/w65c02.h"          /* m6502_t register accessors (extern "C") */
#include "util/m6502dasm.h"  /* m6502dasm_op declaration (impl lives in dbgui.cc) */

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
    optional<string> elf;         /* companion ELF carrying the DWARF line table (llvm-mos) */
    optional<string> dbg;         /* companion cc65 .dbg file (cc65 has no DWARF) */
    optional<boolean> stopOnEntry;
    optional<boolean> stopOnExit; /* keep the session stopped (not terminated) on exit */
};
DAP_DECLARE_STRUCT_TYPEINFO(RP6502LaunchRequest);
DAP_IMPLEMENT_STRUCT_TYPEINFO_EXT(RP6502LaunchRequest, LaunchRequest, "launch",
                                  DAP_FIELD(program, "program"),
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

/* The program's source map: DWARF (.debug_line + .debug_info, llvm-mos ELF) or
 * cc65 (.dbg). Exactly one toolchain is loaded per session; the helpers below
 * dispatch to whichever. g_dinfo is the variable/type half of the DWARF map. */
dwarf_line_t *g_dwarf = nullptr;
dwarf_info_t *g_dinfo = nullptr;
cc65dbg_t *g_cc65 = nullptr;
std::map<std::string, std::vector<uint16_t>> g_src_bps; /* source breakpoints, per file */

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

void post(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_queue.push_back(std::move(fn));
}

m6502_t *cpu() { return (m6502_t *)sys_cpu(); }

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
    default:
        return "pause";
    }
}

void send_stopped(int reason)
{
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

/* Program console output -> the Debug Console (also still shown in the window). */
void stdout_tap(const char *buf, int len)
{
    if (!g_session)
        return;
    dap::OutputEvent ev;
    ev.category = "stdout";
    ev.output = std::string(buf, (size_t)len);
    g_session->send(ev);
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

/* One-instruction disassembly via m6502dasm, capturing text + raw bytes. */
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

uint64_t mem_le(uint16_t addr, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n && i < 8; i++)
        v |= (uint64_t)ram[(uint16_t)(addr + i)] << (8 * i);
    return v;
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
        int64_t s = (int64_t)raw;
        if (sz < 8 && (raw & ((uint64_t)1 << (sz * 8 - 1))))
            s = (int64_t)(raw | (~(uint64_t)0 << (sz * 8)));
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
 * 16-bit word (the cc65 path). */
dap::Variable make_var(const std::string &name, uint16_t addr, bool addr_ok, const dtype_t *t)
{
    dap::Variable v;
    v.name = name;
    v.variablesReference = 0;
    if (t) v.type = dwarf_type_name(t);
    v.memoryReference = hex16(addr);
    if (!addr_ok) { v.value = "<optimized out>"; return v; }
    if (!t)
    {
        char b[24];
        snprintf(b, sizeof b, "0x%04X", (unsigned)mem_le(addr, 2));
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

std::vector<dap::Variable> expand_node(const VarNode &n)
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

} // namespace

extern "C" void dap_start(void)
{
    dbg_set_stopped_cb(on_stopped);
    dbg_set_line_lookup(line_lookup);
    emu_set_stdout_tap(stdout_tap);

    g_session = dap::Session::create();

    g_session->registerHandler([](const dap::InitializeRequest &) {
        dap::InitializeResponse r;
        r.supportsConfigurationDoneRequest = true;
        r.supportsReadMemoryRequest = true;
        r.supportsDisassembleRequest = true;
        r.supportsInstructionBreakpoints = true;
        r.supportsSteppingGranularity = true;
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
        std::string elf = req.elf.value("");
        std::string dbg = req.dbg.value("");
        bool soe = req.stopOnEntry.value(false);
        bool sox = req.stopOnExit.value(true);

        src_free();
        if (!dbg.empty())
        {
            /* cc65 toolchain: an ld65 --dbgfile (cc65 emits no DWARF). */
            g_cc65 = cc65dbg_load(dbg.c_str());
        }
        else
        {
            /* llvm-mos: DWARF in the companion ELF; default it to the program
             * path with ".rp6502" removed (the cmake launch-target ELF). */
            if (elf.empty())
            {
                elf = program;
                const std::string ext = ".rp6502";
                if (elf.size() > ext.size() &&
                    elf.compare(elf.size() - ext.size(), ext.size(), ext) == 0)
                    elf = elf.substr(0, elf.size() - ext.size());
            }
            g_dwarf = dwarf_line_load(elf.c_str()); /* NULL -> source mapping off */
            g_dinfo = dwarf_info_load(elf.c_str()); /* NULL -> variables off */
        }
        push_segments(); /* feed the memory map the program's segment sizes */

        post([program, soe, sox]() {
            g_stop_on_entry = soe;
            g_stop_on_exit = sox;
            g_reached_entry = false;
            g_launch_done = false;
            g_terminated = false;
            dbg_clear_breakpoints();
            g_instr_bps.clear();
            g_src_bps.clear();
            dbg_stop_at_entry();   /* hold at the first instruction for config */
            if (!program.empty())
                emu_exec(program.c_str());
        });
        return dap::LaunchResponse();
    });

    g_session->registerHandler([](const dap::SetBreakpointsRequest &req) {
        dap::SetBreakpointsResponse r;
        std::string path = req.source.path.value("");
        auto sbs = req.breakpoints.value({});
        auto addrs = std::make_shared<std::vector<uint16_t>>();
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
                addrs->push_back(addr);
            }
            else
            {
                ob.verified = false;
                ob.line = sb.line;
            }
            r.breakpoints.push_back(ob);
        }
        /* DAP replace-semantics, per source file: swap this file's address set. */
        std::string p = path;
        post([p, addrs]() {
            auto it = g_src_bps.find(p);
            if (it != g_src_bps.end())
                for (uint16_t a : it->second)
                    dbg_remove_breakpoint(a);
            g_src_bps[p] = *addrs;
            for (uint16_t a : *addrs)
                dbg_add_breakpoint(a);
        });
        return r;
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

    g_session->registerHandler([](const dap::StackTraceRequest &) {
        dap::StackTraceResponse r;
        /* a fresh stop: invalidate last stop's expandable-variable references */
        g_varnodes.clear();
        dap::StackFrame f;
        f.id = 1;
        uint16_t pc = dbg_stop_pc();
        f.instructionPointerReference = hex16(pc);
        f.line = 0;
        f.column = 0;

        const char *file = nullptr;
        int line = 0;
        if (src_addr_to_line(pc, &file, &line))
        {
            dap::Source src;
            src.path = file;
            src.name = base_name(file);
            f.source = src;
            f.line = line;
            f.column = 1;
        }
        const char *fn = src_addr_to_func(pc);
        if (fn)
            f.name = fn;
        else
        {
            char name[16];
            snprintf(name, sizeof name, "$%04X", pc);
            f.name = name;
        }
        r.stackFrames.push_back(f);
        r.totalFrames = 1;
        return r;
    });

    g_session->registerHandler([](const dap::ScopesRequest &) {
        dap::ScopesResponse r;
        /* Locals + Globals only when a source/type map is loaded; Registers
         * always (the machine-level view). */
        if (g_dinfo || g_cc65)
        {
            dap::Scope loc;
            loc.name = "Locals";
            loc.variablesReference = 2;
            loc.expensive = false;
            r.scopes.push_back(loc);
            dap::Scope glb;
            glb.name = "Globals";
            glb.variablesReference = 3;
            glb.expensive = false;
            r.scopes.push_back(glb);
        }
        dap::Scope reg;
        reg.name = "Registers";
        reg.presentationHint = "registers";
        reg.variablesReference = 1;
        reg.expensive = false;
        r.scopes.push_back(reg);
        return r;
    });

    g_session->registerHandler([](const dap::VariablesRequest &req) {
        dap::VariablesResponse r;
        int64_t ref = req.variablesReference;
        if (ref == 1)
        {
            /* Registers. */
            m6502_t *c = cpu();
            auto add = [&](const char *nm, unsigned val, int width) {
                dap::Variable v;
                v.name = nm;
                char b[16];
                snprintf(b, sizeof b, width == 2 ? "$%04X" : "$%02X", val);
                v.value = b;
                v.variablesReference = 0;
                r.variables.push_back(v);
            };
            add("A", m6502_a(c), 1);
            add("X", m6502_x(c), 1);
            add("Y", m6502_y(c), 1);
            add("SP", m6502_s(c), 1);
            add("PC", m6502_pc(c), 2);
            add("P", m6502_p(c), 1);
        }
        else if (ref == 2)
        {
            /* Locals in the frame at the current stop. */
            uint16_t pc = dbg_stop_pc();
            if (g_dinfo)
            {
                dwarf_var_t buf[256];
                int n = dwarf_info_locals(g_dinfo, pc, dap_readmem, buf, 256);
                for (int i = 0; i < n; i++)
                    r.variables.push_back(make_var(buf[i].name, buf[i].addr, buf[i].addr_ok, buf[i].type));
            }
            else if (g_cc65)
            {
                cc65var_t buf[256];
                int n = cc65dbg_locals(g_cc65, pc, dap_readmem, buf, 256);
                for (int i = 0; i < n; i++)
                    r.variables.push_back(make_var(buf[i].name, buf[i].addr, buf[i].addr_ok, nullptr));
            }
        }
        else if (ref == 3)
        {
            /* Global / file-static variables. */
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
                    r.variables.push_back(make_var(buf[i].name, buf[i].addr, buf[i].addr_ok, nullptr));
            }
        }
        else if (ref >= 1000)
        {
            /* Expand a previously emitted aggregate/pointer node. */
            size_t idx = (size_t)(ref - 1000);
            if (idx < g_varnodes.size())
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
        for (long i = 0; i < want; i++)
        {
            DasmCtx ctx;
            ctx.pc = pc;
            uint16_t next = m6502dasm_op(pc, dasm_in, dasm_out, &ctx);
            dap::DisassembledInstruction di;
            di.address = hex16(pc);
            di.instruction = ctx.text;
            di.instructionBytes = ctx.bytes;
            r.instructions.push_back(di);
            pc = next;
        }
        return r;
    });

    /* Bind to the stdio DAP channel; cppdap spawns the reader thread. */
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

    /* Program exit (once): either keep the session alive in a stopped state so
     * the final screen + machine state stay inspectable until the client
     * disconnects (stopOnExit, the default), or terminate the session. */
    if (!g_terminated && g_launch_done && emu_cpu_halted && !dbg_is_stopped())
    {
        g_terminated = true;
        if (!g_session)
            ; /* no client */
        else if (g_stop_on_exit)
        {
            dbg_note_stop(m6502_pc(cpu())); /* present halt as a stop */
            dap::StoppedEvent ev;
            ev.reason = "exited";
            ev.description = "Program exited (code " + std::to_string(emu_exit_code) +
                             ") — press Stop to close";
            ev.threadId = 1;
            ev.allThreadsStopped = true;
            g_session->send(ev);
        }
        else
            g_session->send(dap::TerminatedEvent());
    }
}

extern "C" bool dap_quit_requested(void) { return g_quit.load(); }

extern "C" void dap_stop(void)
{
    g_session.reset();
    src_free();
}
