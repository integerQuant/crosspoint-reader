#!/usr/bin/env python3
"""PTY to USB CDC bridge for CrossPoint's knietty terminal Activity."""

from __future__ import annotations

import argparse
import fcntl
import os
import pty
import select
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import termios
import time
from dataclasses import dataclass
from typing import Iterable, Sequence

import serial
from serial import SerialException
from serial.tools import list_ports

DEFAULT_COLS = 50
DEFAULT_ROWS = 22
DEFAULT_MAX_BPS = 2048
DEFAULT_RETRY_SECONDS = 1.0
ESPRESSIF_VID = 0x303A


class KniettyError(RuntimeError):
    """A user-facing bridge error."""


@dataclass(frozen=True)
class DeviceFilters:
    vid: int | None = None
    pid: int | None = None
    product: str | None = None
    serial_number: str | None = None


def parse_usb_id(value: str) -> int:
    try:
        parsed = int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid USB ID: {value!r}") from exc
    if not 0 <= parsed <= 0xFFFF:
        raise argparse.ArgumentTypeError("USB ID must be between 0 and 0xffff")
    return parsed


def _contains(value: str | None, expected: str | None) -> bool:
    return expected is None or (value is not None and expected.casefold() in value.casefold())


def port_matches(port: object, filters: DeviceFilters) -> bool:
    if filters.vid is not None and getattr(port, "vid", None) != filters.vid:
        return False
    if filters.pid is not None and getattr(port, "pid", None) != filters.pid:
        return False
    if not _contains(getattr(port, "product", None), filters.product):
        return False
    if not _contains(getattr(port, "serial_number", None), filters.serial_number):
        return False
    return True


def _device_path_score(device: str) -> int:
    if sys.platform == "darwin" and device.startswith("/dev/cu.usbmodem"):
        return 20
    if sys.platform.startswith("linux") and device.startswith("/dev/ttyACM"):
        return 20
    return 0


def port_score(port: object) -> int:
    device = str(getattr(port, "device", ""))
    product = str(getattr(port, "product", "") or "").casefold()
    description = str(getattr(port, "description", "") or "").casefold()
    combined = f"{product} {description}"
    score = 0
    if device == "/dev/knietty":
        score += 200
    if "knietty" in combined:
        score += 150
    elif "crosspoint" in combined:
        score += 120
    elif "xteink" in combined:
        score += 100
    elif "esp32" in combined or "usb jtag/serial" in combined:
        score += 30
    if getattr(port, "vid", None) == ESPRESSIF_VID:
        score += 25
    # A generic ACM/modem path is only a preference between identified
    # candidates. It is not enough evidence to open an unrelated serial device.
    return score + _device_path_score(device) if score else 0


def discover_device(filters: DeviceFilters, ports: Iterable[object] | None = None) -> str:
    candidates = [port for port in (list_ports.comports() if ports is None else ports) if port_matches(port, filters)]
    filters_active = any(
        value is not None for value in (filters.vid, filters.pid, filters.product, filters.serial_number)
    )
    scored = []
    for port in candidates:
        score = port_score(port)
        if score == 0 and filters_active:
            score = 1 + _device_path_score(str(getattr(port, "device", "")))
        if score > 0:
            scored.append((score, port))
    if not scored:
        raise KniettyError("no matching USB CDC device found")

    ranked = sorted(scored, key=lambda item: (-item[0], str(getattr(item[1], "device", ""))))
    best_score = ranked[0][0]
    tied = [port for score, port in ranked if score == best_score]
    if len(tied) > 1:
        paths = ", ".join(str(getattr(port, "device", "?")) for port in tied)
        raise KniettyError(f"multiple equally likely devices found ({paths}); pass --device or USB filters")
    return str(getattr(ranked[0][1], "device"))


def format_port(port: object) -> str:
    vid = getattr(port, "vid", None)
    pid = getattr(port, "pid", None)
    usb_id = f"{vid:04x}:{pid:04x}" if vid is not None and pid is not None else "----:----"
    return "\t".join(
        (
            str(getattr(port, "device", "")),
            usb_id,
            str(getattr(port, "product", "") or getattr(port, "description", "") or ""),
            str(getattr(port, "serial_number", "") or ""),
        )
    )


