#!/usr/bin/env python3
#
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
# SPDX-License-Identifier: Unlicense

# RP6502-RIA Developer tool

import os
import re
import time
import binascii
import argparse
import configparser
import platform
import sys
import select
import ctypes
import json
import glob
import shlex
import shutil
import socket
import subprocess
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
        """Quote a name/arg for the monitor parser (LOAD/UPLOAD/CD).

        The monitor stores the decoded bytes verbatim as an OEM code-page
        filename (FatFs FF_LFN_UNICODE=0), so encode to the device code page,
        not UTF-8; the parser decodes \\NNN octal, so non-printable and high
        bytes ride as octal to keep the wire ASCII-clean. Pure-ASCII strings
        encode the same under every code page, so skip the `set cp` round-trip.
        Characters absent from the code page become '?'.
        """
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
        tty.setraw(sys.stdin.fileno())
        ctrl_a_pressed = False
        while True:
            ready, _, _ = select.select([sys.stdin, self.serial], [], [], None)
            if sys.stdin in ready:
                char = os.read(sys.stdin.fileno(), 1).decode("utf-8", errors="ignore")
                if char == "\x01":  # CTRL-A
                    ctrl_a_pressed = True
                    self.serial.write(char.encode(cp))
                elif ctrl_a_pressed and char.lower() in "bf":
                    self.send_break()  # eats prompt
                    sys.stdout.write("\r\n]")  # fake prompt
                    ctrl_a_pressed = False
                elif ctrl_a_pressed and char.lower() in "xq":
                    sys.stdout.write("\r\n")
                    if sys.stdin.isatty():
                        os.system("stty sane")
                    break
                else:
                    ctrl_a_pressed = False
                    self.serial.write(char.encode(cp))
            if self.serial in ready:
                data = self.serial.read(1)
                if len(data) > 0:
                    try:
                        sys.stdout.write(data.decode(cp))
                    except UnicodeDecodeError:
                        sys.stdout.write(f"\\x{data[0]:02x}")
                    sys.stdout.flush()

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
                            self.serial.write(key_in.encode(cp))
                        elif ctrl_a_pressed and key_in.lower() in "bf":
                            self.send_break()  # eats prompt
                            sys.stdout.write("\r\n]")  # fake prompt
                            ctrl_a_pressed = False
                        elif ctrl_a_pressed and key_in.lower() in "xq":
                            sys.stdout.write("\r\n")
                            break
                        else:
                            ctrl_a_pressed = False
                            self.serial.write(key_in.encode(cp))
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

    def __init__(self):
        """Sparse array of virtual ROM with optional named assets."""
        self.data = {}
        self.alloc = {}
        self.assets = []  # list of (name, bytes)

    def add_asset(self, name: str, data: bytes):
        """Append a named asset to the ROM."""
        if any(n == name for n, _ in self.assets):
            raise ROMException(f"Asset name already exists: {name}")
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
        if addr["nmi"]:
            self.add_nmi_vector(addr["nmi"])
        if addr["reset"] is True:
            if len(data) < 2:
                raise ROMException("No reset address found in file.")
            addr["reset"] = data[0] + data[1] * 256
            data = data[2:]
        if addr["reset"]:
            self.add_reset_vector(addr["reset"])
        if addr["irq"] is True:
            if len(data) < 2:
                raise ROMException("No irq address found in file.")
            addr["irq"] = data[0] + data[1] * 256
            data = data[2:]
        if addr["irq"]:
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
            if not re.match(f"^#!{SCRIPT_NAME}\\r?\\n$", command, re.IGNORECASE):
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
        return bool(self.alloc[0xFFFC] and self.alloc[0xFFFD])

    def next_rom_data(self, addr: int):
        """Find next up-to-1k chunk starting at addr, never crossing 64k page."""
        for addr in range(addr, 0x1000000):
            if self.alloc.get(addr):
                page_end = (addr | 0xFFFF) + 1
                length = 0
                while self.alloc.get(addr + length):
                    length += 1
                    if length == 1024 or addr + length == page_end:
                        break
                return addr, bytearray(self.data[addr + i] for i in range(length))
        return None, None


