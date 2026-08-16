#!/usr/bin/env python3
"""PTY bridge for CrossPoint's knietty terminal Activity."""

from __future__ import annotations

import argparse
import datetime
import fcntl
import json
import os
import platform
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
from pathlib import Path
from typing import Iterable, Sequence

import serial
from serial import SerialException
from serial.tools import list_ports

from knietty_protocol import (
    MAX_FRAME_PAYLOAD,
    FrameDecoder,
    FrameProtocolError,
    FrameType,
    DiagnosticCommand,
    DiagnosticEventPhase,
    DiagnosticPattern,
    DiagnosticStatus,
    decode_diagnostic_refresh_event,
    decode_diagnostic_response,
    encode_diagnostic_command,
    encode_frame,
    is_known_frame_type,
    is_optional_frame_type,
    u32_before_or_equal,
)

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
PROTOCOL_V3_PREFIX = "KNIETTY/3"
PROTOCOL_PREFIX = PROTOCOL_V1_PREFIX  # Discovery remains compatible with v1 firmware.
PROTOCOL_RESPONSE_PREFIXES = (PROTOCOL_V1_PREFIX, PROTOCOL_V2_PREFIX, PROTOCOL_V3_PREFIX)
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


@dataclass(frozen=True)
class ServerAccept:
    version: int
    cols: int
    rows: int
    capabilities: tuple[str, ...] = ()


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


def parse_server_accept(response: bytes) -> ServerAccept:
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
    if len(fields) < 2 or fields[0] not in PROTOCOL_RESPONSE_PREFIXES or fields[1] != "ACCEPT":
        raise KniettyError(f"unexpected terminal handshake: {line!r}")
    version = int(fields[0].removeprefix("KNIETTY/"))
    expected_length = 5 if version == 3 else 4
    if len(fields) != expected_length:
        raise KniettyError(f"unexpected terminal handshake: {line!r}")
    try:
        cols, rows = int(fields[2]), int(fields[3])
    except ValueError as exc:
        raise KniettyError(f"invalid terminal geometry in handshake: {line!r}") from exc
    if cols <= 0 or rows <= 0:
        raise KniettyError(f"invalid terminal geometry in handshake: {line!r}")
    capabilities = tuple(fields[4].split(",")) if version == 3 else ()
    if version == 3 and "frame" not in capabilities:
        raise KniettyError("X4 accepted protocol v3 without the frame capability")
    return ServerAccept(version, cols, rows, capabilities)


