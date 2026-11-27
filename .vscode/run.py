#!/usr/bin/env python3
"""Runs a developer's build of an emulator where VS Code has no debugger for
it: the web player in a browser, and the Windows emulator from WSL.

    run.py web ROM      the web player built in build/web, playing ROM
    run.py windows ROM  the Windows emulator in build/sokol/windows/debug

The web player is served on 127.0.0.1 until the run is stopped. A browser
counts 127.0.0.1 as a secure context, which the player's clipboard and
sound need, and from WSL a Windows browser reaches it.
"""

import argparse
import functools
import http.server
import os
import shutil
import subprocess
import sys
import webbrowser
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def in_wsl():
    try:
        return "microsoft" in Path("/proc/version").read_text().lower()
    except OSError:
        return False


def open_browser(url):
    """WSL has no browser of its own; the Windows one is asked."""
    if in_wsl():
        subprocess.run(["cmd.exe", "/c", "start", "", url],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        webbrowser.open(url)


def web(rom):
    """The bundle's player beside a copy of ROM, in build/web/run, with the
    bundle's page made to play it."""
    bundle = ROOT / "build" / "web" / "bundle"
    page = bundle / "index.html"
    if not page.exists():
        sys.exit("run.py: build the web player first (web: Build)")
    run = ROOT / "build" / "web" / "run"
    shutil.rmtree(run, ignore_errors=True)
    run.mkdir(parents=True)
    for name in ("rp6502.js", "rp6502.wasm"):
        shutil.copy2(bundle / name, run / name)
    shutil.copy2(rom, run / rom.name)
    html = page.read_text(encoding="utf-8")
    if "'adventure.rp6502'" not in html:
        sys.exit("run.py: the bundle's index.html no longer names adventure.rp6502")
    html = html.replace("'adventure.rp6502'", repr(rom.name))
    html = html.replace("'Colossal Cave Adventure'", repr(rom.stem))
    # The player tells a page that asks how its sound's queue stands, once a
    # second while its sound runs (least, dry, rate, floor), which is posted
    # here and printed (host/web/page.js).
    stats = ("<script>globalThis.rp6502_audio_stats = (s) => "
             "fetch('/stats', {method: 'POST', body: JSON.stringify(s)});</script>\n")
    html = html.replace("<script", stats + "<script", 1)
    (run / "index.html").write_text(html, encoding="utf-8")

    class Handler(http.server.SimpleHTTPRequestHandler):
        def do_POST(self):
            body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            print(f"sound: {body.decode(errors='replace')}", flush=True)
            self.send_response(204)
            self.end_headers()

        def log_message(self, format, *args):
            pass

    handler = functools.partial(Handler, directory=str(run))
    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler) as server:
        url = f"http://127.0.0.1:{server.server_address[1]}/"
        print(f"serving {rom.name} at {url}", flush=True)
        open_browser(url)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


def windows(rom, args):
    """The exe runs from WSL by interop, which needs its execute bit, and
    takes Windows paths."""
    exe = ROOT / "build" / "sokol" / "windows" / "debug" / "rp6502-emu.exe"
    if not exe.exists():
        sys.exit("run.py: build the Windows emulator first (emu: Build (Windows))")
    exe.chmod(exe.stat().st_mode | 0o111)
    path = subprocess.run(["wslpath", "-w", str(rom)], capture_output=True, text=True,
                          check=True).stdout.strip()
    return subprocess.run([str(exe), *args, path]).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("host", choices=("web", "windows"))
    parser.add_argument("rom", type=Path)
    parser.add_argument("args", nargs=argparse.REMAINDER,
                        help="for windows: options for rp6502-emu before the ROM")
    o = parser.parse_args()
    rom = o.rom if o.rom.is_absolute() else Path(os.getcwd()) / o.rom
    if not rom.is_file():
        sys.exit(f"run.py: no ROM at {rom}")
    if o.host == "web":
        web(rom)
        return 0
    return windows(rom, o.args)


if __name__ == "__main__":
    sys.exit(main())
