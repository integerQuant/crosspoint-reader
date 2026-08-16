from __future__ import annotations

import os
import io
import pty
import select
import struct
import sys
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))

import knietty
import knietty_protocol


def port(
    device: str,
    *,
    vid: int | None = None,
    pid: int | None = None,
    product: str | None = None,
    description: str | None = None,
    serial_number: str | None = None,
) -> object:
    return SimpleNamespace(
        device=device,
        vid=vid,
        pid=pid,
        product=product,
        description=description,
        serial_number=serial_number,
    )


class DiscoveryTest(unittest.TestCase):
    def test_prefers_knietty_metadata(self) -> None:
        ports = [
            port("/dev/ttyACM0", vid=0x1111, pid=1, description="unrelated"),
            port("/dev/ttyACM1", vid=0x303A, pid=0x1001, product="CrossPoint knietty"),
        ]
        self.assertEqual(knietty.discover_device(knietty.DeviceFilters(), ports), "/dev/ttyACM1")

    def test_filters_usb_metadata(self) -> None:
        ports = [
            port("/dev/cu.usbmodem1", vid=0x303A, pid=1, product="ESP32", serial_number="first"),
            port("/dev/cu.usbmodem2", vid=0x303A, pid=2, product="ESP32", serial_number="target"),
        ]
        filters = knietty.DeviceFilters(vid=0x303A, pid=2, serial_number="target")
        self.assertEqual(knietty.discover_device(filters, ports), "/dev/cu.usbmodem2")

    def test_rejects_ambiguous_candidates(self) -> None:
        ports = [
            port("/dev/ttyACM0", vid=0x303A, product="ESP32"),
            port("/dev/ttyACM1", vid=0x303A, product="ESP32"),
        ]
        with self.assertRaisesRegex(knietty.KniettyError, "multiple equally likely"):
            knietty.discover_device(knietty.DeviceFilters(), ports)

    def test_does_not_trust_a_generic_acm_path(self) -> None:
        ports = [port("/dev/ttyACM0", vid=0x1111, pid=1, description="unrelated")]
        with mock.patch.object(knietty.sys, "platform", "linux"):
            with self.assertRaisesRegex(knietty.KniettyError, "no matching"):
                knietty.discover_device(knietty.DeviceFilters(), ports)

    def test_explicit_metadata_filter_allows_an_unknown_product(self) -> None:
        ports = [port("/dev/ttyACM0", vid=0x1111, pid=1, product="Custom device")]
        filters = knietty.DeviceFilters(vid=0x1111, pid=1)
        with mock.patch.object(knietty.sys, "platform", "linux"):
            self.assertEqual(knietty.discover_device(filters, ports), "/dev/ttyACM0")

    def test_prefers_mac_callout_device(self) -> None:
        ports = [
            port("/dev/tty.usbmodem1", vid=0x303A, product="ESP32"),
            port("/dev/cu.usbmodem1", vid=0x303A, product="ESP32"),
        ]
        with mock.patch.object(knietty.sys, "platform", "darwin"):
            self.assertEqual(knietty.discover_device(knietty.DeviceFilters(), ports), "/dev/cu.usbmodem1")