def parse_server_response(response: bytes) -> tuple[int, int]:
    accepted = parse_server_accept(response)
    return accepted.cols, accepted.rows


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
        reconnect: bool = False,
    ) -> None:
        self.session = session
        self.device = device
        self.filters = filters
        self.baud = baud
        self.retry_seconds = retry_seconds
        self.max_bps = max_bps
        self.verbose = verbose
        self.reconnect = reconnect
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
        suffix = "; waiting for device" if self.reconnect else ""
        self.log(f"disconnected ({reason}){suffix}")

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
                if not self.reconnect:
                    return 0
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
        reconnect: bool = False,
        protocol: str = "auto",
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
        self.reconnect = reconnect
        self.protocol = protocol
        self.connection: socket.socket | None = None
        self.pending_output = bytearray()
        self.pending_input = bytearray()
        self.next_write_at = 0.0
        self.next_retry_seconds = retry_seconds
        self.local_exit_requested = False
        self.connected_once = False
        self.last_retry_error = ""
        self.last_retry_log_at = 0.0
        self.protocol_version = 0
        self.frame_decoder = FrameDecoder()
        self.next_tx_sequence = 1

    def log(self, message: str) -> None:
        print(f"knietty: {message}", file=sys.stderr, flush=True)

    def resolve_target(self) -> tuple[str, int, str]:
        if self.host != "auto":
            return self.host, self.port, self.host
        device = discover_network_device(self.discovery_seconds, self.port)
        return device.address, device.port, device.name

    def log_retry_error(self, error: BaseException) -> None:
        if not self.verbose:
            return
        message = str(error)
        now = time.monotonic()
        if message != self.last_retry_error or now - self.last_retry_log_at >= 30.0:
            self.log(message)
            self.last_retry_error = message
            self.last_retry_log_at = now

    def connect(self) -> bool:
        self.next_retry_seconds = self.retry_seconds
        try:
            address, port, label = self.resolve_target()
        except (KniettyError, OSError, TimeoutError) as exc:
            self.log_retry_error(exc)
            return False

        versions = (3, 2, 1) if self.protocol == "auto" else (int(self.protocol),)
        for version in versions:
            connection: socket.socket | None = None
            try:
                connection = socket.create_connection((address, port), timeout=5)
                connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                connection.settimeout(self.approval_seconds)
                client_name = protocol_client_name()
                if version == 3:
                    epoch, offset = protocol_host_time()
                    hello = f"{PROTOCOL_V3_PREFIX} HELLO terminal frame {epoch} {offset} {client_name}\n"
                elif version == 2:
                    epoch, offset = protocol_host_time()
                    hello = f"{PROTOCOL_V2_PREFIX} HELLO {epoch} {offset} {client_name}\n"
                else:
                    hello = f"{PROTOCOL_V1_PREFIX} HELLO {client_name}\n"
                connection.sendall(hello.encode("ascii"))
                self.log(f"requesting approval on {label} ({address}:{port})")
                accepted = parse_server_accept(read_protocol_line(connection))
                if accepted.version != version:
                    raise KniettyError(f"X4 accepted protocol v{accepted.version} after a v{version} request")
                set_pty_size(self.session.master_fd, accepted.cols, accepted.rows)
                self.session.redraw()
                connection.setblocking(False)
                self.connection = connection
                self.connected_once = True
                self.last_retry_error = ""
                self.protocol_version = version
                self.frame_decoder.reset()
                self.next_tx_sequence = 1
                self.log(f"connected to {label} at {accepted.cols}x{accepted.rows} using protocol v{version}")
                return True
            except ProtocolVersionRejected as exc:
                if connection is not None:
                    connection.close()
                if self.protocol == "auto" and version != 1:
                    if self.verbose:
                        self.log(f"{exc}; trying an older protocol")
                    continue
                self.log_retry_error(exc)
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
        self.protocol_version = 0
        self.frame_decoder.reset()
        self.pending_output.clear()
        self.pending_input.clear()
        suffix = "; waiting for terminal" if self.reconnect else ""
        self.log(f"disconnected ({reason}){suffix}")

    def wait_for_retry(self, seconds: float) -> bool:
        if self.local_input_fd is None:
            time.sleep(seconds)
            return True
        ready, _, _ = select.select([self.local_input_fd], [], [], seconds)
        if self.local_input_fd in ready:
            self._read_local_input()
        return not self.local_exit_requested

    def _write_network(self) -> None:
        assert self.connection is not None
        if not self.pending_output:
            try:
                payload = os.read(
                    self.session.master_fd,
                    MAX_FRAME_PAYLOAD if self.protocol_version == 3 else 1024,
                )
            except BlockingIOError:
                return
            if self.protocol_version == 3 and payload:
                self.pending_output.extend(encode_frame(FrameType.TERMINAL_OUTPUT, payload, self.next_tx_sequence))
                self.next_tx_sequence = (self.next_tx_sequence + 1) & 0xFFFFFFFF
            else:
                self.pending_output.extend(payload)
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
        if self.protocol_version == 3:
            for frame in self.frame_decoder.feed(received):
                if frame.frame_type == FrameType.TERMINAL_INPUT:
                    self.pending_input.extend(frame.payload)
                elif frame.frame_type == FrameType.HEARTBEAT or is_optional_frame_type(frame.frame_type):
                    continue
                elif is_known_frame_type(frame.frame_type):
                    raise FrameProtocolError(f"unexpected protocol v3 frame type 0x{frame.frame_type:02x}")
                else:
                    raise FrameProtocolError(f"unknown mandatory protocol v3 frame type 0x{frame.frame_type:02x}")
        else:
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
                    if not self.wait_for_retry(self.next_retry_seconds):
                        return 0
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
            except (OSError, TimeoutError, FrameProtocolError) as exc:
                self.disconnect(exc)
                if self.connected_once and not self.reconnect:
                    return 0
        return self.session.child.returncode or 0


