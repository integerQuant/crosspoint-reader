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


class FrameProtocolError(RuntimeError):
    """The peer sent a malformed or unsupported mandatory frame."""


@dataclass(frozen=True)
class Frame:
    frame_type: int
    flags: int
    sequence: int
    payload: bytes


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
