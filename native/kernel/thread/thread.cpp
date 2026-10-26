#include "gh2k/thread.h"

#include <cstring>

namespace gh2k::thread {

namespace {

struct Guest {
  uint8_t* base;
  void U8(uint32_t a, uint8_t v) { base[a] = v; }
  void U16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); std::memcpy(base + a, &v, 2); }
  void U32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(base + a, &v, 4); }
  void U64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); std::memcpy(base + a, &v, 8); }
  void SelfList(uint32_t head) { U32(head, head); U32(head + 4, head); }   // X_LIST_ENTRY: flink = blink = itself (empty)
};

// SystemHeapAlloc: the runtime takes these from the top 256 MB of the 4 KB heap
bool SystemAlloc(mem::Memory& m, uint32_t size, uint32_t* address) {
  if (!m.v00000000().AllocRange(0x30000000u, 0x3FFFFFFFu, size, 4096, false, address)) return false;
  return m.v00000000().Commit(*address, size, mem::kReadWrite);   // a no-op on the runtime's arena, whose pages are already read/write
}

}  // namespace

bool Create(mem::Memory& m, const Params& p, Block* b, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    Destroy(m, *b);
    *b = Block{};
    return false;
  };
  *b = Block{};
  if (!m.AllocStack(p.stack_size, &b->stack)) return fail("no memory for the stack");
  b->tls_size = p.tls_static_size + p.tls_slots * 4;
  if (!SystemAlloc(m, b->tls_size, &b->tls_static)) return fail("no memory for the TLS block");
  b->tls_dynamic = b->tls_static + p.tls_static_size;
  if (!SystemAlloc(m, pcr::kSize, &b->pcr)) return fail("no memory for the PCR");
  if (!SystemAlloc(m, kthread::kSize, &b->kthread)) return fail("no memory for the KTHREAD");

  Guest g{m.base()};
  // fresh pages of the arena read as zero; the blocks are cleared anyway (an earlier thread may have used them)
  std::memset(m.base() + b->tls_static, 0, b->tls_size);
  std::memset(m.base() + b->pcr, 0, pcr::kSize);
  std::memset(m.base() + b->kthread, 0, kthread::kSize);
  if (p.tls_template && p.tls_static_size)
    std::memcpy(m.base() + b->tls_static, p.tls_template, p.tls_template_size && p.tls_template_size < p.tls_static_size ? p.tls_template_size : p.tls_static_size);

  // ---- KTHREAD ----
  const uint32_t t = b->kthread;
  g.U8(t + kthread::kHeaderType, 6);
  g.U8(t + kthread::kSuspendCount, (p.creation_flags & 1) ? 1 : 0);
  g.U32(t + 0x10, t + 0x10);                      // self-referencing pointers of the wait timeout timer
  g.U32(t + 0x14, t + 0x10);
  g.U32(t + 0x40, t + 0x20);                      // wait_timeout_block: list entry, thread, object, result, type
  g.U32(t + 0x44, t + 0x20);
  g.U32(t + 0x48, t);
  g.U32(t + 0x4C, t + 0x18);
  g.U16(t + 0x54, 0x0100);
  g.U16(t + 0x56, 0x0201);
  g.U32(t + kthread::kStackBase, b->stack.top);
  g.U32(t + kthread::kStackLimit, b->stack.limit);
  g.U32(t + kthread::kStackKernel, b->stack.top - 240);
  g.U32(t + kthread::kTlsDynamic, b->tls_dynamic);
  g.U8(t + kthread::kThreadState, 0);
  g.SelfList(t + kthread::kApcLists);             // kernel and user mode APC lists
  g.SelfList(t + kthread::kApcLists + 8);
  g.U32(t + kthread::kProcess, p.process);
  g.U8(t + kthread::kMayQueueApcs, 1);
  g.U8(t + kthread::kProcessType, p.process_type);
  g.U8(t + kthread::kProcessTypeDup, p.process_type);
  g.U32(t + kthread::kMsrMask, 0xFDFFD7FFu);
  g.U8(t + kthread::kCurrentCpu, static_cast<uint8_t>(p.cpu));
  g.U32(t + kthread::kAPrcb, b->pcr + pcr::kPrcb);
  g.U32(t + kthread::kAnotherPrcb, b->pcr + pcr::kPrcb);
  g.U32(t + kthread::kStackAllocBase, b->stack.top);
  g.U64(t + kthread::kCreateTime, p.create_time);
  g.SelfList(t + kthread::kTimerList);
  g.U32(t + kthread::kThreadId, p.thread_id);
  g.U32(t + kthread::kStartAddress, p.start_address);
  g.SelfList(t + kthread::kList154);
  g.U32(t + kthread::kLastError, 0);
  g.U32(t + kthread::kCreationFlags, p.creation_flags);
  g.U32(t + kthread::kUnk17C, 1);

  // ---- KPCR ----
  const uint32_t c = b->pcr;
  g.U32(c + pcr::kTlsPtr, b->tls_static);
  g.U64(c + pcr::kPcrPtr, c);
  g.U32(c + pcr::kStackBase, b->stack.top);
  g.U32(c + pcr::kStackEnd, b->stack.limit);
  g.U32(c + pcr::kPrcb + pcr::kCurrentThread, t);
  g.U8(c + pcr::kPrcb + pcr::kCurrentCpu, static_cast<uint8_t>(p.cpu));
  g.U32(c + pcr::kPrcbPointer, c + pcr::kPrcb);
  g.U32(c + pcr::kPrcb + pcr::kDpcActive, 0);
  g.U8(c + pcr::kCurrentIrql, 0);
  return true;
}

void DestroyStack(mem::Memory& m, Block* b) {
  if (b->stack.alloc_base) m.FreeStack(b->stack);
  b->stack = mem::Memory::Stack{};
}

void Destroy(mem::Memory& m, const Block& b) {
  if (b.stack.alloc_base) m.FreeStack(b.stack);
  for (uint32_t a : {b.tls_static, b.pcr, b.kthread})
    if (a) m.v00000000().Release(a);
}

}  // namespace gh2k::thread
