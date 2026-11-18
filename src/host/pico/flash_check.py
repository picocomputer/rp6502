#!/usr/bin/env python3
#
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Proves, from a RIA's ELF, that nothing which can run while the RIA rewrites
# its own flash is in flash. Run after every link; any finding fails the build.
#
# The walk is per instruction. addr2line -i gives each instruction its inline
# chain, so a task LTO inlined into sys_walk is still found, and so is every
# function inlined into RAM code. From the roots it follows direct calls, and
# reports:
#   - reached code in flash, and flash data that reached code loads;
#   - indirect calls in reached code that the table below does not cover;
#   - reached functions, inlined or not, with no RAM annotation and no home in
#     a RAM source file, because LTO may stop inlining them.
#
# The roots are the TASK_ALWAYS tasks, read from the driver table, the targets
# of the reviewed indirect calls, the interrupt handlers, core 1's loop, and
# the writer.

import argparse
import collections
import json
import os
import re
import shlex
import subprocess
import sys

# Reached, but a guard keeps them from running while flashing: not followed.
PRUNE = {
    'panic': 'fatal: the RIA has stopped, flashing or not',
    '__assert_func': 'fatal: the RIA has stopped, flashing or not',
    'host_log': 'only prints a log message, and no message is needed while flashing',
    'com_telnet_handle_auth': 'the telnet FLASHING hook closed any session at the passkey prompt',
    'sys_set_flashing': 'the writer calls it first, and it runs the FLASHING hooks from flash still intact',
    'cyw43_ll_bus_init': 'radio start-up: cyw43_ensure_up returns early while the chip is up',
    'initialise_rand': 'runs once, for the first random number at boot (get_rand_64)',
    'btstack_run_loop_poll_data_sources_from_irq':
        'only while the Bluetooth transport is open, and the BLE FLASHING hook closes it',
    'modem_set_conn': 'modem descriptors only, and modem_stop closed them when the 6502 stopped',
    'modem_connect': 'modem descriptors only, and modem_stop closed them when the 6502 stopped',
    'process_remove_event': 'usbh_process_event drops attach and remove events after tuh_keep_only_driver',
    'enum_new_device': 'usbh_process_event drops attach events after tuh_keep_only_driver',
    'enum_resume': 'tuh_keep_only_driver clears its attempt, and only a failed enumeration sets it',
}

STDIO = ['manifold_stdio_out_chars', 'manifold_stdio_out_flush', 'manifold_stdio_in_chars']
PRINTF = ['_out_buffer', '_out_null', '_out_fct', '_out_char', 'stdio_buffered_printer']
# execute_sync is left out: the CYW43 driver uses it only at radio init and deinit.
ASYNC = ['noop', 'async_context_poll_lock_check',
         'async_context_base_add_at_time_worker', 'async_context_base_remove_at_time_worker',
         'async_context_base_add_when_pending_worker',
         'async_context_base_remove_when_pending_worker', 'async_context_poll_requires_update',
         'async_context_poll_poll', 'async_context_poll_wait_until',
         'async_context_poll_wait_for_work_until']
WORKERS = ['cyw43_do_poll', 'cyw43_sleep_timeout_reached', 'update_next_timeout',
           'lwip_timeout_reached']
LWIP_TIMERS = ['tcp_tmr', 'ip_reass_tmr', 'etharp_tmr', 'dhcp_coarse_tmr', 'dhcp_fine_tmr',
               'dns_tmr']
TCP = ['net_recv', 'net_connected', 'net_accept_cb', 'net_err', 'net_pending_err',
       'tcp_recv_null', 'tcp_accept_null']
NO_CALLBACK = 'lwIP netif status and link callbacks: nothing sets them'
BOOTROM = 'the bootrom, or the boot2 copy the SDK keeps in RAM'
MODEM = 'telnet\'s; modem_stop closed the modem\'s when the 6502 stopped'

