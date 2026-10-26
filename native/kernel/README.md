# native/kernel - the kernel, piece by piece (Phase 2.7, F2.4 of the roadmap)

| piece | state | where |
|---|---|---|
| Files (K.3) | `--vfs=native` is the default (approved 2026-10-24) | `vfs/` (README there) |
| IRQL, spin locks, critical sections, TLS slots (K.2, first part) | `--sync=native` is the default (approved 2026-10-24; `sdk` to go back) | `sync/` + `rt/hooks/hooks_sync.cpp` |
| Events, semaphores, waits, handles (K.2, second part) | `--objects=native` is the default (approved 2026-10-24) | `sync/objects.*` + `rt/hooks/hooks_objects.cpp` |
| Memory (K.1) + arena swap (K.4) | the game runs on the gh2k arena and gh2k::mem decides every allocation (`--arena=native`, `--heaps=native`; SDK patches 0010, 0011) | `mem/` |
| Thread state (KPCR, KTHREAD, TLS block, guarded stack) | built natively by `gh2k::thread` | `thread/` |
| **The game's threads** (ExCreateThread, suspend/resume, delays, affinity, TLS slots, the main thread) | **native by default** (`--threads=native`, `--main_thread=native`; approved 2026-10-24): every guest thread of the game is ours, the runtime's main thread only waits | `rt/hooks/hooks_kthreads.cpp` |
| The game's audio client (render-driver callback) | **native by default** (`--audio_pump=native`): the callback runs on a native thread, straight into the SDL3 sink | `rt/hooks/hooks_audio.cpp` |
| XamInputGetState / GetCapabilities | **the imports are ours by default** (`--xam_input=native`); the pad itself (device assignment, dialogs owning the controller) still comes from the application through `HostServices::Pad*` | `rt/hooks/hooks_input.cpp` |

