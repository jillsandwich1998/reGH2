#include "fetch_scan.h"

#include <algorithm>

namespace gh2gfx::d3d {

namespace {

// ControlFlowOpcode (shader_code.h): the ones that carry an exec header (address / count / sequence)
bool IsExec(uint32_t opcode) {
  switch (opcode) {
    case 1:   // Exec
    case 2:   // ExecEnd
    case 3:   // CondExec
    case 4:   // CondExecEnd
    case 5:   // CondExecPred
    case 6:   // CondExecPredEnd
    case 13:  // CondExecPredClean
    case 14:  // CondExecPredCleanEnd
      return true;
    default:
      return false;
  }
}

}  // namespace

unsigned FetchUsage::VertexSlotCount() const {
  unsigned n = 0;
  for (uint32_t w : vertex_slots) n += static_cast<unsigned>(__builtin_popcount(w));
  return n;
}

FetchUsage ScanFetches(const uint32_t* words, size_t count) {
  FetchUsage out;
  if (!words || count < 3) return out;
  // The control flow comes first: pairs of 48-bit instructions packed in three dwords. It ends where the first exec instruction's code starts.
  size_t limit = count * 4;  // bytes
  struct Exec {
    uint32_t address, count, sequence;
  };
  std::vector<Exec> execs;
  for (size_t pos = 0; pos + 12 <= limit && pos / 4 + 3 <= count; pos += 12) {
    const uint32_t* cf = words + pos / 4;
    const uint32_t code[4] = {cf[0], cf[1] & 0xFFFFu, (cf[1] >> 16) | (cf[2] << 16), cf[2] >> 16};
    for (int i = 0; i < 2; ++i) {
      const uint32_t lo = code[2 * i], hi = code[2 * i + 1];
      const uint32_t opcode = (hi >> 12) & 0xF;
      ++out.cf_instructions;
      if (!IsExec(opcode)) continue;
      const Exec e{lo & 0xFFFu, (lo >> 12) & 7u, (lo >> 16) & 0xFFFu};
      if (e.address != 0) limit = std::min<size_t>(limit, static_cast<size_t>(e.address) * 12);
      execs.push_back(e);
    }
  }
  // Executed in program order, which is the order of the exec instructions in the control flow.
  int last_full_slot = -1;
  for (const Exec& e : execs) {
    uint32_t sequence = e.sequence;
    for (uint32_t i = 0; i < e.count; ++i, sequence >>= 2) {
      const size_t index = static_cast<size_t>(e.address) + i;
      if (index * 3 + 3 > count) break;
      const uint32_t* f = words + index * 3;
      if ((sequence & 1u) == 0) {  // an ALU instruction: does it export to memory? (export bit 15, destination register 32..60)
        if ((f[0] >> 15) & 1u) {
          const uint32_t vector_dest = f[0] & 0x3F, scalar_dest = (f[0] >> 8) & 0x3F, vmask = (f[0] >> 16) & 0xF, smask = (f[0] >> 20) & 0xF;
          if ((vmask && vector_dest >= 32 && vector_dest <= 60) || (smask && scalar_dest >= 32 && scalar_dest <= 60)) ++out.memexport_instructions;
        }
        continue;
      }
      const uint32_t opcode = f[0] & 0x1F;
      ++out.fetch_instructions;
      if (opcode == 0) {  // vertex fetch
        VertexFetch v;
        v.instruction = static_cast<uint16_t>(index);
        v.mini = ((f[1] >> 30) & 1u) != 0;
        if (v.mini) {
          if (last_full_slot < 0) continue;
          v.slot = static_cast<uint8_t>(last_full_slot);
        } else {
          const uint32_t constant = (f[0] >> 20) & 0x1F, select = (f[0] >> 25) & 3u;
          v.slot = static_cast<uint8_t>(constant * 3 + select);
          last_full_slot = v.slot;
        }
        v.dst = static_cast<uint8_t>((f[0] >> 12) & 0x3F);
        v.dst_swizzle = static_cast<uint16_t>(f[1] & 0xFFF);
        v.format = static_cast<uint8_t>((f[1] >> 16) & 0x3F);
        v.is_signed = ((f[1] >> 12) & 1u) != 0;
        v.integer = ((f[1] >> 13) & 1u) != 0;
        v.stride = static_cast<uint8_t>(f[2] & 0xFF);
        const uint32_t off = (f[2] >> 8) & 0x7FFFFFu;
        v.offset = static_cast<int32_t>((off ^ 0x400000u) - 0x400000u);  // sign extend 23 bits
        if (v.slot < 96) out.vertex_slots[v.slot / 32] |= 1u << (v.slot % 32);
        out.vertex_fetches.push_back(v);
      } else if (opcode == 1 || opcode == 19) {  // texture fetch / get texture weights: the two XenosRecomp translates
        const uint32_t slot = (f[0] >> 20) & 0x1F;
        out.texture_slots |= 1u << slot;
        if (opcode == 1 && out.texture_fetch_count[slot] < 255) ++out.texture_fetch_count[slot];
      }
    }
  }
  out.valid = true;
  return out;
}

}  // namespace gh2gfx::d3d
