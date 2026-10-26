// File hooks: NtCreateFile / NtReadFile / NtQueryInformationFile / NtClose, the four kernel calls through which the title reads its files (d:\\gen\\main.hdr,
// main_0.ark, videos). They follow (and optionally hide) the intro video, and they are where the native VFS (kernel/vfs, Phase 2.7) plugs in:
//   --vfs=sdk     the runtime serves the files (default);
//   --vfs=shadow  the runtime serves them and every open/read/query is repeated on the native VFS and compared byte by byte;
//   --vfs=native  the native VFS serves every file under d:/game:/\\Device\\Cdrom0 that exists and is only read; saves, content, directories and everything else stay with the runtime.
// No SDK dependency.
#include "gh2_pch.h"  // REX_LOAD_* (generated), PPCContext, REX_EXTERN

#include <gh2rt/boundary_trace.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "hooks_internal.h"
#include "vfs_bridge.h"

using gh2rt::hooks::SinceStart;
using gh2rt::hooks::SkipIntroEnabled;
using namespace gh2rt::hooks::detail;
namespace vb = gh2rt::hooks::vfsbridge;

namespace {

// OBJECT_ATTRIBUTES { root_dir, name_ptr -> ANSI_STRING { u16 len, u16 max, u32 buffer }, attrs }
std::string GuestPath(uint8_t* base, uint32_t oa) {
  std::string path;
  const uint32_t name_ptr = REX_LOAD_U32(oa + 4);
  if (!name_ptr) return path;
  const uint32_t len = REX_LOAD_U16(name_ptr);
  const uint32_t buf = REX_LOAD_U32(name_ptr + 4);
  for (uint32_t i = 0; i < len && i < 260; ++i) path.push_back(static_cast<char>(REX_LOAD_U8(buf + i)));
  return path;
}

void StoreIosb(uint8_t* base, uint32_t iosb, uint32_t status, uint32_t information) {
  if (!iosb) return;
  REX_STORE_U32(iosb, status);
  REX_STORE_U32(iosb + 4, information);
}

void Store64(uint8_t* base, uint32_t address, uint64_t v) {
  REX_STORE_U32(address, static_cast<uint32_t>(v >> 32));
  REX_STORE_U32(address + 4, static_cast<uint32_t>(v));
}
uint64_t Load64(uint8_t* base, uint32_t address) { return (static_cast<uint64_t>(REX_LOAD_U32(address)) << 32) | REX_LOAD_U32(address + 4); }

// Follows the intro video's handle (the skip logic closes the intro when the game closes it).
void NoteIntroOpen(uint8_t* base, uint32_t oa, uint32_t handle_ptr, uint32_t status) {
  if (!(SkipIntroEnabled() || g_fast_forward.load() || g_video_open_s.load() < 0 || getenv("GH2_TIMELINE"))) return;
  const std::string p = GuestPath(base, oa);
  if (!p.empty() && status == 0 && p.find("intro.wmv") != std::string::npos) {
    g_intro_video_handle = REX_LOAD_U32(handle_ptr);
    if (g_video_open_s.load() < 0) g_video_open_s = SinceStart();
    fprintf(stderr, "T+%.1fs open intro.wmv handle=%x\n", SinceStart(), g_intro_video_handle.load());
  }
}

constexpr uint32_t kWriteAccess = 0x40000000u | 0x2u | 0x4u | 0x100u | 0x10000u;  // GENERIC_WRITE, FILE_WRITE_DATA, FILE_APPEND_DATA, FILE_WRITE_ATTRIBUTES, DELETE
constexpr uint32_t kFileOpen = 1;                                                  // CreateDisposition
constexpr uint32_t kFileOpened = 1;                                                // IO_STATUS_BLOCK.Information of a successful open
constexpr uint32_t kInfoStandard = 5, kInfoInternal = 6, kInfoPosition = 14, kInfoNetworkOpen = 34;

}  // namespace

// Intro video skipping (GH2_SKIP_INTRO_VIDEO=1): make d:\videos\intro.wmv "not found" so the game moves on without playing it.
// Experimental: the game's reaction to a missing video is what this tests.
REX_EXTERN(__real___imp__NtCreateFile);

