// Input hooks (moved from app/src/gh2_host.cpp): XamInputGetState / XamInputGetCapabilities. Wrapped at link time (--wrap, see
// native/CMakeLists.txt). No SDK dependency: players, guitars and the keyboard come from the host services.
//
// Xplorer fix: the game must see a guitar (XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE, sub-type 7) and the unused axes must be idle.
// GH2_INPUT_TRACE=1 logs pad changes. GH2_INPUT_LOG=FILE records every change of the buttons a user sees, with the steady-clock
// nanoseconds of the poll that first saw it (the clock gh2_input_selftest uses), to measure press-to-guest latency.
#include "gh2_pch.h"  // REX_LOAD_* / REX_STORE_* (generated), PPCContext, REX_EXTERN

#include <gh2rt/boundary_trace.h>
#include <gh2rt/host_services.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "hooks_internal.h"

using gh2rt::Host;
using gh2rt::hooks::InputTrace;
using gh2rt::hooks::SinceStart;

namespace {

// GH2_INPUT_LOG: one line per change of a user's buttons: "<steady ns of the poll> <user> <packet> <buttons hex>". Costs one
// static check when unset; when set, one fprintf per change (changes are rare next to polls).
struct GuestInputLog {
  FILE* file = nullptr;
  uint16_t last_buttons[4] = {0, 0, 0, 0};
  bool seen[4] = {false, false, false, false};
  GuestInputLog() {
    if (const char* path = getenv("GH2_INPUT_LOG")) file = fopen(path, "w");
    if (file) fprintf(file, "# steady_clock ns of the XamInputGetState call, user, packet number, buttons (hex)\n");
  }
  void Note(uint64_t t_ns, uint32_t user, uint32_t packet, uint16_t buttons) {
    if (!file || user >= 4) return;
    if (seen[user] && buttons == last_buttons[user]) return;
    seen[user] = true;
    last_buttons[user] = buttons;
    fprintf(file, "%llu %u %u %04x\n", static_cast<unsigned long long>(t_ns), user, packet, buttons);
    fflush(file);
  }
};

GuestInputLog& InputLog() {
  static GuestInputLog log;
  return log;
}

uint64_t SteadyNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// --xam_input=native: the imports themselves are ours (the runtime's entries are not called). What they do, read from the runtime's sources: a query for a device class other than the
// gamepad is "not connected"; user 0xFF or the "any user" flag pins the user to 0; the pad then comes from the host (its device assignment and dialogs included).
constexpr uint32_t kFlagGamepad = 0x01, kFlagAnyUser = 1u << 30, kBadArguments = 0xA0;

bool NativeEntries() {
  static const bool on = [] {
    std::string mode = Host().GetSettings().xam_input;
    if (const char* e = getenv("GH2_XAM_INPUT"); e && *e) mode = e;
    if (mode != "native") return false;
    if (!Host().PadSourceAvailable()) return false;   // no pad source behind the host (a bare process): the runtime answers
    fprintf(stderr, "INPUT native entries: XamInputGetState / XamInputGetCapabilities are answered by the hooks, the pad comes from the host\n");
    return true;
  }();
  return on;
}

uint32_t ActualUser(uint32_t user, uint32_t flags) { return ((user & 0xFF) == 0xFF || (flags & kFlagAnyUser)) ? 0 : user; }

}  // namespace

REX_EXTERN(__real___imp__XamInputGetState);
REX_EXTERN(__real___imp__XamInputGetCapabilities);

// XamInputGetState(user, flags, state*) -> status. X_INPUT_STATE: be32 packet, be16 buttons, u8 LT, u8 RT, be16 LX, LY, RX, RY.
static void NativeGetState(PPCContext& ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32, flags = ctx.r4.u32, sp = ctx.r5.u32;
  if ((flags & 0xFF) && !(flags & kFlagGamepad)) { ctx.r3.u64 = gh2rt::kPadNotConnected; return; }
  gh2rt::PadState pad;
  const uint32_t result = Host().PadGetState(ActualUser(user, flags), sp ? &pad : nullptr);
  if (result == 0 && sp) {
    REX_STORE_U32(sp, pad.packet);
    REX_STORE_U16(sp + 4, pad.buttons);
    REX_STORE_U8(sp + 6, pad.left_trigger);
    REX_STORE_U8(sp + 7, pad.right_trigger);
    REX_STORE_U16(sp + 8, static_cast<uint16_t>(pad.lx));
    REX_STORE_U16(sp + 10, static_cast<uint16_t>(pad.ly));
    REX_STORE_U16(sp + 12, static_cast<uint16_t>(pad.rx));
    REX_STORE_U16(sp + 14, static_cast<uint16_t>(pad.ry));
  }
  ctx.r3.u64 = result;
}

