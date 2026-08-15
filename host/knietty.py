#!/usr/bin/env python3
"""PTY bridge for CrossPoint's knietty terminal Activity."""

from __future__ import annotations

import argparse
import datetime
import fcntl
import os
import pty
import select
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import termios
import time
import tty
from dataclasses import dataclass
from typing import Iterable, Sequence

import serial
from serial import SerialException
from serial.tools import list_ports

DEFAULT_COLS = 80
DEFAULT_ROWS = 24
DEFAULT_USB_MAX_BPS = 2048
DEFAULT_WIFI_MAX_BPS = 65536
DEFAULT_RETRY_SECONDS = 1.0
DEFAULT_DISCOVERY_SECONDS = 2.0
DEFAULT_APPROVAL_SECONDS = 60.0
DEFAULT_WIFI_PORT = 29380
DENIED_RETRY_SECONDS = 300.0
ESPRESSIF_VID = 0x303A
PROTOCOL_V1_PREFIX = "KNIETTY/1"
PROTOCOL_V2_PREFIX = "KNIETTY/2"
PROTOCOL_PREFIX = PROTOCOL_V1_PREFIX  # Discovery remains compatible with v1 firmware.
PROTOCOL_RESPONSE_PREFIXES = (PROTOCOL_V1_PREFIX, PROTOCOL_V2_PREFIX)
LOCAL_EXIT_BYTE = b"\x1c"  # Ctrl+backslash, consumed by the bridge rather than the PTY.


class KniettyError(RuntimeError):
    """A user-facing bridge error."""


class ConnectionDenied(KniettyError):
    """The user denied this host on the X4."""


class ProtocolVersionRejected(KniettyError):
    """The X4 rejected the requested handshake version."""


@dataclass(frozen=True)
class DeviceFilters:
    vid: int | None = None
    pid: int | None = None
    product: str | None = None
    serial_number: str | None = None


@dataclass(frozen=True)
class NetworkDevice:
    name: str
    address: str
    port: int
    device_id: str = ""


def parse_discovery_response(response: bytes, address: str) -> NetworkDevice | None:
    try:
        fields = response.decode("ascii").strip().split()
    except UnicodeDecodeError:
        return None
    if len(fields) != 4 or fields[:2] != [PROTOCOL_PREFIX, "HERE"]:
        return None
    try:
        port = int(fields[3])
    except ValueError:
        return None
    if not 0 < port <= 65535:
        return None
    return NetworkDevice(fields[2], address, port, fields[2])


def discover_network_devices(
    timeout: float = DEFAULT_DISCOVERY_SECONDS, port: int = DEFAULT_WIFI_PORT
) -> list[NetworkDevice]:
    probe = f"{PROTOCOL_PREFIX} DISCOVER\n".encode("ascii")
    devices: dict[tuple[str, int], NetworkDevice] = {}
    connection = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        connection.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        connection.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        connection.bind(("", 0))
        connection.sendto(probe, ("255.255.255.255", port))
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            connection.settimeout(remaining)
            try:
                response, source = connection.recvfrom(256)
            except socket.timeout:
                break
            device = parse_discovery_response(response, source[0])
            if device is not None:
                devices[(device.address, device.port)] = device
    finally:
        connection.close()
    return sorted(devices.values(), key=lambda device: (device.name, device.address, device.port))


def discover_network_device(
    timeout: float = DEFAULT_DISCOVERY_SECONDS, port: int = DEFAULT_WIFI_PORT
) -> NetworkDevice:
    devices = discover_network_devices(timeout, port)
    if not devices:
        raise KniettyError("no knietty terminal found on the local network")
    if len(devices) > 1:
        choices = ", ".join(f"{device.name} ({device.address})" for device in devices)
        raise KniettyError(f"multiple knietty terminals found ({choices}); pass --host")
    return devices[0]


def format_network_device(device: NetworkDevice) -> str:
    return "\t".join((device.name, device.address, str(device.port), device.device_id))


def protocol_client_name(hostname: str | None = None) -> str:
    source = hostname or socket.gethostname() or "host"
    safe = "".join(character if character.isalnum() or character in " ._-" else "?" for character in source)
    return safe[:32] or "host"