def set_pty_size(fd: int, cols: int, rows: int) -> None:
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def get_pty_size(fd: int) -> tuple[int, int]:
    rows, cols, _, _ = struct.unpack("HHHH", fcntl.ioctl(fd, termios.TIOCGWINSZ, b"\0" * 8))
    return cols, rows


def default_command() -> str:
    if shutil.which("tmux"):
        return "tmux new-session -A -s knietty"
    return os.environ.get("SHELL") or "/bin/sh"


@dataclass
class PtySession:
    master_fd: int
    child: subprocess.Popen[bytes]

    @classmethod
    def spawn(cls, command: str, cols: int, rows: int, term: str) -> "PtySession":
        master_fd, slave_fd = pty.openpty()
        set_pty_size(slave_fd, cols, rows)
        os.set_blocking(master_fd, False)

        child_env = os.environ.copy()
        child_env.update(TERM=term, COLUMNS=str(cols), LINES=str(rows))

        def child_setup() -> None:
            os.setsid()
            if hasattr(termios, "TIOCSCTTY"):
                fcntl.ioctl(slave_fd, termios.TIOCSCTTY, 0)

        try:
            child = subprocess.Popen(
                ["/bin/sh", "-lc", command],
                stdin=slave_fd,
                stdout=slave_fd,
                stderr=slave_fd,
                close_fds=True,
                env=child_env,
                preexec_fn=child_setup,
            )
        except BaseException:
            os.close(master_fd)
            os.close(slave_fd)
            raise
        os.close(slave_fd)
        return cls(master_fd=master_fd, child=child)

    def redraw(self) -> None:
        try:
            os.killpg(self.child.pid, signal.SIGWINCH)
        except (ProcessLookupError, PermissionError):
            pass

    def close(self) -> None:
        try:
            os.close(self.master_fd)
        except OSError:
            pass
        if self.child.poll() is None:
            try:
                os.killpg(self.child.pid, signal.SIGHUP)
            except (ProcessLookupError, PermissionError):
                pass
            try:
                self.child.wait(timeout=2)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(self.child.pid, signal.SIGTERM)
                except (ProcessLookupError, PermissionError):
                    pass
                try:
                    self.child.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(self.child.pid, signal.SIGKILL)
                    except (ProcessLookupError, PermissionError):
                        pass
                    self.child.wait(timeout=2)


