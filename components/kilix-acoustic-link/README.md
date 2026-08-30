# kilix-acoustic-link

Bounded, reliable short-message transfer between nearby devices over
speakers and microphones, with the PCM owned by the caller.

**State: BUILT — AWAITING INDEPENDENT ACCEPTANCE.**
**Physical profiles graduated: 0/6.** No profile in this component has been
qualified on real hardware in a real room. Every number below is synthetic or
engine-derived. See [Physical qualification](#physical-qualification).

## What it is

A C11 library that wraps a pinned [ggwave](THIRD-PARTY-NOTICES.md) modem
behind a stable Kilix ABI and adds everything the modem deliberately omits:
fixed 64-byte KAL1 frames, persistent sessions, selective-repeat
retransmission, duplicate suppression, bounded reassembly, whole-message
SHA-256 validation, cancellation and diagnostics. A diagnostic CLI and the
audio child adapters sit outside the library core.

The core never opens a device, never spawns a process, never reaches the
network, never calls back into application code and never allocates after
`kal_link_create`. Audio in and audio out are `int16_t` buffers the caller
supplies.

## What it is not

The acoustic channel is **observable, recordable, replayable, jammable and
relayable**. Directional sound and physical distance do **not** authenticate
a peer, and this component does not claim they do. The `HELLO` challenge is a
liveness check on one exchange; it is not authentication. Encryption,
identity and authorization belong to the consumer.

The high-frequency profile is called high-frequency, **not inaudible**. It is
refused unless the consumer sets `allow_high_frequency`, because hardware
response varies and some people and animals hear it.

## Build and test

```sh
make            # static + shared library, diagnostic tool, pkg-config file
make test       # normal and AddressSanitizer/UndefinedBehaviorSanitizer runs
make install DESTDIR=/staging PREFIX=/usr
make check-offline
```

Everything is vendored: the build and the tests make no network access.

### Current evidence — normal build

| Suite | Result |
| --- | ---: |
| `test_frame` — KAL1 codec conformance | 38/38 assertions, 4,759/4,759 items |
| `test_link` — reliability matrix under virtual time | 74/74 assertions, 43/43 items |
| `test_bounds` — bounds, hostile input, integrity | 52/52 assertions, 21/21 items |
| `test_modem` — pinned-engine conformance | 22/22 assertions, 20/20 items |
| `test_channel` — synthetic impairment matrix | 106/106 assertions, 474/474 items |
| `test_adapter` — audio child lifecycle | 27/27 assertions, 205/205 items |
| `test_release_guard` — shipped library rejects the test backend | 4/4 assertions |
| `fuzz_frame` — parser fuzz | 200,019/200,019 inputs, 0 canonical failures |

The same six suites plus the fuzz target run again under
AddressSanitizer + UndefinedBehaviorSanitizer with `-fno-sanitize-recover=all`:
**7/7 sanitizer binaries pass.**

These are synthetic results. They graduate **0/6** physical profiles.

## Using it

```c
#include <kilix_acoustic_link.h>

kal_options options = {0};
options.sample_rate = KAL_SAMPLE_RATE;      /* 48000, mono, s16le */
options.profile     = KAL_PROFILE_AUDIBLE_NORMAL;
options.role        = KAL_ROLE_INITIATOR;
options.modem       = KAL_MODEM_GGWAVE;

kal_link *link = NULL;
if (kal_link_create(&link, &options) != KAL_OK) { /* handle */ }

kal_send(link, message, message_size);

/* drive from your own audio loop, with your own monotonic clock */
kal_tx_pull_s16(link, out_pcm, capacity, &written, now_ms);
kal_rx_push_s16(link, in_pcm, sample_count, now_ms);

uint8_t buffer[4096];
size_t  size = 0;
if (kal_receive(link, buffer, sizeof buffer, &size) == KAL_OK) { /* use it */ }
```

Bounds: one active transmit and one active receive message per link, messages
of at most 4,096 bytes, beacons of at most 44 bytes, windows of 1 to 16
frames, and a duplicate cache of 8 entries with airtime-derived expiry.

The engine keeps a process-global table of 4 instances, so at most **4/4**
ggwave-backed links can exist in one process. Exhaustion is reported as
`KAL_ERR_MEMORY`.

One thread owns one `kal_link`.

## Profiles and airtime

Fixed 64-byte frames at 48 kHz mono, measured through this component against
the pin:

| Profile | Samples per frame | Airtime | Graduated |
| --- | ---: | ---: | ---: |
| `audible-normal` | 276,480 | 5.760 s | 0/1 |
| `audible-fast` | 184,320 | 3.840 s | 0/1 |
| `audible-fastest` | 92,160 | 1.920 s | 0/1 |
| `dt-normal` | 811,008 | 16.896 s | 0/1 |
| `dt-fast` | 540,672 | 11.264 s | 0/1 |
| `high-normal` | 276,480 | 5.760 s | 0/1 |

Reproduce with `kilix-acoustic-link probe`. These are engine numbers on a
lossless in-memory channel. They are a floor, not a room measurement.

## Channel behaviour under a synthetic impairment model

One KAL1 frame is modulated by the pinned engine, damaged by a named
impairment, and offered back to a receiving link. **Every waveform is in
memory. This is not a room, a transducer, a microphone or a loudspeaker.**
It graduates **0/6** physical profiles, and it decides profile choice in
**0/1** cases — profile choice remains undecidable from digital results.

What it does give is a reproducible floor and an ordering: an impairment a
profile already fails here cannot be expected to pass in air.

| Impairment | audible-normal | audible-fast | audible-fastest | dt-normal | dt-fast | high-normal | Asserted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `clean` | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | yes |
| `awgn-20dB` | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | yes |
| `awgn-6dB` | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | yes |
| `awgn-0dB` | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | yes |
| `awgn-minus9dB` | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | yes |
| `awgn-minus11dB` | 3/3 | 3/3 | 2/3 | 3/3 | 3/3 | 3/3 | measured |
| `awgn-minus12dB` | 3/3 | 0/3 | 0/3 | 3/3 | 3/3 | 3/3 | measured |
| `gain-0.25x` | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | yes |
| `gain-0.003x` | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | yes |
| `gain-4x-sat` | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | yes |
| `clip-0.5` | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | yes |
| `clip-0.005` | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | yes |
| `drift-plus100` | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | yes |
| `drift-minus100` | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | yes |
| `drift-plus1000` | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | yes |
| `drift-plus2000` | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | 0/3 | measured |
| `drift-plus5000` | 0/3 | 0/3 | 0/3 | 3/3 | 3/3 | 0/3 | measured |
| `drift-plus10000` | 0/3 | 0/3 | 0/3 | 0/3 | 0/3 | 0/3 | measured |
| `echo-10ms-0.5` | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | yes |
| `echo-20ms-0.9` | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | yes |
| `echo-50ms-0.25` | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 | yes |

378/378 trials executed; 96/96 asserted cells fully delivered.

Three things this measured, none of which was assumed:

1. **Amplitude barely matters.** Every profile survived 0.003x gain, 4x
   saturating gain and clipping at 0.5% of full scale. Levels and automatic
   gain control are not where this modem breaks.
2. **Noise tolerance orders the profiles.** All six survive to −9 dB SNR
   under this model. At −11 dB `audible-fastest` becomes marginal (2/3); at
   −12 dB both `audible-fast` and `audible-fastest` fail (0/3) while
   `audible-normal`, both dual-tone profiles and `high-normal` still deliver
   3/3.
3. **Sample-clock mismatch is the sharpest edge, and it is the one two real
   devices always have.** All profiles survive ±1,000 ppm. `high-normal`
   fails first at 2,000 ppm — its carrier is highest, so the same relative
   error is the largest absolute error. The audible profiles fail at
   5,000 ppm; the dual-tone profiles still deliver 3/3 there and fail by
   10,000 ppm.

Finding 3 is the one to carry into the room: an audio server that resamples,
or two devices whose clocks disagree by more than about 0.1%, is a more
likely failure mode here than either noise or level. **It does not select a
profile.** It says which measurement the room matrix should make sure to
take.

Reproduce, or extend with more trials:

```sh
kilix-acoustic-link channel                       # all profiles, 3 trials
kilix-acoustic-link channel --profile dt-normal --seconds 10
```

## Diagnostic tool

```sh
kilix-acoustic-link probe
kilix-acoustic-link channel --profile audible-fastest
kilix-acoustic-link loopback --profile audible-fastest --text "hello"
kilix-acoustic-link dump-frame --hex <128 hex characters>
kilix-acoustic-link beacon --text hi --output-pcm beacon.raw
kilix-acoustic-link receive --input-pcm beacon.raw
```

`loopback` runs both ends in one process over the real modem with the PCM in
memory: it opens no device and makes no sound.

**Device-backed commands refuse to run.** `send`, `receive`, `listen`,
`beacon` to a device, and `calibrate` all require both `--allow-audio` and
`KILIX_ACOUSTIC_LINK_ALLOW_AUDIO=1`, and F111's physical-qualification
authorization has not been granted, so on this workspace they must refuse
rather than emit sound.

## Physical qualification

Nothing in this repository is physical evidence. The synthetic suites, the
in-memory loopback and the airtime table are all lossless-channel results.

**Blocked, with its exact condition and owner:**

> **Condition:** graduating any physical profile needs the named two-device
> real-room matrix to be executed and witnessed — quiet and noisy rooms,
> distance/orientation/gain legs, capture DSP on and off, a small-board
> device, and a concurrent second link pair — with the equipment and rooms
> bound to named devices, and with sound authorized. The recorded population
> is **0/34 equipment and rooms, 0/18,204 blocks, groups and observations,
> and 0/1 sound authorizations.**
>
> **Owner:** the release owner and the F111 qualification operator, with an
> independent witness, and an independent qualification reviewer for
> graduation itself.

Until that is executed, `0/6` profiles are graduated and this component
must not be described as qualified.

## Wire

[`docs/KAL1-WIRE.md`](docs/KAL1-WIRE.md). **The KAL1 wire is a candidate and
is not frozen**; freezing is a joint F111/F113 decision and is not made here.

## Licence

MIT, see [`LICENSE`](LICENSE). Vendored third-party notices are in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).
