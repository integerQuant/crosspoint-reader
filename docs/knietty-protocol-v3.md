# knietty protocol v3

Protocol v3 keeps the v1 UDP discovery request and TCP port so old and new
hosts find the same device. It replaces the post-approval raw stream with
bounded frames that can later be wrapped unchanged by TLS.

## Negotiation

All negotiation lines are ASCII, newline terminated, and limited to 127 bytes.
The client name is 1–32 sanitized characters. Integers are decimal.

Terminal request:

```text
KNIETTY/3 HELLO terminal frame <epoch> <UTC-offset-minutes> <client-name>\n
```

Diagnostics request:

```text
KNIETTY/3 HELLO diagnostics frame,diag1 <epoch> <UTC-offset-minutes> <client-name>\n
```

Acceptance is mode-specific:

```text
KNIETTY/3 ACCEPT 80 24 frame\n
KNIETTY/3 ACCEPT 80 24 frame,diag1\n
```

`KNIETTY/1 ERROR`, `DENY`, and `BUSY` retain their existing meanings. A v3 host
may reconnect with v2 and then v1 after `ERROR`. Firmware continues accepting
the exact existing v1/v2 greetings and uses their raw stream after approval.

## Frame format

Every multi-byte integer is unsigned and network byte order:

```text
u8 type | u8 flags | u16 payload_length | u32 sequence | payload
```

The header is 8 bytes. Payload length is at most 512 bytes and flags must be
zero in version 3. Initial types are:

| Value | Name | Direction |
| --- | --- | --- |
| `0x01` | `TERMINAL_OUTPUT` | host to X4 |
| `0x02` | `TERMINAL_INPUT` | X4 to host |
| `0x03` | `CONTROL_REQUEST` | host to X4 |
| `0x04` | `CONTROL_RESPONSE` | X4 to host |
| `0x05` | `REFRESH_EVENT` | X4 to host |
| `0x06` | `HEARTBEAT` | either direction |

Unknown types below `0x80`, nonzero flags, and oversized payloads are protocol
errors and close the connection. Types with bit `0x80` set are optional: a
receiver validates their header and bounded length, consumes them, and ignores
their payload. TCP provides ordering and integrity, so frames add no CRC.

Sequence numbers wrap modulo 2^32. Terminal data uses a monotonically
increasing sequence independently in each direction. Control responses and
refresh events copy the request sequence they describe.

Golden frame for sequence `0x01020304` and terminal payload `abc`:

```text
01 00 00 03 01 02 03 04 61 62 63
```

## Resource limits

Firmware decodes into one fixed 512-byte payload buffer and queues outbound
bytes in one fixed 1,024-byte ring. It never allocates per frame. A complete
frame can remain pending while its consumer drains it; the decoder does not
overwrite that payload.