class DiagnosticClient:
    def __init__(
        self,
        host: str,
        port: int,
        discovery_seconds: float,
        approval_seconds: float,
        command_seconds: float,
        output: Path,
        verbose: bool,
    ) -> None:
        self.host = host
        self.port = port
        self.discovery_seconds = discovery_seconds
        self.approval_seconds = approval_seconds
        self.command_seconds = command_seconds
        self.output = output
        self.verbose = verbose
        self.connection: socket.socket | None = None
        self.decoder = FrameDecoder()
        self.pending_frames: list[object] = []
        self.sequence = 1
        self.target_label = host

    def log(self, message: str) -> None:
        print(f"knietty diagnose: {message}", file=sys.stderr, flush=True)

    def connect(self) -> ServerAccept:
        if self.host == "auto":
            target = discover_network_device(self.discovery_seconds, self.port)
            address, port, self.target_label = target.address, target.port, target.name
        else:
            address, port = self.host, self.port
        connection = socket.create_connection((address, port), timeout=5)
        try:
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            connection.settimeout(self.approval_seconds)
            epoch, offset = protocol_host_time()
            hello = (
                f"{PROTOCOL_V3_PREFIX} HELLO diagnostics frame,diag1 {epoch} {offset} "
                f"{protocol_client_name()}\n"
            )
            connection.sendall(hello.encode("ascii"))
            self.log(f"approve the bounded display test on {self.target_label} ({address}:{port})")
            accepted = parse_server_accept(read_protocol_line(connection))
            if accepted.version != 3 or "diag1" not in accepted.capabilities:
                raise KniettyError("X4 does not advertise diagnostics protocol diag1")
            connection.settimeout(self.command_seconds)
            self.connection = connection
            return accepted
        except BaseException:
            connection.close()
            raise

    def close(self) -> None:
        if self.connection is not None:
            self.connection.close()
            self.connection = None

    def _next_frame(self) -> object:
        assert self.connection is not None
        while not self.pending_frames:
            received = self.connection.recv(512)
            if not received:
                raise KniettyError("X4 disconnected during diagnostics")
            self.pending_frames.extend(self.decoder.feed(received))
        return self.pending_frames.pop(0)

    @staticmethod
    def _write_record(output: object, record: dict[str, object]) -> None:
        output.write(json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n")
        output.flush()

    def _request(
        self,
        output: object,
        command: DiagnosticCommand,
        *,
        pattern: DiagnosticPattern | None = None,
        variant: int | None = None,
        expect_refresh: bool = False,
        context: dict[str, object] | None = None,
    ) -> dict[str, object] | None:
        assert self.connection is not None
        sequence = self.sequence
        self.sequence = (self.sequence + 1) & 0xFFFFFFFF
        payload = encode_diagnostic_command(command, pattern, variant)
        sent_at_ns = time.monotonic_ns()
        self.connection.sendall(encode_frame(FrameType.CONTROL_REQUEST, payload, sequence))
        response_metadata: dict[str, object] | None = None
        phases: list[int] = []
        presented_at_us: int | None = None
        while True:
            frame = self._next_frame()
            received_at_ns = time.monotonic_ns()
            if frame.sequence != sequence:
                raise FrameProtocolError(
                    f"diagnostic response sequence {frame.sequence} does not match request {sequence}"
                )
            if frame.frame_type == FrameType.CONTROL_RESPONSE:
                response = decode_diagnostic_response(frame.payload)
                if response.schema != 1 or response.command != int(command):
                    raise FrameProtocolError("diagnostic status does not match its request")
                record: dict[str, object] = {
                    **(context or {}),
                    "record": "response",
                    "sequence": sequence,
                    "command": int(command),
                    "status": response.status,
                    "error": response.error,
                    "host_sent_ns": sent_at_ns,
                    "host_received_ns": received_at_ns,
                }
                if command != DiagnosticCommand.SESSION_INFO:
                    self._write_record(output, record)
                if response.status != DiagnosticStatus.ACCEPTED:
                    raise KniettyError(f"diagnostic command {command.name} was rejected (error {response.error})")
                response_metadata = response.metadata
                if not expect_refresh:
                    return response_metadata
            elif frame.frame_type == FrameType.REFRESH_EVENT:
                event = decode_diagnostic_refresh_event(frame.payload)
                if (
                    event.schema != 1
                    or event.command != int(command)
                    or event.values["first_sequence"] != sequence
                    or event.values["last_sequence"] != sequence
                    or event.values["coalesced"] != 1
                ):
                    raise FrameProtocolError("diagnostic refresh does not match its request")
                event_record: dict[str, object] = {
                    **(context or {}),
                    "record": "refresh",
                    "sequence": sequence,
                    "phase": event.phase,
                    "command": event.command,
                    "requested_path": event.requested_path,
                    "actual_path": event.actual_path,
                    "fallback_reason": event.fallback_reason,
                    "flags": event.flags,
                    "queue_depth": event.queue_depth,
                    "host_received_ns": received_at_ns,
                    **event.values,
                }
                self._write_record(output, event_record)
                phases.append(event.phase)
                if event.phase == DiagnosticEventPhase.PRESENTED:
                    presented_at_us = event.values["timestamp_us"]
                if phases == [DiagnosticEventPhase.PRESENTED, DiagnosticEventPhase.READY]:
                    assert presented_at_us is not None
                    if not u32_before_or_equal(presented_at_us, event.values["timestamp_us"]):
                        raise FrameProtocolError("diagnostic READY timestamp precedes PRESENTED")
                    return response_metadata
                if phases not in ([DiagnosticEventPhase.PRESENTED], [DiagnosticEventPhase.PRESENTED,
                                                                      DiagnosticEventPhase.READY]):
                    raise FrameProtocolError("diagnostic refresh events arrived out of order")
            elif frame.frame_type == FrameType.HEARTBEAT or is_optional_frame_type(frame.frame_type):
                continue
            else:
                raise FrameProtocolError(f"unexpected diagnostics frame type 0x{frame.frame_type:02x}")

    def _send_pattern_batch(
        self,
        output: object,
        requests: Sequence[tuple[DiagnosticPattern, int, dict[str, object]]],
        *,
        interval_ms: int,
        context: dict[str, object],
    ) -> None:
        assert self.connection is not None
        sent: dict[int, tuple[int, dict[str, object]]] = {}
        sequence_order: list[int] = []
        started_at_ns = time.monotonic_ns()
        for index, (pattern, variant, request_context) in enumerate(requests):
            deadline_ns = started_at_ns + index * interval_ms * 1_000_000
            remaining_ns = deadline_ns - time.monotonic_ns()
            if remaining_ns > 0:
                time.sleep(remaining_ns / 1_000_000_000)
            sequence = self.sequence
            self.sequence = (self.sequence + 1) & 0xFFFFFFFF
            sent_at_ns = time.monotonic_ns()
            payload = encode_diagnostic_command(DiagnosticCommand.PATTERN, pattern, variant)
            self.connection.sendall(encode_frame(FrameType.CONTROL_REQUEST, payload, sequence))
            sent[sequence] = (sent_at_ns, request_context)
            sequence_order.append(sequence)

        sequence_index = {sequence: index for index, sequence in enumerate(sequence_order)}
        responses: set[int] = set()
        covered: set[int] = set()
        phases: dict[tuple[int, int], list[int]] = {}
        presented_at_us: dict[tuple[int, int], int] = {}
        while len(responses) != len(sequence_order) or len(covered) != len(sequence_order):
            frame = self._next_frame()
            received_at_ns = time.monotonic_ns()
            if frame.frame_type == FrameType.CONTROL_RESPONSE:
                if frame.sequence not in sent or frame.sequence in responses:
                    raise FrameProtocolError(f"unexpected diagnostic batch response sequence {frame.sequence}")
                response = decode_diagnostic_response(frame.payload)
                if response.schema != 1 or response.command != int(DiagnosticCommand.PATTERN):
                    raise FrameProtocolError("diagnostic batch status does not match its request")
                sent_at_ns, request_context = sent[frame.sequence]
                self._write_record(
                    output,
                    {
                        **context,
                        **request_context,
                        "record": "response",
                        "sequence": frame.sequence,
                        "command": int(DiagnosticCommand.PATTERN),
                        "status": response.status,
                        "error": response.error,
                        "host_sent_ns": sent_at_ns,
                        "host_received_ns": received_at_ns,
                    },
                )
                if response.status != DiagnosticStatus.ACCEPTED:
                    raise KniettyError(
                        f"diagnostic batch sequence {frame.sequence} was rejected (error {response.error})"
                    )
                responses.add(frame.sequence)
                continue
            if frame.frame_type == FrameType.REFRESH_EVENT:
                event = decode_diagnostic_refresh_event(frame.payload)
                first_sequence = event.values["first_sequence"]
                last_sequence = event.values["last_sequence"]
                if (
                    frame.sequence != last_sequence
                    or first_sequence not in sequence_index
                    or last_sequence not in sequence_index
                    or sequence_index[first_sequence] > sequence_index[last_sequence]
                ):
                    raise FrameProtocolError("diagnostic batch event has an invalid sequence range")
                event_sequences = sequence_order[
                    sequence_index[first_sequence] : sequence_index[last_sequence] + 1
                ]
                if (
                    event.schema != 1
                    or event.command != int(DiagnosticCommand.PATTERN)
                    or event.values["coalesced"] != len(event_sequences)
                    or event.queue_depth != len(event_sequences) - 1
                ):
                    raise FrameProtocolError("diagnostic batch event has inconsistent coalescing metadata")
                key = (first_sequence, last_sequence)
                event_phases = phases.setdefault(key, [])
                event_phases.append(event.phase)
                if event.phase == DiagnosticEventPhase.PRESENTED:
                    presented_at_us[key] = event.values["timestamp_us"]
                elif event.phase == DiagnosticEventPhase.READY:
                    if event_phases != [DiagnosticEventPhase.PRESENTED, DiagnosticEventPhase.READY]:
                        raise FrameProtocolError("diagnostic batch refresh events arrived out of order")
                    if not u32_before_or_equal(presented_at_us[key], event.values["timestamp_us"]):
                        raise FrameProtocolError("diagnostic batch READY timestamp precedes PRESENTED")
                    if any(sequence in covered for sequence in event_sequences):
                        raise FrameProtocolError("diagnostic batch refresh ranges overlap")
                    covered.update(event_sequences)
                else:
                    raise FrameProtocolError("diagnostic batch returned a failed refresh event")
                self._write_record(
                    output,
                    {
                        **context,
                        "record": "refresh",
                        "sequence": frame.sequence,
                        "phase": event.phase,
                        "command": event.command,
                        "requested_path": event.requested_path,
                        "actual_path": event.actual_path,
                        "fallback_reason": event.fallback_reason,
                        "flags": event.flags,
                        "queue_depth": event.queue_depth,
                        "first_host_sent_ns": sent[first_sequence][0],
                        "last_host_sent_ns": sent[last_sequence][0],
                        "first_sample_index": sent[first_sequence][1]["sample_index"],
                        "last_sample_index": sent[last_sequence][1]["sample_index"],
                        "host_received_ns": received_at_ns,
                        **event.values,
                    },
                )
                continue
            if frame.frame_type == FrameType.HEARTBEAT or is_optional_frame_type(frame.frame_type):
                continue
            raise FrameProtocolError(f"unexpected diagnostics frame type 0x{frame.frame_type:02x}")

    def _run_smoke(self, output: object) -> None:
        commands: tuple[tuple[DiagnosticCommand, DiagnosticPattern | None, int | None], ...] = (
            (DiagnosticCommand.RESET, None, None),
            (DiagnosticCommand.PATTERN, DiagnosticPattern.CELL, 0),
            (DiagnosticCommand.PATTERN, DiagnosticPattern.CELL, 1),
            (DiagnosticCommand.PATTERN, DiagnosticPattern.CURSOR, 1),
            (DiagnosticCommand.PATTERN, DiagnosticPattern.CURSOR, 0),
            (DiagnosticCommand.PATTERN, DiagnosticPattern.ROW, 0),
            (DiagnosticCommand.PATTERN, DiagnosticPattern.DISJOINT_ROWS, 1),
            (DiagnosticCommand.PATTERN, DiagnosticPattern.SCROLL, 0),
            (DiagnosticCommand.PATTERN, DiagnosticPattern.CHECKER, 0),
            (DiagnosticCommand.PATTERN, DiagnosticPattern.FULL, 0),
            (DiagnosticCommand.SET_POLARITY, None, 1),
            (DiagnosticCommand.SET_POLARITY, None, 0),
            (DiagnosticCommand.CLEAN, None, None),
        )
        for command, pattern, variant in commands:
            label = command.name if pattern is None else f"{command.name}/{pattern.name}/{variant}"
            self.log(f"running {label}")
            self._request(output, command, pattern=pattern, variant=variant, expect_refresh=True)

    def _run_latency(self, output: object, repetitions: int) -> None:
        patterns: tuple[tuple[str, DiagnosticPattern], ...] = (
            ("cell_top", DiagnosticPattern.CELL),
            ("cell_middle", DiagnosticPattern.CELL_MIDDLE),
            ("cell_bottom", DiagnosticPattern.CELL_BOTTOM),
            ("adjacent_cells", DiagnosticPattern.ADJACENT_CELLS),
            ("cursor", DiagnosticPattern.CURSOR),
            ("row", DiagnosticPattern.ROW),
            ("disjoint_rows", DiagnosticPattern.DISJOINT_ROWS),
            ("scroll", DiagnosticPattern.SCROLL),
            ("boundary_under_8k", DiagnosticPattern.BOUNDARY_UNDER),
            ("boundary_over_8k", DiagnosticPattern.BOUNDARY_OVER),
            ("checker", DiagnosticPattern.CHECKER),
            ("full", DiagnosticPattern.FULL),
        )
        self._request(
            output,
            DiagnosticCommand.RESET,
            expect_refresh=True,
            context={"case": "setup_reset"},
        )
        polarity = 0
        for repetition in range(repetitions):
            requested_polarity = repetition & 1
            if requested_polarity != polarity:
                self.log(f"latency repetition {repetition + 1}: polarity {requested_polarity}")
                self._request(
                    output,
                    DiagnosticCommand.SET_POLARITY,
                    variant=requested_polarity,
                    expect_refresh=True,
                    context={"case": "setup_polarity", "repetition": repetition + 1},
                )
                polarity = requested_polarity
            pattern_order = patterns if repetition % 2 == 0 else tuple(reversed(patterns))
            for case, pattern in pattern_order:
                for variant in (1, 0):
                    direction = (
                        "white_to_black" if (variant == 1) != bool(polarity) else "black_to_white"
                    )
                    self.log(f"latency {repetition + 1}/{repetitions}: {case} {direction}")
                    self._request(
                        output,
                        DiagnosticCommand.PATTERN,
                        pattern=pattern,
                        variant=variant,
                        expect_refresh=True,
                        context={
                            "case": case,
                            "pattern": int(pattern),
                            "variant": variant,
                            "direction": direction,
                            "repetition": repetition + 1,
                        },
                    )
        if polarity != 0:
            self._request(
                output,
                DiagnosticCommand.SET_POLARITY,
                variant=0,
                expect_refresh=True,
                context={"case": "restore_polarity"},
            )
        self._request(output, DiagnosticCommand.CLEAN, expect_refresh=True, context={"case": "final_clean"})

    def _run_cadence(self, output: object, settle_seconds: float) -> None:
        self._request(
            output,
            DiagnosticCommand.RESET,
            expect_refresh=True,
            context={"case": "setup_reset"},
        )
        next_variant = 1
        for polarity in (0, 1):
            if polarity != 0:
                self._request(
                    output,
                    DiagnosticCommand.SET_POLARITY,
                    variant=polarity,
                    expect_refresh=True,
                    context={"case": "setup_polarity", "polarity": polarity},
                )
            intervals = (600, 400, 200, 100, 50, 25) if polarity == 0 else (25, 50, 100, 200, 400, 600)
            for interval_ms in intervals:
                requests: list[tuple[DiagnosticPattern, int, dict[str, object]]] = []
                for sample_index in range(1, 7):
                    direction = (
                        "white_to_black" if (next_variant == 1) != bool(polarity) else "black_to_white"
                    )
                    requests.append(
                        (
                            DiagnosticPattern.CELL_MIDDLE,
                            next_variant,
                            {
                                "sample_index": sample_index,
                                "variant": next_variant,
                                "direction": direction,
                            },
                        )
                    )
                    next_variant = 1 - next_variant
                self.log(f"cadence: polarity {polarity}, 6 updates at {interval_ms} ms")
                self._send_pattern_batch(
                    output,
                    requests,
                    interval_ms=interval_ms,
                    context={
                        "case": "cadence",
                        "polarity": polarity,
                        "interval_ms": interval_ms,
                        "requested_count": len(requests),
                    },
                )
                if settle_seconds > 0:
                    time.sleep(settle_seconds)
        self._request(
            output,
            DiagnosticCommand.SET_POLARITY,
            variant=0,
            expect_refresh=True,
            context={"case": "restore_polarity"},
        )
        self._request(output, DiagnosticCommand.CLEAN, expect_refresh=True, context={"case": "final_clean"})

    def _run_burst(self, output: object) -> None:
        patterns: tuple[tuple[int, DiagnosticPattern], ...] = (
            (1, DiagnosticPattern.BURST_1),
            (2, DiagnosticPattern.BURST_2),
            (5, DiagnosticPattern.BURST_5),
            (10, DiagnosticPattern.BURST_10),
            (25, DiagnosticPattern.BURST_25),
            (100, DiagnosticPattern.BURST_100),
        )
        self._request(
            output,
            DiagnosticCommand.RESET,
            expect_refresh=True,
            context={"case": "setup_reset"},
        )
        for polarity in (0, 1):
            if polarity != 0:
                self._request(
                    output,
                    DiagnosticCommand.SET_POLARITY,
                    variant=polarity,
                    expect_refresh=True,
                    context={"case": "setup_polarity", "polarity": polarity},
                )
            pattern_order = patterns if polarity == 0 else tuple(reversed(patterns))
            for requested_cells, pattern in pattern_order:
                for variant in (1, 0):
                    direction = (
                        "white_to_black" if (variant == 1) != bool(polarity) else "black_to_white"
                    )
                    self.log(f"burst: polarity {polarity}, {requested_cells} cells {direction}")
                    self._request(
                        output,
                        DiagnosticCommand.PATTERN,
                        pattern=pattern,
                        variant=variant,
                        expect_refresh=True,
                        context={
                            "case": "burst",
                            "polarity": polarity,
                            "requested_cells": requested_cells,
                            "pattern": int(pattern),
                            "variant": variant,
                            "direction": direction,
                        },
                    )
        self._request(
            output,
            DiagnosticCommand.SET_POLARITY,
            variant=0,
            expect_refresh=True,
            context={"case": "restore_polarity"},
        )
        self._request(output, DiagnosticCommand.CLEAN, expect_refresh=True, context={"case": "final_clean"})

    def run_suite(self, suite: str, repetitions: int, settle_seconds: float) -> int:
        accepted = self.connect()
        self.output.parent.mkdir(parents=True, exist_ok=True)
        with self.output.open("w", encoding="utf-8", buffering=1) as output:
            metadata = self._request(output, DiagnosticCommand.SESSION_INFO)
            assert metadata is not None
            self._write_record(
                output,
                {
                    "record": "session",
                    "schema": 1,
                    "suite": suite,
                    "suite_version": 1,
                    "repetitions": repetitions if suite == "latency" else 1,
                    "target": self.target_label,
                    "host_os": platform.system(),
                    "host_release": platform.release(),
                    "columns": accepted.cols,
                    "rows": accepted.rows,
                    **metadata,
                },
            )
            if suite == "smoke":
                self._run_smoke(output)
            elif suite == "latency":
                self._run_latency(output, repetitions)
            elif suite == "cadence":
                self._run_cadence(output, settle_seconds)
            elif suite == "burst":
                self._run_burst(output)
            else:
                raise ValueError(f"unknown diagnostic suite {suite}")
            self._request(output, DiagnosticCommand.STOP)
        self.log(f"wrote {self.output}")
        return 0


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
        "--protocol",
        choices=("auto", "3", "2", "1"),
        default="auto",
        help="Wi-Fi protocol version; auto tries v3, v2, then v1",
    )
    parser.add_argument(
        "--reconnect",
        action="store_true",
        help="keep discovering and reconnect after a terminal disconnects",
    )
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


