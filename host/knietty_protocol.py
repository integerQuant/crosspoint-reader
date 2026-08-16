"""Bounded knietty protocol v3 frame codec."""

from __future__ import annotations

import enum
import struct
from dataclasses import dataclass

FRAME_HEADER_SIZE = 8
MAX_FRAME_PAYLOAD = 512
OPTIONAL_TYPE_MASK = 0x80
_FRAME_HEADER = struct.Struct("!BBHI")


class FrameType(enum.IntEnum):
    TERMINAL_OUTPUT = 0x01
    TERMINAL_INPUT = 0x02
    CONTROL_REQUEST = 0x03
    CONTROL_RESPONSE = 0x04
    REFRESH_EVENT = 0x05
    HEARTBEAT = 0x06


class DiagnosticCommand(enum.IntEnum):
    SESSION_INFO = 1
    RESET = 2
    PATTERN = 3
    SET_POLARITY = 4
    CLEAN = 5
    STOP = 6


class DiagnosticPattern(enum.IntEnum):
    CELL = 1
    CURSOR = 2
    ROW = 3
    DISJOINT_ROWS = 4
    SCROLL = 5
    CHECKER = 6
    FULL = 7
    CELL_MIDDLE = 8
    CELL_BOTTOM = 9
    ADJACENT_CELLS = 10
    BOUNDARY_UNDER = 11
    BOUNDARY_OVER = 12
    BURST_1 = 13
    BURST_2 = 14
    BURST_5 = 15
    BURST_10 = 16
    BURST_25 = 17
    BURST_100 = 18


class DiagnosticStatus(enum.IntEnum):
    ACCEPTED = 0
    REJECTED = 1


class DiagnosticEventPhase(enum.IntEnum):
    PRESENTED = 1
    READY = 2
    FAILED = 3


class FrameProtocolError(RuntimeError):
    """The peer sent a malformed or unsupported mandatory frame."""


@dataclass(frozen=True)
class Frame:
    frame_type: int
    flags: int
    sequence: int
    payload: bytes


@dataclass(frozen=True)
class DiagnosticResponse:
    schema: int
    command: int
    status: int
    error: int
    metadata: dict[str, object] | None = None


@dataclass(frozen=True)
class DiagnosticRefreshEvent:
    schema: int
    phase: int
    command: int
    requested_path: int
    actual_path: int
    fallback_reason: int
    flags: int
    queue_depth: int
    values: dict[str, int]


def is_known_frame_type(frame_type: int) -> bool:
    return frame_type in FrameType._value2member_map_


def is_optional_frame_type(frame_type: int) -> bool:
    return bool(frame_type & OPTIONAL_TYPE_MASK)


def encode_frame(frame_type: int | FrameType, payload: bytes, sequence: int, flags: int = 0) -> bytes:
    if not 0 <= int(frame_type) <= 0xFF:
        raise ValueError("frame type must fit in one byte")
    if flags != 0:
        raise ValueError("protocol v3 flags must be zero")
    if len(payload) > MAX_FRAME_PAYLOAD:
        raise ValueError(f"frame payload exceeds {MAX_FRAME_PAYLOAD} bytes")
    if not 0 <= sequence <= 0xFFFFFFFF:
        raise ValueError("frame sequence must fit in 32 bits")
    return _FRAME_HEADER.pack(int(frame_type), flags, len(payload), sequence) + payload


class FrameDecoder:
    def __init__(self) -> None:
        self._buffer = bytearray()

    def reset(self) -> None:
        self._buffer.clear()

    def feed(self, data: bytes) -> list[Frame]:
        self._buffer.extend(data)
        frames: list[Frame] = []
        while len(self._buffer) >= FRAME_HEADER_SIZE:
            frame_type, flags, length, sequence = _FRAME_HEADER.unpack_from(self._buffer)
            if flags != 0:
                raise FrameProtocolError("protocol v3 frame has unsupported flags")
            if length > MAX_FRAME_PAYLOAD:
                raise FrameProtocolError(f"protocol v3 frame exceeds {MAX_FRAME_PAYLOAD} bytes")
            frame_size = FRAME_HEADER_SIZE + length
            if len(self._buffer) < frame_size:
                break
            payload = bytes(self._buffer[FRAME_HEADER_SIZE:frame_size])
            del self._buffer[:frame_size]
            frames.append(Frame(frame_type, flags, sequence, payload))
        return frames