## What `sync` does (2.6 M of the 2.9 M kernel calls of a song)
`RtlEnter/LeaveCriticalSection` 1.25 M each, `KeRaiseIrqlToDpcLevel`/`KfLowerIrql`/`Ke*SpinLock*` 2.5 M, `KeTlsGetValue` 154 k, plus `RtlInitializeCriticalSection`,
`RtlTryEnterCriticalSection`, `KeTlsSetValue`, `KeTryToAcquireSpinLockAtRaisedIrql`. They run on the structures the guest sees, in guest memory (offsets read from the runtime's headers, FACTS):
KPCR (r13) +0x18 IRQL byte, +0x100 current KTHREAD; KTHREAD +0x68 TLS slots; KSPINLOCK = be32 owner PCR address; critical section = dispatcher header (type 1, spin count/256 at +1),
+0x10 lock count (raw int32, atomic, -1 free), +0x14 recursion (be32), +0x18 owner KTHREAD (be32). A contended section sleeps in a parking table keyed by its address (a wake token per Leave).
`KeEnter/LeaveCriticalRegion` (2.8 k calls) and `KeTlsAlloc/Free` stay with the runtime.
- Tests: `gh2_sync_selftest` (6 threads x 60 000 critical sections and spin lock sections, recursion, a sleeper woken by the holder) passes, also under ThreadSanitizer (`gh2_sync_selftest_tsan`, no warning).
- In the game (`GH2_SYNC=native`, vfs native): intro 60.1 fps, title 60.3, Quick Play song 60.3, no hang; 5-minute title soak 17 103 frames (60.2 fps; frames over 20/25/33 ms: 81/30/12, worst 115 ms)
  against 17 094 (117/31/11, worst 226 ms) with the runtime's primitives. Critical sections that had to sleep: 983 of 1.25 M in a song (0.08%), 2 406 in the soak. Golden traces: PASS in the three scenarios
  (`results/trace_diff_oracle_vs_f28s_sync_native.txt`; `--expect-diff RtlNtStatusToDosError` because the VFS completes reads synchronously).

## What the next layers need (read from the runtime's sources; HYPOTHESES for how to do it, FACTS for what the guest sees)
- A guest thread is: a stack (16 KB minimum) with guard pages, a TLS block (slots = XEX TLS header slot count, plus its raw data copied in front; `0(r13)` reaches the static part), a 0x2D8-byte PCR
  (tls_ptr, current_irql, prcb.current_thread, stack bounds, `pcr_ptr`, a host stash), a KTHREAD guest object (stack_base 0x5C, stack_limit 0x60, tls_address 0x68, priority 0x70, suspend_count 0xBC, ...)
  and a host thread with a PPCContext whose r13 is the PCR. All of it needs a guest heap: K.1 (memory) comes first.
- Events and semaphores are dispatcher objects in guest memory (KEVENT 0x10 bytes) with a host object behind them; the runtime keeps the link in the header's wait-list words. The audio worker, the XMA decoder and the notification
  listeners of the runtime signal events/semaphores the game waits on (9 000 `KeReleaseSemaphore` per song come from the audio path), so events cannot move before those producers do.
- The audio callback (`XAudioRegisterRenderDriverClient`) is a guest function the runtime's audio thread calls on a guest thread: running it from our own thread needs the guest-thread machinery above.

## K.1 memory (`mem/`, 2026-10-24)
`gh2k::mem::Memory` reserves 0x120000000 bytes (guest 0..4 GB, physical memory 4 GB above), creates the 512 MB physical memory as a memfd and maps it four times: the physical view and the guest windows
0xA0000000 (64 KB pages), 0xC0000000 (16 MB pages), 0xE0000000 (4 KB pages, ends at 0xFFCFFFFF): the same bytes, as the GPU needs. Virtual heaps: 0x00000000 (4 KB pages; first 64 KB kept), 0x40000000-0x7EFFFFFF (64 KB),
XEX 0x80000000 (64 KB) and 0x90000000 (4 KB). Heaps are page-granular interval allocators (first fit bottom-up; the physical one top-down from 0x1FD00000) with commit / decommit (reads back zero) / protect / release / query.
`AllocStack(size)` returns a stack between two inaccessible 64 KB guard regions, filled with 0xBE. Layout is FACT from the runtime's memory manager; the physical policy is FACT from the game's allocation trace.
- `gh2_mem_selftest`: aliasing, heap semantics, stack guards (a write one byte past either end SIGSEGVs, in a child process), 200 000 random alloc/free operations against a model (13.7 us each, 3 000 live blocks), and
  `--replay FILE` of `GH2_MEM_TRACE=FILE` recorded in a 115-s Quick Play song: **12 997 of 12 997 MmAllocatePhysicalMemoryEx succeed, and the native top-down policy returns exactly the runtime's address for 12 992 (99.96%)**
  (one shared top-down range for the 4 KB, 64 KB and 16 MB windows, starting just below the system's early allocations); 5 965 frees find their allocation; 104 of 104 NtAllocateVirtualMemory ranges (reserve then commit) are
  accepted; 75 of 75 NtFreeVirtualMemory; 18 963 of 18 963 MmQueryAllocationSize agree. Recording: `GH2_MEM_TRACE=<file>` (hooks_memory.cpp).
- What the game does (song): 13 k physical allocations (about 110/s), 6 k frees, 19 k size queries, 104 virtual reserves; stacks and the early system buffers are the runtime's own, outside the trace.
- Not done: the runtime still owns the arena (the generated code's `base` comes from it), so nothing in the game uses `gh2k::mem` yet. The step that does is the loader/arena switch (K.4); until then thread
  creation can use this library only after the arena is shared (hypothesis: construct `Memory` over the runtime's reservation, or let the runtime adopt ours).

## K.4 arena swap (2026-10-24, first half)
SDK patch `sdk-patch-series/0010`: `rex::memory::SetExternalArena({base, memfd})` makes `Memory::Initialize` lay its heaps over a host-made reservation and never unmap/close it. `gh2k::mem::Arena` reproduces the
runtime's views exactly (one memfd of 0x120001000 bytes; virtual views on their own offsets, XEX views on 0x80000000, physical views on 0x100000000, 0xE0000000 one page further). The app reserves it in OnPreSetup.
- Proof it is in use: `/proc/<pid>/maps` shows 147 mappings of `gh2-guest-memory` and none of the runtime's `xenia_memory`.
- Quick Play song (the only scenario used from now on): 60.2-60.3 fps, 5 907-6 023 frames, no SIGSEGV, no hang; golden `autostart` PASS (3 runs vs the oracle, `--expect-diff RtlNtStatusToDosError`);
  the memory trace recorded ON the native arena replays against `gh2k::mem` with 12 952 of 12 952 physical allocations at the runtime's exact address, 75/75 frees, 18 874/18 874 size queries.
- Correction found on the way: the 0xE0000000 window is physical + 0x1000; with that coordinate the native policy matches the runtime's addresses 100% (it was 99.96% before).
- Not done: the runtime's heaps (BaseHeap) still own the bookkeeping and the system allocations (PCR, stacks, TLS of its threads). Handing those to `gh2k::mem::Heap` is the second half; it is a precondition
  for native guest threads, because both allocators must not manage the same ranges.

## K.4 second half: allocation authority (2026-10-24)
SDK patch 0011 adds `rex::memory::HeapAuthority`. Every placement in the runtime goes through `BaseHeap::AllocRange` (the physical windows call their parent heap's), so the five heaps that own address space
(0x00000000 4 KB, 0x40000000 64 KB, XEX 0x80000000 and 0x90000000, and the 512 MB physical parent) now ask the host: `AllocRange(low, high, size, alignment, top_down)`, `AllocFixed` for new reservations, `Release`.
`app/src/native_arena.cpp` answers with passive `gh2k::mem::Heap`s (`Memory::Over(arena)`: registry only, no mprotect/zeroing: the runtime still maps, protects, commits and watches pages in its page table, which now
only records the host's decisions and errors out if the host picked a page the table has reserved). The physical windows 0xA0/0xC0/0xE0000000 keep pinning their parent's allocations. `--heaps=sdk` brings the old behaviour back.
- Quick Play song, 60 s: 11 664 allocations decided natively (5 fixed), 4 524 releases, **0 refused, 0 divergence errors from the runtime**; live at the end: 81 + 18 + 3 virtual blocks, 7 043 physical (160 MB).
- Placement: the same count of physical allocations as with the runtime's heaps (11 426 vs 11 426) and the same 104 virtual ones; position-by-position equality is 55-59% after allocation #2600 in BOTH comparisons (native vs runtime, and runtime vs
  runtime run to run) because the loader threads race from there: the order of allocations is not deterministic, only the policy is (deterministic replays are 100%: `gh2_mem_selftest --replay`).
- fps 60.0-60.3 over 3 runs, 2 641-2 652 frames, golden `autostart` (now 60 s; oracle re-recorded at 60 s, old one kept as `oracle_old50`) PASS.
- Not done: the runtime's own page tables and write-watch machinery remain (needed while its GPU/XMA still use them); threads' PCR/KTHREAD/TLS are still allocated by the runtime, through the authority.

## Thread state and the first native guest threads (2026-10-24)
`gh2k::thread::Create` builds, from gh2k::mem, what translated code sees through r13: a 0x2D8-byte KPCR (TLS pointer, IRQL, own address, stack bounds, KPRCB -> current KTHREAD), a 0xAB0-byte KTHREAD (type 6, stack and TLS pointers,
self-linked wait block / APC lists / timer list, ids, MSR mask, process type), the TLS block (static data + slots) and a stack with an inaccessible guard region at both ends. Stack from the title-thread range 0x70000000..; the rest from the system
part of the 4 KB heap, one page granule each, as the runtime's SystemHeapAlloc does.
- `gh2_thread_selftest`: layout, per-thread TLS slots and IRQL, guards fault on both sides, two native threads contending on a critical section owned through their KTHREADs (no lost update), destroy unmaps the stack.
- **Fidelity against a live runtime-made thread** (the game's main render thread, compared word by word at frame 120): PCR 174 of 182 words identical + 5 equal as relative pointers; KTHREAD 657 of 684 identical + 19 relative-equal. Every remaining
  difference is explained: PCR +38/+3C (the runtime's host-context stash), KTHREAD +8/+C (the runtime's object signature and handle), +110/+114 (link in the process thread list), +160 (the game's last error), +BC (suspend count: the live one was
  resumed), and the absolute stack addresses (+74 in the PCR, +60/+64 in the KTHREAD: ours is elsewhere).
- **Running**: with `GH2_NATIVE_THREADS=N` the first VdSwap after 120 frames starts N host threads, each with its own PPCContext (r13 = its PCR, r1 in its stack). Every loop it runs the game's own recompiled memcpy (the original body of sub_826A85E8) on buffers in its stack and
  verifies the copy, takes a critical section and a spin lock shared by all native threads (raising the IRQL), and round-trips a TLS slot.
  Quick Play song, 60 s: **4 threads: 265 951 iterations, 2.18 GB copied, 0 errors; 12 threads: 797 744 iterations, 6.5 GB, 0 errors, the counters kept in guest memory by the shared critical section and spin lock equal the iteration counts exactly (no lost update)**;
  fps 60.0-60.1, 2 639-2 646 frames, no SIGSEGV, no deadlock.
- What blocked replacing the runtime's scheduler (FACT, from its sources; resolved, see "The game's threads on native threads" below): imports that wait, signal, sleep or ask who the current thread is (KeWaitFor*, NtWaitForSingleObjectEx, NtSetEvent, KeDelayExecutionThread, NtCreateEvent...) find the thread through the runtime's `XThread::GetCurrentThread()` (a
  thread_local filled only for threads it created) and its handle table; a native thread calling one of them would not be known. So the next step is NOT more threads but the kernel objects: events, semaphores and waits implemented natively (dispatcher objects in guest memory + a host
  wait queue), including the objects the runtime's audio, XMA and notification code signal for the game, and then ExCreateThread itself.

## Events, semaphores and waits (K.2 second part, 2026-10-24; `--objects=native|sdk`, default `native` since the user's approval the same day)
`sync/objects.*` (plain C++, own selftest, also under ThreadSanitizer) implements the dispatcher objects on the guest's own memory: a KEVENT / KSEMAPHORE header (type, signal state) IS the object; one mutex + condition variable
answer set / reset / release and `Wait` (any / all, poll, timeout in 100 ns units, infinite). `rt/hooks/hooks_objects.cpp` routes NtCreateEvent/NtCreateSemaphore (objects carved from 4 KB system pages + a handle in 0x7D000000..),
NtSetEvent, NtClearEvent, NtReleaseSemaphore, NtWaitForSingleObjectEx, KeSetEvent, KeResetEvent, KeReleaseSemaphore, KeInitializeSemaphore, KeWaitForSingleObject, KeWaitForMultipleObjects, ObReferenceObjectByHandle,
ObDereferenceObject and NtClose of our handles to it. Anything else (thread objects and thread handles, the notification listener's handle) is forwarded to the runtime.
- **Who signals (FACT, read from the runtime's sources and from the census of a 60 s song)**: the runtime's audio (`audio_system`, `sdl_audio_driver`), XMA (`xma_decoder`), GPU (`command_processor`) and I/O (`xfile`) signal
  objects private to the host; none of them touches a dispatcher header in guest memory. The two exceptions are handled: (1) the completion event of an asynchronous file read that the runtime still performs (saves, content) -
  the read is issued without the event and `WatchIoCompletion` signals ours when the io status block leaves STATUS_PENDING (0 such reads in a song); the native VFS signals ours directly; (2) the notification listener's wait
  handle belongs to the runtime, so waits on it are forwarded (4 per 50 s). A `KeWaitForMultipleObjects` that mixes native and runtime objects would never be woken: it is forwarded, counted and warned about (0 in a song).
- **Test, Quick Play song 60 s, `GH2_OBJECTS=native`**: in 50 s 29 612 sets, 5 512 resets, 9 184 semaphore releases, 37 097 waits (36 240 of them slept, 135 timeouts, 9 182 multi-object), 18 handles created, 4 waits forwarded,
  0 mixed, 0 bridged reads. 4 runs of the harness: 60.0-60.3 fps, 2 642-2 653 frames, no SIGSEGV, no deadlock, no underrun, saves untouched. The same binary with `GH2_OBJECTS=sdk`: 60.0 fps, 2 640-2 642 frames.
  `trace_diff` against the oracle (3 runs, `--expect-diff RtlNtStatusToDosError`): PASS (`golden/results/trace_diff_oracle_vs_f30_objects_native.txt`); `golden/results/objects/` has the counters and the census.
- **The 120-130 ms frame in the harness summary (`max=`) is a menu-transition hitch, not the objects**: it shows in `objects=sdk` as well (3 of 3 runs), and the frozen oracle has the same kind (133 ms at T+12 s, 77 ms at
  T+6 s). Timeline of one run: slow window T+4.5..8.6 s (main menu -> song list, hitch ~127 ms at T+8.2 s), a second hitch at T+15.7 s (probably song chosen -> venue load; HYPOTHESIS, the log does not name the step).
  The harness only counts windows after T+15 s, so a transition that lands after that mark inflates `max=`. After T+20 s: 20 windows, 2 413 frames, mean 16.67 ms, max 24-27 ms, 0 frames > 33 ms.
- Done afterwards in the same step: `ExCreateThread` and the rest of the thread life cycle (section "The game's threads on native threads" below).

## The game's threads on native threads (K.2 final, 2026-10-24)
`hooks_kthreads.cpp` replaces the runtime's scheduler for the game. `ExCreateThread` builds PCR, KTHREAD, TLS block and stack with `gh2k::thread` (stack size, TLS slot count and static data come from the XEX
header through `XexGetModuleHandle` + `RtlImageXexHeaderField`: default stack 0x40000, 64 slots, no static data), makes a handle in the same table as the events (0x7D000000..) and starts a host thread
(pthread, 16 MB host stack like the runtime's) that runs the guest function exactly as `XThread::Execute` does: through the XDK trampoline `XapiThreadStartup` (0x823BB290) when given, else directly. The KTHREAD
(header type 6) is a dispatcher object that becomes signalled when the thread ends, so waits on thread handles are answered by `gh2k::sync` like any event.
- **Semantics taken from the runtime's sources (FACT)**: creation flags bit 0 = suspended (the suspend count lives in KTHREAD+0xBC), bit 1 = system process (here the creator's process), 0x80 = hand out the
  KTHREAD instead of a handle, bits 24..31 = processor mask (-> `current_cpu` in the PCR and KTHREAD); NtSuspendThread on one's own handle blocks until resumed, NtResumeThread / KeResumeThread return the previous
  count; KeSetAffinityThread returns the previous mask through the pointer; KeSetBasePriorityThread returns the previous host priority class; KeDelayExecutionThread sleeps whole milliseconds (relative times);
  KeEnter/LeaveCriticalRegion count in KTHREAD+0xB0; KeGetCurrentProcessType reads the PCR / KTHREAD; ExTerminateThread ends the calling thread (`pthread_exit`, with a cleanup handler so the thread object is
  signalled and its stack returned however the thread ends). KeTlsAlloc / KeTlsFree work on the process bitmap (KPROCESS+0x30, a set bit = free).
- **The main thread**: the title's entry point `xstart` (0x823B51E0) is overridden: the original runs on a native thread named `gh2-main`; the runtime's main thread (created by its launcher) only waits for it, which
  is what the launcher expects. With this, **all 12 guest threads of a song (main, audio pump, 10 of the game) are native**; what stays the runtime's are its own host workers (GPU command processor, GPU vsync, XMA
  decoder, kernel dispatch, the SDK's idle audio worker), none of which the game can wait for.
- **Identity bridge (the runtime still exists)**: kernel code of the runtime asks "which XThread am I" and "which PPCContext am I" and has no answer on a thread it did not create. Two ABI calls (docs/abi_recomp_runtime.md,
  item 8) fix that at the backend: `NoteRuntimeThread()` remembers the runtime's main thread; `AdoptCurrentHostThread(id, stack, pcr)` binds a native thread to it (the XThread identity is borrowed, the stack, TLS,
  KTHREAD and PCR stay ours) and hands back the runtime's `ThreadState` context, which the native thread then runs guest code with. FOUND BY TESTING: before that, the first native `KeEnterCriticalRegion` (SIGSEGV)
  and `KeTlsAlloc` (main thread hung at start-up, black screen) failed; a static scan (`work/analysis/imports_need_xthread.py`) lists the imports whose runtime implementation reaches the current thread (all now native or
  reachable through the bridge).
- **Deliberately not done** (0 uses in a 60 s song, measured): APC delivery (0 of 874 reads pass an APC routine), alertable waits that can be interrupted, host affinity / priority (the runtime pins each guest thread to a
  core; here the OS schedules; no measurable difference), the process thread list. Suspending a thread other than the caller is cooperative (it stops at its next wait or delay): the game only suspends itself (3 per song).
- **Shutdown**: closing the window runs the SDK's `OnClosing` (TerminateTitle, then `std::_Exit(0)`): the native threads die with the process (`GH2_TEST_QUIT=<s>` exercises exactly that path: exit code 0, 0.0 s, 3/3).
  The shared state of the sync layers is never destroyed, and `QuiesceNativeThreads()` (called by `OnShutdown`) makes native threads park at their next wait.
- **Evidence (Quick Play, 60 s, everything native)**: 60.0-60.2 fps, 2 641-2 650 frames, 0 underruns, no SIGSEGV, saves untouched; `trace_diff` against the oracle PASS (97 imports, 0 outside the band); 12 native threads
  created, 4 finished, 3 self-suspends, 14 resumes, 6 delays; 0 waits forwarded to the runtime; sync/thread selftests plain and under TSAN PASS; `check_isolation` PASS. `results/f32_all_native.txt`.
- **The ~125 ms frame in the harness `max=`** is a menu-transition hitch (main menu -> song list at ~T+8 s, song chosen -> venue at ~T+15.7 s; HYPOTHESIS for the second), present in the frozen oracle and with the
  runtime's threads; the harness counts windows after T+15 s, so a transition landing after that mark inflates it. From T+20 s: mean 16.67 ms, max 24-27 ms, 0 frames > 33 ms.

## The game's audio client (2026-10-24)
`hooks_audio.cpp` answers `XAudioRegisterRenderDriverClient`, `XAudioSubmitRenderDriverFrame`, `XAudioUnregisterRenderDriverClient` and the two volume queries instead of the runtime's `AudioSystem`: the same
`AudioSink` (SDL3 stream, lock-free 256-sample x 6-channel ring) with the same queue (`audio_maxqframes`, 4/8/16 by `audio_latency`) and the same credits; a native thread (`gh2-audio`) waits for a credit (the sink returns
one for every real frame the device takes), then calls the game's callback with the address of a 4-byte cell holding its argument, exactly as the runtime's worker does (frame padded by 176 bytes, LR 0xBCBCBCBC).
Measured over 50 s: 9 377 frames submitted = 187.5/s (48000/256), 0 underruns, 0 dropped, submit gap 5.336 ms (256/48000 = 5.333). Captured from the sound server (PipeWire monitor of the real output): same level
by 5 s window as the runtime's pump within 0.5 dB (-20.1/-20.6/-36.3/-20.2/-18.8/-17.7/-15.9/-15.7/-15.2 dBFS native; -20.2/-23.5/-37.1/-20.4/-19.3/-17.5/-16.0/-15.7/-15.1 with `GH2_AUDIO_PUMP=sdk`), never silent.
NOT verified by me: how it SOUNDS (nobody listened), a second client (a second registration goes to the runtime), XMA (still the runtime's decoder).

## XamInputGetState / XamInputGetCapabilities (2026-10-24)
The imports are implemented in `hooks_input.cpp` (flags and user pinning as the runtime does: a non-gamepad class is "not connected", user 0xFF / the any-user flag mean user 0; the big-endian X_INPUT_STATE and
X_INPUT_CAPABILITIES are written by the hook). The pad comes from `HostServices::PadGetState/PadGetCapabilities`, implemented by the application on top of the SDK's InputSystem for now: it owns the
policy that needs the application (device assignment with guitars first, dialogs owning the controller, consumed buttons, hot-plug notification, the F2 menu). Moving that policy out is a separate step; the reader
itself (SDL3 thread, 1 kHz, lock-free snapshots) has been native since F2.5. `--xam_input=sdk` brings the runtime's entries back.
Measured with the uinput injector (`native/tools/input_ab.py ... song`, `GH2_INAB_DELAY=24`, `GH2_INPUT_IGNORE_FOCUS=1`): 118 presses injected, 118 seen by the guest, 0 missed, press-to-guest latency p50 9.1-9.3 ms
(p99 17-18 ms), identical to the runtime's entries (p50 8.9-9.2 ms). An earlier run with the tool's old fixed delay (52 s, from when a song started at ~50 s) and with the window losing focus while the machine was in use
showed lost presses and multi-second "latencies": that was the test's schedule and the focus gating, not the entries (found by repeating with the schedule fitted to the song window).
