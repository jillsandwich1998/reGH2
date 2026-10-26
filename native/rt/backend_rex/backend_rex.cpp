// Backend of the gh2abi contract on top of the ReXGlue SDK: the interim implementation that lets the recompiled game code and the
// hooks (both compiled against native/rt/abi only) run on the SDK runtime. This is the ONLY translation unit of the native module
// that sees SDK headers; when our own runtime replaces the SDK, this file is replaced by it (see docs/abi_recomp_runtime.md).
//
// It must NOT see the shim headers (native/rt/abi/shim/rex/*), which alias SDK names onto gh2abi: the include path of this target
// has the contract directory only.
#include <gh2abi/abi.h>
#include <gh2rt/boundary_trace.h>

#include <atomic>

#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>  // rex::runtime::ResolveIndirectFunction
#include <rex/system/mmio_handler.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/thread/mutex.h>

// the runtime's "current XThread" of a host thread (exported by librexruntime)
namespace rex::system {
extern thread_local XThread* current_xthread_tls_;
}

namespace gh2abi {

namespace {
std::mutex g_borrow_mutex;
rex::system::object_ref<rex::system::XThread> g_borrowed;
}  // namespace

void NoteRuntimeThread() {
  std::lock_guard<std::mutex> lock(g_borrow_mutex);
  if (!g_borrowed && rex::system::XThread::IsInThread()) g_borrowed = rex::system::retain_object(rex::system::XThread::GetCurrentThread());
}

namespace {
thread_local std::unique_ptr<rex::runtime::ThreadState> tl_thread_state;
}

PPCContext* AdoptCurrentHostThread(uint32_t thread_id, uint32_t stack_top, uint32_t pcr) {
  if (!rex::system::XThread::IsInThread()) {
    std::lock_guard<std::mutex> lock(g_borrow_mutex);
    if (g_borrowed) rex::system::current_xthread_tls_ = g_borrowed.get();
  }
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !kernel->memory()) return nullptr;
  tl_thread_state = std::make_unique<rex::runtime::ThreadState>(thread_id, stack_top, pcr, kernel->memory());
  rex::runtime::ThreadState::Bind(tl_thread_state.get());
  return tl_thread_state->context();
}

void ReleaseCurrentHostThread() {
  rex::runtime::ThreadState::Bind(nullptr);
  tl_thread_state.reset();
  rex::system::current_xthread_tls_ = nullptr;
}

PPCFunc* ResolveIndirectFunction(uint32_t guest_address) {
  PPCFunc* func = rex::runtime::ResolveIndirectFunction(guest_address);
  if (gh2rt::trace::Enabled()) {
    static const uint16_t id = gh2rt::trace::IdOf(gh2rt::trace::kPseudoResolveIndirect);
    gh2rt::trace::RecordPseudo(id, guest_address, 0, func ? 1u : 0u);
  }
  return func;
}

uint64_t Clock::QueryGuestTickCount() { return rex::chrono::Clock::QueryGuestTickCount(); }

std::recursive_mutex& global_critical_region::mutex() { return rex::thread::global_critical_region::mutex(); }

MMIOHandler* MMIOHandler::global_handler() {
  static MMIOHandler handler;  // stateless: every call goes to the SDK's handler
  return &handler;
}

namespace {
NativeMmio g_native_mmio;
std::atomic<bool> g_native_mmio_set{false};
}  // namespace

void SetNativeMmio(const NativeMmio& handler) {
  g_native_mmio = handler;
  g_native_mmio_set.store(true, std::memory_order_release);
}

bool MMIOHandler::CheckLoad(uint32_t guest_address, uint32_t* out_value) {
  const bool handled = (g_native_mmio_set.load(std::memory_order_acquire) && g_native_mmio.load && g_native_mmio.load(g_native_mmio.user, guest_address, out_value)) ||
                       rex::runtime::MMIOHandler::global_handler()->CheckLoad(guest_address, out_value);
  if (gh2rt::trace::Enabled()) {
    static const uint16_t id = gh2rt::trace::IdOf(gh2rt::trace::kPseudoMmioLoad);
    gh2rt::trace::RecordPseudo(id, guest_address, handled ? *out_value : 0u, handled ? 1u : 0u);
  }
  return handled;
}

bool MMIOHandler::CheckStore(uint32_t guest_address, uint32_t value) {
  const bool handled = (g_native_mmio_set.load(std::memory_order_acquire) && g_native_mmio.store && g_native_mmio.store(g_native_mmio.user, guest_address, value)) ||
                       rex::runtime::MMIOHandler::global_handler()->CheckStore(guest_address, value);
  if (gh2rt::trace::Enabled()) {
    static const uint16_t id = gh2rt::trace::IdOf(gh2rt::trace::kPseudoMmioStore);
    gh2rt::trace::RecordPseudo(id, guest_address, value, handled ? 1u : 0u);
  }
  return handled;
}

void LogMessage(LogLevel level, std::string_view message) {
  switch (level) {
    case LogLevel::kDebug: REXCPU_DEBUG("{}", message); break;
    case LogLevel::kInfo: REXCPU_INFO("{}", message); break;
    case LogLevel::kWarn: REXCPU_WARN("{}", message); break;
    case LogLevel::kError: REXCPU_ERROR("{}", message); break;
  }
}

}  // namespace gh2abi