class Emulator:
    """rp6502-emu discovery and debug-adapter error reporting."""

    # True once we know this run is an `emu` launch: we are the IDE's debug
    # adapter, so errors must be promoted to DAP (see fatal).
    launching = False

    @staticmethod
    def find():
        """Locate the rp6502-emu executable for first-run config hinting."""
        system = platform.system()
        exe = "rp6502-emu.exe" if system == "Windows" else "rp6502-emu"
        try:
            candidates = []
            # Anything already on PATH.
            try:
                found = shutil.which(exe)
            except Exception:
                found = None  # bizarre PATH/PATHEXT; keep scanning
            if found:
                candidates.append(found)
            # A source checkout the dev built themselves (~/rp6502).
            for sub in ("release", "debug"):
                candidates.append(
                    os.path.expanduser(
                        os.path.join("~", "rp6502", "build", "emulator", sub, exe)
                    )
                )
            # A downloaded release artifact.
            for d in ("~", "~/Downloads", "~/Desktop"):
                base = os.path.expanduser(d)
                if system == "Darwin":
                    # macOS ships rp6502-emu.app; run its inner Mach-O.
                    candidates.append(
                        os.path.join(
                            base, "rp6502-emu.app", "Contents", "MacOS", "rp6502-emu"
                        )
                    )
                else:
                    candidates.append(os.path.join(base, exe))
                    # Linux release binaries are arch-suffixed.
                    if system == "Linux":
                        candidates.append(
                            os.path.join(base, f"rp6502-emu-{platform.machine()}")
                        )
            for candidate in candidates:
                # isfile returns False on OSError/ValueError (3.8+).
                if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
                    try:
                        return os.path.realpath(candidate)
                    except Exception:
                        return candidate  # canonicalizing is optional
        except Exception:
            pass  # Best effort; the hint is optional
        return exe

    @staticmethod
    def send_dap_error(message: str):
        """Speak minimal DAP: acknowledge `initialize`, then fail `launch`/`attach`.

        Reads Content-Length framed messages from our stdin (the DAP request
        stream) and writes responses to stdout. VSCode shows the message from a
        failed launch/attach response in an error dialog.
        """
        stdin = sys.stdin.buffer
        stdout = sys.stdout.buffer
        out_seq = 0

        def read_request():
            header = b""
            while not header.endswith(b"\r\n\r\n"):
                byte = stdin.read(1)
                if not byte:
                    return None  # stream closed before a full header
                header += byte
            length = 0
            for line in header.split(b"\r\n"):
                name, sep, value = line.partition(b":")
                if sep and name.strip().lower() == b"content-length":
                    length = int(value.strip())
            body = b""
            while len(body) < length:
                chunk = stdin.read(length - len(body))
                if not chunk:
                    return None
                body += chunk
            return json.loads(body.decode("utf-8"))

        def send(response):
            nonlocal out_seq
            out_seq += 1
            response["seq"] = out_seq
            data = json.dumps(response).encode("utf-8")
            stdout.write(
                b"Content-Length: " + str(len(data)).encode("ascii") + b"\r\n\r\n"
            )
            stdout.write(data)
            stdout.flush()

        while True:
            request = read_request()
            if request is None:
                return
            if request.get("type") != "request":
                continue
            command = request.get("command")
            request_seq = request.get("seq", 0)
            if command in ("launch", "attach"):
                send(
                    {
                        "type": "response",
                        "request_seq": request_seq,
                        "success": False,
                        "command": command,
                        "message": message,
                        "body": {
                            "error": {"id": 1, "format": message, "showUser": True}
                        },
                    }
                )
                return
            # `initialize` (and anything else before launch) gets a bare success
            # so the client proceeds to send the launch request we then fail.
            response = {
                "type": "response",
                "request_seq": request_seq,
                "success": True,
                "command": command,
            }
            if command == "initialize":
                response["body"] = {}
            send(response)