_SESSION_INFO_FIXED = struct.Struct("!11Bb3B2H2IB")
_REFRESH_EVENT = struct.Struct("!8B15I8HIHBB4I")


def encode_diagnostic_command(
    command: DiagnosticCommand,
    pattern: DiagnosticPattern | None = None,
    variant: int | None = None,
) -> bytes:
    if command is DiagnosticCommand.PATTERN:
        if pattern is None or variant not in (0, 1):
            raise ValueError("pattern commands require a named pattern and variant 0 or 1")
        return bytes((command, pattern, variant))
    if command is DiagnosticCommand.SET_POLARITY:
        if pattern is not None or variant not in (0, 1):
            raise ValueError("polarity commands require variant 0 or 1")
        return bytes((command, variant))
    if pattern is not None or variant is not None:
        raise ValueError("this diagnostic command takes no arguments")
    return bytes((command,))


def decode_diagnostic_response(payload: bytes) -> DiagnosticResponse:
    if len(payload) < 4:
        raise FrameProtocolError("diagnostic response is shorter than its status header")
    schema, command, status, error = payload[:4]
    metadata: dict[str, object] | None = None
    if command == DiagnosticCommand.SESSION_INFO and status == DiagnosticStatus.ACCEPTED:
        if len(payload) < _SESSION_INFO_FIXED.size:
            raise FrameProtocolError("diagnostic session metadata is truncated")
        fields = _SESSION_INFO_FIXED.unpack_from(payload)
        build_length = fields[-1]
        freeink_length_at = _SESSION_INFO_FIXED.size + build_length
        if len(payload) <= freeink_length_at:
            raise FrameProtocolError("diagnostic build revision length is invalid")
        try:
            build = payload[_SESSION_INFO_FIXED.size : freeink_length_at].decode("utf-8")
            freeink_length = payload[freeink_length_at]
            if len(payload) != freeink_length_at + 1 + freeink_length:
                raise FrameProtocolError("diagnostic FreeInk revision length is invalid")
            freeink = payload[freeink_length_at + 1 :].decode("utf-8")
        except UnicodeDecodeError as exc:
            raise FrameProtocolError("diagnostic build revision is not UTF-8") from exc
        metadata = {
            "profile": fields[4],
            "spi_mhz": fields[5],
            "flags": fields[6],
            "orientation": fields[7],
            "board": fields[8],
            "controller": fields[9],
            "battery_percent": fields[10],
            "rssi_dbm": fields[11],
            "columns": fields[12],
            "rows": fields[13],
            "font": fields[14],
            "display_width": fields[15],
            "display_height": fields[16],
            "free_heap": fields[17],
            "minimum_free_heap": fields[18],
            "build": build,
            "freeink": freeink,
        }
    elif len(payload) != 4:
        raise FrameProtocolError("diagnostic status response has trailing bytes")
    return DiagnosticResponse(schema, command, status, error, metadata)


def decode_diagnostic_refresh_event(payload: bytes) -> DiagnosticRefreshEvent:
    if len(payload) != _REFRESH_EVENT.size:
        raise FrameProtocolError(f"diagnostic refresh event must be {_REFRESH_EVENT.size} bytes")
    fields = _REFRESH_EVENT.unpack(payload)
    names = (
        "timestamp_us",
        "rx_at_us",
        "parsed_at_us",
        "queued_at_us",
        "render_started_at_us",
        "queue_us",
        "render_us",
        "transfer_us",
        "lut_us",
        "plane_us",
        "activation_to_busy_us",
        "waveform_us",
        "baseline_us",
        "power_off_us",
        "total_us",
        "logical_x",
        "logical_y",
        "logical_width",
        "logical_height",
        "aligned_x",
        "aligned_y",
        "aligned_width",
        "aligned_height",
        "transfer_bytes",
        "dirty_cells",
        "dirty_rows",
        "coalesced",
        "first_sequence",
        "last_sequence",
        "free_heap",
        "minimum_free_heap",
    )
    values = dict(zip(names, fields[8:], strict=True))
    return DiagnosticRefreshEvent(*fields[:8], values)


def u32_before_or_equal(first: int, second: int) -> bool:
    """Compare device monotonic timestamps across one uint32 wrap."""
    return ((second - first) & 0xFFFFFFFF) < 0x80000000