REX_EXTERN(__wrap___imp__NtCreateFile) {
  GH2_TRACE_HOOK("NtCreateFile");
  static const bool hide_video = [] {
    const char* e = getenv("GH2_SKIP_INTRO_VIDEO");
    return e && e[0] != '0';
  }();
  if (hide_video || g_fast_forward.load()) {
    // r5 = OBJECT_ATTRIBUTES { root_dir, name_ptr -> ANSI_STRING { u16 len, u16 max, u32 buffer }, attrs }
    const uint32_t oa = ctx.r5.u32;
    const uint32_t name_ptr = REX_LOAD_U32(oa + 4);
    if (name_ptr) {
      const uint32_t len = REX_LOAD_U16(name_ptr);
      const uint32_t buf = REX_LOAD_U32(name_ptr + 4);
      char path[160] = {};
      for (uint32_t i = 0; i < len && i < sizeof(path) - 1; ++i) path[i] = static_cast<char>(REX_LOAD_U8(buf + i));
      if (strstr(path, "intro.wmv")) {
        FastForwardEnd("intro video reached");  // the video must play (or be skipped) at normal speed
        if (hide_video) {
          fprintf(stderr, "T+%.1fs SKIP: hiding %s\n", SinceStart(), path);
          ctx.r3.u64 = 0xC0000034u;  // STATUS_OBJECT_NAME_NOT_FOUND
          return;
        }
      }
    }
  }
  const uint32_t handle_ptr = ctx.r3.u32;
  const uint32_t access = ctx.r4.u32;
  const uint32_t oa2 = ctx.r5.u32;
  const uint32_t iosb = ctx.r6.u32;
  const uint32_t disposition = ctx.r10.u32;
  const vb::Mode mode = vb::GetMode();
  gh2k::Vfs* vfs = vb::Get();
  std::string vpath;
  bool claimed = false;
  if (vfs) {
    vb::Stats().creates++;
    vpath = GuestPath(base, oa2);
    claimed = vfs->Claims(vpath);
    if (claimed) vb::Stats().creates_claimed++;
    else vb::NoteUnclaimed(vpath.c_str());
  }
  if (mode == vb::Mode::kNative && claimed && (access & kWriteAccess) == 0 && disposition == kFileOpen) {
    gh2k::NtStatus status = 0;
    const auto host = vfs->Resolve(vpath, &status);
    std::error_code ec;
    if (!host || !std::filesystem::is_directory(*host, ec)) {   // directories stay with the runtime
      uint32_t handle = 0;
      if (host) status = vfs->Open(vpath, &handle);
      if (status == 0) REX_STORE_U32(handle_ptr, handle);
      StoreIosb(base, iosb, status, status == 0 ? kFileOpened : 0);
      NoteIntroOpen(base, oa2, handle_ptr, status);   // the intro-skip logic follows the video's handle whoever opened it
      vb::Stats().creates_native++;
      ctx.r3.u64 = status;
      return;
    }
  }
  if (vfs) vb::Stats().creates_forwarded++;
  __real___imp__NtCreateFile(ctx, base);
  if (mode == vb::Mode::kShadow && claimed && ctx.r3.u32 == 0) {
    uint32_t native = 0;
    if (vfs->Open(vpath, &native) == 0) vb::ShadowMap(REX_LOAD_U32(handle_ptr), native);
  }
  NoteIntroOpen(base, oa2, handle_ptr, ctx.r3.u32);
}

// The video is done (played to the end or skipped) when the game closes the intro.wmv handle.
REX_EXTERN(__real___imp__NtClose);

REX_EXTERN(__wrap___imp__NtClose) {
  GH2_TRACE_HOOK("NtClose");
  const uint32_t h = ctx.r3.u32;
  if (h != 0 && h == g_intro_video_handle.load()) {
    g_intro_video_handle = 0;
    fprintf(stderr, "T+%.1fs close intro.wmv\n", SinceStart());
    if (SkipIntroEnabled() && !g_intro_done.exchange(true)) {
      g_menu_reached_s = SinceStart();
      fprintf(stderr, "T+%.1fs INTRO done: intro video closed\n", SinceStart());
    }
  }
  if (NativeObjectClose(h)) {   // a handle of the native events / semaphores
    ctx.r3.u64 = 0;
    return;
  }
  if (gh2k::Vfs* vfs = vb::Get()) {
    vb::Stats().closes++;
    if (vfs->Owns(h)) {   // a handle of the native VFS: the runtime does not know it
      ctx.r3.u64 = vfs->Close(h);
      return;
    }
    if (const uint32_t native = vb::ShadowTake(h)) vfs->Close(native);
  }
  __real___imp__NtClose(ctx, base);
}

