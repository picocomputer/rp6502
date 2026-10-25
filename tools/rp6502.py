#!/usr/bin/env python3
#
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
# SPDX-License-Identifier: Unlicense

# RP6502-RIA Developer tool

# Keep this file working on Python 3.9 for macOS.

import io
import os
import re
import time
import binascii
import codecs
import argparse
import configparser
import platform
import sys
import select
import ctypes
import json
import glob
import shlex
import socket
import struct
import subprocess
import concurrent.futures
from typing import Union

# POSIX
try:
    import tty
    import termios
    import fcntl
except ImportError:
    pass

# Windows
try:
    kernel32 = ctypes.windll.kernel32
    from ctypes import wintypes
except (ImportError, AttributeError):
    pass

# Rename this file for use on other platforms.
SCRIPT_FILE = os.path.basename(__file__)
SCRIPT_NAME = os.path.splitext(SCRIPT_FILE)[0].upper()

# Two seconds is generous but not
# so short that it becomes suspect.
RESPONSE_TIMEOUT = 2.0


class SerialDevice:
    """Cross-platform serial port implementation."""

    def __init__(self, port: str):
        self._port = port
        self._baudrate = 115200
        self._fd = None
        self._handle = None
        self._is_posix = "tty" in globals()

    def open(self):
        """Open the serial port."""
        if self._is_posix:
            self._open_posix()
        else:
            self._open_windows()

    def _open_posix(self):
        """Open serial port on POSIX systems (Linux, macOS, BSD)."""
        self._fd = os.open(self._port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        # Configure termios
        try:
            attrs = termios.tcgetattr(self._fd)
            # Set baud rate
            baud_constant = getattr(termios, f"B{self._baudrate}", None)
            if baud_constant is None:
                raise OSError(f"Unsupported baud rate: {self._baudrate}")
            attrs[0] = 0  # Input flags: no processing
            attrs[1] = 0  # Output flags: no processing
            attrs[2] = (
                termios.CS8  # 8 data bits, no parity, 1 stop bit
                | termios.CREAD  # Enable receiver
                | termios.CLOCAL  # No modem control
            )
            attrs[3] = 0  # Local flags: raw mode
            attrs[4] = baud_constant  # Input baud rate
            attrs[5] = baud_constant  # Output baud rate
            termios.tcsetattr(self._fd, termios.TCSANOW, attrs)  # Set attributes
            termios.tcflush(self._fd, termios.TCIOFLUSH)
        except:
            os.close(self._fd)
            self._fd = None
            raise

    def _open_windows(self):
        """Open serial port on Windows."""
        # Open COM port
        port_name = (
            f"\\\\.\\{self._port}"
            if not self._port.startswith("\\\\.\\")
            else self._port
        )
        GENERIC_READ_WRITE = 0xC0000000
        OPEN_EXISTING = 3
        self._handle = kernel32.CreateFileW(
            port_name, GENERIC_READ_WRITE, 0, None, OPEN_EXISTING, 0, None
        )
        # Mimic POSIX error here
        if self._handle in (-1, 0):
            raise FileNotFoundError(f"No such device: '{self._port}'")

        # Configure DCB (Device Control Block)
        class DCB(ctypes.Structure):
            _fields_ = [
                ("DCBlength", wintypes.DWORD),
                ("BaudRate", wintypes.DWORD),
                ("fBinary", wintypes.DWORD, 1),
                ("fParity", wintypes.DWORD, 1),
                ("fOutxCtsFlow", wintypes.DWORD, 1),
                ("fOutxDsrFlow", wintypes.DWORD, 1),
                ("fDtrControl", wintypes.DWORD, 2),
                ("fDsrSensitivity", wintypes.DWORD, 1),
                ("fTXContinueOnXoff", wintypes.DWORD, 1),
                ("fOutX", wintypes.DWORD, 1),
                ("fInX", wintypes.DWORD, 1),
                ("fErrorChar", wintypes.DWORD, 1),
                ("fNull", wintypes.DWORD, 1),
                ("fRtsControl", wintypes.DWORD, 2),
                ("fAbortOnError", wintypes.DWORD, 1),
                ("fDummy2", wintypes.DWORD, 17),
                ("wReserved", wintypes.WORD),
                ("XonLim", wintypes.WORD),
                ("XoffLim", wintypes.WORD),
                ("ByteSize", ctypes.c_ubyte),
                ("Parity", ctypes.c_ubyte),
                ("StopBits", ctypes.c_ubyte),
                ("XonChar", ctypes.c_char),
                ("XoffChar", ctypes.c_char),
                ("ErrorChar", ctypes.c_char),
                ("EofChar", ctypes.c_char),
                ("EvtChar", ctypes.c_char),
                ("wReserved1", wintypes.WORD),
            ]

        dcb = DCB()
        dcb.DCBlength = ctypes.sizeof(DCB)
        if not kernel32.GetCommState(self._handle, ctypes.byref(dcb)):
            kernel32.CloseHandle(self._handle)
            raise OSError(f"Could not get COM state for {self._port}")
        # Set 8N1 format with DTR/RTS enabled
        dcb.BaudRate = self._baudrate
        dcb.ByteSize = 8
        dcb.Parity = 0
        dcb.StopBits = 0
        dcb.fBinary = 1
        dcb.fParity = 0
        dcb.fDtrControl = 1
        dcb.fRtsControl = 1
        dcb.fOutxCtsFlow = 0
        dcb.fOutxDsrFlow = 0
        if not kernel32.SetCommState(self._handle, ctypes.byref(dcb)):
            kernel32.CloseHandle(self._handle)
            raise OSError(f"Could not set COM state for {self._port}")

        # Configure read/write timeouts
        class COMMTIMEOUTS(ctypes.Structure):
            _fields_ = [
                ("ReadIntervalTimeout", wintypes.DWORD),
                ("ReadTotalTimeoutMultiplier", wintypes.DWORD),
                ("ReadTotalTimeoutConstant", wintypes.DWORD),
                ("WriteTotalTimeoutMultiplier", wintypes.DWORD),
                ("WriteTotalTimeoutConstant", wintypes.DWORD),
            ]

        timeouts = COMMTIMEOUTS()
        timeouts.ReadIntervalTimeout = 0
        timeouts.ReadTotalTimeoutMultiplier = 0
        timeouts.ReadTotalTimeoutConstant = 1
        timeouts.WriteTotalTimeoutMultiplier = 0
        timeouts.WriteTotalTimeoutConstant = 0
        if not kernel32.SetCommTimeouts(self._handle, ctypes.byref(timeouts)):
            kernel32.CloseHandle(self._handle)
            raise OSError(f"Could not set timeouts for {self._port}")

    def write(self, data: bytes):
        """Write data to the serial port."""
        if self._is_posix:
            total_written = 0
            while total_written < len(data):
                try:
                    written = os.write(self._fd, data[total_written:])
                    total_written += written
                except BlockingIOError:
                    select.select([], [self._fd], [])
        else:
            written = wintypes.DWORD()
            buffer = ctypes.create_string_buffer(bytes(data))
            if not kernel32.WriteFile(
                self._handle, buffer, len(data), ctypes.byref(written), None
            ):
                raise OSError("kernel32.WriteFile failed")

    def read(self, size: int = 1) -> bytes:
        """Read up to size bytes from the serial port."""
        if self._is_posix:
            return self._read_posix(size)
        else:
            return self._read_windows(size)

    def _read_posix(self, size: int) -> bytes:
        """Read with timeout on POSIX systems."""
        start = time.monotonic()
        data = b""
        while len(data) < size:
            if time.monotonic() - start > RESPONSE_TIMEOUT:
                break
            try:
                chunk = os.read(self._fd, size - len(data))
                if chunk:
                    data += chunk
                    start = time.monotonic()
                else:
                    time.sleep(0.001)
            except BlockingIOError:
                time.sleep(0.001)
        return data

    def _read_windows(self, size: int) -> bytes:
        """Read with timeout on Windows."""
        buffer = ctypes.create_string_buffer(size)
        bytes_read = wintypes.DWORD()
        success = kernel32.ReadFile(
            self._handle, buffer, size, ctypes.byref(bytes_read), None
        )
        return buffer.raw[: bytes_read.value] if success else b""

    def read_until(self, delimiter: bytes = b"\n") -> bytes:
        """Read until delimiter is found or timeout occurs."""
        start = time.monotonic()
        buffer = b""
        while True:
            if delimiter in buffer:
                return buffer
            if time.monotonic() - start > RESPONSE_TIMEOUT:
                return buffer
            chunk = self.read(1)
            if chunk:
                buffer += chunk
            else:
                time.sleep(0.001)

    def flush_read_bufs(self):
        """Discard all pending input data."""
        if self._is_posix:
            termios.tcflush(self._fd, termios.TCIFLUSH)
        else:
            kernel32.PurgeComm(self._handle, 0x0008)  # PURGE_RXCLEAR

    def send_break(self):
        """Send a break signal."""
        duration = 0.1  # works down to 300bps
        if self._is_posix:
            if platform.system() == "Darwin":
                TIOCSBRK, TIOCCBRK = 0x2000747B, 0x2000747A
            else:
                TIOCSBRK, TIOCCBRK = 0x5427, 0x5428
            try:
                fcntl.ioctl(self._fd, TIOCSBRK)
                time.sleep(duration)
                fcntl.ioctl(self._fd, TIOCCBRK)
            except Exception:
                termios.tcsendbreak(self._fd, 0)
        else:
            try:
                kernel32.EscapeCommFunction(self._handle, 8)  # SETBREAK
                time.sleep(duration)
            finally:
                kernel32.EscapeCommFunction(self._handle, 9)  # CLRBREAK

    def fileno(self) -> int:
        """Return the file descriptor (POSIX only, for select())."""
        if self._is_posix:
            return self._fd
        raise NotImplementedError("fileno() not supported on Windows")

    def close(self):
        """Close the serial port."""
        if self._is_posix and self._fd is not None:
            os.close(self._fd)
            self._fd = None
        elif not self._is_posix and self._handle is not None:
            kernel32.CloseHandle(self._handle)
            self._handle = None


class TelnetDevice:
    """Telnet connection per RFC 854/855 with Q-method (RFC 1143) negotiation."""

    # IAC and commands (RFC 854)
    IAC = 0xFF
    DONT = 0xFE
    DO = 0xFD
    WONT = 0xFC
    WILL = 0xFB
    SB = 0xFA
    SE = 0xF0
    BRK = 0xF3
    # Options (RFC 856 binary transmission)
    BINARY = 0x00

    # Q-method states. We never initiate disable, so WANT_NO is unused.
    _NO, _YES, _WANT_YES = 0, 1, 2

    def __init__(self, host: str, port: int, key: str):
        self._host = host
        self._port = port
        self._key = key
        self._sock = None
        self._read_buf = b""
        self._iac_pending = b""
        # Per-option negotiation state: opt -> [us, him]
        self._opts = {}

    # --- low-level send ---

    def _send_raw(self, data: bytes):
        """Send raw bytes, waiting on backpressure via select."""
        total = 0
        while total < len(data):
            try:
                total += self._sock.send(data[total:])
            except BlockingIOError:
                select.select([], [self._sock], [])

    def _send_iac(self, *parts: int):
        """Send IAC followed by one or more command bytes."""
        self._send_raw(bytes((self.IAC, *parts)))

    # --- option negotiation (RFC 1143 Q-method) ---

    def _want(self, opt: int) -> bool:
        """Policy: which options we accept on either side."""
        return opt == self.BINARY

    def _opt(self, opt: int) -> list:
        if opt not in self._opts:
            self._opts[opt] = [self._NO, self._NO]
        return self._opts[opt]

    def _offer_will(self, opt: int):
        """Request to enable `opt` on our side."""
        s = self._opt(opt)
        if s[0] == self._NO:
            s[0] = self._WANT_YES
            self._send_iac(self.WILL, opt)

    def _offer_do(self, opt: int):
        """Request to enable `opt` on peer's side."""
        s = self._opt(opt)
        if s[1] == self._NO:
            s[1] = self._WANT_YES
            self._send_iac(self.DO, opt)

    def _recv_do(self, opt: int):
        s = self._opt(opt)
        if s[0] == self._NO:
            if self._want(opt):
                s[0] = self._YES
                self._send_iac(self.WILL, opt)
            else:
                self._send_iac(self.WONT, opt)
        elif s[0] == self._WANT_YES:
            s[0] = self._YES

    def _recv_dont(self, opt: int):
        s = self._opt(opt)
        if s[0] == self._YES:
            s[0] = self._NO
            self._send_iac(self.WONT, opt)
        elif s[0] == self._WANT_YES:
            s[0] = self._NO

    def _recv_will(self, opt: int):
        s = self._opt(opt)
        if s[1] == self._NO:
            if self._want(opt):
                s[1] = self._YES
                self._send_iac(self.DO, opt)
            else:
                self._send_iac(self.DONT, opt)
        elif s[1] == self._WANT_YES:
            s[1] = self._YES

    def _recv_wont(self, opt: int):
        s = self._opt(opt)
        if s[1] == self._YES:
            s[1] = self._NO
            self._send_iac(self.DONT, opt)
        elif s[1] == self._WANT_YES:
            s[1] = self._NO

    # --- IAC scanner ---

    def _strip_iac(self, data: bytes) -> bytes:
        """Extract user data from incoming bytes, handling telnet commands."""
        data = self._iac_pending + data
        self._iac_pending = b""
        out = bytearray()
        negotiators = {
            self.DO: self._recv_do,
            self.DONT: self._recv_dont,
            self.WILL: self._recv_will,
            self.WONT: self._recv_wont,
        }
        i, n = 0, len(data)
        while i < n:
            if data[i] != self.IAC:
                out.append(data[i])
                i += 1
                continue
            if i + 1 >= n:
                self._iac_pending = data[i:]
                break
            cmd = data[i + 1]
            if cmd == self.IAC:
                # IAC IAC = literal 0xFF
                out.append(0xFF)
                i += 2
            elif cmd in negotiators:
                if i + 2 >= n:
                    self._iac_pending = data[i:]
                    break
                negotiators[cmd](data[i + 2])
                i += 3
            elif cmd == self.SB:
                # IAC SB ... IAC SE -- skip subnegotiation payload
                j = i + 2
                while j + 1 < n:
                    if data[j] == self.IAC:
                        if data[j + 1] == self.SE:
                            j += 2
                            break
                        j += 2  # IAC IAC = escaped 0xFF inside subneg
                    else:
                        j += 1
                else:
                    self._iac_pending = data[i:]
                    break
                i = j
            else:
                # 2-byte commands without option (NOP, DM, BRK, IP, AO, AYT, EC, EL, GA, SE)
                i += 2
        return bytes(out)

    # --- connection lifecycle ---

    def open(self):
        """Connect and perform passkey login."""
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._sock.settimeout(RESPONSE_TIMEOUT)
        try:
            self._sock.connect((self._host, self._port))
            # Request BINARY transmission on both sides (RFC 856)
            # while still in blocking mode so the offers get out promptly.
            self._offer_will(self.BINARY)
            self._offer_do(self.BINARY)
            self._sock.setblocking(False)
            self.read_until(b":")
            self.write(self._key.encode("ascii") + b"\r\n")
            self.read_until(b"\n")  # passkey echo
            response = self.read_until(b"\n").decode("ascii", errors="replace").strip()
            if response.startswith("?"):
                raise RuntimeError(response)
        except Exception:
            self._sock.close()
            self._sock = None
            raise

    # --- public I/O ---

    def write(self, data: bytes):
        """Write data, escaping IAC (0xFF) per telnet spec."""
        self._send_raw(data.replace(b"\xff", b"\xff\xff"))

    def read(self, size: int = 1) -> bytes:
        """Read up to `size` bytes; times out after RESPONSE_TIMEOUT of silence."""
        deadline = time.monotonic() + RESPONSE_TIMEOUT
        while len(self._read_buf) < size:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                # Only recv what we still need so fileno() readiness stays in
                # sync with _read_buf -- callers select() on us for terminals.
                chunk = self._sock.recv(size - len(self._read_buf))
                if not chunk:
                    break  # peer closed
                self._read_buf += self._strip_iac(chunk)
                deadline = time.monotonic() + RESPONSE_TIMEOUT
            except BlockingIOError:
                ready, _, _ = select.select([self._sock], [], [], remaining)
                if not ready:
                    break
            except OSError:
                break
        result = self._read_buf[:size]
        self._read_buf = self._read_buf[size:]
        return result

    def read_until(self, delimiter: bytes = b"\n") -> bytes:
        """Read until delimiter is found or timeout occurs."""
        start = time.monotonic()
        buffer = b""
        while True:
            if delimiter in buffer:
                return buffer
            if time.monotonic() - start > RESPONSE_TIMEOUT:
                return buffer
            chunk = self.read(1)
            if chunk:
                buffer += chunk
            else:
                time.sleep(0.001)

    def flush_read_bufs(self):
        """Discard all pending input data."""
        self._read_buf = b""
        self._iac_pending = b""
        while True:
            try:
                if not self._sock.recv(4096):
                    break
            except (BlockingIOError, OSError):
                break

    def send_break(self):
        """Send telnet BREAK (RFC 854)."""
        self._send_iac(self.BRK)

    def fileno(self) -> int:
        """Return the socket file descriptor for select()."""
        return self._sock.fileno()

    def close(self):
        """Close the telnet connection."""
        if self._sock is not None:
            self._sock.close()
            self._sock = None


class Console:
    """Manages the RIA console over a serial connection."""

    def default_device():
        # Hint at where the USB CDC mounts on various OSs
        if platform.system() == "Windows":
            return "COM1"
        elif platform.system() == "Darwin":
            devices = sorted(glob.glob("/dev/cu.usbmodem*"))
            if devices:
                return devices[0]
            return "/dev/cu.usbmodem"
        elif platform.system() == "Linux":
            return "/dev/ttyACM0"
        else:
            return "/dev/tty"

    def __init__(self, port):
        """Initialize console over serial or telnet connection."""
        self.serial = port
        self._code_page = None
        self.serial.open()

    def code_page(self, timeout: float = RESPONSE_TIMEOUT) -> str:
        """Fetch (and cache) the device code page for terminal/filename encoding."""
        if self._code_page is None:
            self.serial.write(b"set cp\r")
            self.wait_for_prompt(":", timeout)
            result = self.serial.read_until().decode("ascii")
            self.wait_for_prompt("]", timeout)
            self._code_page = f"cp{re.sub(r'[^0-9]', '', result)}"
        return self._code_page

    def quote(self, s: str) -> str:
        """Quote a name/arg for the monitor parser (LOAD/UPLOAD/CD)."""
        encoding = "ascii" if s.isascii() else self.code_page()
        try:
            raw = s.encode(encoding, "replace")
        except LookupError:
            raw = s.encode("ascii", "replace")  # unknown code page; degrade
        out = ['"']
        for byte in raw:
            if byte in (0x22, 0x5C):  # " and backslash
                out.append("\\" + chr(byte))
            elif 0x20 <= byte < 0x7F:
                out.append(chr(byte))
            else:
                out.append(f"\\{byte:03o}")
        out.append('"')
        return "".join(out)

    def terminal(self, cp):
        """Dispatch to the correct terminal emulator"""
        print("Console terminal. CTRL-A then B for break or X for exit.")
        # We also accept CTRL-A F and CTRL-A Q for minicom habits.
        if "tty" in globals():
            self.term_posix(cp)
        else:
            self.term_windows(cp)

    def term_posix(self, cp: str):
        """POSIX terminal emulator for Linux, BSD, MacOS, etc."""
        fd = sys.stdin.fileno()
        saved = termios.tcgetattr(fd) if sys.stdin.isatty() else None
        if saved:
            tty.setraw(fd)
        # A keystroke arrives a byte at a time and only a whole character can
        # be spelled in the device code page.
        decoder = codecs.getincrementaldecoder("utf-8")("ignore")
        try:
            ctrl_a_pressed = False
            while True:
                ready, _, _ = select.select([sys.stdin, self.serial], [], [], None)
                if sys.stdin in ready:
                    char = decoder.decode(os.read(fd, 1))
                    if not char:
                        continue  # a character still arriving
                    if char == "\x01":  # CTRL-A
                        ctrl_a_pressed = True
                        self.serial.write(char.encode(cp, "replace"))
                    elif ctrl_a_pressed and char.lower() in "bf":
                        self.send_break()  # eats prompt
                        sys.stdout.write("\r\n]")  # fake prompt
                        ctrl_a_pressed = False
                    elif ctrl_a_pressed and char.lower() in "xq":
                        sys.stdout.write("\r\n")
                        break
                    else:
                        ctrl_a_pressed = False
                        self.serial.write(char.encode(cp, "replace"))
                if self.serial in ready:
                    data = self.serial.read(1)
                    if len(data) > 0:
                        try:
                            sys.stdout.write(data.decode(cp))
                        except UnicodeDecodeError:
                            sys.stdout.write(f"\\x{data[0]:02x}")
                        sys.stdout.flush()
        finally:
            if saved:
                termios.tcsetattr(fd, termios.TCSADRAIN, saved)

    def term_windows(self, cp):
        """Windows terminal emulator using Console API"""
        ctrl_a_pressed = False
        while True:
            try:
                data = self.serial.read(1)
                if len(data) > 0:
                    try:
                        sys.stdout.write(data.decode(cp))
                    except UnicodeDecodeError:
                        sys.stdout.write(f"\\x{data[0]:02x}")
                    sys.stdout.flush()
                else:
                    key_in = self.term_windows_keyboard()
                    if key_in:
                        if key_in == "\x01":  # CTRL-A
                            ctrl_a_pressed = True
                            self.serial.write(key_in.encode(cp, "replace"))
                        elif ctrl_a_pressed and key_in.lower() in "bf":
                            self.send_break()  # eats prompt
                            sys.stdout.write("\r\n]")  # fake prompt
                            ctrl_a_pressed = False
                        elif ctrl_a_pressed and key_in.lower() in "xq":
                            sys.stdout.write("\r\n")
                            break
                        else:
                            ctrl_a_pressed = False
                            self.serial.write(key_in.encode(cp, "replace"))
                    else:
                        time.sleep(0.001)
            except KeyboardInterrupt:
                self.serial.write(b"\x03")

    def term_windows_keyboard(self) -> Union[str, None]:
        """Get a key event as ANSI using Windows Console API"""
        if not hasattr(self, "_stdin_handle"):
            self._stdin_handle = ctypes.windll.kernel32.GetStdHandle(-10)

        class KEY_EVENT_RECORD(ctypes.Structure):
            _fields_ = [
                ("bKeyDown", wintypes.BOOL),
                ("wRepeatCount", wintypes.WORD),
                ("wVirtualKeyCode", wintypes.WORD),
                ("wVirtualScanCode", wintypes.WORD),
                ("uChar", wintypes.WCHAR),
                ("dwControlKeyState", wintypes.DWORD),
            ]

        class INPUT_RECORD(ctypes.Structure):
            _fields_ = [
                ("EventType", wintypes.WORD),
                ("Event", KEY_EVENT_RECORD),
            ]

        # Check if input is available
        events_available = wintypes.DWORD()
        ctypes.windll.kernel32.GetNumberOfConsoleInputEvents(
            self._stdin_handle, ctypes.byref(events_available)
        )
        if events_available.value == 0:
            return None

        # Read input event
        input_record = INPUT_RECORD()

        if not ctypes.windll.kernel32.ReadConsoleInputW(
            self._stdin_handle,
            ctypes.byref(input_record),
            1,
            ctypes.byref(wintypes.DWORD()),
        ):
            return None

        # Only process key down events (EventType 1 = KEY_EVENT)
        if input_record.EventType != 1 or not input_record.Event.bKeyDown:
            return None

        # Modifier state
        alt = bool(input_record.Event.dwControlKeyState & (0x0001 | 0x0002))
        ctrl = bool(input_record.Event.dwControlKeyState & (0x0004 | 0x0008))
        shift = bool(input_record.Event.dwControlKeyState & 0x0010)
        modifier = 1
        if shift:
            modifier += 1
        if alt:
            modifier += 2
        if ctrl:
            modifier += 4
        if modifier == 1:
            modifier = False

        # Virtual key codes
        vk_code = input_record.Event.wVirtualKeyCode
        if vk_code == 0x0D:  # Enter/Return
            return "\r"
        elif vk_code == 0x08:  # Backspace
            return "\b"
        elif vk_code == 0x57 and ctrl:  # Ctrl+Backspace
            return "\b"
        elif vk_code == 0x09:  # Tab
            return "\t"
        elif vk_code == 0x1B:  # Escape
            return "\x1b"
        elif vk_code == 0x20:  # Space
            return " "
        elif vk_code == 0x70:  # F1
            return f"\x1b[1;{modifier}P" if modifier else "\x1bOP"
        elif vk_code == 0x71:  # F2
            return f"\x1b[1;{modifier}Q" if modifier else "\x1bOQ"
        elif vk_code == 0x72:  # F3
            return f"\x1b[1;{modifier}R" if modifier else "\x1bOR"
        elif vk_code == 0x73:  # F4
            return f"\x1b[1;{modifier}S" if modifier else "\x1bOS"
        elif vk_code == 0x74:  # F5
            return f"\x1b[15;{modifier}~" if modifier else "\x1b[15~"
        elif vk_code == 0x75:  # F6
            return f"\x1b[17;{modifier}~" if modifier else "\x1b[17~"
        elif vk_code == 0x76:  # F7
            return f"\x1b[18;{modifier}~" if modifier else "\x1b[18~"
        elif vk_code == 0x77:  # F8
            return f"\x1b[19;{modifier}~" if modifier else "\x1b[19~"
        elif vk_code == 0x78:  # F9
            return f"\x1b[20;{modifier}~" if modifier else "\x1b[20~"
        elif vk_code == 0x79:  # F10
            return f"\x1b[21;{modifier}~" if modifier else "\x1b[21~"
        elif vk_code == 0x7A:  # F11
            return f"\x1b[23;{modifier}~" if modifier else "\x1b[23~"
        elif vk_code == 0x7B:  # F12
            return f"\x1b[24;{modifier}~" if modifier else "\x1b[24~"
        elif vk_code == 0x26:  # Up arrow
            return f"\x1b[1;{modifier}A" if modifier else "\x1b[A"
        elif vk_code == 0x28:  # Down arrow
            return f"\x1b[1;{modifier}B" if modifier else "\x1b[B"
        elif vk_code == 0x27:  # Right arrow
            return f"\x1b[1;{modifier}C" if modifier else "\x1b[C"
        elif vk_code == 0x25:  # Left arrow
            return f"\x1b[1;{modifier}D" if modifier else "\x1b[D"
        elif vk_code == 0x24:  # Home
            return f"\x1b[1;{modifier}H" if modifier else "\x1b[H"
        elif vk_code == 0x23:  # End
            return f"\x1b[1;{modifier}F" if modifier else "\x1b[F"
        elif vk_code == 0x21:  # Page Up
            return f"\x1b[5;{modifier}~" if modifier else "\x1b[5~"
        elif vk_code == 0x22:  # Page Down
            return f"\x1b[6;{modifier}~" if modifier else "\x1b[6~"
        elif vk_code == 0x2D:  # Insert
            return f"\x1b[2;{modifier}~" if modifier else "\x1b[2~"
        elif vk_code == 0x2E:  # Delete
            return f"\x1b[3;{modifier}~" if modifier else "\x1b[3~"

        # ASCII codes
        char = input_record.Event.uChar
        if ctrl and not alt:
            if char:
                ch = ord(char)
                if ord("`") <= ch <= ord("~"):
                    return chr(ch - 96)
                elif ord("@") <= ch <= ord("_"):
                    return chr(ch - 64)
            # Ctrl+A through Ctrl+Z using virtual key codes
            if 65 <= vk_code <= 90:
                return chr(vk_code - 64)
            return None
        if char and ord(char) != 0:
            return char
        return None

    def send_break(self):
        """Stop the 6502 and return to monitor."""
        # Try twice in case RIA is writing
        try:
            self.serial.flush_read_bufs()
            self.serial.send_break()
            self.wait_for_prompt("]")
        except TimeoutError:
            self.serial.flush_read_bufs()
            self.serial.send_break()
            self.wait_for_prompt("]")

    def command(self, cmd: str, timeout: float = RESPONSE_TIMEOUT):
        """Send one command and wait for next monitor prompt."""
        self.serial.write(bytes(cmd, "ascii"))
        self.serial.write(b"\r")
        self.wait_for_prompt("]", timeout)

    def binary(self, addr: int, data: bytes):
        """Send data to memory using BINARY command."""
        command = f"BINARY ${addr:04X} ${len(data):03X} ${binascii.crc32(data):08X}\r"
        self.serial.write(bytes(command, "utf-8"))
        self.serial.write(data)
        self.wait_for_prompt("]")

    def upload(self, file, name: str):
        """Upload readable file to remote file "name"."""
        self.serial.write(bytes(f"UPLOAD {self.quote(name)}\r", "ascii"))
        self.wait_for_prompt("}")
        file.seek(0)
        while True:
            chunk = file.read(1024)
            if len(chunk) == 0:
                break
            command = f"${len(chunk):03X} ${binascii.crc32(chunk):08X}\r"
            self.serial.write(bytes(command, "ascii"))
            self.serial.read_until(b"\n")
            self.serial.write(chunk)
            self.wait_for_prompt("}")
        self.serial.write(b"END\r")
        self.wait_for_prompt("]")

    def load(self, name: str, args=()):
        """Load a previously uploaded ROM file, passing args as its argv."""
        line = f"LOAD {self.quote(name)}"
        for arg in args:
            line += f" {self.quote(arg)}"
        self.serial.write(f"{line}\r".encode("ascii"))
        self.serial.read_until()

    def reset(self):
        """Start the 6502."""
        self.serial.write(b"RESET\r")
        self.serial.read_until()

    def send_rom(self, rom):
        """Send rom."""
        addr, data = rom.next_rom_data(0)
        while data is not None:
            self.binary(addr, data)
            addr += len(data)
            addr, data = rom.next_rom_data(addr)

    def wait_for_prompt(self, prompt: str, timeout: float = RESPONSE_TIMEOUT):
        """Wait for a specific prompt from the device."""
        prompt_bytes = bytes(prompt, "ascii")
        start = time.monotonic()
        at_line_start = True
        while True:
            if len(prompt) == 1:
                data = self.serial.read(1)
                if at_line_start and data == b"?":
                    monitor_result = data.decode("ascii")
                    monitor_result += self.serial.read_until().decode("ascii").strip()
                    raise RuntimeError(monitor_result)
                at_line_start = data == b"\n" or data == b"\r"
            else:
                data = self.serial.read_until()
                if data.startswith(b"?"):
                    monitor_result = data.decode("ascii")
                    monitor_result += self.serial.read_until().decode("ascii").strip()
                    raise RuntimeError(monitor_result)
            if data.strip().lower() == prompt_bytes.lower():
                break
            if len(data) == 0:
                if time.monotonic() - start > timeout:
                    raise TimeoutError("Timeout: console did not respond")


class ROMException(Exception):
    """Custom exception for ROM-related errors."""


class ROM:
    """Virtual ROM builder."""

    @staticmethod
    def parse_int(s: str) -> int:
        """Parse a numeric string with support for MOS $FFFF format."""
        s = re.sub(r"^\$", "0x", s)
        if not re.match(r"^(0x)?[0-9A-Fa-f]+$", s):
            raise ValueError(f"Invalid hex address: {s!r}")
        return int(s, 0)

    def __init__(self, replace=()):
        """Sparse array of virtual ROM with optional named assets."""
        self.data = {}
        self.alloc = {}
        self.assets = []  # list of (name, bytes)
        self.replace = set(replace)

    def add_asset(self, name: str, data: bytes):
        """Append a named asset to the ROM."""
        for i, (n, _) in enumerate(self.assets):
            if n == name:
                if name not in self.replace:
                    raise ROMException(f"Asset name already exists: {name}")
                self.assets[i] = (name, data)
                return
        self.assets.append((name, data))

    def add_binary_data(self, data: bytes, addr: int):
        """Add binary data to ROM."""
        length = len(data)
        self.allocate_rom(addr, length)
        for i in range(length):
            self.data[addr + i] = data[i]

    def add_nmi_vector(self, addr: int):
        """Set NMI vector in $FFFA and $FFFB."""
        if not (0 <= addr <= 0xFFFF):
            raise ROMException(f"Invalid NMI vector: ${addr:04X}")
        self.allocate_rom(0xFFFA, 2)
        self.data[0xFFFA] = addr & 0xFF
        self.data[0xFFFB] = addr >> 8

    def add_reset_vector(self, addr: int):
        """Set reset vector in $FFFC and $FFFD."""
        if not (0 <= addr <= 0xFFFF):
            raise ROMException(f"Invalid reset vector: ${addr:04X}")
        self.allocate_rom(0xFFFC, 2)
        self.data[0xFFFC] = addr & 0xFF
        self.data[0xFFFD] = addr >> 8

    def add_irq_vector(self, addr: int):
        """Set IRQ vector in $FFFE and $FFFF."""
        if not (0 <= addr <= 0xFFFF):
            raise ROMException(f"Invalid IRQ vector: ${addr:04X}")
        self.allocate_rom(0xFFFE, 2)
        self.data[0xFFFE] = addr & 0xFF
        self.data[0xFFFF] = addr >> 8

    def add_binary_file(self, file: str, **addr):
        """Add binary memory data from file. The addr kwargs are: data, nmi, reset, and irq."""
        """Data is where to load the data, the rest are CPU vectors for $FFFA-$FFFF."""
        """Addresses should be an int, None to not provide, or True to read from the file."""
        """Vectors are read from the file in the order listed above."""
        with open(file, "rb") as f:
            data = f.read()
        if addr["data"] is None:
            raise ROMException("Address for data is required.")
        if addr["data"] is True:
            if len(data) < 2:
                raise ROMException("No data address found in file.")
            addr["data"] = data[0] + data[1] * 256
            data = data[2:]
        if addr["nmi"] is True:
            if len(data) < 2:
                raise ROMException("No nmi address found in file.")
            addr["nmi"] = data[0] + data[1] * 256
            data = data[2:]
        if addr["nmi"] is not None:
            self.add_nmi_vector(addr["nmi"])
        if addr["reset"] is True:
            if len(data) < 2:
                raise ROMException("No reset address found in file.")
            addr["reset"] = data[0] + data[1] * 256
            data = data[2:]
        if addr["reset"] is not None:
            self.add_reset_vector(addr["reset"])
        if addr["irq"] is True:
            if len(data) < 2:
                raise ROMException("No irq address found in file.")
            addr["irq"] = data[0] + data[1] * 256
            data = data[2:]
        if addr["irq"] is not None:
            self.add_irq_vector(addr["irq"])
        self.add_binary_data(data, addr["data"])

    def _parse_memory_chunks(self, data: bytes):
        """Parse classic memory chunk binary data and load into ROM."""
        i = 0
        while i < len(data):
            try:
                end = data.index(b"\n", i)
            except ValueError:
                raise ROMException("Truncated memory chunk header")
            line = data[i:end].decode("ascii").rstrip()
            i = end + 1
            m = re.match(r"^(\S+)\s+(\S+)\s+(\S+)$", line)
            if not m:
                raise ROMException(f"Invalid memory chunk header: {line!r}")
            try:
                addr = ROM.parse_int(m.group(1))
                length = ROM.parse_int(m.group(2))
                crc = ROM.parse_int(m.group(3))
            except ValueError as e:
                raise ROMException(str(e)) from e
            chunk = data[i : i + length]
            if len(chunk) != length or binascii.crc32(chunk) != crc:
                raise ROMException(f"Invalid CRC in block address: ${addr:04X}")
            self.add_binary_data(chunk, addr)
            i += length

    def add_rom_file(self, file: str):
        """Add ROM data from file."""
        with open(file, "rb") as f:
            # Decode first line as cp850 because binary garbage can
            # raise here before our better message gets to the user.
            command = f.readline().decode("cp850")
            if not re.match(f"^#!.*{SCRIPT_NAME}", command, re.IGNORECASE):
                raise ROMException(f"Invalid ROM file: {file}")
            while True:
                line = f.readline()
                if not line:
                    break  # EOF
                header = line.decode("ascii").rstrip("\r\n")
                if not header:
                    break  # empty line signals end
                if not header.startswith("#>"):
                    raise ROMException(f"Invalid ROM file: {file}")
                parts = header[2:].split(None, 2)
                if len(parts) < 2:
                    raise ROMException(f"Invalid asset header: {header!r}")
                try:
                    asset_len = ROM.parse_int(parts[0])
                    # CRC is present for tooling but not verified at load time
                except ValueError as e:
                    raise ROMException(str(e)) from e
                asset_name = parts[2].strip() if len(parts) > 2 else None
                asset_data = f.read(asset_len)
                if len(asset_data) != asset_len:
                    raise ROMException(
                        f"Truncated asset data{f' for: {asset_name}' if asset_name else ''}"
                    )
                if asset_name is None:
                    self._parse_memory_chunks(asset_data)
                else:
                    self.add_asset(asset_name, asset_data)

    def allocate_rom(self, addr: int, length: int):
        """Marks a range of memory as used."""
        if addr + length > 0x1000000 or addr < 0 or length < 0:
            raise ROMException(
                f"ROM address invalid ${addr:04X} or length ${length:03X}"
            )
        for i in range(length):
            if self.alloc.get(addr + i):
                raise ROMException(f"ROM data already exists at ${addr+i:04X}")
            self.alloc[addr + i] = 1

    def has_reset_vector(self) -> bool:
        """Returns true if $FFFC and $FFFD have been set."""
        return bool(self.alloc.get(0xFFFC) and self.alloc.get(0xFFFD))

    def next_rom_data(self, addr: int):
        """Find next up-to-1k chunk starting at addr, never crossing 64k page."""
        # Bounded by what was allocated rather than by the address space: the
        # scan to $1000000 costs a third of a second per image, which every
        # generated ROM and every send_rom was paying to find nothing.
        for addr in range(addr, max(self.alloc, default=-1) + 1):
            if self.alloc.get(addr):
                page_end = (addr | 0xFFFF) + 1
                length = 0
                while self.alloc.get(addr + length):
                    length += 1
                    if length == 1024 or addr + length == page_end:
                        break
                return addr, bytearray(self.data[addr + i] for i in range(length))
        return None, None

    def to_bytes(self) -> bytes:
        """The .rp6502 image: the magic line, the memory chunks as one null
        asset, then the named ones."""
        out = f"#!{SCRIPT_NAME}\r\n".encode("ascii")
        chunks = b""
        addr, data = self.next_rom_data(0)
        while data is not None:
            header = f"${addr:04X} ${len(data):03X} ${binascii.crc32(data):08X}\r\n"
            chunks += header.encode("ascii") + bytes(data)
            addr += len(data)
            addr, data = self.next_rom_data(addr)
        if chunks:
            out += f"#>${len(chunks):08X} ${binascii.crc32(chunks):08X}\r\n".encode(
                "ascii"
            )
            out += chunks
        for asset_name, asset_data in self.assets:
            out += (
                f"#>${len(asset_data):08X} "
                f"${binascii.crc32(asset_data):08X} {asset_name}\r\n"
            ).encode("ascii")
            out += asset_data
        return out

    def write(self, path) -> int:
        """The image on disk. Returns its length."""
        data = self.to_bytes()
        with open(path, "wb") as file:
            file.write(data)
        return len(data)


class Symbols(dict):
    """The names and values of a .sym file. A name with two values is not
    in the table but in self.ambiguous. A missing file gives an empty table."""

    def __init__(self, path):
        super().__init__()
        self.ambiguous = set()
        try:
            with open(path) as f:
                for line in f:
                    name, value = line.split()
                    value = int(value, 0)
                    if name in self.ambiguous:
                        continue
                    if self.get(name, value) != value:
                        del self[name]
                        self.ambiguous.add(name)
                    else:
                        self[name] = value
        except FileNotFoundError:
            pass

    def __missing__(self, name):
        if name in self.ambiguous:
            raise KeyError(f"{name} is ambiguous")
        raise KeyError(name)

    @staticmethod
    def text(value: int) -> str:
        """The text of a value in a .sym file: hex from 0x0000 to 0xFFFF, else decimal."""
        return f"0x{value:04X}" if 0 <= value <= 0xFFFF else str(value)

    @staticmethod
    def write(path, pairs):
        """Writes (name, value) pairs as a .sym file and returns its lines as pairs.
        Repeated pairs make one line. Values outside int64 are left out."""
        lines = sorted({(n, v) for n, v in pairs if -(1 << 63) <= v < (1 << 63)})
        with open(path, "w") as f:
            f.writelines(f"{name} {Symbols.text(value)}\n" for name, value in lines)
        return lines

    @staticmethod
    def _split_command(command: str) -> list:
        """The words of a command from compile_commands.json."""
        if os.name != "nt":
            return shlex.split(command)
        # Windows commands use Windows quoting, and shlex removes the backslashes.
        parse = ctypes.windll.shell32.CommandLineToArgvW
        parse.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_int)]
        parse.restype = ctypes.POINTER(wintypes.LPWSTR)
        argc = ctypes.c_int()
        argv = parse(command, ctypes.byref(argc))
        words = [argv[i] for i in range(argc.value)]
        kernel32.LocalFree(argv)
        return words

    @staticmethod
    def _sources(commands: str, objects: str) -> list:
        """The compile_commands.json entries of the C sources that have their
        object under the objects folder, each with the words before its -o."""
        with open(commands) as f:
            entries = json.load(f)
        objects = os.path.abspath(objects) + os.sep
        sources = []
        for entry in entries:
            words = Symbols._split_command(entry["command"])
            at = len(words) - 1 - words[::-1].index("-o")
            obj = os.path.normpath(os.path.join(entry["directory"], words[at + 1]))
            if obj.startswith(objects) and entry["file"].endswith(".c"):
                sources.append((entry, words[:at]))
        return sources

    @staticmethod
    def _make_deps(path: str, directory: str) -> list:
        """The prerequisites in a Make dependency file, as absolute paths."""
        with open(path) as f:
            text = re.sub(r"\\\r?\n", " ", f.read())
        deps = []
        for word in re.split(r"(?<!\\)\s+", text):
            if word and not word.endswith(":"):
                word = word.replace("\\ ", " ").replace("\\#", "#").replace("$$", "$")
                deps.append(os.path.normpath(os.path.join(directory, word)))
        return deps

    @staticmethod
    def _first_line(files: list, pattern: str):
        """The path:line of the first line that matches pattern, or None."""
        regex = re.compile(pattern)
        for path in files:
            with open(path, errors="replace") as f:
                for number, line in enumerate(f, 1):
                    if regex.match(line):
                        return f"{path}:{number}"
        return None

    @staticmethod
    def _run_compiler(argv: list, cwd: str):
        """Runs a compiler, with its stderr in its stdout."""
        return subprocess.run(
            argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
        )

    @staticmethod
    def _read_source(entry: dict, flags: list, cc65: bool, work: str, n: int):
        """The (name, value) pairs of the integer macros of one C source, the
        files that its compile reads, and an error for each struct over 65535 bytes."""
        directory, source = entry["directory"], entry["file"]

        # A function-like macro has "(" after its name, so it does not match.
        listing = os.path.join(work, f"{n}.dm")
        mode = ["-E", "-Wc", "-dM"] if cc65 else ["-dM", "-E"]
        result = Symbols._run_compiler(
            flags + mode + ["-o", listing, source], directory
        )
        if result.returncode:
            sys.exit(result.stdout.rstrip("\n"))
        names = []
        with open(listing, errors="replace") as f:
            for line in f:
                match = re.match(r"#define ([A-Za-z_]\w*) +\S", line)
                if match:
                    names.append(match.group(1))

        # Each macro becomes two constants: its value, and 1 if it is negative.
        # "| 0" is an error for floats and pointers. A name with an error is removed.
        stub = os.path.join(work, f"{n}.c")
        asm = os.path.join(work, f"{n}.s")
        dep = os.path.join(work, f"{n}.d")
        wide = "unsigned long" if cc65 else "unsigned long long"
        if cc65:
            mode = ["--create-full-dep", dep]
        else:
            mode = ["-fno-lto", "-ferror-limit=0", "-fno-color-diagnostics"]
            mode += ["-MD", "-MF", dep]
        # CMake 3.21 reads a -S in the cc65 wrapper call as its own option, which
        # takes the next word as a folder, so -S comes right before the stub.
        # cc65 aborts on an error message with a long file name, so the stub is
        # named by its path from the compile folder.
        argv = flags + ["-I", os.path.dirname(source)] + mode
        argv += ["-o", asm, "-S", os.path.relpath(stub, directory)]
        # The stub includes the source by its real path from the stub's folder,
        # so the compiler opens this source and no other file of the same name.
        include = os.path.relpath(os.path.realpath(source), os.path.realpath(work))
        include = include.replace(os.sep, "/")
        live = list(range(len(names)))
        while True:
            with open(stub, "w") as f:
                f.write(f'#include "{include}"\n')
                for i in live:
                    f.write(f"const {wide} _sym_{i} = ({wide})(({names[i]}) | 0);\n")
                    f.write(f"const unsigned char _neg_{i} = ({names[i]}) < 0;\n")
            result = Symbols._run_compiler(argv, directory)
            if result.returncode == 0:
                break
            # Lines 2+2k and 3+2k of the stub hold the name live[k].
            bad = set()
            for match in re.finditer(
                r"^(.+?):(\d+):(?:\d+:)? (?:fatal error|error|fatal):",
                result.stdout,
                re.M | re.I,
            ):
                k = (int(match.group(2)) - 2) // 2
                path = os.path.normpath(os.path.join(directory, match.group(1)))
                if path == stub and 0 <= k < len(live):
                    bad.add(live[k])
            if not bad:
                sys.exit(result.stdout.rstrip("\n"))
            live = [i for i in live if i not in bad]

        # cc65 writes ".dword $XXXXXXXX" and ".byte $XX", llvm-mos writes decimal.
        if cc65:
            pattern = r"^__(sym|neg)_(\d+):\s*\.(?:dword|byte)\s+\$([0-9A-F]+)"
            base, bits = 16, 32
        else:
            pattern = r"^_(sym|neg)_(\d+):\s*\.(?:quad|byte)\s+(-?\d+)"
            base, bits = 10, 64
        with open(asm) as f:
            found = {
                (m.group(1), int(m.group(2))): int(m.group(3), base)
                for m in re.finditer(pattern, f.read(), re.M)
            }
        pairs = []
        for i in live:
            value = found[("sym", i)] & ((1 << bits) - 1)
            if found[("neg", i)]:
                value -= 1 << bits
            pairs.append((names[i], value))

        files = []
        for path in Symbols._make_deps(dep, directory):
            if path != stub and path not in files:
                files.append(path)

        # cc65 gives an error for a struct over 65535 bytes, llvm-mos does not.
        errors = []
        if not cc65:
            argv = flags + [
                "-Xclang",
                "-fdump-record-layouts-complete",
                "-fsyntax-only",
            ]
            layouts = subprocess.run(
                argv + [source],
                cwd=directory,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
            ).stdout
            for block in layouts.split("*** Dumping AST Record Layout")[1:]:
                head = re.search(r"^\s*0 \| (struct|union) (.+)$", block, re.M)
                size = re.search(r"\[sizeof=(\d+),", block)
                if not head or not size or int(size.group(1)) <= 0xFFFF:
                    continue
                kind, name = head.groups()
                unnamed = re.fullmatch(r".*\(unnamed at (.+?)(:\d+:\d+)\)", name)
                if unnamed:
                    path = os.path.normpath(os.path.join(directory, unnamed.group(1)))
                    where, what = path + unnamed.group(2), kind
                else:
                    where = Symbols._first_line(
                        files, rf".*\b{kind}\s+{re.escape(name)}\b"
                    )
                    where, what = where or source, f"{kind} {name}"
                errors.append(
                    f"{where}: error: {what} is {size.group(1)} bytes, larger than 65535"
                )
        return pairs, files, errors

    @staticmethod
    def _dbg_symbols(path: str) -> list:
        """The (name, value) pairs of a cc65 .dbg file. cc65 puts _ before a C
        name, so a name _x is also given as x."""
        pairs = []
        with open(path, errors="replace") as f:
            for line in f:
                if not line.startswith("sym\t"):
                    continue
                fields = dict(re.findall(r'(\w+)=("[^"]*"|[^,\n]*)', line[4:]))
                name = fields.get("name", "").strip('"')
                skip = fields.get("type") == "imp" or name.startswith("@")
                if skip or "val" not in fields:
                    continue
                value = int(fields["val"], 0)
                pairs.append((name, value))
                if name.startswith("_") and len(name) > 1:
                    pairs.append((name[1:], value))
        return pairs

    @staticmethod
    def _elf_symbols(path: str) -> list:
        """The (name, value) pairs of the symbol table of an ELF32 file."""
        with open(path, "rb") as f:
            data = f.read()
        (shoff,) = struct.unpack_from("<I", data, 0x20)
        shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
        sections = [
            struct.unpack_from("<10I", data, shoff + i * shentsize)
            for i in range(shnum)
        ]
        pairs = []
        for _, kind, _, _, offset, size, link, _, _, _ in sections:
            if kind != 2:  # SHT_SYMTAB
                continue
            strings = sections[link][4]
            for at in range(offset, offset + size, 16):
                name, value, _, info = struct.unpack_from("<IIIB", data, at)
                if not name or (info & 0xF) in (3, 4):  # STT_SECTION, STT_FILE
                    continue
                end = data.index(b"\0", strings + name)
                pairs.append((data[strings + name : end].decode(), value))
        return pairs

    @staticmethod
    def build(linked, out, commands, objects, work, aligns) -> list:
        """Writes the .sym file of a linked program. Returns the error lines."""
        target = os.path.splitext(os.path.basename(out))[0]
        errors = []
        patterns = []
        for regex in aligns:
            try:
                patterns.append((regex, re.compile(regex)))
            except re.error as e:
                errors.append(f'rp6502_align({target} "{regex}"): {e}')

        cc65 = linked.endswith(".dbg")
        pairs = Symbols._dbg_symbols(linked) if cc65 else Symbols._elf_symbols(linked)
        work = os.path.abspath(work)
        os.makedirs(work, exist_ok=True)
        sources = Symbols._sources(commands, objects)
        files = []
        with concurrent.futures.ThreadPoolExecutor(len(sources) or 1) as pool:
            jobs = [
                pool.submit(Symbols._read_source, entry, flags, cc65, work, n)
                for n, (entry, flags) in enumerate(sources)
            ]
            for job in jobs:
                found, read, records = job.result()
                pairs += found
                files += [path for path in read if path not in files]
                errors += [line for line in records if line not in errors]

        lines = Symbols.write(out, pairs)
        for number, (name, value) in enumerate(lines, 1):
            regex = next((r for r, p in patterns if p.fullmatch(name)), None)
            if regex is None or (0 <= value <= 0xFFFF and value % 2 == 0):
                continue
            why = "odd" if 0 <= value <= 0xFFFF else "outside 0x0000-0xFFFF"
            where = Symbols._first_line(files, rf"\s*#\s*define\s+{re.escape(name)}\b")
            where = where or f"{out}:{number}"
            errors.append(
                f"{where}: error: {name} is {Symbols.text(value)}, which is {why}; "
                f'rp6502_align({target} "{regex}")'
            )
        return errors