# Indirect calls reviewed: the function making the call -> (why, what it can
# call while flashing). Every target is followed as a root.
INDIRECT = {
    'stdio_flush': ('stdio drivers: only ria/com/manifold.c registers one', STDIO),
    'stdio_out_chars_crlf': ('stdio drivers', STDIO),
    'stdio_put_string': ('stdio drivers', STDIO),
    '_vsnprintf': ('printf output functions', PRINTF),
    '_out_rev': ('printf output functions', PRINTF),
    '_out_fct': ('the vfctprintf printer', PRINTF),
    'weak_raw_vprintf': ('panic prints through vprintf', STDIO),
    'manifold_term_feed': ('the console\'s terminal; its FLASHING hook clears the pointer', []),
    'com_read': ('the console sources, RP6502_COM_SOURCES',
                 ['keymap_in_chars', 'manifold_uart_read', 'com_telnet_read']),
    'async_context_acquire_lock_blocking': ('the poll context type', ASYNC),
    'async_context_release_lock': ('the poll context type', ASYNC),
    'async_context_set_work_pending': ('the poll context type', ASYNC),
    'async_context_wait_for_work_until': ('the poll context type', ASYNC),
    'async_context_wait_until': ('the poll context type', ASYNC),
    'async_context_poll': ('the poll context type', ASYNC),
    'async_context_lock_check': ('the poll context type', ASYNC),
    'async_context_add_at_time_worker': ('the poll context type', ASYNC),
    'async_context_remove_at_time_worker': ('the poll context type', ASYNC),
    'async_context_base_poll_until_idle': ('workers: CYW43\'s and lwIP\'s; the BLE FLASHING '
                                           'hook removes BTstack\'s', WORKERS),
    'async_context_base_execute_once': ('workers: CYW43\'s and lwIP\'s; the BLE FLASHING '
                                        'hook removes BTstack\'s', WORKERS),
    'cyw43_do_poll': ('the radio poll, set while the chip is up', ['cyw43_poll_func']),
    'cyw43_cb_process_async_event': ('the Wi-Fi scan callback', ['wifi_scan_cb']),
    'cyw43_cb_process_ethernet': ('netif input', ['ethernet_input']),
    'ethernet_output': ('netif linkoutput', ['cyw43_netif_output']),
    'ip4_output_if_src': ('netif output', ['etharp_output']),
    'ip4_frag': ('netif output', ['etharp_output']),
    'netif_add': ('netif init', ['cyw43_netif_init']),
    'netif_set_up': (NO_CALLBACK, []),
    'netif_set_down': (NO_CALLBACK, []),
    'netif_set_link_up': (NO_CALLBACK, []),
    'netif_set_link_down': (NO_CALLBACK, []),
    'netif_do_set_ipaddr': (NO_CALLBACK, []),
    'raw_input': ('raw pcbs: none are opened', []),
    'pbuf_free': ('custom pbufs: only IP fragments', ['ipfrag_free_pbuf_custom']),
    'sys_check_timeouts': ('lwIP timeouts', ['lwip_cyclic_timer', 'tcpip_tcp_timer']),
    'lwip_cyclic_timer': ('lwIP cyclic timers, IPv4', LWIP_TIMERS),
    'tcp_input': ('TCP callbacks', TCP),
    'tcp_input_delayed_close': ('TCP callbacks', TCP),
    'tcp_listen_input': ('TCP callbacks', TCP),
    'tcp_process': ('TCP callbacks', TCP),
    'tcp_process_refused_data': ('TCP callbacks', TCP),
    'tcp_slowtmr': ('TCP callbacks', TCP),
    'tcp_abandon': ('TCP error callbacks', ['net_err', 'net_pending_err']),
    'udp_input': ('UDP receive callbacks', ['dns_recv', 'dhcp_recv', 'ntp_udp_recv']),
    'dns_call_found': ('DNS answers', ['net_dns_found', 'ntp_dns_found']),
    'net_close': ('net.c close hooks: ' + MODEM, ['com_telnet_on_disconnect']),
    'net_recv': ('net.c close hooks: ' + MODEM, ['com_telnet_on_disconnect']),
    'net_accept_cb': ('net.c accept hooks: the telnet FLASHING hook closed telnet\'s listener, '
                      'and modem_stop closed the modem\'s', []),
    'edpt_xfer_complete': ('class drivers: after the USB FLASHING hook only MSC\'s endpoints remain',
                           ['msc_class_driver_xfer_cb']),
    'control_xfer_complete': ('control transfer callbacks of the drives, usbh\'s stall clearing '
                              'and a string fetch still in flight; GET_MAX_LUN is sent only during '
                              'enumeration, which the USB FLASHING hook ended',
                              ['msc_recovery_xfer_cb', 'msc_cbi_adsc_complete', 'edpt_halt_cleared',
                               'usb_string_fetch_cb', 'control_xfer_sync_complete']),
    'usbh_process_event': ('FUNC_CALL events: this build never queues one', []),
    'usbh_task_run': ('call_after: only enumeration sets it, and the USB FLASHING hook ended it', []),
    'flash_range_erase': (BOOTROM, []),
    'flash_range_program': (BOOTROM, []),
    'flash_enable_xip_via_boot2': (BOOTROM, []),
    'rom_func_lookup_inline': (BOOTROM, []),
    'rom_data_lookup_inline': (BOOTROM, []),
}