// XamInputGetCapabilities(user, flags, caps*) -> status. X_INPUT_CAPABILITIES (20 bytes): u8 type, u8 sub-type, be16 flags, the gamepad masks (12 bytes), be16 left / right motor.
static void NativeGetCapabilities(PPCContext& ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32, flags = ctx.r4.u32, cp = ctx.r5.u32;
  if (!cp) { ctx.r3.u64 = kBadArguments; return; }
  if ((flags & 0xFF) && !(flags & kFlagGamepad)) { ctx.r3.u64 = gh2rt::kPadNotConnected; return; }
  gh2rt::PadCaps caps;
  const uint32_t result = Host().PadGetCapabilities(ActualUser(user, flags), flags, &caps);
  if (result == 0) {
    REX_STORE_U8(cp, caps.type);
    REX_STORE_U8(cp + 1, caps.sub_type);
    REX_STORE_U16(cp + 2, caps.flags);
    REX_STORE_U16(cp + 4, caps.gamepad.buttons);
    REX_STORE_U8(cp + 6, caps.gamepad.left_trigger);
    REX_STORE_U8(cp + 7, caps.gamepad.right_trigger);
    REX_STORE_U16(cp + 8, static_cast<uint16_t>(caps.gamepad.lx));
    REX_STORE_U16(cp + 10, static_cast<uint16_t>(caps.gamepad.ly));
    REX_STORE_U16(cp + 12, static_cast<uint16_t>(caps.gamepad.rx));
    REX_STORE_U16(cp + 14, static_cast<uint16_t>(caps.gamepad.ry));
    REX_STORE_U16(cp + 16, caps.left_motor);
    REX_STORE_U16(cp + 18, caps.right_motor);
  }
  ctx.r3.u64 = result;
}