class ScriptError(Exception):
    """A script command that failed, or an emulator that exited with an error.
    status holds the fields of the status line of a failed command."""

    def __init__(self, message, status=None):
        super().__init__(message)
        self.status = status or {}


class Emu:
    """An emulator that a test drives with script commands. The command is
    $RP6502_EMU --script - <args> <sys.argv[1:]>, with the ROM arguments after
    -- from one of the two. A with statement closes the emulator, or kills it
    when the block raises."""

    # The options of the emulator that take a value, and the one option that
    # can repeat, from src/host/sokol/cli.
    _VALUED = {"screenshot", "frames", "scale", "filter", "script", "install",
               "save-dir", "bgcolor", "phi2", "cp", "memseed", "memfill", "ini"}
    _REPEATABLE = {"install"}

    def __init__(self, *args):
        cmd = Emu.command("--script", "-", *args)
        self.sym = Symbols(os.path.splitext(Emu._boots(cmd[1:]))[0] + ".sym")
        self._proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            encoding="utf-8",
            errors="replace",
        )
        self.cmd("reply")

    @staticmethod
    def command(*args) -> list:
        """The command line $RP6502_EMU <args> <sys.argv[1:]>, with the ROM
        arguments after -- from one of the two. An option other than --install
        or a ROM in both, or ROM arguments in both, raise ScriptError."""
        given, given_rom = Emu._split(list(args))
        argv, argv_rom = Emu._split(sys.argv[1:])
        given_names, given_roms = Emu._names(given)
        argv_names, argv_roms = Emu._names(argv)
        for name in given_names:
            if name in argv_names and name not in Emu._REPEATABLE:
                raise ScriptError(f"--{name} is in Emu() and in the arguments of the test")
        if given_roms and argv_roms:
            raise ScriptError("a ROM is in Emu() and in the arguments of the test")
        if given_rom and argv_rom:
            raise ScriptError(
                "ROM arguments are in Emu() and in the arguments of the test")
        cmd = [os.environ["RP6502_EMU"]] + given + argv
        rom_args = given_rom or argv_rom
        if rom_args:
            cmd += ["--"] + rom_args
        return cmd

    @staticmethod
    def _split(words) -> tuple:
        """Splits <words> at the first -- into the options and the ROM
        arguments."""
        if "--" not in words:
            return words, []
        split = words.index("--")
        return words[:split], words[split + 1 :]

    @staticmethod
    def _names(options) -> tuple:
        """The option names and the ROMs in <options>. An empty word is no
        ROM, as for the emulator."""
        names, roms = [], []
        i = 0
        while i < len(options):
            word = options[i]
            if word.startswith("--") and len(word) > 2:
                name = word[2:].split("=", 1)[0]
                names.append(name)
                if "=" not in word and name in Emu._VALUED:
                    i += 1
            elif word:
                roms.append(word)
            i += 1
        return names, roms

    @staticmethod
    def _boots(words) -> str:
        """The ROM that the emulator boots: the first .rp6502 before -- that is
        not the value of --install, else the first --install value, else ''."""
        installs = []
        for before, word in zip([""] + words, words):
            if word == "--":
                break
            if before == "--install":
                installs.append(word)
            elif word.endswith(".rp6502"):
                return word
        return installs[0] if installs else ""

    def cmd(self, line):
        """Sends one script command and returns the text after ok. A fail
        raises ScriptError with the line, the reason and the rest of stdout."""
        self._proc.stdin.write(line + "\n")
        self._proc.stdin.flush()
        reply = self._proc.stdout.readline()
        if not reply:
            raise ScriptError(f"{line}: the emulator exited before it answered")
        reply = reply.rstrip("\n")
        if reply.startswith("fail"):
            status = self._proc.stdout.readline()
            rest = self._proc.stdout.read()
            # An emulator from an older release prints no status line.
            if not status.startswith("PC="):
                status, rest = "", status + rest
            status = status.rstrip("\n")
            raise ScriptError(f"{line}: {reply[5:]}\n{status}\n{rest}".rstrip("\n"),
                              Emu._fields(status) if status else {})
        if reply != "ok" and not reply.startswith("ok "):
            raise ScriptError(f"{line}: unexpected answer {reply!r}")
        return reply[3:]

    def status(self) -> dict:
        """Sends status and returns its fields as a dict of ints, with ROM as a
        str."""
        return Emu._fields(self.cmd("status"))

    @staticmethod
    def _fields(line) -> dict:
        """The fields of a status line. ROM is last, and its value is the rest
        of the line."""
        fields, _, rom = line.partition(" ROM=")
        status = {}
        for field in fields.split():
            name, _, value = field.partition("=")
            status[name] = int(value[1:], 16) if value.startswith("$") else int(value)
        status["ROM"] = rom
        return status

    def close(self):
        """Ends the script and waits for the emulator to exit. An exit code
        other than 0 raises ScriptError."""
        self._proc.communicate()
        if self._proc.returncode:
            raise ScriptError(f"the emulator exited with code {self._proc.returncode}")

    def __enter__(self):
        return self

    def __exit__(self, kind, *_):
        if kind is None:
            self.close()
        else:
            self._proc.kill()
            self._proc.communicate()


