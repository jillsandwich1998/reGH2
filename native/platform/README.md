# native/platform - SDL3 input and audio (Phase 2, F2.3)

Two small libraries that replace what the SDK's SDL drivers do, written against SDL3's public API only (the SDK's own SDL3 build, whose
symbols `librexruntime.so` exports; a second SDL3 in the process would be a bug). Neither includes a `rex/` header (`tools/check_isolation.py`
rule S1). Both are the DEFAULT now (2026-10-24, after the user's physical X-plorer test): `--input_reader=native`, `--audio_output=native`; `sdk`
brings the SDK's driver back for either. Nothing here is connected to a renderer.

## input_sdl3
```
guitar_classifier   pure C++: SDL type -> USB id (the instruments SDL's own tables list) -> name words; XINPUT sub-type (guitar = 6, the hook of
                    XamInputGetCapabilities shows the game 7)
snapshot_slot.h     seqlock over relaxed atomics: one writer, any number of lock-free readers; no torn read (stress test + TSAN)
input_core          ONE thread: SDL_UpdateJoysticks() ~1 kHz, hot-plug by SDL event watch + 2 s re-scan, a slot per device (XInput conversions
                    identical to the SDK driver's: same button table, Y axes ~v, triggers >> 7), rumble requests applied on the thread
uinput_guitar       a virtual Xbox 360 guitar through /dev/uinput with the real X-plorer's ids and idle values (tests)
selftest            gh2_input_selftest unit | list [s] | e2e [n] [hz] | inject HZ SECS LOG [DELAY] [PRODUCT]
sdl_probe           gh2_sdl_probe [--watch s]: raw SDL view of every device (type, ids, GUID, mapping, idle axes)
```
In the game (`app/src/native_input.cpp`): `--input_reader=native` (default) swaps the SDK's SDL *driver* for `gh2plat::InputCore` inside the SDK's `InputSystem`,
which keeps the policy (player assignment, merge, dead zones, UI blockers, the app's keyboard merge).

Verified here (2026-10-24, this machine, SDK's SDL 3.4.14):
- The real X-plorer arrives as `SDL_JOYSTICK_TYPE_GAMEPAD`, `XBOX360`, `/dev/input/eventN` (kernel `xpad`), with an automatic mapping
  (`a:b0,b:b1,x:b2,y:b3,...,dpup:h0.1,...,rightx:a3,righty:a4`). SDL does not call it a guitar; the classifier does, by USB id 1430:4748.
  The HIDAPI drivers (GIP = Xbox One guitars, PS3/PS4/PS5) tag GUITAR themselves; none of them handles the X-plorer.
- Press -> snapshot through the kernel (uinput -> evdev -> SDL -> thread), 600 events at 1 kHz: p50 0.44 ms, p99 1.0 ms, max 1.02 ms;
  the thread uses 0.9% of one core, longest tick 59 us, 0 overruns.
- In the real game the press -> guest latency is the same for `sdk` and `native` (mean 8.9 ms, p99 18-25 ms): the floor is the game's own
  60 polls a second. The dedicated thread removes the dependence on the UI thread; it does not make the game see presses sooner.
- At rest the native reader reports the real axes (RX -32768, RY 32767, LY -1); the SDK driver reports 0 until an axis moves (it is event
  driven). After the first movement both report the same. This is the one visible difference to check with the physical guitar.
- NOT tested: Xbox One / GIP and PS3-protocol instruments on real hardware (the raw-layout fallback is exercised only with an SDL virtual
  joystick of type GUITAR); Windows.

## audio_sdl3
```
audio_format        one guest frame = 6 planes x 256 big-endian float32 (6144 bytes, 5.33 ms), channel order FL FR FC LFE BL BR -> interleaved host floats
frame_ring.h        SPSC ring of frames: no lock, no allocation, cached indices (13.5 M frames stress, 0 bad, TSAN clean)
audio_sink          SubmitGuestFrame (guest thread, converts in place) -> ring -> Pull (SDL device callback or software clock); WaitForRoom paces the
                    producer with a futex (the role of the SDK's per-client semaphore); underrun = silence + counter; full = drop + counter
xma_port.h          interface only: the seam for a native XMA unit (MMIO 0x7FEA0000 + XMACreateContext/ReleaseContext)
audio_selftest      gh2_audio_selftest | device [s] (feeds SILENCE to the default device)
```
The SDK's SDL3 has no `dummy` or `disk` audio driver (pipewire, pulseaudio, alsa, sndio, jack only), so the tests drive the consumer step into a
device-less `SDL_AudioStream` and read it back (bit exact, gain, mute, starvation) and use a muted real stream for the device-clock check.
In the game (`app/src/native_audio.cpp`): `--audio_output=native` (default) makes the SDK's `AudioSystem` create OUR driver per client. The SDK still
registers the client and runs the worker that calls the game's render callback (that needs guest threads: the kernel step of the roadmap); the frame the
game submits goes into the sink's ring, the SDL device callback plays it, and every real frame played releases the SDK's per-client semaphore. On a
stereo/mono endpoint the sink does the same stereo fold the SDK did (SDL's own mix is never used); the user's UMC204HD is a 6-channel endpoint, so 6 channels
go through untouched. `GH2_AUDIO_TRACE=1` prints the counters every 10 s; they are also in the problem report.

Verified (2026-10-24): `native/tools/audio_parity.cpp` compares the conversion with the SDK's own functions on 20 000 random frames (6-channel path with
gains 1 / 0.5 / 0.8 and the clamp, stereo fold): 0 bits differ. `native/tools/audio_capture.py` records the game's own PipeWire stream in both modes:
intro/logos FL -15.77 vs -15.80 dBFS, FR -14.98 vs -14.98, per-second envelope correlation 1.0000; Quick Play song FL -20.01 vs -20.00, FR -20.55 vs -20.53,
correlation 1.0000; same stream format (float32 6ch 48 kHz), same channel map, node name `rexglue`, `node.latency = 256/48000`. In a 60 s song: 11 246 frames
submitted / 11 242 played, 1 underrun (the start), 0 dropped, queue 4, longest device callback 45 us. NOT done: a high-priority mix thread, measuring three
whole songs, a native XMA unit, replacing the SDK's client registry / worker.

### What the traces say about XMA (golden Quick Play run, 48 s)
- The contract has two XMA imports (`XMACreateContext` 25 calls, `XMAReleaseContext` 23); everything else is MMIO at 0x7FEA0000: reads of
  +0x1800 (ContextArrayAddress, once) and +0x1818 (CurrentContextIndex, polled), writes to +0x1940.. (Kick), +0x1A40.. (Lock), +0x1A80.. (Clear).
- The ARK holds 794 `.mogg` files (1.07 GB, Ogg Vorbis multitrack songs) and no `.xma`; the song audio is therefore NOT what the 25 XMA
  contexts decode. Which assets do is not verified (hypothesis: sound effects / short streams).
- XMA decodes to 16-bit PCM in guest memory; the title's own mixer mixes it and submits the final 6 x 256 float frames through
  `XAudioSubmitRenderDriverFrame` (9011 calls in 48 s = 187.6/s), which is what `AudioSink` plays. So XMA is never routed to the SDL stream
  itself: a native XMA unit has to write PCM back into the guest's output buffers (`xma_port.h`).
