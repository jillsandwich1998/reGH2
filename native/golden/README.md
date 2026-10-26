# Golden runs

`oracle/` = the PoC as it was before the isolation (SDK-compiled generated code, hooks in `app/src/gh2_host.cpp`) with only the
boundary tracer added: 3 scenarios x 3 repetitions, 2026-10-23/24, windowed, on a copy of the saves.

| Scenario | What runs | Length |
|---|---|---|
| `title` | `GH2_SKIP_INTRO=1`: logos and intro skipped, stops at the title screen | 22 s |
| `autostart` | `GH2_AUTOSTART=1`: title, menus, Quick Play, a song loads (nobody plays it) | 50 s |
| `normal` | no skipping: logos and the intro video play | 28 s |

Kept here (small): `oracle_summary.txt` (one line per run: frames, late frames, exit code, whether the saves changed) and the
exact per-import counts (`*.counts.json`) with the import tables (`*.meta.json`). The event files (`*.trace`, 2.5-5 MB each) are
in `work/analysis/golden/oracle/` and are not versioned; the oracle binary that made them is `work/analysis/golden/oracle_bin/gh2`.

Regenerate / compare:
```
export GH2_USERDATA_PRISTINE=<copy of ~/.local/share/gh2>
native/tools/run_golden_set.sh <build dir with the tracer> <out dir> <prefix>
native/tools/trace_diff.py work/analysis/golden/oracle oracle <out dir> <prefix>
```

## F2.3-F2.5 (2026-10-24)
- `results/trace_diff_oracle_vs_f24.txt`, `results/f24_summary.txt`: the final build of the phase (native input and audio as defaults, D3D hooks idle) against the oracle,
  3 scenarios x 3 runs: PASS, 60.2-60.3 fps, saves unchanged. (One earlier single title run was out of the band; three repeats of the same binary were inside.)
- `results/d3d_observer/*.json`: what the D3D shader observer (GH2_D3D_OBSERVE) reported for the intro and for a Quick Play song: every bound shader mapped to the cache,
  pixel microcode identical to the container, vertex microcode with 1-18 words rewritten by the library.

- `results/f26_summary.txt`, `results/trace_diff_oracle_vs_f26.txt`, `results/d3d_observer/graphics_*_verify.json` (Phase 2.6, D3D hooks of the whole API, observer off): trace_diff against the 2026-10-23 oracle
  reports FAIL on `KeDelayExecutionThread` (2 vs 6), `MmQueryAddressProtect` (24 vs 36) and `XamInputGetCapabilities` (2320 vs 1169). **Not caused by the hooks:** the frozen PoC binary
  (`work/analysis/golden/oracle_bin`) run on 2026-10-24 after a reboot gives exactly 2 / 24 / 2320, the same as f26; everything else (import set, order, main-thread prefix, returns) passes.
  The oracle traces predate that environment change; re-record them before using them as a gate again.
- **Oracle re-recorded 2026-10-24** (after the reboot) with the frozen PoC binary: `work/analysis/golden/oracle` (the 2026-10-23 set is kept as `oracle_2026-10-23`), `results/oracle_2026-10-24_summary.txt`.
  `results/trace_diff_oracle_vs_f27.txt` (Phase 2.7, default `--vfs=sdk`): PASS in the three scenarios. `results/trace_diff_oracle_vs_f27n_native_vfs.txt` (`GH2_VFS=native`): the only difference is
  `RtlNtStatusToDosError` (about 160 calls in a title run with the runtime, 2 natively): the runtime answers file reads with STATUS_PENDING and the game converts statuses on that path; the native VFS completes
  synchronously. Import set, order and main-thread prefix otherwise pass. `results/vfs/*.json` = the shadow and native reports.
- **Phase 2.7 part 2**: with `--vfs=native` the default, every candidate differs from the oracle in `RtlNtStatusToDosError` only (documented above); judge with
  `trace_diff.py ... --expect-diff RtlNtStatusToDosError`. `results/trace_diff_oracle_vs_f28_vfs_native_default.txt` (default build) and `results/trace_diff_oracle_vs_f28s_sync_native.txt` (`GH2_SYNC=native`): PASS.

## Fase 3 (2026-10-24)
- `results/trace_diff_oracle_vs_f33_phase3_sdk_path.txt`, `results/f33_phase3_sdk_path_summary.txt`: o build da Fase 3 com `--gpu=sdk` (explícito nos scripts de golden) contra o oráculo, 3 cenários x 3 execuções: `autostart` PASS (0 fora da banda), 60,0-60,3 fps, saves intactos.
  `title` e `normal` repetem as mesmas diferenças de contagem já documentadas acima (KeDelay 6 vs 2, MmQuery 36 vs 24, XamInputGetCapabilities ~870 vs ~1788: o binário congelado do PoC as reproduz no ambiente atual); SET, ORDER e MAIN passam.
- A GPU nativa (`--gpu=native`, padrão) não tem golden de trace: o que ela produz é visual e foi medido com os contadores do renderer (ver `native/gfx/render/README.md`).
