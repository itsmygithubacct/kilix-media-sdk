# Third-party notices — kilix-acoustic-link 0.1.0

## ggwave (the modem)

| Field | Value |
| --- | --- |
| Upstream | `https://github.com/ggerganov/ggwave` |
| Tag | `ggwave-v0.4.3` |
| Commit | `a38e38b7373f9adf45baf737a104206664b225a1` |
| Upstream git tree | `d4b7e59fd7f989be2e1322d53888a505e3154f1b` |
| Licence | MIT |
| Upstream `LICENSE` SHA-256 | `463a87b0f9e9c23ccf82e4cae3ea50fc4653ed63fca5e3e148f6513cf03d38ae` |
| Delivery | vendored source under `third_party/ggwave`; not a submodule, not a wheel, not a build-time download |
| Vendored tree digest | `24b5b18deb767e4ce1156aed0f9359b79c6ade2b647848ca3c6a255827307de2` |

The vendored tree digest is reproduced with:

```sh
(cd third_party/ggwave && find . -type f | sort | xargs sha256sum | sha256sum)
```

### Vendored file inventory — 8/8 files

| Path | SHA-256 |
| --- | --- |
| `LICENSE` | `463a87b0f9e9c23ccf82e4cae3ea50fc4653ed63fca5e3e148f6513cf03d38ae` |
| `include/ggwave/ggwave.h` | `ed03a28e7299a2d686bf28d747b177a1ead059e0563e11807ee6e96fa9e3dda0` |
| `src/fft.h` | `1ec89c52f15a9bb25badbb6dde4612d5aedb445d946a7f571c4363523903de55` |
| `src/ggwave.cpp` | `97404255fe603511e7123efa1bf41b25fd7bdc80e837ac4742a3170fab287a0f` |
| `src/reed-solomon/LICENSE` | `5bf6ddd498c8be8f49960f331c883449ce7bbd8a40bb9c969fc6e0c441cc731b` |
| `src/reed-solomon/gf.hpp` | `4b0f806c4bda5dfee0a01343b664e83d2064a198c1c7cc69b3ae36d8dfba3055` |
| `src/reed-solomon/poly.hpp` | `9c8fa3b3099144009002a8543a11d756e26d633ac005f1e07ce0e5eff900f020` |
| `src/reed-solomon/rs.hpp` | `b1e42030ed26981c2074152a839d2fa3006af97925f687052e8497e02497c2b7` |

### Modifications

**No upstream source file is modified.** The vendored set is the modem
library only: `src/ggwave.cpp`, `src/fft.h`, `src/reed-solomon/` and
`include/ggwave/ggwave.h`, plus both licences. Upstream's examples,
bindings (Python, JavaScript, iOS/Swift), SDL support, tests, media and
snap packaging are **not vendored**, and no upstream build system is used:
this component compiles the engine with its own Makefile.

That choice also removes a real upstream build hazard. Upstream's top-level
`CMakeLists.txt` unconditionally reads `bindings/ios/Makefile-tmpl`, so a
non-recursive clone fails to configure even with Swift, Python, SDL and the
examples disabled. Building only `src/` with our own rules avoids needing
either a build-system patch or the irrelevant submodules.

### Upstream behaviour recorded during integration

In `ggwave-v0.4.3`, calling the **size-query form** of `ggwave_encode`
(the `query != 0` overload) on an instance permanently disables that
instance's decoder. A link that sized its own transmit buffer on its own
instance would transmit correctly and never receive again. A real encode
(`query == 0`) has no such effect. This component therefore performs the
size query on a throwaway instance that is freed before the working instance
is created; see `src/kal_modem.c`. This is recorded as observed upstream
behaviour at the pin, not as a defect report.

The engine also keeps a process-global instance table of
`GGWAVE_MAX_INSTANCES` = 4 entries, so at most **4/4** ggwave-backed links
can exist in one process; exhaustion is reported as `KAL_ERR_MEMORY` rather
than worked around.

## Reed-Solomon (inside ggwave)

`third_party/ggwave/src/reed-solomon/` carries its own MIT licence at
`third_party/ggwave/src/reed-solomon/LICENSE`, retained verbatim.

## Development-only tools

Python `ggwave`, NumPy, SciPy, SoX and `uv` are **not** runtime or build
dependencies of this component. Nothing in this tree downloads anything at
build or test time; `make check-offline` asserts it.