# Interrupt handlers, core 1's loop, and the writer, from sys_set_flashing to
# the reboot or the lock-up.
OTHER_ROOTS = ['aud_irq', 'vga_backchannel_irq_handler', 'vga_vsync_trap_irq_handler',
               'vga_vsync_alarm_irq_handler', 'hcd_rp2350_irq', 'cyw43_gpio_irq_handler',
               'act_loop', 'uf2_write_ria']

SCHEDULER = 'sys_walk'
# Our own sources. A static inline in one of our headers needs an annotation
# like any other function; one in an SDK or vendor header does not.
SRC = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
# A literal loaded only to be handed to one of these is a message. No message
# is needed while flashing: the image was read whole before the first erase,
# and the writer's own progress and failure lines are RAM strings.
MESSAGE_SINKS = ('__assert_func', 'panic', '__wrap_printf', '__wrap_vprintf', '__wrap_puts',
                 'printf', 'vprintf', 'puts', 'host_log')
# flatten and always_inline force inlining, so what they hold needs no
# annotation of its own.
FORCED = re.compile(r'HOST_ISR_FUNC|\bflatten\b|always_inline|__force_inline')
RAM_TOKENS = ('HOST_IN_RAM_FUNC', '__not_in_flash_func', '__time_critical_func',
              '__no_inline_not_in_flash_func', '__not_in_flash(', '__scratch_x', '__scratch_y')


def flash(a):
    return 0x10000000 <= a < 0x20000000


def run(*cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, check=True, **kw).stdout


def compile_args(entry):
    args = entry['arguments'] if 'arguments' in entry else shlex.split(entry['command'])
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
        elif a == '-o':
            skip = True
        elif a != '-c' and a != entry['file']:
            out.append(a)
    return out


def always_tasks(cc):
    """The TASK_ALWAYS tasks, from the driver table as the compiler sees it."""
    entry = next(e for e in cc if e['file'].replace('\\', '/').endswith('/src/core/sys/sys.c'))
    src = ('#include "drivers.h"\n#define TASK_ALWAYS(f) FLASH_CHECK_ROOT f;\n'
           '#include "core/sys/driver_walk.h"\nDRIVERS_FORWARD(RP6502_MACH_DRIVERS)\n'
           '#include "core/sys/driver_walk_end.h"\n')
    out = run(*compile_args(entry), '-E', '-P', '-x', 'c', '-', input=src, cwd=entry['directory'])
    return re.findall(r'FLASH_CHECK_ROOT (\w+);', out)


