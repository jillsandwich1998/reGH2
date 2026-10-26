// fetch_scan.h - which fetch constants a Xenos shader microcode reads (Phase 2.6).
//
// A draw needs, besides the two shaders, one vertex buffer for every vertex fetch constant the vertex shader reads and one texture for every
// fetch constant a texture fetch reads. Both are encoded in the microcode: control flow "exec" instructions point at runs of instructions and a
// 2-bit field per instruction says whether it is an ALU or a fetch instruction; a fetch instruction names its fetch constant. The walk below is the one
// XenosRecomp does when it translates a shader (native/third_party/XenosRecomp/XenosRecomp/shader_recompiler.cpp, shader_code.h), so what the
// translated module samples and what is reported here are the same instructions.
//
// For vertex shaders scan the microcode AFTER the D3D library patched it to the current vertex declaration (it rewrites the fetch constant, offset,
// stride and format of every vertex fetch; README.md "vertex fetch patching"): that is the microcode the GPU runs and the one that names the streams.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gh2gfx::d3d {

struct VertexFetch {
  uint16_t instruction = 0;   // index of the fetch instruction in the microcode (3 dwords each); the shader container's vertex element table is keyed by it
  uint8_t slot = 0;           // vertex fetch constant 0..95 (the library binds stream n to constant 95 - n)
  uint8_t stride = 0;         // in dwords, as patched
  int32_t offset = 0;         // in dwords, as patched
  uint8_t format = 0;         // GPU vertex format (xenos::VertexFormat: 6 = 8_8_8_8, 25 = 16_16, 37 = 32_32_FLOAT, 38 = 32_32_32_32_FLOAT, 57 = 32_32_32_FLOAT)
  bool is_signed = false;     // dword 1 bit 12 (format_comp_all): the components are signed
  bool integer = false;       // dword 1 bit 13 (num_format_all): integer rather than normalized fraction
  bool mini = false;          // a mini fetch reads the constant of the full fetch before it
  uint8_t dst = 0;            // destination register: the library reorders the fetches when it patches the shader, the register stays with the element
  uint16_t dst_swizzle = 0;   // the 12-bit destination swizzle of the instruction
};

struct FetchUsage {
  bool valid = false;
  uint32_t texture_slots = 0;     // bit n = some texture fetch reads fetch constant n (0..31)
  uint8_t texture_fetch_count[32] = {};   // how many texture fetch instructions read fetch constant n: 2 or more = a multi-tap shader (blur, bloom)
  uint32_t vertex_slots[3] = {};  // 96 bits: bit j = some vertex fetch reads vertex fetch constant j
  std::vector<VertexFetch> vertex_fetches;
  uint32_t cf_instructions = 0, fetch_instructions = 0;
  uint32_t memexport_instructions = 0;   // ALU instructions that export to the memory export registers (32..60): the shader writes vertices / data to memory (Xenos "memexport")

  bool UsesVertexSlot(unsigned j) const { return j < 96 && ((vertex_slots[j / 32] >> (j % 32)) & 1u) != 0; }
  unsigned VertexSlotCount() const;
};

// `words` are the microcode's dwords in host order (the guest stores them big endian).
FetchUsage ScanFetches(const uint32_t* words, size_t count);

}  // namespace gh2gfx::d3d