class Bridge:
    def __init__(
        self,
        session: PtySession,
        device: str,
        filters: DeviceFilters,
        baud: int,
        retry_seconds: float,
        max_bps: int,
        verbose: bool,
    ) -> None:
        self.session = session
        self.device = device
        self.filters = filters
        self.baud = baud
        self.retry_seconds = retry_seconds
        self.max_bps = max_bps
        self.verbose = verbose
        self.serial_port: serial.Serial | None = None
        self.pending_output = bytearray()
        self.pending_input = bytearray()
        self.next_write_at = 0.0

    def log(self, message: str) -> None:
        print(f"knietty: {message}", file=sys.stderr, flush=True)

    def resolve_device(self) -> str:
        return discover_device(self.filters) if self.device == "auto" else self.device

    def connect(self) -> bool:
        try:
            path = self.resolve_device()
            self.serial_port = serial.Serial(
                path,
                baudrate=self.baud,
                timeout=0,
                write_timeout=1,
            )
        except (KniettyError, OSError, SerialException) as exc:
            if self.verbose:
                self.log(str(exc))
            self.serial_port = None
            return False
        self.log(f"connected to {path}")
        self.session.redraw()
        return True

    def disconnect(self, reason: BaseException | str) -> None:
        if self.serial_port is not None:
            try:
                self.serial_port.close()
            except OSError:
                pass
        self.serial_port = None
        self.log(f"disconnected ({reason}); waiting for device")

    def _write_serial(self) -> None:
        assert self.serial_port is not None
        if not self.pending_output:
            try:
                self.pending_output.extend(os.read(self.session.master_fd, 256))
            except BlockingIOError:
                return
        if not self.pending_output:
            return
        written = self.serial_port.write(self.pending_output)
        if written:
            del self.pending_output[:written]
            self.next_write_at = time.monotonic() + written / self.max_bps

    def _read_serial(self) -> None:
        assert self.serial_port is not None
        waiting = self.serial_port.in_waiting
        self.pending_input.extend(self.serial_port.read(min(max(waiting, 1), 256)))
        self._flush_pty_input()

    def _flush_pty_input(self) -> None:
        if not self.pending_input:
            return
        try:
            written = os.write(self.session.master_fd, self.pending_input)
        except BlockingIOError:
            return
        del self.pending_input[:written]

    def run(self) -> int:
        while self.session.child.poll() is None:
            self._flush_pty_input()
            if self.serial_port is None:
                if not self.connect():
                    time.sleep(self.retry_seconds)
                    continue

            assert self.serial_port is not None
            try:
                now = time.monotonic()
                readers: list[int] = [] if self.pending_input else [self.serial_port.fileno()]
                if not self.pending_output and now >= self.next_write_at:
                    readers.append(self.session.master_fd)
                writers = [self.session.master_fd] if self.pending_input else []
                timeout = max(0.0, min(0.25, self.next_write_at - now)) if self.pending_output else 0.25
                ready, writable, _ = select.select(readers, writers, [], timeout)
                if self.serial_port.fileno() in ready:
                    self._read_serial()
                if self.session.master_fd in writable:
                    self._flush_pty_input()
                if self.session.master_fd in ready or (self.pending_output and time.monotonic() >= self.next_write_at):
                    self._write_serial()
            except (OSError, SerialException) as exc:
                self.disconnect(exc)
        return self.session.child.returncode or 0


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Bridge an XTEINK X4 knietty terminal to a host PTY")
    parser.add_argument("--device", default="auto", help="serial path, or 'auto' (default)")
    parser.add_argument("--cols", type=positive_int, default=DEFAULT_COLS)
    parser.add_argument("--rows", type=positive_int, default=DEFAULT_ROWS)
    parser.add_argument("--command", help="command to run; defaults to persistent tmux, then $SHELL")
    parser.add_argument("--term", default="vt100", help="TERM value for the PTY (default: vt100)")
    parser.add_argument("--baud", type=positive_int, default=115200, help="CDC baud hint (default: 115200)")
    parser.add_argument("--max-bps", type=positive_int, default=DEFAULT_MAX_BPS, help="PTY output pacing limit")
    parser.add_argument("--retry-interval", type=float, default=DEFAULT_RETRY_SECONDS)
    parser.add_argument("--vid", type=parse_usb_id, help="required USB VID, e.g. 0x303a")
    parser.add_argument("--pid", type=parse_usb_id, help="required USB PID")
    parser.add_argument("--product", help="case-insensitive product substring")
    parser.add_argument("--serial-number", help="case-insensitive USB serial substring")
    parser.add_argument("--list-devices", action="store_true", help="list serial metadata and exit")
    parser.add_argument("--verbose", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.retry_interval <= 0:
        raise SystemExit("--retry-interval must be greater than zero")
    if args.list_devices:
        print("DEVICE\tVID:PID\tPRODUCT/DESCRIPTION\tSERIAL")
        for port in list_ports.comports():
            print(format_port(port))
        return 0

    filters = DeviceFilters(args.vid, args.pid, args.product, args.serial_number)
    command = args.command or default_command()
    if args.verbose:
        print(f"knietty: starting {shlex.quote(command)} at {args.cols}x{args.rows}", file=sys.stderr)

    session = PtySession.spawn(command, args.cols, args.rows, args.term)
    bridge = Bridge(
        session,
        args.device,
        filters,
        args.baud,
        args.retry_interval,
        args.max_bps,
        args.verbose,
    )
    try:
        return bridge.run()
    except KeyboardInterrupt:
        return 130
    finally:
        if bridge.serial_port is not None:
            bridge.serial_port.close()
        session.close()


if __name__ == "__main__":
    raise SystemExit(main())
