# native/ - Phase 2: the boundary between the recompiled game and the runtime

The recompiled game (`generated/default`, 106 C++ files) touches its runtime through one small, closed contract:
179 kernel imports, 8 runtime services and a flat 4 GB memory model. This directory holds that contract, the pieces that are
compiled against it and nothing else. Full description: [`docs/abi_recomp_runtime.md`](../../../docs/abi_recomp_runtime.md).

```
rt/abi/include/gh2abi/abi.h     the contract (runtime services); no SDK header, no `rex::`
rt/abi/shim/rex/                what the generated text includes: vendored CPU headers + one-line aliases (see rt/abi/VENDORED.md)
rt/abi/contract/imports.txt     the 179 kernel imports (checked against the generated code)
rt/hooks/                       hand-written hooks of imports and guest functions (shim only) + gh2rt::HostServices
rt/trace/                       boundary tracer (GH2_BOUNDARY_TRACE=<file>) + generated import wrappers (shim only)
rt/backend_rex/                 the contract implemented on the ReXGlue SDK (interim; the only code here that sees SDK headers)
platform/input_sdl3/            SDL3 input: guitar classifier, dedicated polling thread, lock-free snapshots (no `rex::`); see platform/README.md
platform/audio_sdl3/            SDL3 audio: lock-free 256x6 frame queue, AudioSink, XMA port interface (no `rex::`); see platform/README.md
gfx/shader_cache/               shaders.pak reader + unknown-hash log
gfx/guest_d3d/                  hooks of the game's own D3D library (G.0): shader creation + draw-state commit, discovery probe, observer; see its README
third_party/                    fetch.sh (XenosRecomp at a pinned commit, not committed) + patches/ (ours, applied by fetch.sh)
tools/                          checks, trace comparison, golden runs, generators (see below)
golden/                         summaries of golden runs (the traces themselves stay in work/analysis/golden, they are large)
```

## Build (from `app/CMakeLists.txt`)
- `GH2_ISOLATED_CPU=OFF` (default): generated code compiled against the SDK headers, as in the PoC; hooks and tracer are still
  compiled against `rt/abi` only.
- `GH2_ISOLATED_CPU=ON`: generated code compiled into `libgh2_cpu.a` against `rt/abi` only; services come from `rt/backend_rex`.
Configure a throw-away workspace with `tools/mkworkspace.sh NAME` (own copy of `generated/`; a new build directory always
re-runs the codegen step, which is a no-op when nothing changed), then `cmake -S work/gh2_v10_NAME -B .../out/build/main
-DCMAKE_PREFIX_PATH=<sdk> -DGH2_ISOLATED_CPU=ON ...` as for the dev project.

## Rules (CLAUDE.md, "Regras da Fase 2")
- `rt/abi/include`, `rt/hooks`, `rt/trace`, `platform` never use `rex::` (`platform` includes no `rex/` header either) and never see SDK headers; `tools/check_isolation.sh BUILD_DIR` enforces it
  (sources, include paths of the shim-only targets, symbols of `libgh2_cpu.a` and of the hook/trace objects).
- The generated code is never edited; if it starts to need a new external symbol, the contract (and `abi.h`) changes first.
- Reimplement, do not copy, code of the Xenia project. Nothing Xenia-derived lives under `native/` (`check_isolation` rule S2).

## Tools
| Tool | Purpose |
|---|---|
| `check_isolation.sh [BUILD_DIR]` | all the rules above |
| `external_surface.py FILE...` | external symbols of archives/objects, classified (contract / toolchain / FORBIDDEN / other) |
| `extract_imports.py GEN_DIR --check FILE` | `contract/imports.txt` follows the generated code |
| `gen_import_tracer.py [--check]` | regenerates `rt/trace/import_trace.gen.cpp` and `trace_wraps.cmake` from the contract |
| `check_vendored_drift.py` | vendored headers vs the SDK source they were copied from |
| `golden_run.py`, `run_golden_set.sh` | scripted, traced runs (title / autostart / normal) on a COPY of the saves |
| `trace_diff.py` | candidate traces vs golden traces: import set, exact counts, first-seen order, main-thread prefix, new failure codes |
| `postcodegen.sh DIR` | replaces `db16cyc` comments by `gh2_spin_hint()` calls after a codegen run |
| `mkworkspace.sh NAME` | throw-away project directory with its own `generated/` |
| `ark_extract.py GEN_DIR --summary\|--list\|--extract OUT --match RE` | reads the player's own Harmonix ARK (encrypted v3 header included); S.0 of the shader pipeline |
| `third_party/fetch.sh`, `build_xenosrecomp.sh` | clone XenosRecomp at the pinned commit, apply `third_party/patches/*.patch`, build it (~16 s) |
| `shader_pipeline.py --gen-dir GAME/gen --out DIR [--pe default.pe]` | S.0-S.5 on the player's machine: extract from the ARK (+ the containers embedded in the executable), XenosRecomp -> HLSL -> SPIR-V, `spirv-val`, `shaders.pak`, self-test; prints every success rate |
| `shader_cache_build.py`, `shader_coverage.py` | pack the SPIR-V into `shaders.pak` / compare the microcode the game submits (`--dump_shaders`) with the extracted containers |
| `d3d_window_report.py PREFIX functions\|toplevel\|sequence\|function\|draws\|swaps` | read a `GH2_D3D_PROBE_WINDOW` recording (every call of 3 frames with PM4 and pointer dumps) |
| `ark_manifest.py GAME_DIR OUT.tsv` | reference manifest (path, offset, size, crc32) of every ARK entry, to compare with `gh2_vfs_selftest GAME_DIR MANIFEST` |
| `gen_d3d_probes.py GENERATED_DIR`, `d3d_probe_report.py PREFIX ...` | regenerate the discovery hooks of the D3D block / read a `GH2_D3D_PROBE` recording |
| `audio_capture.py BUILD_DIR OUT_DIR sdk\|native [START] [SECS] [normal\|song]` | records the game's own PipeWire stream and compares `sdk` against `native` (level per channel, envelope correlation) |
| `audio_parity.cpp` (see its header) | bit-exact comparison of gh2plat's audio conversion with the SDK's own, on random frames |
| `input_ab.py BUILD_DIR OUT_DIR sdk\|native [SECS] [HZ] [title\|song]` | press -> guest latency in the real game: a uinput guitar presses, the hook (`GH2_INPUT_LOG`) timestamps what the guest sees |