class NetworkProtocolTest(unittest.TestCase):
    def test_discovery_reprobes_until_a_device_returns(self) -> None:
        clock = [0.0]

        class FakeSocket:
            def __init__(self) -> None:
                self.probes = 0
                self.timeout = 0.0
                self.returned_device = False

            def setsockopt(self, *_args: object) -> None:
                pass

            def bind(self, _address: object) -> None:
                pass

            def sendto(self, data: bytes, address: object) -> None:
                self.assert_probe(data, address)
                self.probes += 1

            def assert_probe(self, data: bytes, address: object) -> None:
                testcase.assertEqual(data, b"KNIETTY/1 DISCOVER\n")
                testcase.assertEqual(address, ("255.255.255.255", 29380))

            def settimeout(self, timeout: float) -> None:
                self.timeout = timeout

            def recvfrom(self, _size: int) -> tuple[bytes, tuple[str, int]]:
                clock[0] += self.timeout
                if self.probes >= 2 and not self.returned_device:
                    self.returned_device = True
                    return b"KNIETTY/1 HERE knietty-aabbcc 29380\n", ("192.0.2.10", 29380)
                raise knietty.socket.timeout

            def close(self) -> None:
                pass

        testcase = self
        fake_socket = FakeSocket()
        with (
            mock.patch.object(knietty.socket, "socket", return_value=fake_socket),
            mock.patch.object(knietty.time, "monotonic", side_effect=lambda: clock[0]),
        ):
            devices = knietty.discover_network_devices(timeout=0.75)

        self.assertEqual(devices, [knietty.NetworkDevice("knietty-aabbcc", "192.0.2.10", 29380, "knietty-aabbcc")])
        self.assertGreaterEqual(fake_socket.probes, 2)

    def test_discovery_response(self) -> None:
        self.assertEqual(
            knietty.parse_discovery_response(b"KNIETTY/1 HERE knietty-aabbcc 29380\n", "192.0.2.10"),
            knietty.NetworkDevice("knietty-aabbcc", "192.0.2.10", 29380, "knietty-aabbcc"),
        )
        self.assertIsNone(knietty.parse_discovery_response(b"not knietty\n", "192.0.2.10"))

    def test_accept_response_contains_geometry(self) -> None:
        self.assertEqual(knietty.parse_server_response(b"KNIETTY/1 ACCEPT 50 22\n"), (50, 22))
        self.assertEqual(knietty.parse_server_response(b"KNIETTY/2 ACCEPT 80 24\n"), (80, 24))
        self.assertEqual(knietty.parse_server_response(b"KNIETTY/3 ACCEPT 80 24 frame\n"), (80, 24))

    def test_v3_accept_requires_framing(self) -> None:
        accepted = knietty.parse_server_accept(b"KNIETTY/3 ACCEPT 80 24 frame\n")
        self.assertEqual(accepted, knietty.ServerAccept(3, 80, 24, ("frame",)))
        with self.assertRaisesRegex(knietty.KniettyError, "frame capability"):
            knietty.parse_server_accept(b"KNIETTY/3 ACCEPT 80 24 diag1\n")

    def test_protocol_error_requests_v1_fallback(self) -> None:
        with self.assertRaises(knietty.ProtocolVersionRejected):
            knietty.parse_server_response(b"KNIETTY/1 ERROR\n")

    def test_denied_response_is_distinct(self) -> None:
        with self.assertRaisesRegex(knietty.ConnectionDenied, "denied"):
            knietty.parse_server_response(b"KNIETTY/1 DENY\n")

    def test_rejects_malformed_response(self) -> None:
        with self.assertRaisesRegex(knietty.KniettyError, "unexpected"):
            knietty.parse_server_response(b"hello\n")

    def test_client_name_is_bounded_and_protocol_safe(self) -> None:
        name = knietty.protocol_client_name("workstation/example:name-with-a-very-long-suffix")
        self.assertEqual(name, "workstation?example?name-with-a-")
        self.assertLessEqual(len(name), 32)

    def test_host_time_metadata_uses_epoch_and_minute_offset(self) -> None:
        epoch, offset = knietty.protocol_host_time(1_700_000_000)
        self.assertEqual(epoch, 1_700_000_000)
        self.assertIsInstance(offset, int)
        self.assertGreaterEqual(offset, -14 * 60)
        self.assertLessEqual(offset, 14 * 60)

    def test_auto_discovery_requires_one_terminal(self) -> None:
        device = knietty.NetworkDevice("x4", "192.0.2.10", 29380, "abc")
        with mock.patch.object(knietty, "discover_network_devices", return_value=[device]):
            self.assertEqual(knietty.discover_network_device(), device)
        with mock.patch.object(knietty, "discover_network_devices", return_value=[]):
            with self.assertRaisesRegex(knietty.KniettyError, "no knietty"):
                knietty.discover_network_device()
        with mock.patch.object(knietty, "discover_network_devices", return_value=[device, device]):
            with self.assertRaisesRegex(knietty.KniettyError, "multiple"):
                knietty.discover_network_device()