class Image:
    def __init__(self, args):
        self.args = args
        self.name = {}                           # symbol address -> name
        self.byname = collections.defaultdict(list)
        self.insns = collections.defaultdict(list)
        self.words = {}
        self.load_sections()
        self.load_code()
        self.a2l = subprocess.Popen([args.addr2line, '-e', args.elf, '-i', '-f', '-a'],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.lines = {}
        self.placed = {}

    def load_sections(self):
        lo, hi = 0x20000000, 0x10000000
        for l in run(self.args.readelf, '-S', '-W', self.args.elf).split('\n'):
            m = re.match(r'\s*\[\s*\d+\]\s+\S+\s+\S+\s+([0-9a-f]{8})\s+[0-9a-f]+\s+([0-9a-f]+)', l)
            if m and flash(int(m.group(1), 16)) and int(m.group(2), 16):
                lo = min(lo, int(m.group(1), 16))
                hi = max(hi, int(m.group(1), 16) + int(m.group(2), 16))
        self.image = (lo, hi)

    def in_image(self, a):
        return self.image[0] <= a < self.image[1]

    def load_code(self):
        sym = None
        for l in run(self.args.objdump, '-d', '--no-show-raw-insn', self.args.elf).split('\n'):
            m = re.match(r'^([0-9a-f]{8}) <(.+)>:$', l)
            if m:
                sym = int(m.group(1), 16)
                self.name[sym] = m.group(2)
                self.byname[m.group(2)].append(sym)
                base = re.sub(r'\.(lto_priv|constprop|isra|part)\..*$', '', m.group(2))
                if base != m.group(2):
                    self.byname[base].append(sym)
                continue
            m = re.match(r'^ *([0-9a-f]+):\t(.*)$', l)
            if not m or sym is None:
                continue
            a, text = int(m.group(1), 16), m.group(2)
            w = re.match(r'\.word\t0x([0-9a-f]{8})', text)
            if w:
                self.words[a] = int(w.group(1), 16)
            else:
                self.insns[sym].append((a, text))
        order = sorted(self.name)
        self.end = dict(zip(order, order[1:] + [1 << 32]))

    def veneer(self, a):
        # a veneer loads its target's address from the word that follows it
        if re.match(r'^__(.+)_veneer$', self.name[a]) and a + 4 in self.words:
            return self.words[a + 4] & ~1
        return a

    def jump_table(self, sym, a):
        """A switch: ldr pc from a table of words right after it, inside sym."""
        t = (a + 4 + 3) & ~3
        return t in self.words and sym <= (self.words[t] & ~1) < self.end[sym]

    def decode(self, sym):
        out, movw = [], {}
        for a, text in self.insns[sym]:
            d = {'addr': a}
            m = re.match(r'(b[a-z]*(?:\.[wn])?|cbn?z)\t(?:r\d+, )?([0-9a-f]+) <([^>+]+)(\+0x[0-9a-f]+)?>',
                         text)
            t = int(m.group(2), 16) if m else None
            if m and not m.group(4) and t != sym and t in self.name:
                d['call'] = self.veneer(t)
            elif m and m.group(1) in ('b', 'b.n', 'b.w') and sym <= t < self.end[sym]:
                d['jump'] = t
            elif re.match(r'(blx|bx)\t(r\d+|ip|sl|fp)\b', text) or re.match(r'mov\tpc,', text) \
                    or (re.match(r'ldr(\.w)?\tpc,', text) and not self.jump_table(sym, a)):
                d['indirect'] = True
            m = re.search(r'@ \(([0-9a-f]{8}) <', text)
            if m and int(m.group(1), 16) in self.words and self.in_image(self.words[int(m.group(1), 16)]):
                d['lit'] = self.words[int(m.group(1), 16)]
            m = re.match(r'movw\t(r\d+), #(\d+)', text)
            if m:
                movw[m.group(1)] = int(m.group(2))
            m = re.match(r'movt\t(r\d+), #(\d+)', text)
            if m and m.group(1) in movw and self.in_image((int(m.group(2)) << 16) | movw[m.group(1)]):
                d['lit'] = (int(m.group(2)) << 16) | movw[m.group(1)]
            out.append(d)
        at = {e['addr']: k for k, e in enumerate(out)}
        for i, d in enumerate(out):
            v = d.get('lit')
            if v is None:
                continue
            if v & 1 and (v & ~1) in self.name:
                del d['lit']  # a function pointer, not data
                continue
            k, steps = i + 1, 0
            while k < len(out) and steps < 64:
                e = out[k]
                steps += 1
                if 'call' in e:
                    if self.name.get(e['call']) in MESSAGE_SINKS:
                        del d['lit']
                    break
                if 'indirect' in e:
                    break
                if 'jump' in e and e['jump'] in at:
                    k = at[e['jump']]
                    continue
                k += 1
        return out

    def chains(self, addrs):
        """addr -> inline chain, innermost first, of (function, file:line)."""
        self.a2l.stdin.write(''.join('0x%x\n' % a for a in addrs) + '0x0\n')
        self.a2l.stdin.flush()
        res, cur = {}, None
        while True:
            l = self.a2l.stdout.readline().rstrip('\n')
            if re.match(r'^0x[0-9a-f]+$', l):
                if int(l, 16) == 0:
                    self.a2l.stdout.readline()
                    self.a2l.stdout.readline()
                    return res
                cur = int(l, 16)
                res[cur] = []
                continue
            res[cur].append((l, self.a2l.stdout.readline().rstrip('\n')))

    def source(self, path):
        if path not in self.lines:
            try:
                with open(path, errors='replace') as f:
                    self.lines[path] = f.read().split('\n')
            except OSError:
                self.lines[path] = None
        return self.lines[path]

    def placement(self, fn, loc, ram_sources):
        """For fn, whose body holds loc: ram-file, forced, annotated, header (not ours), none or unknown."""
        path = loc.split(':')[0]
        if (fn, path) in self.placed:
            return self.placed[(fn, path)]
        rp = os.path.realpath(path) if path not in ('??', '') else None
        r = 'unknown'
        if rp is None:
            pass
        elif rp in ram_sources:
            r = 'ram-file'
        elif path.endswith('.h') and not rp.startswith(SRC + os.sep):
            r = 'header'
        else:
            ls = self.source(rp)
            m = re.match(r'.*:(\d+)', loc)
            if ls is not None and m:
                base = re.sub(r'\.(lto_priv|constprop|isra|part|cold)\b.*$', '', fn)
                pat = re.compile(r'^[A-Za-z_].*\b' + re.escape(base) + r'\b\s*\)?\s*\(')
                for k in range(min(int(m.group(1)) - 1, len(ls) - 1), -1, -1):
                    if pat.match(ls[k]) and not ls[k].startswith('#'):
                        head = (ls[k - 1] + ' ' if k else '') + ls[k]
                        r = 'forced' if FORCED.search(head) else \
                            'annotated' if any(t in head for t in RAM_TOKENS) else 'none'
                        break
        self.placed[(fn, path)] = r
        return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--compile-commands', required=True)
    ap.add_argument('--objdump', required=True)
    ap.add_argument('--addr2line', required=True)
    ap.add_argument('--readelf', required=True)
    args = ap.parse_args()

    with open(args.compile_commands) as f:
        cc = json.load(f)
    ram_sources = {os.path.realpath(os.path.join(e['directory'], e['file'])) for e in cc
                   if '-fno-lto' in compile_args(e)}
    tasks = always_tasks(cc)
    img = Image(args)

    targets = sorted({t for _, ts in INDIRECT.values() for t in ts} | set(OTHER_ROOTS))
    reached = {}   # symbol address -> (caller symbol, call address), or None for a root
    work = collections.deque()

    def enter(a, frm):
        if a in img.insns and a not in reached and img.name[a] != SCHEDULER:
            reached[a] = frm
            work.append(a)

    for r in tasks + targets:
        for a in img.byname.get(r, []):
            enter(a, None)

    flash_code, flash_data, indirect, unannotated, sites = {}, {}, {}, {}, set()
    pruned = set(PRUNE)

    def scan(s, only=None):
        dec = img.decode(s)
        ch = img.chains([d['addr'] for d in dec])
        for d in dec:
            c = ch.get(d['addr'], [])
            names = [f for f, _ in c]
            if only is not None and not (set(names) & only):
                if 'call' in d and img.name.get(d['call']) in only:
                    enter(d['call'], (s, d['addr']))
                continue
            if set(names) & pruned:
                continue  # inlined code a guard keeps from running while flashing
            inner = names[0] if names and names[0] != '??' else img.name[s]
            if flash(d['addr']):
                flash_code.setdefault(s, inner)
            # outermost frame first: a forced inline covers all it holds
            for fn, loc in reversed(c):
                if fn == '??':
                    continue
                p = img.placement(fn, loc, ram_sources)
                if p == 'forced':
                    break
                if p == 'none':
                    unannotated.setdefault(fn, (loc, s))
            if 'lit' in d:
                flash_data.setdefault((inner, d['lit']), s)
            if d.get('indirect'):
                if inner in INDIRECT:
                    sites.add(inner)
                else:
                    indirect.setdefault(inner, s)
            if 'call' in d and img.name.get(d['call']) not in pruned:
                enter(d['call'], (s, d['addr']))

    # Only the TASK_ALWAYS tasks run in sys_walk while flashing.
    for a in img.byname.get(SCHEDULER, []):
        scan(a, set(tasks))
    while work:
        scan(work.popleft())

    def path(a):
        out = []
        while reached.get(a) is not None and len(out) < 16:
            out.append(img.name[a])
            a = reached[a][0]
        out.append(img.name.get(a, '?'))
        return ' <- '.join(out)

    # A reviewed call reached here whose target no source of this build
    # defines points at a renamed function.
    text = ''
    for e in cc:
        try:
            with open(os.path.join(e['directory'], e['file']), errors='replace') as f:
                text += f.read()
        except OSError:
            pass
    wanted = sorted({t for site in sites for t in INDIRECT[site][1]})
    missing = [t for t in wanted if not re.search(r'\b' + re.escape(t) + r'\s*\(', text)]
    home = os.path.expanduser('~')
    report = []
    for a in sorted(flash_code, key=lambda a: img.name[a]):
        report.append('code in flash: ' + path(a))
    for (inner, v), s in sorted(flash_data.items()):
        report.append('flash data 0x%08x loaded by %s in %s' % (v, inner, path(s)))
    for inner, s in sorted(indirect.items()):
        report.append('indirect call not reviewed: %s in %s' % (inner, path(s)))
    for fn, (loc, s) in sorted(unannotated.items()):
        report.append('no RAM annotation: %s (%s) in %s' % (fn, loc.replace(home, '~'), img.name[s]))
    for t in missing:
        report.append('no source defines the reviewed target ' + t)
    elf = os.path.basename(args.elf)
    if report:
        print('%s: %d findings on what runs while flashing' % (elf, len(report)), file=sys.stderr)
        for r in report:
            print('  ' + r, file=sys.stderr)
        sys.exit(1)
    print('%s: %d functions can run while flashing, all in RAM' % (elf, len(reached)))


if __name__ == '__main__':
    main()