REX_EXTERN(__wrap___imp__XamInputGetState) {
  GH2_TRACE_HOOK("XamInputGetState");
  static const bool timeline = getenv("GH2_TIMELINE") != nullptr;
  if (timeline) {
    static std::atomic<bool> first{true};
    if (first.exchange(false)) fprintf(stderr, "T+%.1fs first XamInputGetState\n", SinceStart());
  }
  uint32_t sp = ctx.r5.u32;
  const uint32_t user = ctx.r3.u32;
  static const bool log_enabled = getenv("GH2_INPUT_LOG") != nullptr;
  const uint64_t call_ns = log_enabled ? SteadyNs() : 0;
  if (NativeEntries()) NativeGetState(ctx, base);
  else __real___imp__XamInputGetState(ctx, base);
  if (!sp) return;   // a query without a buffer: nothing to merge into
  // Keyboard play: the keyboard is a guitar-style device for one player. It is merged into that player's state, or becomes the
  // player's whole state when no device is connected there.
  const gh2rt::KeyboardState kb = Host().PollKeyboard(user);
  if (kb.active) {
    if (ctx.r3.u32 != 0) {
      for (uint32_t i = 0; i < 16; ++i) REX_STORE_U8(sp + i, 0);
      ctx.r3.u64 = 0;
    }
    REX_STORE_U32(sp, REX_LOAD_U32(sp) + kb.changes);
    REX_STORE_U16(sp + 4, REX_LOAD_U16(sp + 4) | kb.buttons);
    if (kb.whammy) REX_STORE_U16(sp + 12, 0x7FFF);  // right stick X = whammy axis on guitars
  }
  if (ctx.r3.u32 != 0) return;
  // A guitar's unused axes read as pressed triggers (the X-plorer reports LT/RT garbage), so they are zeroed for guitars only; a
  // gamepad's triggers are real buttons (green / blue in the game's gamepad mode).
  if (Host().UserIsGuitar(user)) {
    REX_STORE_U8(sp + 6, 0);
    REX_STORE_U8(sp + 7, 0);
  }
  if (log_enabled) InputLog().Note(call_ns, user, REX_LOAD_U32(sp), REX_LOAD_U16(sp + 4));
  if (InputTrace()) {
    static std::atomic<uint32_t> calls{0};
    static auto t0 = std::chrono::steady_clock::now();
    const uint32_t c = ++calls;
    if (c == 1 || c % 200 == 0)
      fprintf(stderr, "GETSTATE call #%u at %.1fs\n", c,
              std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  }
  if (gh2rt::hooks::detail::IntroSkipActive()) {
    // 4 polls pressed, 4 released: the game sees clean button-down edges.
    const uint32_t n = gh2rt::hooks::detail::g_intro_poll_count.fetch_add(1, std::memory_order_relaxed);
    static const uint16_t mask = [] {
      const char* e = getenv("GH2_SKIP_INTRO_BUTTONS");  // XINPUT wButtons bits; default A
      return e ? static_cast<uint16_t>(strtoul(e, nullptr, 0)) : static_cast<uint16_t>(0x1000);
    }();
    // Games usually re-read the pad only when dwPacketNumber changes, so bump it on every injected edge.
    static std::atomic<uint32_t> bias{0};
    static std::atomic<int> last_pressed{-1};
    static const uint32_t period = [] {  // polls per pressed/released phase; 0 = hold pressed
      const char* e = getenv("GH2_SKIP_INTRO_PERIOD");
      return e ? static_cast<uint32_t>(strtoul(e, nullptr, 10)) : 4u;
    }();
    const int pressed = period == 0 ? 1 : (((n / period) & 1) == 0);
    if (last_pressed.exchange(pressed) != pressed) ++bias;
    REX_STORE_U32(sp, REX_LOAD_U32(sp) + bias.load());
    if (pressed) REX_STORE_U16(sp + 4, REX_LOAD_U16(sp + 4) | mask);
  }
  if (!InputTrace()) return;
  static uint32_t last_btn = 0xFFFFFFFF;
  static uint64_t last_axes = ~0ull;
  uint32_t btn = REX_LOAD_U16(sp + 4);
  uint64_t axes = ((uint64_t)REX_LOAD_U16(sp + 8) << 32) | ((uint64_t)REX_LOAD_U16(sp + 10) << 16) | REX_LOAD_U16(sp + 12);
  axes = (axes << 16) ^ REX_LOAD_U16(sp + 14);
  if (btn != last_btn || axes != last_axes) {
    last_btn = btn;
    last_axes = axes;
    fprintf(stderr, "PAD btn=%04x LX=%d LY=%d RX=%d RY=%d\n", btn, (int16_t)REX_LOAD_U16(sp + 8), (int16_t)REX_LOAD_U16(sp + 10),
            (int16_t)REX_LOAD_U16(sp + 12), (int16_t)REX_LOAD_U16(sp + 14));
  }
}

REX_EXTERN(__wrap___imp__XamInputGetCapabilities) {
  GH2_TRACE_HOOK("XamInputGetCapabilities");
  gh2rt::hooks::detail::Heartbeat();
  uint32_t cp = ctx.r5.u32;
  const uint32_t cap_user = ctx.r3.u32;
  if (NativeEntries()) NativeGetCapabilities(ctx, base);
  else __real___imp__XamInputGetCapabilities(ctx, base);
  const int kb_slot = Host().KeyboardSlot();
  if (kb_slot >= 0 && static_cast<uint32_t>(kb_slot) == cap_user && !Host().UserHasRealDevice(cap_user)) {
    // No real device at the keyboard's slot (the host may still offer a "None" stand-in, which is a plain gamepad and would put
    // the game in gamepad mode, with other fret buttons): report a guitar (type 1 = gamepad class, sub-type 7) with every
    // control present.
    for (uint32_t i = 0; i < 20; ++i) REX_STORE_U8(cp + i, 0);
    REX_STORE_U8(cp, 1);
    REX_STORE_U8(cp + 1, 7);
    REX_STORE_U16(cp + 4, 0xF3FF);
    REX_STORE_U8(cp + 6, 0xFF);
    REX_STORE_U8(cp + 7, 0xFF);
    for (uint32_t i = 8; i < 16; ++i) REX_STORE_U8(cp + i, 0xFF);
    ctx.r3.u64 = 0;
  }
  // The game's pad routine only treats a device as a guitar when SubType == 7 (GUITAR_ALTERNATE). The host reports real guitars
  // as 6 (or 7 / 11 for bass); present all of them as 7. Gamepads (subtype 1) stay gamepads.
  if (ctx.r3.u32 == 0 && REX_LOAD_U8(cp) == 1) {
    const uint8_t st = REX_LOAD_U8(cp + 1);
    if (st == 0x06 || st == 0x07 || st == 0x0B) REX_STORE_U8(cp + 1, 7);
  }
  if (InputTrace()) {  // what the game is told about each player's device (capabilities struct, 20 bytes)
    static std::atomic<int> printed[4];
    if (cap_user < 4 && printed[cap_user].fetch_add(1) < 2) {
      char hex[64] = {};
      for (uint32_t i = 0; i < 20; ++i) snprintf(hex + i * 3, 4, "%02x ", REX_LOAD_U8(cp + i));
      fprintf(stderr, "CAPS user=%u result=%u %s\n", cap_user, ctx.r3.u32, hex);
    }
  }
}