def exec_args():
    # Standard library argument parser
    class CustomFormatter(argparse.HelpFormatter):
        def __init__(self, prog):
            super().__init__(prog, max_help_position=27)

    parser = argparse.ArgumentParser(
        description="Interface with RIA console. Manage ROM packaging.",
        formatter_class=CustomFormatter,
    )
    sp = parser.add_subparsers(dest="command", required=True)
    cmds = {
        "term": ("Attach to the RIA console.", None),
        "emu": ("Launch emulator from config (for IDE).", None),
        "run": ("Run local ROM by sending to RIA.", 1),
        "upload": ("Upload local files to RIA USB storage.", "+"),
        "basic": ("Executes a program with the installed BASIC.", 1),
        "create": (
            "Create local ROM file from a file. Additional local ROM files will be merged.",
            "+",
        ),
    }
    parsers = {}
    for cmd, (desc, nargs) in cmds.items():
        parsers[cmd] = sp.add_parser(cmd, description=desc, help=desc)
        if nargs:
            parsers[cmd].add_argument(
                "filename",
                nargs=nargs,
                help="Local filename." if nargs == 1 else "Local filename(s).",
            )
    # Everything after the ROM filename is the ROM's argv, like `LOAD rom args...`.
    parsers["run"].add_argument(
        "rom_args",
        nargs=argparse.REMAINDER,
        metavar="args",
        help="Arguments passed to the ROM.",
    )
    parser.add_argument(
        "-a",
        "--address",
        dest="address",
        metavar="addr",
        help="Asset name (string) or starting address of binary data.",
    )
    parser.add_argument("-o", dest="out", metavar="name", help="Output path/filename.")
    parser.add_argument(
        "-n",
        "--nmi",
        dest="nmi",
        metavar="addr",
        help="NMI vector for $FFFA-$FFFB or `file` to read from file.",
    )
    parser.add_argument(
        "-r",
        "--reset",
        dest="reset",
        metavar="addr",
        help="Reset vector for $FFFC-$FFFD or `file` to read from file.",
    )
    parser.add_argument(
        "-i",
        "--irq",
        dest="irq",
        metavar="addr",
        help="IRQ vector for $FFFE-$FFFF or `file` to read from file.",
    )

    parser.add_argument(
        "-c",
        "--config",
        dest="config",
        metavar="name",
        help=f"Configuration file for debug settings.",
    )
    parser.add_argument(
        "-d",
        "--device",
        dest="device",
        metavar="dev",
        default=Console.default_device(),
        help=f"Serial device or telnet address:port. Default={Console.default_device()}",
    )
    parser.add_argument(
        # Hidden alias for anyone used to minicom -D /dev/
        "-D",
        dest="device",
        metavar="dev",
        default=argparse.SUPPRESS,
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "-k",
        "--key",
        dest="key",
        metavar="key",
        default=None,
        help="Passkey for telnet authentication. Device becomes telnet host.",
    )
    parser.add_argument(
        "-w",
        "--workdir",
        dest="workdir",
        metavar="dir",
        default=None,
        help="Remote directory to work in.",
    )
    parser.add_argument(
        "-t",
        "--term",
        dest="term",
        metavar="bool",
        default="True",
        help=f"Attach to console terminal on run.",
    )
    args = parser.parse_args()
    Emulator.launching = args.command == "emu"

    # Config file (shared with the emulator, which owns it in ImGui ini format).
    if args.config:
        launch = f"{SCRIPT_NAME}][Launch"
        config = configparser.ConfigParser(interpolation=None)
        try:
            existed = os.path.exists(args.config)
            if existed:
                # configparser.read() silently skips unreadable files
                if not os.access(args.config, os.R_OK):
                    raise PermissionError("permission denied")
                config.read(args.config)
            # Upgrade a legacy plain [RP6502] to [RP6502][Launch].
            upgrading = config.has_section(SCRIPT_NAME) and not config.has_section(
                launch
            )
            if (not existed) or upgrading:
                old = (
                    dict(config[SCRIPT_NAME]) if config.has_section(SCRIPT_NAME) else {}
                )
                pick = lambda k: old.get(k, "")
                config.remove_section(SCRIPT_NAME)  # drop legacy [RP6502]
                # User always sees the full list of keys, even when blank.
                config[launch] = {
                    "emulator": pick("emulator") or Emulator.find(),
                    "device": pick("device") or args.device,
                    "key": pick("key") or args.key or "",
                    "workdir": pick("workdir") or args.workdir or "",
                    "args": pick("args"),
                    "term": pick("term") or args.term,
                }
                with open(args.config, "w") as cfg:
                    config.write(cfg)
        except (configparser.Error, OSError) as e:
            raise RuntimeError(f"Cannot load config {args.config}: {e}")
        if config.has_section(launch):
            sec = config[launch]
            args.workdir = sec.get("workdir", "") or args.workdir or None
            args.emulator = sec.get("emulator", "")
            args.device = sec.get("device", args.device)
            args.key = sec.get("key", "") or args.key or None
            args.term = sec.get("term", args.term)
            args.config_args = sec.get("args", "")

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
        args.address = str_to_address_or_name(args.address)
        args.nmi = str_to_address(parser, args.nmi, "-n/--nmi")
        args.reset = str_to_address(parser, args.reset, "-r/--reset")
        args.irq = str_to_address(parser, args.irq, "-i/--irq")
        print(f"[{os.path.basename(__file__)}] Creating {args.out}")
        rom = ROM()
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
            if args.nmi:
                rom.add_nmi_vector(args.nmi)
            if args.reset:
                rom.add_reset_vector(args.reset)
            if args.irq:
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
        with open(args.out, "wb+") as file:
            file.write(f"#!{SCRIPT_NAME}\r\n".encode("ascii"))
            # Build null asset (memory chunks blob)
            chunks = b""
            addr, data = rom.next_rom_data(0)
            while data is not None:
                header = f"${addr:04X} ${len(data):03X} ${binascii.crc32(data):08X}\r\n"
                chunks += header.encode("ascii") + bytes(data)
                addr += len(data)
                addr, data = rom.next_rom_data(addr)
            if chunks:
                file.write(
                    f"#>${len(chunks):08X} ${binascii.crc32(chunks):08X}\r\n".encode(
                        "ascii"
                    )
                )
                file.write(chunks)
            # Write named assets
            for asset_name, asset_data in rom.assets:
                file.write(
                    f"#>${len(asset_data):08X} ${binascii.crc32(asset_data):08X} {asset_name}\r\n".encode(
                        "ascii"
                    )
                )
                file.write(asset_data)

    if args.command == "emu":
        # `emu` exists to launch the emulator as the IDE's debug adapter, which
        # always passes the project config (for the emulator path and --ini), so
        # an invocation without one is a misconfiguration.
        if not args.config:
            raise RuntimeError(
                "emu requires -c/--config <file> with an 'emulator' path."
            )
        config_hint = f" in {args.config}"
        emulator = getattr(args, "emulator", "")
        if not emulator:
            raise RuntimeError(
                f"No emulator configured — set 'emulator'{config_hint} "
                f"to the rp6502-emu executable path."
            )
        emulator = os.path.expanduser(os.path.expandvars(emulator))
        # A macOS .app is a directory; run its inner executable.
        if platform.system() == "Darwin" and emulator.rstrip("/").endswith(".app"):
            emulator = os.path.join(
                emulator.rstrip("/"), "Contents", "MacOS", "rp6502-emu"
            )
        # An explicit path (with a separator) must exist; a bare name is resolved
        # against PATH so we can report "not found on PATH" precisely (rather than
        # a misleading errno from execvp on non-executable PATH entries).
        has_sep = os.sep in emulator or (os.altsep and os.altsep in emulator)
        if has_sep:
            if not os.path.isfile(emulator):
                raise FileNotFoundError(
                    f"Emulator '{emulator}' not found — fix 'emulator'{config_hint}."
                )
        else:
            resolved = shutil.which(emulator)
            if resolved is None:
                raise FileNotFoundError(
                    f"Emulator '{emulator}' not found on PATH — fix 'emulator'{config_hint}."
                )
            emulator = resolved
        cmd = [emulator, "--dap", "--ini", args.config]
        # Config args ride the emulator command line as the ROM's argv;
        # a launch request that carries its own args overrides them there.
        rom_args = config_rom_args()
        if rom_args:
            cmd += ["--"] + rom_args
        # Status to stderr only: stdout carries the lldb-dap DAP stream.
        print(f"[{SCRIPT_FILE}] Launching {emulator}", file=sys.stderr)
        try:
            if os.name == "nt":
                sys.exit(subprocess.Popen(cmd).wait())
            os.execvp(cmd[0], cmd)
        except OSError as e:
            # Backstop for exec failures on a path shutil.which deemed runnable.
            raise RuntimeError(f"Cannot run emulator '{emulator}'{config_hint}: {e}")


# This file may be included or run like a program.
if __name__ == "__main__":
    # VSCode SIGKILLs the terminal while in raw mode, return to cooked mode.
    if "tty" in globals() and sys.stdin.isatty():
        os.system("stty sane")
    try:
        exec_args()
    except Exception as e:
        # On an emu launch we are the IDE's debug adapter.
        if Emulator.launching:
            print(f"[{SCRIPT_FILE}] {e}", file=sys.stderr)
            if not sys.stdin.isatty():
                try:
                    Emulator.send_dap_error(f"{e}")
                except Exception:
                    pass  # Best effort
            sys.exit(1)
        # Exceptions to show in VS Code output instead of Python debugger.
        if not isinstance(
            e,
            (
                ROMException,
                FileNotFoundError,
                TimeoutError,
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
