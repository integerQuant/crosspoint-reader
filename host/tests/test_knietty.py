from __future__ import annotations

import os
import pty
import select
import sys
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))

import knietty


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
    def test_discovery_response(self) -> None:
        self.assertEqual(
            knietty.parse_discovery_response(b"KNIETTY/1 HERE knietty-aabbcc 29380\n", "192.0.2.10"),
            knietty.NetworkDevice("knietty-aabbcc", "192.0.2.10", 29380, "knietty-aabbcc"),
        )
        self.assertIsNone(knietty.parse_discovery_response(b"not knietty\n", "192.0.2.10"))

    def test_accept_response_contains_geometry(self) -> None:
        self.assertEqual(knietty.parse_server_response(b"KNIETTY/1 ACCEPT 50 22\n"), (50, 22))
        self.assertEqual(knietty.parse_server_response(b"KNIETTY/2 ACCEPT 80 24\n"), (80, 24))

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


class CliTest(unittest.TestCase):
    def test_defaults_match_firmware_geometry(self) -> None:
        args = knietty.build_parser().parse_args([])
        self.assertEqual((args.cols, args.rows, args.term), (80, 24, "vt100"))
        self.assertEqual((args.transport, args.host), ("wifi", "auto"))
        self.assertIsNone(args.local_input)
        self.assertFalse(args.reconnect)

    def test_usb_id_parser_accepts_hex(self) -> None:
        self.assertEqual(knietty.parse_usb_id("0x303a"), 0x303A)


if __name__ == "__main__":
    unittest.main()
