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

    def test_local_input_restores_terminal_mode(self) -> None:
        master, slave = pty.openpty()
        original = knietty.termios.tcgetattr(slave)
        local_input = knietty.LocalInput(slave)
        try:
            local_input.enable()
            active = knietty.termios.tcgetattr(slave)
            self.assertEqual(active[3] & knietty.termios.ICANON, 0)
        finally:
            local_input.close()
            os.close(master)
            restored = knietty.termios.tcgetattr(slave)
            os.close(slave)
        self.assertEqual(restored, original)


class CliTest(unittest.TestCase):
    def test_defaults_match_firmware_geometry(self) -> None:
        args = knietty.build_parser().parse_args([])
        self.assertEqual((args.cols, args.rows, args.term), (50, 22, "vt100"))
        self.assertEqual((args.transport, args.host), ("wifi", "auto"))
        self.assertIsNone(args.local_input)

    def test_usb_id_parser_accepts_hex(self) -> None:
        self.assertEqual(knietty.parse_usb_id("0x303a"), 0x303A)


if __name__ == "__main__":
    unittest.main()
