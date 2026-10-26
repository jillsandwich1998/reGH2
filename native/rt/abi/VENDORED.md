# Vendored headers (native/rt/abi/shim/rex)

The recompiled code includes `<rex/...>` headers. For it to compile without the SDK, the headers that are pure CPU foundation
(no runtime, no kernel, no logging library) were copied here; the rest are written for this project.

Origin: the ReXGlue SDK source used by the PoC (`work/sdk-src`, commit 9daf5b7, patch series on top of nightly bd833a2a),
BSD-3-Clause, Copyright (c) 2026 Tom Clay; per-file headers say "Based on XenonRecomp/UnleashedRecomp" (hedge-dev, MIT). The
file headers (copyright and license lines) are kept as they were. Third-party notice: `THIRD_PARTY_NOTICES.md`.

| File | Status | Note |
|---|---|---|
| `rex/ppc/context.h` | verbatim copy | `PPCContext`: registers, `lr`/`ctr`/`cr`/`xer`/`fpscr`, `v0..v127`, `last_indirect_target` |
| `rex/ppc/intrinsics.h` | verbatim copy | PowerPC vector semantics on top of simde |
| `rex/ppc/func.h` | copy, **one change** | `ResolveIndirectFunction` is declared in `gh2abi` and aliased to `rex::runtime` (diff in `func.h.expected.diff`) |
| `rex/types.h` | verbatim copy | integer aliases, `be<T>` endian-store types |
| `rex/platform/fpscr.h` | verbatim copy | FPSCR / rounding-mode helpers |
| `rex/image_info.h` | verbatim copy | `PPCImageInfo` (the generated `PPCImageConfig`) |
| `rex/platform.h`, `rex/assert.h` | **written for this project** | the SDK's versions of these two derive from the Xenia project (copyright Ben Vanik); ours define only the platform / architecture / compiler macros and the rotate builtins the generated code uses |
| `rex/ppc.h`, `rex/logging.h`, `rex/chrono/clock.h`, `rex/thread/mutex.h`, `rex/system/*.h`, `rex/perf/counter.h` | written for this project | one-line aliases onto `gh2abi::*` (logging.h: a tiny `{}` formatter) |

Why the data types keep the historical `rex::` / global names: while the SDK backend exists, the SDK compiles the same types from its
own headers and both sides must agree bit for bit. `native/tools/check_vendored_drift.py` compares the copies with the SDK source
(default `work/sdk-src/include`; `GH2_SDK_INCLUDE` or an argument override it) and fails if the SDK changed them. When the SDK is gone,
the copies become the single source of truth and can be renamed freely.