class PtyTest(unittest.TestCase):
    def test_portable_window_size_round_trip(self) -> None:
        master, slave = pty.openpty()
        try:
            knietty.set_pty_size(slave, 42, 21)
            self.assertEqual(knietty.get_pty_size(master), (42, 21))
        finally:
            os.close(master)
            os.close(slave)

    def test_tmux_default_and_shell_fallback(self) -> None:
        with mock.patch.object(knietty.shutil, "which", return_value="/usr/bin/tmux"):
            self.assertEqual(knietty.default_command(), "tmux new-session -A -s knietty")
        with mock.patch.object(knietty.shutil, "which", return_value=None), mock.patch.dict(
            os.environ, {"SHELL": "/bin/zsh"}
        ):
            self.assertEqual(knietty.default_command(), "/bin/zsh")

    def test_spawned_child_receives_terminal_environment(self) -> None:
        session = knietty.PtySession.spawn("printf '%s:%s:%s' \"$TERM\" \"$COLUMNS\" \"$LINES\"", 42, 21, "vt100")
        try:
            ready, _, _ = select.select([session.master_fd], [], [], 2)
            self.assertEqual(ready, [session.master_fd])
            self.assertIn(b"vt100:42:21", os.read(session.master_fd, 128))
            self.assertEqual(session.child.wait(timeout=2), 0)
        finally:
            session.close()

    def test_ctrl_c_byte_signals_only_the_pty_child_group(self) -> None:
        session = knietty.PtySession.spawn("exec sleep 30", 80, 24, "vt100")
        try:
            self.assertNotEqual(os.getpgrp(), os.getpgid(session.child.pid))
            os.write(session.master_fd, b"\x03")
            self.assertIn(session.child.wait(timeout=2), (-knietty.signal.SIGINT, 128 + knietty.signal.SIGINT))
        finally:
            session.close()

    def test_local_input_restores_terminal_mode(self) -> None:
        master, slave = pty.openpty()
        original = knietty.termios.tcgetattr(slave)
        local_input = knietty.LocalInput(slave)
        try:
            local_input.enable()
            active = knietty.termios.tcgetattr(slave)
            self.assertEqual(active[3] & knietty.termios.ICANON, 0)
            self.assertEqual(active[3] & knietty.termios.ISIG, 0)
        finally:
            local_input.close()
            os.close(master)
            restored = knietty.termios.tcgetattr(slave)
            os.close(slave)
        self.assertEqual(restored, original)

    def test_local_ctrl_backslash_exits_after_forwarding_prior_input(self) -> None:
        session = SimpleNamespace(master_fd=99)
        bridge = knietty.NetworkBridge(session, "x4", 29380, 1, 1, 1, 65536, False, 42)
        writes: list[tuple[int, bytes]] = []

        def capture_write(fd: int, data: bytes) -> int:
            writes.append((fd, bytes(data)))
            return len(data)

        with mock.patch.object(knietty.os, "read", return_value=b"abc\x03\x1cignored"), mock.patch.object(
            knietty.os, "write", side_effect=capture_write
        ):
            bridge._read_local_input()
        self.assertEqual(writes, [(99, b"abc\x03")])
        self.assertTrue(bridge.local_exit_requested)
        self.assertEqual(bridge.pending_input, b"")

    def test_retry_wait_can_be_interrupted_by_local_ctrl_backslash(self) -> None:
        session = SimpleNamespace(master_fd=99)
        bridge = knietty.NetworkBridge(session, "x4", 29380, 1, 1, 1, 65536, False, 42)
        with mock.patch.object(knietty.select, "select", return_value=([42], [], [])), mock.patch.object(
            knietty.os, "read", return_value=b"\x1c"
        ):
            self.assertFalse(bridge.wait_for_retry(300))

    def test_repeated_discovery_error_is_rate_limited(self) -> None:
        session = SimpleNamespace(master_fd=99)
        bridge = knietty.NetworkBridge(session, "auto", 29380, 1, 1, 1, 65536, True)
        with mock.patch.object(bridge, "log") as log, mock.patch.object(
            knietty.time, "monotonic", side_effect=(10.0, 20.0, 41.0)
        ):
            bridge.log_retry_error(knietty.KniettyError("not found"))
            bridge.log_retry_error(knietty.KniettyError("not found"))
            bridge.log_retry_error(knietty.KniettyError("not found"))
        self.assertEqual(log.call_count, 2)

    def test_v3_bridge_frames_pty_output(self) -> None:
        session = SimpleNamespace(master_fd=99)
        bridge = knietty.NetworkBridge(session, "x4", 29380, 1, 1, 1, 65536, False)
        bridge.protocol_version = 3
        connection = mock.Mock()
        writes: list[bytes] = []

        def capture_send(data: bytes) -> int:
            writes.append(bytes(data))
            return len(data)

        connection.send.side_effect = capture_send
        bridge.connection = connection
        with mock.patch.object(knietty.os, "read", return_value=b"abc"):
            bridge._write_network()
        self.assertEqual(
            writes,
            [knietty_protocol.encode_frame(knietty_protocol.FrameType.TERMINAL_OUTPUT, b"abc", 1)],
        )
        self.assertEqual(bridge.next_tx_sequence, 2)

    def test_v3_bridge_decodes_fragmented_device_input(self) -> None:
        session = SimpleNamespace(master_fd=99)
        bridge = knietty.NetworkBridge(session, "x4", 29380, 1, 1, 1, 65536, False)
        bridge.protocol_version = 3
        encoded = knietty_protocol.encode_frame(knietty_protocol.FrameType.TERMINAL_INPUT, b"\x1b[A", 9)
        connection = mock.Mock()
        connection.recv.side_effect = (encoded[:5], encoded[5:])
        bridge.connection = connection
        with mock.patch.object(bridge, "_flush_pty_input"):
            bridge._read_network()
            self.assertEqual(bridge.pending_input, b"")
            bridge._read_network()
        self.assertEqual(bridge.pending_input, b"\x1b[A")