def protocol_host_time(epoch: float | None = None) -> tuple[int, int]:
    epoch_seconds = int(time.time() if epoch is None else epoch)
    offset = datetime.datetime.fromtimestamp(epoch_seconds).astimezone().utcoffset()
    offset_minutes = 0 if offset is None else int(offset.total_seconds() // 60)
    return epoch_seconds, offset_minutes


def parse_server_response(response: bytes) -> tuple[int, int]:
    try:
        line = response.decode("ascii").strip()
    except UnicodeDecodeError as exc:
        raise KniettyError("terminal returned a non-ASCII handshake") from exc
    fields = line.split()
    if len(fields) >= 2 and fields[0] in PROTOCOL_RESPONSE_PREFIXES and fields[1] == "ERROR":
        raise ProtocolVersionRejected("X4 rejected this protocol version")
    if len(fields) >= 2 and fields[0] in PROTOCOL_RESPONSE_PREFIXES and fields[1] == "DENY":
        raise ConnectionDenied("connection denied on the X4")
    if len(fields) >= 2 and fields[0] in PROTOCOL_RESPONSE_PREFIXES and fields[1] == "BUSY":
        raise KniettyError("X4 is already handling another host")
    if len(fields) != 4 or fields[0] not in PROTOCOL_RESPONSE_PREFIXES or fields[1] != "ACCEPT":
        raise KniettyError(f"unexpected terminal handshake: {line!r}")
    try:
        cols, rows = int(fields[2]), int(fields[3])
    except ValueError as exc:
        raise KniettyError(f"invalid terminal geometry in handshake: {line!r}") from exc
    if cols <= 0 or rows <= 0:
        raise KniettyError(f"invalid terminal geometry in handshake: {line!r}")
    return cols, rows


def read_protocol_line(connection: socket.socket, limit: int = 128) -> bytes:
    response = bytearray()
    while len(response) < limit:
        chunk = connection.recv(1)
        if not chunk:
            raise KniettyError("X4 disconnected during handshake")
        response.extend(chunk)
        if chunk == b"\n":
            return bytes(response)
    raise KniettyError("X4 handshake exceeded size limit")


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


class LocalInput:
    def __init__(self, fd: int) -> None:
        self.fd = fd
        self.saved_attributes: list[object] | None = None
        self.saved_blocking: bool | None = None

    def enable(self) -> None:
        self.saved_attributes = termios.tcgetattr(self.fd)
        self.saved_blocking = os.get_blocking(self.fd)
        tty.setraw(self.fd, termios.TCSANOW)
        os.set_blocking(self.fd, False)

    def close(self) -> None:
        if self.saved_attributes is not None:
            termios.tcsetattr(self.fd, termios.TCSANOW, self.saved_attributes)
            self.saved_attributes = None
        if self.saved_blocking is not None:
            os.set_blocking(self.fd, self.saved_blocking)
            self.saved_blocking = None


class SerialBridge:
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


class NetworkBridge:
    def __init__(
        self,
        session: PtySession,
        host: str,
        port: int,
        retry_seconds: float,
        discovery_seconds: float,
        approval_seconds: float,
        max_bps: int,
        verbose: bool,
        local_input_fd: int | None = None,
    ) -> None:
        self.session = session
        self.host = host
        self.port = port
        self.retry_seconds = retry_seconds
        self.discovery_seconds = discovery_seconds
        self.approval_seconds = approval_seconds
        self.max_bps = max_bps
        self.verbose = verbose
        self.local_input_fd = local_input_fd
        self.connection: socket.socket | None = None
        self.pending_output = bytearray()
        self.pending_input = bytearray()
        self.next_write_at = 0.0
        self.next_retry_seconds = retry_seconds
        self.local_exit_requested = False

    def log(self, message: str) -> None:
        print(f"knietty: {message}", file=sys.stderr, flush=True)

    def resolve_target(self) -> tuple[str, int, str]:
        if self.host != "auto":
            return self.host, self.port, self.host
        device = discover_network_device(self.discovery_seconds, self.port)
        return device.address, device.port, device.name

    def connect(self) -> bool:
        self.next_retry_seconds = self.retry_seconds
        try:
            address, port, label = self.resolve_target()
        except (KniettyError, OSError, TimeoutError) as exc:
            if self.verbose:
                self.log(str(exc))
            return False

        for version in (2, 1):
            connection: socket.socket | None = None
            try:
                connection = socket.create_connection((address, port), timeout=5)
                connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                connection.settimeout(self.approval_seconds)
                client_name = protocol_client_name()
                if version == 2:
                    epoch, offset = protocol_host_time()
                    hello = f"{PROTOCOL_V2_PREFIX} HELLO {epoch} {offset} {client_name}\n"
                else:
                    hello = f"{PROTOCOL_V1_PREFIX} HELLO {client_name}\n"
                connection.sendall(hello.encode("ascii"))
                self.log(f"requesting approval on {label} ({address}:{port})")
                cols, rows = parse_server_response(read_protocol_line(connection))
                set_pty_size(self.session.master_fd, cols, rows)
                self.session.redraw()
                connection.setblocking(False)
                self.connection = connection
                self.log(f"connected to {label} at {cols}x{rows}")
                return True
            except ProtocolVersionRejected as exc:
                if connection is not None:
                    connection.close()
                if version == 2:
                    if self.verbose:
                        self.log(f"{exc}; falling back to protocol v1")
                    continue
                if self.verbose:
                    self.log(str(exc))
                return False
            except ConnectionDenied as exc:
                self.next_retry_seconds = DENIED_RETRY_SECONDS
                if connection is not None:
                    connection.close()
                self.log(f"{exc}; retrying in {DENIED_RETRY_SECONDS:g}s")
                return False
            except (KniettyError, OSError, TimeoutError) as exc:
                if connection is not None:
                    connection.close()
                if self.verbose:
                    self.log(str(exc))
                return False
        return False

    def disconnect(self, reason: BaseException | str) -> None:
        if self.connection is not None:
            try:
                self.connection.close()
            except OSError:
                pass
        self.connection = None
        self.pending_output.clear()
        self.pending_input.clear()
        self.log(f"disconnected ({reason}); waiting for terminal")

    def _write_network(self) -> None:
        assert self.connection is not None
        if not self.pending_output:
            try:
                self.pending_output.extend(os.read(self.session.master_fd, 1024))
            except BlockingIOError:
                return
        if not self.pending_output:
            return
        try:
            written = self.connection.send(self.pending_output)
        except BlockingIOError:
            return
        if written == 0:
            raise ConnectionResetError("socket closed while writing")
        del self.pending_output[:written]
        self.next_write_at = time.monotonic() + written / self.max_bps

    def _read_network(self) -> None:
        assert self.connection is not None
        try:
            received = self.connection.recv(256)
        except BlockingIOError:
            return
        if not received:
            raise ConnectionResetError("socket closed by X4")
        self.pending_input.extend(received)
        self._flush_pty_input()

    def _read_local_input(self) -> None:
        assert self.local_input_fd is not None
        try:
            received = os.read(self.local_input_fd, 256)
        except BlockingIOError:
            return
        if not received:
            self.local_input_fd = None
            return
        exit_at = received.find(LOCAL_EXIT_BYTE)
        if exit_at >= 0:
            self.pending_input.extend(received[:exit_at])
            self.local_exit_requested = True
        else:
            self.pending_input.extend(received)
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
            if self.connection is None:
                if not self.connect():
                    time.sleep(self.next_retry_seconds)
                    continue

            assert self.connection is not None
            try:
                now = time.monotonic()
                readers: list[int | socket.socket] = [] if self.pending_input else [self.connection]
                if not self.pending_input and self.local_input_fd is not None:
                    readers.append(self.local_input_fd)
                if not self.pending_output and now >= self.next_write_at:
                    readers.append(self.session.master_fd)
                writers: list[int] = [self.session.master_fd] if self.pending_input else []
                timeout = max(0.0, min(0.1, self.next_write_at - now)) if self.pending_output else 0.1
                ready, writable, _ = select.select(readers, writers, [], timeout)
                if self.connection in ready:
                    self._read_network()
                if self.local_input_fd is not None and self.local_input_fd in ready:
                    self._read_local_input()
                    if self.local_exit_requested:
                        return 0
                if self.session.master_fd in writable:
                    self._flush_pty_input()
                if self.session.master_fd in ready or (self.pending_output and time.monotonic() >= self.next_write_at):
                    self._write_network()
            except (OSError, TimeoutError) as exc:
                self.disconnect(exc)
        return self.session.child.returncode or 0


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Bridge an XTEINK X4 knietty terminal to a host PTY")
    parser.add_argument("--transport", choices=("wifi", "usb"), default="wifi", help="bridge transport (default: wifi)")
    parser.add_argument("--host", default="auto", help="X4 IP/hostname, or 'auto' for LAN discovery (default)")
    parser.add_argument("--port", type=positive_int, default=DEFAULT_WIFI_PORT, help="TCP port for an explicit host")
    parser.add_argument("--device", default="auto", help="legacy USB serial path, or 'auto'")
    parser.add_argument("--cols", type=positive_int, default=DEFAULT_COLS)
    parser.add_argument("--rows", type=positive_int, default=DEFAULT_ROWS)
    parser.add_argument("--command", help="command to run; defaults to persistent tmux, then $SHELL")
    parser.add_argument("--term", default="vt100", help="TERM value for the PTY (default: vt100)")
    parser.add_argument("--baud", type=positive_int, default=115200, help="CDC baud hint (default: 115200)")
    parser.add_argument("--max-bps", type=positive_int, help="PTY output pacing limit")
    parser.add_argument("--retry-interval", type=float, default=DEFAULT_RETRY_SECONDS)
    parser.add_argument("--discovery-timeout", type=float, default=DEFAULT_DISCOVERY_SECONDS)
    parser.add_argument("--approval-timeout", type=float, default=DEFAULT_APPROVAL_SECONDS)
    parser.add_argument(
        "--local-input",
        action=argparse.BooleanOptionalAction,
        default=None,
        help="forward this terminal's keyboard; defaults on for interactive Wi-Fi sessions",
    )
    parser.add_argument("--vid", type=parse_usb_id, help="required USB VID, e.g. 0x303a")
    parser.add_argument("--pid", type=parse_usb_id, help="required USB PID")
    parser.add_argument("--product", help="case-insensitive product substring")
    parser.add_argument("--serial-number", help="case-insensitive USB serial substring")
    parser.add_argument("--list-devices", action="store_true", help="list discovered devices for the selected transport")
    parser.add_argument("--verbose", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.retry_interval <= 0:
        raise SystemExit("--retry-interval must be greater than zero")
    if args.discovery_timeout <= 0:
        raise SystemExit("--discovery-timeout must be greater than zero")
    if args.approval_timeout <= 0:
        raise SystemExit("--approval-timeout must be greater than zero")
    if args.list_devices:
        if args.transport == "wifi":
            print("NAME\tADDRESS\tPORT\tID")
            for device in discover_network_devices(args.discovery_timeout, args.port):
                print(format_network_device(device))
        else:
            print("DEVICE\tVID:PID\tPRODUCT/DESCRIPTION\tSERIAL")
            for port in list_ports.comports():
                print(format_port(port))
        return 0

    filters = DeviceFilters(args.vid, args.pid, args.product, args.serial_number)
    command = args.command or default_command()
    local_input_enabled = args.transport == "wifi" and (
        sys.stdin.isatty() if args.local_input is None else args.local_input
    )
    if args.local_input and not sys.stdin.isatty():
        raise SystemExit("--local-input requires an interactive terminal")
    if args.verbose:
        print(f"knietty: starting {shlex.quote(command)} at {args.cols}x{args.rows}", file=sys.stderr)

    session = PtySession.spawn(command, args.cols, args.rows, args.term)
    local_input = LocalInput(sys.stdin.fileno()) if local_input_enabled else None
    if args.transport == "wifi":
        bridge: SerialBridge | NetworkBridge = NetworkBridge(
            session,
            args.host,
            args.port,
            args.retry_interval,
            args.discovery_timeout,
            args.approval_timeout,
            args.max_bps or DEFAULT_WIFI_MAX_BPS,
            args.verbose,
            local_input.fd if local_input is not None else None,
        )
    else:
        bridge = SerialBridge(
            session,
            args.device,
            filters,
            args.baud,
            args.retry_interval,
            args.max_bps or DEFAULT_USB_MAX_BPS,
            args.verbose,
        )
    try:
        if local_input is not None:
            local_input.enable()
            print("knietty: local keyboard enabled (Ctrl+\\ exits; Ctrl+C is forwarded)", file=sys.stderr, flush=True)
        return bridge.run()
    except KeyboardInterrupt:
        return 130
    finally:
        if local_input is not None:
            local_input.close()
        if isinstance(bridge, SerialBridge) and bridge.serial_port is not None:
            bridge.serial_port.close()
        if isinstance(bridge, NetworkBridge) and bridge.connection is not None:
            bridge.connection.close()
        session.close()


if __name__ == "__main__":
    raise SystemExit(main())