def build_diagnostics_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="knietty diagnose", description="Run a physically approved, bounded X4 display diagnostic"
    )
    parser.add_argument("--host", default="auto", help="X4 IP/hostname, or 'auto' for LAN discovery (default)")
    parser.add_argument("--port", type=positive_int, default=DEFAULT_WIFI_PORT)
    parser.add_argument("--suite", choices=("smoke", "latency", "cadence", "burst"), default="smoke")
    parser.add_argument("--output", type=Path, required=True, help="JSON Lines result path")
    parser.add_argument("--repetitions", type=positive_int, default=3, help="latency repetitions, 1-3 (default: 3)")
    parser.add_argument(
        "--settle-seconds",
        type=float,
        default=1.0,
        help="quiet interval between cadence groups (default: 1.0)",
    )
    parser.add_argument("--discovery-timeout", type=float, default=DEFAULT_DISCOVERY_SECONDS)
    parser.add_argument("--approval-timeout", type=float, default=DEFAULT_APPROVAL_SECONDS)
    parser.add_argument("--command-timeout", type=float, default=15.0)
    parser.add_argument("--verbose", action="store_true")
    return parser


def diagnostics_main(argv: Sequence[str]) -> int:
    args = build_diagnostics_parser().parse_args(argv)
    if args.discovery_timeout <= 0 or args.approval_timeout <= 0 or args.command_timeout <= 0:
        raise SystemExit("diagnostic timeouts must be greater than zero")
    if args.repetitions > 3:
        raise SystemExit("--repetitions must be between 1 and 3")
    if args.settle_seconds < 0:
        raise SystemExit("--settle-seconds must be zero or greater")
    client = DiagnosticClient(
        args.host,
        args.port,
        args.discovery_timeout,
        args.approval_timeout,
        args.command_timeout,
        args.output,
        args.verbose,
    )
    try:
        return client.run_suite(args.suite, args.repetitions, args.settle_seconds)
    except KeyboardInterrupt:
        return 130
    except (KniettyError, OSError, TimeoutError, FrameProtocolError) as exc:
        client.log(str(exc))
        return 1
    finally:
        client.close()


def main(argv: Sequence[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if arguments and arguments[0] == "diagnose":
        return diagnostics_main(arguments[1:])
    args = build_parser().parse_args(arguments)
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
            args.reconnect,
            args.protocol,
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
            args.reconnect,
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
