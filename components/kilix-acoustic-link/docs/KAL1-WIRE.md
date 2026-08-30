# KAL1 wire, version 1 — CANDIDATE

**This wire is not frozen.** MASTER's F111 row requires that the KAL1 ABI
must not freeze before the joint F111/F113 redesign settles, because
freezing first would lock the ceremony's framing overhead in and push the
expensive fix back into the cryptography. This document describes what the
implementation does today so that a reviewer can check it; it does not
declare a freeze, and freezing is not this component's decision to make.

## Frame

Every modem payload is exactly 64 bytes.

```text
offset  size  field
0       2     magic "KA"
2       1     version = 1
3       1     frame type
4       4     session id, big endian
8       2     message id, big endian
10      2     sequence / cumulative base, big endian
12      2     type-specific value, big endian
14      1     payload length, 0..44
15      1     flags
16      44    payload, zero padded
60      4     CRC-32C over bytes 0..59, big endian
```

CRC-32C (Castagnoli, reflected, polynomial `0x82f63b78`) catches framing and
parser corruption beyond the modem's Reed-Solomon decoding. **It is not
authentication.**

## Types

| Type | Value | `sequence` | `value` | `payload` | Flags |
| --- | ---: | --- | --- | --- | --- |
| `HELLO` | `0x01` | 0 | capability | 16-byte challenge | 0 |
| `HELLO_ACK` | `0x02` | 0 | capability | echoed challenge | 0 |
| `DATA` | `0x03` | segment index | total segments | 1..44 bytes | bit 0 `FINAL` |
| `ACK` | `0x04` | cumulative base | selective bitmap | empty | bit 0 `DELIVERED` |
| `CANCEL` | `0x05` | 0 | reason 0..4 | empty | 0 |
| `BEACON` | `0x06` | 0 | 0 | 1..44 bytes | 0 |

The capability word is `(profile << 8) | window_frames`.

Every other type value, every reserved flag bit and every non-zero pad byte
is rejected in version 1.

## Differences from the design sketch, and why

The design document sketches eight frame types including separate `END` and
`FIN` frames. This implementation has six, because the recorded F111/F113
ceremony airtime finding measured that **62% of the literal
ceremony's airtime was KAL1 framing overhead rather than cryptographic
payload**, and identified the two fixes that are F111's to make and that
alter no KP1 byte:

1. **`END` is folded into the final `DATA` frame.** The 32-byte SHA-256 of
   the application message is appended to the message and segmented with it,
   so the digest arrives inside the last `DATA` payload. The `FINAL` flag
   marks that frame. The receiver still validates the exact length and the
   whole-message digest before delivering anything.
2. **`FIN` is folded into the `DELIVERED` flag on the `ACK`.** A delivered
   ACK names the total segment count in `sequence`, carries no selective
   bitmap, and is cached so a lost delivered ACK is answered by replay rather
   than by a second delivery.

The third recorded fix, the **persistent session**, is also implemented: one
`HELLO`/`HELLO_ACK` pair opens a session that carries every subsequent
message in both directions. Initiator message ids are even and responder ids
odd, so the two directions cannot collide.

`tests/test_link.c` executes the ceremony shape: five messages of 102, 118,
102, 90 and 90 bytes over one session produce **18/18 DATA frames, 5/5
delivered ACKs and 2/2 session-opening frames = 25/25 transmissions**, which
is the recorded no-loss, no-human-delay floor.

The remaining recorded item, the 9→5 message collapse, is F113's and is not
in this component.

## Segment equation

```text
segments = ceil((application_bytes + 32) / 44)
```

with `application_bytes` in `0..4096`, giving `1..94` segments. A message of
4,096 bytes needs 94 segments. `tests/test_frame.c` checks all 4,097
application sizes.

## Reliability

- Window: 4 frames by default, configurable 1..16, negotiated down to the
  smaller of the two sides.
- Retries: 5 per window by default.
- Timeouts: derived from **measured frame airtime**, never from a wall-clock
  constant. The base is `(frames_in_flight + 1) x airtime x 1.25`; backoff
  doubles up to a ceiling of eight times the base. A fixed 30-second cap
  would be provably too low: a four-frame window at Dual-Tone Normal alone
  occupies 67.6 seconds of air.
- A partial `ACK` reports the first missing segment in `sequence` and sets
  bit *i* of `value` when segment `sequence + 1 + i` has been stored.
- Selective repeat: only unacknowledged segments that are not already queued
  are retransmitted.
- Simultaneous initiation: each side detects the collision by receiving a
  `HELLO` while awaiting `HELLO_ACK`; the numerically larger session id
  yields.

## Engine repeat suppression

The pinned engine can report one transmission more than once while its
waveform is still being consumed. A byte-identical frame arriving within two
frame airtimes is therefore treated as one reception, because a transmission
cannot physically repeat faster than its own airtime and a genuine
retransmission cannot arrive sooner than the minimum timeout of 2.5
airtimes. Without this, each repeat provoked an ACK replay, and the extra
airtime pushed the sender past its own deadline: measured over the real
modem, a 256-byte message cost 7 retransmissions and 30.72 s of air before
the fix and 0 retransmissions and 21.12 s after it.