def exec_args():
    # Standard library argument parser
    class CustomFormatter(argparse.HelpFormatter):
        def __init__(self, prog):
            super().__init__(prog, max_help_position=27)

        def _format_args(self, action, default_metavar):
            # The usage shows the metavar of REMAINDER, not "...".
            if action.nargs == argparse.REMAINDER:
                return f"[{action.metavar} ...]"
            return super()._format_args(action, default_metavar)

    parser = argparse.ArgumentParser(
        description="Run ROMs on an RP6502-PICO, and package ROMs.",
        epilog="For the options of a subcommand, add -h after it, as in: "
        "rp6502.py run -h",
        formatter_class=CustomFormatter,
    )
    # Support for older projects that give the options before the subcommand.
    for flags, dest, default in (
        (("-a", "--address"), "address", None),
        (("-o",), "out", None),
        (("-n", "--nmi"), "nmi", None),
        (("-r", "--reset"), "reset", None),
        (("-i", "--irq"), "irq", None),
        (("-c", "--config"), "config", None),
        (("-d", "-D", "--device"), "device", None),
        (("-k", "--key"), "key", None),
        (("-w", "--workdir"), "workdir", None),
        (("-t", "--term"), "term", None),
    ):
        parser.add_argument(*flags, dest=dest, default=default, help=argparse.SUPPRESS)
    sp = parser.add_subparsers(dest="command", required=True, metavar="subcommand")

    def subcommand(name, text):
        return sp.add_parser(
            name, help=text, description=text, formatter_class=CustomFormatter
        )

    def option(sub, *flags, **kwargs):
        # An option not given after the subcommand keeps its value from before
        # the subcommand.
        sub.add_argument(*flags, default=argparse.SUPPRESS, **kwargs)

    def connection(sub):
        option(
            sub,
            "-c",
            "--config",
            dest="config",
            metavar="file",
            help="The settings file.",
        )
        option(
            sub,
            "-d",
            "--device",
            dest="device",
            metavar="dev",
            help="The serial port, or the hostname or IP address for telnet. "
            f"Default {Console.default_device()}.",
        )
        # Hidden alias for anyone used to minicom -D /dev/
        option(sub, "-D", dest="device", help=argparse.SUPPRESS)
        option(
            sub,
            "-k",
            "--key",
            dest="key",
            metavar="key",
            help="The passkey that telnet requires.",
        )
        option(
            sub,
            "-w",
            "--workdir",
            dest="workdir",
            metavar="dir",
            help="The directory to work in, from the root of the current drive.",
        )

    def terminal(sub, when):
        option(
            sub,
            "-t",
            "--term",
            dest="term",
            metavar="bool",
            help=f"Open a terminal on the console {when}. Default True.",
        )

    run = subcommand("run", "Upload a ROM to the RP6502-PICO, load it, and open a terminal.")
    connection(run)
    terminal(run, "after the load")
    run.add_argument("filename", nargs=1, metavar="rom", help="The ROM.")
    # Everything after the ROM is the ROM's argv, like `LOAD rom args...`.
    run.add_argument(
        "rom_args",
        nargs=argparse.REMAINDER,
        metavar="args",
        help="Arguments for the ROM.",
    )

    upload = subcommand("upload", "Upload files to the RP6502-PICO.")
    connection(upload)
    option(
        upload,
        "-o",
        dest="out",
        metavar="name",
        help="The destination name, when you upload one file.",
    )
    upload.add_argument("filename", nargs="+", metavar="file", help="A file to upload.")

    term = subcommand("term", "Open a terminal on the console.")
    connection(term)

    basic = subcommand(
        "basic",
        "Send a BASIC program to the BASIC installed on the RP6502-PICO, and run it.",
    )
    connection(basic)
    terminal(basic, "after the run")
    basic.add_argument("filename", nargs=1, metavar="program", help="The BASIC program.")

    create = subcommand("create", "Package a file into a ROM, and merge ROMs into it.")
    option(create, "-o", dest="out", metavar="out", help="The ROM file to write.")
    option(
        create,
        "-a",
        "--address",
        dest="address",
        metavar="addr",
        help="An address, RAM(<x>), XRAM(<x>), an asset name, or 'file' for the "
        "address at the start of the file. <x> is a number or a symbol of --sym.",
    )
    for flags, dest, vector in (
        (("-r", "--reset"), "reset", "reset vector at $FFFC-$FFFD"),
        (("-i", "--irq"), "irq", "IRQ vector at $FFFE-$FFFF"),
        (("-n", "--nmi"), "nmi", "NMI vector at $FFFA-$FFFB"),
    ):
        option(
            create,
            *flags,
            dest=dest,
            metavar="addr",
            help=f"The {vector}: an address, or 'file' for the address in the file.",
        )
    create.add_argument(
        "--replace",
        action="append",
        default=[],
        metavar="name",
        help="A later asset with this name replaces an earlier one. Repeatable.",
    )
    create.add_argument(
        "--sym",
        nargs="?",
        metavar="symfile",
        help="The .sym file with the symbols for -a RAM(<x>) and -a XRAM(<x>).",
    )
    create.add_argument(
        "filename",
        nargs=1,
        metavar="file",
        help="The file to package. Without -a, a ROM to merge.",
    )
    create.add_argument("roms", nargs="*", metavar="rom", help="A ROM to merge.")

    sym = subcommand("sym", "Write the .sym file of a linked program.")
    option(sym, "-o", dest="out", metavar="out", help="The .sym file to write.")
    sym.add_argument(
        "--commands",
        required=True,
        metavar="file",
        help="The compile_commands.json of the build.",
    )
    sym.add_argument(
        "--objects",
        required=True,
        metavar="dir",
        help="The object directory of the target, CMakeFiles/<target>.dir.",
    )
    sym.add_argument(
        "--work", required=True, metavar="dir", help="A directory for temporary files."
    )
    sym.add_argument(
        "--align",
        action="append",
        default=[],
        metavar="regex",
        help="The value of each symbol that matches must be even and within "
        "0x0000-0xFFFF. Repeatable.",
    )
    sym.add_argument("filename", nargs=1, metavar="linked", help="The linker output.")

    args = parser.parse_args()
    if args.command in ("create", "sym") and not args.out:
        parser.error(f"{args.command} requires -o")
    if args.command == "create":
        args.filename += args.roms

    # Config file (shared with the emulator, which owns it in ImGui ini format).
    if args.config:
        pico = f"{SCRIPT_NAME}][PICO"
        # The older name of the section, still read and never renamed.
        launch = f"{SCRIPT_NAME}][Launch"
        config = configparser.ConfigParser(interpolation=None)
        # Python 3.9 and older end a section name at the first "]".
        config.SECTCRE = re.compile(r"\[(?P<header>.+)\]")
        try:
            if os.path.exists(args.config):
                # configparser.read() silently skips unreadable files
                if not os.access(args.config, os.R_OK):
                    raise PermissionError("permission denied")
                config.read(args.config)
            if not config.has_section(pico) and not config.has_section(launch):
                old = (
                    dict(config[SCRIPT_NAME]) if config.has_section(SCRIPT_NAME) else {}
                )
                pick = lambda k: old.get(k, "")
                # User always sees the full list of keys, even when blank.
                config[pico] = {
                    "device": pick("device") or args.device or Console.default_device(),
                    "key": pick("key") or args.key or "",
                    "workdir": pick("workdir") or args.workdir or "",
                    "args": pick("args"),
                    "term": pick("term") or args.term or "True",
                }
                # Inserted as text at the front of the file, where the user
                # finds it. A configparser rewrite would lowercase the keys of
                # the emulator's sections.
                section = configparser.ConfigParser(interpolation=None)
                section[pico] = config[pico]
                text = io.StringIO()
                section.write(text)
                rest = b""
                if os.path.exists(args.config):
                    with open(args.config, "rb") as cfg:
                        rest = cfg.read()
                with open(args.config, "wb") as cfg:
                    cfg.write(text.getvalue().encode() + rest)
        except (configparser.Error, OSError) as e:
            raise RuntimeError(f"Cannot load config {args.config}: {e}")
        # An option on the command line takes precedence over the file.
        sec = config[pico if config.has_section(pico) else launch]
        args.workdir = args.workdir or sec.get("workdir", "") or None
        args.device = args.device or sec.get("device", "") or None
        args.key = args.key or sec.get("key", "") or None
        args.term = args.term or sec.get("term", "") or None
        args.config_args = sec.get("args", "")

    if args.device is None:
        args.device = Console.default_device()
    if args.term is None:
        args.term = "True"

    if args.workdir:
        args.workdir = args.workdir.strip().strip("/") or None

    # Because parser is bad at bool
    if args.term.lower() in ["t", "true"] or (args.term.isdigit() and args.term != "0"):
        args.term = True
    else:
        args.term = False

    # Additional validation and conversion
    def str_to_address(parser, s, errmsg):
        """Parse an address string; returns int, True for 'file', or calls parser.error."""
        if s:
            if s.lower() == "file":
                return True
            try:
                return ROM.parse_int(s)
            except ValueError:
                parser.error(f"argument {errmsg}: invalid address: '{s}'")

    def str_to_address_or_name(s):
        """Returns int for a parseable hex number, True for 'file', string for asset name."""
        if s:
            if s.lower() == "file":
                return True
            try:
                return ROM.parse_int(s)
            except ValueError:
                return s
        return None

    def timed_upload(console, file, name):
        """Upload with timing and throughput logging."""
        file.seek(0)
        total_bytes = file.seek(0, 2)
        file.seek(0)
        start = time.monotonic()
        console.upload(file, name)
        elapsed = time.monotonic() - start
        if elapsed > 0:
            rate = total_bytes / elapsed
            print(
                f"[{SCRIPT_FILE}] {total_bytes} bytes in {elapsed:.2f}s ({rate:.0f} bytes/s)"
            )

    def config_rom_args():
        """The ROM's argv[1..] from the config 'args' key, shell-style quoted."""
        try:
            return shlex.split(getattr(args, "config_args", ""))
        except ValueError as e:
            raise RuntimeError(f"Cannot parse 'args' in {args.config}: {e}")

    # Open console and extend error with a hint about the config file
    if args.command in ["term", "run", "upload", "basic"]:
        if args.config:
            print(f"[{SCRIPT_FILE}] Using device config in {args.config}")
        if args.key:
            host, _, port_str = args.device.rpartition(":")
            if host and port_str.isdigit():
                port_num = int(port_str)
            else:
                host = args.device
                port_num = 23
            print(f"[{SCRIPT_FILE}] Connecting to {host}:{port_num}")
            transport = TelnetDevice(host, port_num, args.key)
        else:
            print(f"[{SCRIPT_FILE}] Opening device {args.device}")
            transport = SerialDevice(args.device)
        console = Console(transport)
        console.send_break()
        if args.workdir:
            console.command(f"CD {console.quote('/' + args.workdir)}")

    if args.command == "term":
        code_page = console.code_page()
        console.terminal(code_page)

    if args.command == "run":
        if args.term:
            code_page = console.code_page()
        print(f"[{SCRIPT_FILE}] Reading ROM {args.filename[0]}")
        rom = ROM()
        rom.add_rom_file(args.filename[0])
        print(f"[{SCRIPT_FILE}] Uploading ROM")
        with open(args.filename[0], "rb") as f:
            timed_upload(console, f, os.path.basename(args.filename[0]))
        print(f"[{SCRIPT_FILE}] Loading ROM")
        rom_args = args.rom_args
        if rom_args and rom_args[0] == "--":  # REMAINDER keeps a leading "--"
            rom_args = rom_args[1:]
        if not rom_args:
            rom_args = config_rom_args()
        console.load(os.path.basename(args.filename[0]), rom_args)
        if args.term:
            console.terminal(code_page)

    if args.command == "upload":
        for file in args.filename:
            print(f"[{SCRIPT_FILE}] Uploading {file}")
            with open(file, "rb") as f:
                if len(args.filename) == 1 and args.out != None:
                    dest = args.out
                else:
                    dest = os.path.basename(file)
                timed_upload(console, f, dest)

    if args.command == "basic":
        code_page = console.code_page()
        print(f"[{SCRIPT_FILE}] Starting BASIC")
        console.serial.write(b"BASIC\r")
        console.wait_for_prompt("READY\r\n")
        print(f"[{SCRIPT_FILE}] Uploading program")
        with open(args.filename[0], "r", encoding="utf-8") as f:
            for line_num, line in enumerate(f):
                # Wait the perfect amount of time it takes to parse the line
                # by waiting for a character to echo, then deleting it.
                console.serial.write(b"0")
                echo = console.serial.read(1)
                console.serial.write(b"\b")
                if echo != b"0":
                    msg = console.serial.read_until(b"\r\n").decode("ascii").strip()
                    raise RuntimeError(f"Line {line_num}: {msg}")
                console.serial.write(line.encode(code_page) + b"\r")
                console.serial.read_until(b"\r\n")
        print(f"[{os.path.basename(__file__)}] Running program")
        console.serial.write(b"RUN\r")
        if args.term:
            console.terminal(code_page)

    if args.command == "create":
        memory = re.fullmatch(r"(X?RAM)\((.+)\)", args.address or "")
        if memory:
            form, name = memory.groups()
            try:
                value = ROM.parse_int(name)
                shown = args.address
            except ValueError:
                if not args.sym:
                    raise RuntimeError(
                        f"{args.address}: {name} is not a number, and the ROM has no .sym file"
                    )
                symbols = Symbols(args.sym)
                if name in symbols.ambiguous:
                    raise RuntimeError(f"{args.address}: {name} is ambiguous")
                if name not in symbols:
                    raise RuntimeError(f"{args.address}: {name} is not in {args.sym}")
                value = symbols[name]
                shown = f"{args.address}: {name} is {Symbols.text(value)}, which"
            end = 0xFFFF if form == "RAM" else 0x1FFFF
            if not 0 <= value <= end:
                raise RuntimeError(f"{shown} is outside 0x0000-0x{end:X}")
            args.address = value | 0x10000 if form == "XRAM" else value
        else:
            args.address = str_to_address_or_name(args.address)
        args.nmi = str_to_address(parser, args.nmi, "-n/--nmi")
        args.reset = str_to_address(parser, args.reset, "-r/--reset")
        args.irq = str_to_address(parser, args.irq, "-i/--irq")
        print(f"[{os.path.basename(__file__)}] Creating {args.out}")
        rom = ROM(args.replace)
        if args.address is None:
            for vec_value, vec_flag in (
                (args.nmi, "-n/--nmi"),
                (args.reset, "-r/--reset"),
                (args.irq, "-i/--irq"),
            ):
                if vec_value is True:
                    parser.error(
                        f"argument {vec_flag}: 'file' requires a binary asset (-a)"
                    )
            if args.nmi is not None:
                rom.add_nmi_vector(args.nmi)
            if args.reset is not None:
                rom.add_reset_vector(args.reset)
            if args.irq is not None:
                rom.add_irq_vector(args.irq)
            extras_start = 0
        else:
            print(
                f"[{os.path.basename(__file__)}] Adding binary asset {args.filename[0]}"
            )
            if isinstance(args.address, str):
                with open(args.filename[0], "rb") as f:
                    rom.add_asset(args.address, f.read())
            else:
                rom.add_binary_file(
                    args.filename[0],
                    data=args.address,
                    nmi=args.nmi,
                    reset=args.reset,
                    irq=args.irq,
                )
            extras_start = 1
        for file in args.filename[extras_start:]:
            print(f"[{os.path.basename(__file__)}] Adding ROM asset {file}")
            rom.add_rom_file(file)
        rom.write(args.out)

    if args.command == "sym":
        errors = Symbols.build(
            args.filename[0],
            args.out,
            args.commands,
            args.objects,
            args.work,
            args.align,
        )
        # Raw lines, without the [rp6502.py] prefix, so IDEs read file:line.
        for line in errors:
            print(line, file=sys.stderr)
        if errors:
            sys.exit(1)


# This file may be included or run like a program.
if __name__ == "__main__":
    # VSCode SIGKILLs the terminal while in raw mode, return to cooked mode.
    if "tty" in globals() and sys.stdin.isatty():
        os.system("stty sane")
    try:
        exec_args()
    except Exception as e:
        # Exceptions to show in VS Code output instead of Python debugger.
        if not isinstance(
            e,
            (
                ROMException,
                FileNotFoundError,
                TimeoutError,
                socket.timeout,
                RuntimeError,
                ConnectionError,
                socket.gaierror,
            ),
        ):
            raise
        # Unresolved variable substitutions like ${command:cmake.launchTargetPath}.
        if re.search(r"\$\{[^}]*\}", str(e)):
            print(f"[{SCRIPT_FILE}] Check build for failures", file=sys.stderr)
        print(f"[{SCRIPT_FILE}] {e}", file=sys.stderr)
        os._exit(1)  # Special exit without raising debugger.