// ---- NtReadFile(handle, event, apc_routine, apc_context, io_status_block, buffer, length, byte_offset*) ----
REX_EXTERN(__real___imp__NtReadFile);
REX_EXTERN(__real___imp__NtSetEvent);
REX_EXTERN(__real___imp__NtQueryInformationFile);

REX_EXTERN(__wrap___imp__NtReadFile) {
  GH2_TRACE_HOOK("NtReadFile");
  gh2k::Vfs* vfs = vb::Get();
  if (!vfs) {
    __real___imp__NtReadFile(ctx, base);
    return;
  }
  vb::Counters& c = vb::Stats();
  const uint32_t handle = ctx.r3.u32, event = ctx.r4.u32, apc = ctx.r5.u32, iosb = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, offset_ptr = ctx.r10.u32;
  uint64_t offset = gh2k::kCurrentPosition;
  if (offset_ptr) {
    const uint64_t v = Load64(base, offset_ptr);
    if (v < 0xFFFFFFFFFFFFFFFEull) offset = v;   // -1 / -2 = FILE_USE_FILE_POINTER_POSITION
  }
  ++c.reads;
  if (event) ++c.reads_with_event;
  if (apc) ++c.reads_with_apc;
  if (offset == gh2k::kCurrentPosition) ++c.reads_own_position;
  if (vfs->Owns(handle)) {
    uint32_t got = 0;
    const gh2k::NtStatus status = vfs->Read(handle, offset, REX_RAW_ADDR(buffer), length, &got);
    StoreIosb(base, iosb, status, got);
    ++c.reads_native;
    c.bytes_native += got;
    if (event && !NativeEventSetByHandle(base, event)) {   // an event of the runtime's: signal it through the runtime
      ctx.r3.u64 = event;
      ctx.r4.u64 = 0;
      __real___imp__NtSetEvent(ctx, base);
    }
    ctx.r3.u64 = status;
    return;
  }
  const uint32_t native = vb::GetMode() == vb::Mode::kShadow ? vb::ShadowLookup(handle) : 0;
  // the runtime reads this file (a save, content ...) and would signal an event handle it does not know: read without the event and signal ours on completion
  const bool bridge = event && NativeEventHandle(event);
  if (bridge) ctx.r4.u64 = 0;
  __real___imp__NtReadFile(ctx, base);
  if (bridge) {
    if (ctx.r3.u32 == 0x103) WatchIoCompletion(base, iosb, event);
    else NativeEventSetByHandle(base, event);
  }
  if (!native) {
    if (vb::GetMode() == vb::Mode::kShadow) ++c.reads_unmapped;
    return;
  }
  // shadow: read the same range natively now; compare with the guest buffer when the runtime has filled it
  const uint32_t status = ctx.r3.u32;
  std::vector<uint8_t> mine(length);
  uint32_t got = 0;
  const gh2k::NtStatus mine_status = vfs->Read(native, offset, mine.data(), length, &got);
  mine.resize(got);
  uint32_t final_status = status;
  if (status == 0x103 && iosb) {   // STATUS_PENDING: the runtime completes the read on another thread, normally within microseconds; wait for it here so that the
    ++c.reads_pending;             // comparison sees the buffer before the game can touch it (it decrypts main.hdr in place the moment the read completes)
    const volatile uint32_t* block = reinterpret_cast<const volatile uint32_t*>(REX_RAW_ADDR(iosb));
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    while (__builtin_bswap32(*block) == 0x103 && std::chrono::steady_clock::now() < until) std::this_thread::yield();
    final_status = __builtin_bswap32(*block);
  }
  if (final_status == 0x103 && iosb) {
    vb::PendingRead p;
    p.iosb = reinterpret_cast<const volatile uint32_t*>(REX_RAW_ADDR(iosb));
    p.buffer = REX_RAW_ADDR(buffer);
    p.expected = std::move(mine);
    p.expected_status = mine_status;
    p.offset = offset;
    p.issued = std::chrono::steady_clock::now();
    vb::AddPending(std::move(p));
    return;
  }
  ++c.reads_compared;
  const uint32_t info = iosb ? REX_LOAD_U32(iosb + 4) : 0;
  if ((final_status == 0) != (mine_status == 0) || (final_status == 0 && info != got)) {
    ++c.read_status_mismatches;
    if (c.read_status_mismatches < 10) fprintf(stderr, "VFS read status: runtime %08X/%u bytes, native %08X/%u bytes (offset %llx length %u)\n", final_status, info, mine_status, got, (unsigned long long)offset, length);
    return;
  }
  if (final_status == 0) {
    c.bytes_compared += got;
    if (std::memcmp(REX_RAW_ADDR(buffer), mine.data(), got) != 0) {
      ++c.read_mismatches;
      if (c.read_mismatches < 10) fprintf(stderr, "VFS read BYTES DIFFER (offset %llx length %u)\n", (unsigned long long)offset, got);
    }
  }
}

