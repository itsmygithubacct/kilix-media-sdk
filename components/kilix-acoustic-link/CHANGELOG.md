# Changelog — kilix-acoustic-link

## 0.1.0 — unreleased

First implementation. **BUILT — AWAITING INDEPENDENT ACCEPTANCE.**
Physical profiles graduated: **0/6**.

### Added

- C11 public ABI `kilix_acoustic_link.h`: link lifecycle, `kal_send`,
  `kal_beacon`, `kal_cancel`, caller-owned PCM in and out, `kal_receive`,
  `kal_tick`, `kal_next_deadline`, statistics and frame description.
- KAL1 frame codec: exact 64-byte frames, six types, CRC-32C, canonical-form
  validation, zero-padding and reserved-flag rejection.
- Session layer: persistent sessions, `HELLO`/`HELLO_ACK` capability
  negotiation with an echoed challenge, even/odd message-id split by role,
  and the larger-session-id-yields rule for simultaneous initiation.
- Selective-repeat ARQ: window 1..16, in-flight tracking, cumulative base
  plus 16-bit selective bitmap, airtime-derived adaptive timeouts with a
  bounded ceiling, and bounded retry exhaustion.
- Bounded reassembly with whole-message SHA-256 validation, an 8-entry
  duplicate cache with airtime-derived expiry, and exactly-once delivery
  across lost delivered ACKs.
- Pinned ggwave v0.4.3 vendored as modem-library source only, wrapped so no
  upstream type appears in the Kilix ABI.
- Diagnostic tool with `probe`, `channel`, `loopback`, `dump-frame`, file-backed
  `send`/`receive`/`beacon`, and `calibrate`; every device-backed path is
  refused without explicit authorization.
- Audio child adapters with fixed argv, no shell, bounded teardown and
  reaping.
- Synthetic channel impairment model (`tools/kal_channel.c`, diagnostic and
  outside the library core): additive white Gaussian noise at a target SNR,
  gain, hard clipping, sample-clock drift in ppm, and a single delayed
  reflection. Deterministic by seed, and self-checked — the achieved SNR is
  verified against the requested one.
- Test suites: codec conformance, reliability matrix under virtual time,
  bounds and hostile input, pinned-engine conformance, a channel impairment
  matrix, adapter lifecycle, a release-library guard, and a parser fuzz
  target; all run again under AddressSanitizer and UndefinedBehaviorSanitizer.

### Recorded during integration

- The size-query form of `ggwave_encode` permanently disables the same
  instance's decoder at this pin; the component sizes on a throwaway
  instance. See `THIRD-PARTY-NOTICES.md`.
- The engine can report one transmission more than once; byte-identical
  frames inside two frame airtimes are treated as one reception. Over the
  real modem this took a 256-byte transfer from 7 retransmissions and
  30.72 s of air to 0 retransmissions and 21.12 s.
- Selective repeat without in-flight tracking live-locked under a duplicating
  channel: every duplicate provoked an ACK, and every ACK re-queued the whole
  window. Found by the duplication scenario in `tests/test_link.c`.
- Under the synthetic impairment model, sample-clock mismatch is the sharpest
  edge: every profile survives +/-1,000 ppm, `high-normal` fails at 2,000 ppm,
  the audible profiles fail at 5,000 ppm and the dual-tone profiles survive
  5,000 ppm. Amplitude is nearly irrelevant by comparison (0.003x gain and
  0.5% clipping both deliver 3/3 everywhere). This orders the profiles under
  one model; it selects none.

### Not done, and why

- **0/6 physical profiles are graduated.** The two-device real-room matrix
  has not been executed and sound is not authorized. See `README.md`.
- **The KAL1 wire is not frozen.** Freezing is a joint F111/F113 decision.