class FrameProtocolTest(unittest.TestCase):
    def test_golden_vector(self) -> None:
        encoded = knietty_protocol.encode_frame(
            knietty_protocol.FrameType.TERMINAL_OUTPUT,
            b"abc",
            0x01020304,
        )
        self.assertEqual(encoded, bytes.fromhex("01 00 00 03 01 02 03 04 61 62 63"))

    def test_every_fragment_boundary_and_coalesced_frames(self) -> None:
        first = knietty_protocol.encode_frame(knietty_protocol.FrameType.TERMINAL_OUTPUT, b"abc", 1)
        second = knietty_protocol.encode_frame(knietty_protocol.FrameType.HEARTBEAT, b"", 2)
        expected = [
            knietty_protocol.Frame(knietty_protocol.FrameType.TERMINAL_OUTPUT, 0, 1, b"abc"),
            knietty_protocol.Frame(knietty_protocol.FrameType.HEARTBEAT, 0, 2, b""),
        ]
        for split in range(len(first) + 1):
            decoder = knietty_protocol.FrameDecoder()
            decoded = decoder.feed(first[:split]) + decoder.feed(first[split:] + second)
            self.assertEqual(decoded, expected)

    def test_rejects_nonzero_flags_and_oversized_length(self) -> None:
        decoder = knietty_protocol.FrameDecoder()
        with self.assertRaisesRegex(knietty_protocol.FrameProtocolError, "flags"):
            decoder.feed(bytes.fromhex("01 01 00 00 00 00 00 01"))
        decoder.reset()
        with self.assertRaisesRegex(knietty_protocol.FrameProtocolError, "exceeds"):
            decoder.feed(bytes.fromhex("01 00 02 01 00 00 00 01"))

    def test_sequence_rollover_and_optional_type_helpers(self) -> None:
        encoded = knietty_protocol.encode_frame(knietty_protocol.FrameType.HEARTBEAT, b"", 0xFFFFFFFF)
        self.assertEqual(knietty_protocol.FrameDecoder().feed(encoded)[0].sequence, 0xFFFFFFFF)
        self.assertTrue(knietty_protocol.is_optional_frame_type(0x80))
        self.assertFalse(knietty_protocol.is_optional_frame_type(0x07))

    def test_diagnostic_commands_are_named_and_bounded(self) -> None:
        self.assertEqual(
            knietty_protocol.encode_diagnostic_command(
                knietty_protocol.DiagnosticCommand.PATTERN,
                knietty_protocol.DiagnosticPattern.ROW,
                1,
            ),
            b"\x03\x03\x01",
        )
        with self.assertRaisesRegex(ValueError, "variant"):
            knietty_protocol.encode_diagnostic_command(
                knietty_protocol.DiagnosticCommand.PATTERN,
                knietty_protocol.DiagnosticPattern.ROW,
                2,
            )

    def test_decodes_session_metadata_and_refresh_event(self) -> None:
        build = b"1.5.0-test"
        session = struct.pack(
            "!11Bb3B2H2IB",
            1,
            knietty_protocol.DiagnosticCommand.SESSION_INFO,
            knietty_protocol.DiagnosticStatus.ACCEPTED,
            0,
            0,
            20,
            0,
            3,
            0,
            0,
            87,
            -51,
            80,
            24,
            1,
            800,
            480,
            50000,
            42000,
            len(build),
        ) + build + bytes((7,)) + b"abc1234"
        response = knietty_protocol.decode_diagnostic_response(session)
        self.assertEqual(response.metadata["rssi_dbm"], -51)
        self.assertEqual(response.metadata["build"], "1.5.0-test")
        self.assertEqual(response.metadata["freeink"], "abc1234")
        self.assertEqual(response.metadata["font"], 1)

        values = tuple(range(1, 32))
        event_payload = struct.pack(
            "!8B15I8HIHBB4I",
            1,
            knietty_protocol.DiagnosticEventPhase.PRESENTED,
            knietty_protocol.DiagnosticCommand.PATTERN,
            1,
            1,
            0,
            2,
            0,
            *values,
        )
        event = knietty_protocol.decode_diagnostic_refresh_event(event_payload)
        self.assertEqual(event.values["timestamp_us"], 1)
        self.assertEqual(event.values["minimum_free_heap"], 31)

    def test_device_timestamp_order_handles_uint32_wrap(self) -> None:
        self.assertTrue(knietty_protocol.u32_before_or_equal(0xFFFFFFF0, 0x00000010))
        self.assertFalse(knietty_protocol.u32_before_or_equal(100, 99))