// ---- NtQueryInformationFile(handle, io_status_block, info*, length, class) ----
REX_EXTERN(__wrap___imp__NtQueryInformationFile) {
  GH2_TRACE_HOOK("NtQueryInformationFile");
  gh2k::Vfs* vfs = vb::Get();
  if (!vfs) {
    __real___imp__NtQueryInformationFile(ctx, base);
    return;
  }
  vb::Counters& c = vb::Stats();
  const uint32_t handle = ctx.r3.u32, iosb = ctx.r4.u32, info = ctx.r5.u32, cls = ctx.r7.u32;
  ++c.queries;
  ++c.query_class[cls & 63];
  if (vfs->Owns(handle)) {
    gh2k::FileInfo fi;
    gh2k::NtStatus status = vfs->Info(handle, &fi);
    uint32_t written = 0;
    if (status == 0) {
      switch (cls) {
        case kInfoStandard:   // AllocationSize, EndOfFile, NumberOfLinks, DeletePending, Directory
          Store64(base, info, fi.size);
          Store64(base, info + 8, fi.size);
          REX_STORE_U32(info + 16, 1);
          REX_STORE_U8(info + 20, 0);
          REX_STORE_U8(info + 21, fi.directory ? 1 : 0);
          written = 24;
          break;
        case kInfoInternal:
          Store64(base, info, handle);
          written = 8;
          break;
        case kInfoPosition:
          Store64(base, info, fi.position);
          written = 8;
          break;
        case kInfoNetworkOpen:   // 4 times, AllocationSize, EndOfFile, FileAttributes
          for (uint32_t i = 0; i < 4; ++i) Store64(base, info + 8 * i, 0);
          Store64(base, info + 32, fi.size);
          Store64(base, info + 40, fi.size);
          REX_STORE_U32(info + 48, fi.directory ? 0x10u : 0x80u);
          REX_STORE_U32(info + 52, 0);
          written = 56;
          break;
        default:
          status = 0xC0000003;   // STATUS_INVALID_INFO_CLASS
          break;
      }
    }
    StoreIosb(base, iosb, status, written);
    ++c.queries_native;
    ctx.r3.u64 = status;
    return;
  }
  const uint32_t native = vb::GetMode() == vb::Mode::kShadow ? vb::ShadowLookup(handle) : 0;
  __real___imp__NtQueryInformationFile(ctx, base);
  if (!native) {
    if (vb::GetMode() == vb::Mode::kShadow) ++c.queries_unmapped;
    return;
  }
  if (ctx.r3.u32 != 0 || (cls != kInfoStandard && cls != kInfoNetworkOpen)) return;
  gh2k::FileInfo fi;
  if (vfs->Info(native, &fi) != 0) return;
  const uint64_t runtime_end = Load64(base, info + (cls == kInfoStandard ? 8 : 40));
  if (runtime_end != fi.size) {
    ++c.query_mismatches;
    if (c.query_mismatches < 10) fprintf(stderr, "VFS query class %u: runtime size %llu, native %llu\n", cls, (unsigned long long)runtime_end, (unsigned long long)fi.size);
  }
}
