// Shared by the hook translation units only. No SDK headers, no `rex::` (native/tools/check_isolation.sh).
#pragma once

#include <gh2rt/hooks_api.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

struct PPCContext;

namespace gh2rt::hooks::detail {

// Intro-skip state (intro_skip.cpp), updated by the VdSwap / NtCreateFile / NtClose hooks.
extern std::atomic<bool> g_intro_done;
extern std::atomic<uint32_t> g_intro_video_handle;
extern std::atomic<double> g_video_open_s;
extern std::atomic<double> g_menu_reached_s;  // SinceStart() when the guest first ran at menu speed
extern std::atomic<bool> g_fast_forward;
extern std::atomic<uint32_t> g_intro_poll_count;
extern std::atomic<double> g_ff_started_s;

bool IntroSkipActive();               // press-A pulses are due now
void FastForwardEnd(const char* why);

// Native guest threads (hooks_threads.cpp, GH2_NATIVE_THREADS=N): called from the VdSwap hook with the PCR (r13) of the calling guest thread.
void NativeThreadsOnFrame(uint32_t live_pcr, uint8_t* base);

// Native events / semaphores (hooks_objects.cpp, --objects=native)
bool NativeObjectsActive();
bool NativeObjectClose(uint32_t handle);                    // a handle of ours: forget it (true) or not ours (false)
bool NativeEventSetByHandle(uint8_t* base, uint32_t handle);  // signal our event with this handle (false: not ours)
bool NativeEventHandle(uint32_t handle);
void WatchIoCompletion(uint8_t* base, uint32_t iosb, uint32_t event_handle);
uint32_t ObjectHandleCreate(uint32_t guest_object);   // a handle (0x7D000000..) for a native object or thread; the table is shared
uint32_t ObjectOfHandle(uint32_t handle);             // the guest object behind one of our handles, 0 if it is not ours

// The game's threads on native threads (hooks_kthreads.cpp, --threads=native)
bool NativeThreadsActive();
void ThreadHandleClosed(uint32_t kthread);   // NtClose of a handle to a native thread
void ParkIfQuiescing();                      // shutdown: a native thread that gets here stops for good
void ThreadStats(uint64_t out[6]);
void StopGpu();   // shutdown: stop the native command processor and the vertical blank clock (hooks_gpu.cpp)
// A native thread (PCR, KTHREAD, TLS and stack of its own) that runs `body(ctx, base)` instead of a guest entry point: the host side of services the game drives through callbacks (the audio pump
// calls the game's render-driver callback from one). `caller_pcr` is the creating thread's PCR (its process is inherited). The thread ends when `body` returns.
bool StartGuestHostThread(uint8_t* base, uint32_t caller_pcr, uint32_t stack_size, const char* name, uint32_t start_address, std::function<void(PPCContext&, uint8_t*)> body,
                          std::function<void()> on_finish = nullptr);   // on_finish runs once when the thread is over, however it ends
uint32_t GuestScratchAlloc(uint8_t* base);   // a zeroed 0x20-byte block of guest memory that lives for the process (0 if memory ran out)           // created, finished, self suspends, cooperative suspends, resumes, delays

// Stall tracing / profiling heartbeat (stall_watchdog.cpp); called from the per-frame hooks.
void Heartbeat();

}  // namespace gh2rt::hooks::detail