class DiagnosticClientTest(unittest.TestCase):
    @staticmethod
    def refresh_payload(
        phase: int,
        timestamp: int,
        *,
        first_sequence: int = 7,
        last_sequence: int = 7,
        coalesced: int = 1,
        command: int = knietty_protocol.DiagnosticCommand.RESET,
        queue_depth: int = 0,
    ) -> bytes:
        timing = [timestamp] + [0] * 14
        trailing = [0] * 8 + [0, 1, 1, coalesced, first_sequence, last_sequence, 50000, 49000]
        return struct.pack(
            "!8B15I8HIHBB4I",
            1,
            phase,
            command,
            1,
            1,
            0,
            2,
            queue_depth,
            *timing,
            *trailing,
        )

    def test_request_writes_complete_ordered_json_lines_without_a_pty(self) -> None:
        client = knietty.DiagnosticClient("x4", 29380, 1, 1, 1, knietty.Path("unused"), False)
        connection = mock.Mock()
        client.connection = connection
        client.sequence = 7
        client.pending_frames = [
            knietty_protocol.Frame(
                knietty_protocol.FrameType.CONTROL_RESPONSE,
                0,
                7,
                bytes((1, knietty_protocol.DiagnosticCommand.RESET, 0, 0)),
            ),
            knietty_protocol.Frame(
                knietty_protocol.FrameType.REFRESH_EVENT,
                0,
                7,
                self.refresh_payload(knietty_protocol.DiagnosticEventPhase.PRESENTED, 0xFFFFFFF0),
            ),
            knietty_protocol.Frame(
                knietty_protocol.FrameType.REFRESH_EVENT,
                0,
                7,
                self.refresh_payload(knietty_protocol.DiagnosticEventPhase.READY, 0x10),
            ),
        ]
        output = io.StringIO()
        client._request(output, knietty_protocol.DiagnosticCommand.RESET, expect_refresh=True)
        records = [knietty.json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual([record["record"] for record in records], ["response", "refresh", "refresh"])
        self.assertEqual([record.get("phase") for record in records], [None, 1, 2])
        self.assertEqual(connection.sendall.call_count, 1)

    def test_cadence_batch_accepts_one_coalesced_refresh_range(self) -> None:
        client = knietty.DiagnosticClient("x4", 29380, 1, 1, 1, knietty.Path("unused"), False)
        connection = mock.Mock()
        client.connection = connection
        client.sequence = 7
        accepted = bytes((1, knietty_protocol.DiagnosticCommand.PATTERN, 0, 0))
        client.pending_frames = [
            knietty_protocol.Frame(knietty_protocol.FrameType.CONTROL_RESPONSE, 0, 7, accepted),
            knietty_protocol.Frame(knietty_protocol.FrameType.CONTROL_RESPONSE, 0, 8, accepted),
            knietty_protocol.Frame(
                knietty_protocol.FrameType.REFRESH_EVENT,
                0,
                8,
                self.refresh_payload(
                    knietty_protocol.DiagnosticEventPhase.PRESENTED,
                    100,
                    first_sequence=7,
                    last_sequence=8,
                    coalesced=2,
                    command=knietty_protocol.DiagnosticCommand.PATTERN,
                    queue_depth=1,
                ),
            ),
            knietty_protocol.Frame(
                knietty_protocol.FrameType.REFRESH_EVENT,
                0,
                8,
                self.refresh_payload(
                    knietty_protocol.DiagnosticEventPhase.READY,
                    110,
                    first_sequence=7,
                    last_sequence=8,
                    coalesced=2,
                    command=knietty_protocol.DiagnosticCommand.PATTERN,
                    queue_depth=1,
                ),
            ),
        ]
        requests = (
            (knietty_protocol.DiagnosticPattern.CELL_MIDDLE, 1, {"sample_index": 1}),
            (knietty_protocol.DiagnosticPattern.CELL_MIDDLE, 0, {"sample_index": 2}),
        )
        output = io.StringIO()
        client._send_pattern_batch(output, requests, interval_ms=0, context={"interval_ms": 25})
        records = [knietty.json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual([record["record"] for record in records], ["response", "response", "refresh", "refresh"])
        self.assertEqual(records[-1]["coalesced"], 2)
        self.assertEqual((records[-1]["first_sample_index"], records[-1]["last_sample_index"]), (1, 2))
        self.assertEqual(connection.sendall.call_count, 2)


class CliTest(unittest.TestCase):
    def test_defaults_match_firmware_geometry(self) -> None:
        args = knietty.build_parser().parse_args([])
        self.assertEqual((args.cols, args.rows, args.term), (80, 24, "vt100"))
        self.assertEqual((args.transport, args.host), ("wifi", "auto"))
        self.assertIsNone(args.local_input)
        self.assertFalse(args.reconnect)
        self.assertEqual(args.protocol, "auto")

    def test_usb_id_parser_accepts_hex(self) -> None:
        self.assertEqual(knietty.parse_usb_id("0x303a"), 0x303A)

    def test_diagnostics_cli_is_explicit_and_has_no_pty_options(self) -> None:
        args = knietty.build_diagnostics_parser().parse_args(["--output", "run.jsonl"])
        self.assertEqual((args.host, args.suite, args.repetitions), ("auto", "smoke", 3))
        self.assertFalse(hasattr(args, "command"))


if __name__ == "__main__":
    unittest.main()
