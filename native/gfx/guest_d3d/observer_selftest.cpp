// gh2_d3d_observer_selftest - the observer against a synthetic guest memory: a fake container, fake shader objects laid out the way the D3D
// library lays them out (gh2gfx/guest_d3d.h), a fake device and a fake command buffer with IM_LOAD packets. No game, no GPU.
//
//   gh2_d3d_observer_selftest SHADER_CACHE_DIR [FX_XBOX_DIR]
//     SHADER_CACHE_DIR  the real shaders.pak (copied to a temp dir; nothing is written to the original)
//     FX_XBOX_DIR       the extracted containers (shader_pipeline.py's fx_xbox/): also runs the whole chain with REAL containers - guest memory ->
//                       creation -> bind -> Resolve() -> the SPIR-V module of the cache
// Exit status 0 = everything passed.
#include <unistd.h>

#include <cstdio>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "fetch_scan.h"
#include "graphics_state.h"
#include "observer.h"
#include "shader_cache.h"

namespace fs = std::filesystem;
using namespace gh2gfx::d3d;

namespace {

int g_failures = 0;
void Check(bool ok, const std::string& what, const std::string& detail = "") {
  std::printf("[%s] %s%s%s\n", ok ? "ok" : "!!", what.c_str(), detail.empty() ? "" : " - ", detail.c_str());
  if (!ok) ++g_failures;
}

// A flat fake "guest memory": host pointer = base + guest address.
struct Guest {
  std::vector<uint8_t> mem = std::vector<uint8_t>(0x200000, 0);
  const uint8_t* base() const { return mem.data(); }
  void Put32(uint32_t a, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(&mem[a], &v, 4);
  }
  uint32_t Get32(uint32_t a) const {
    uint32_t v;
    std::memcpy(&v, &mem[a], 4);
    return __builtin_bswap32(v);
  }
};

constexpr uint32_t kContainer = 0x10000;  // fake container in the engine's data
constexpr uint32_t kUcodeOffset = 8, kUcodeBytes = 24;  // microcode inside the container's physical part


// ---- Phase 2.6: fetch scan and graphics state against a synthetic device ----
void TestFetchScan() {
  // control flow: one pair at dwords 0..2: exec(address 1, count 2, sequence 0b0101 = two fetches); instructions follow at dword 3
  uint32_t u[9] = {};
  u[0] = 1u | (2u << 12) | (5u << 16);
  u[1] = 0x1000u;  // instruction 0 of the pair: opcode Exec in bits 12..15 of its second half
  u[3] = (31u << 20) | (2u << 25);            // vertex fetch, constant 31 select 2 = vertex fetch constant 95
  u[4] = 57u << 16;                           // format 32_32_32_FLOAT
  u[5] = 6u | (3u << 8);                      // stride 6, offset 3
  u[6] = 1u | (3u << 20);                     // texture fetch, fetch constant 3
  const FetchUsage f = ScanFetches(u, 9);
  Check(f.valid && f.UsesVertexSlot(95) && f.VertexSlotCount() == 1, "fetch scan: one vertex fetch, constant 95");
  Check(f.texture_slots == (1u << 3), "fetch scan: one texture fetch, constant 3");
  Check(f.vertex_fetches.size() == 1 && f.vertex_fetches[0].format == 57 && f.vertex_fetches[0].stride == 6 && f.vertex_fetches[0].offset == 3, "fetch scan: format, stride, offset decoded");
}

void TestGraphicsState() {
  Guest g;
  const uint32_t dev = 0x60000, vb = 0x20000, ib = 0x21000, tex = 0x22000, rt = 0x23000, decl = 0x24000, cmd = 0x70000;
  g.Put32(vb, 0x00100001);
  g.Put32(vb + 24, 0x40000003);
  g.Put32(vb + 28, 0x10000102);
  g.Put32(ib, 0x00100002);
  g.Put32(ib + 24, 0x41000000);
  g.Put32(ib + 28, 64);
  g.Put32(tex, 0x00100003);
  const uint32_t tw[6] = {0x88000002, 0x42000014, 0x003FE1FF, 0x00A80D10, 0x00000143, 0x00000000};
  for (int i = 0; i < 6; ++i) g.Put32(tex + 28 + 4 * i, tw[i]);
  g.Put32(rt, 0x00100004);
  g.Put32(decl, 0x00100005);
  // the device as the library keeps it
  g.Put32(dev + layout::kDeviceVertexDeclaration, decl);
  g.Put32(dev + layout::kDeviceIndexBuffer, ib);
  g.Put32(dev + layout::kDeviceRenderTarget, rt);
  g.Put32(dev + layout::kDeviceStreamSource, vb);
  g.mem[dev + layout::kDeviceStreamStride] = 6;
  g.Put32(dev + layout::kDeviceTexture, tex);
  g.Put32(dev + layout::kDeviceFetchConstants + 0, tw[0]);
  g.Put32(dev + layout::kDeviceFetchConstants + 4, PhysicalAddress(tw[1]));
  g.Put32(dev + layout::kDeviceFetchConstants + 8, tw[2]);
  g.Put32(dev + layout::kDeviceFetchConstants + 20, PhysicalAddress(tw[5]));
  g.Put32(dev + layout::kDeviceFetchConstants + 31 * 24 + 16, PhysicalAddress(0x40000003));  // vertex fetch constant 95: slot 31, dwords 4 and 5
  g.Put32(dev + layout::kDeviceFetchConstants + 31 * 24 + 20, 0x10000102);
  // the draw's packets: DRAW_INDX {0, triangle list | 6 indices, DMA base, 6}
  g.Put32(cmd + 4, 0xC0032201);
  g.Put32(cmd + 8, 0);
  g.Put32(cmd + 12, 4u | (6u << 16));
  g.Put32(cmd + 16, PhysicalAddress(0x41000000));
  g.Put32(cmd + 20, 6);

  GraphicsState gs(true);
  auto call = [&](Api api, std::initializer_list<uint32_t> regs, uint32_t ret = 0) {
    Call c;
    c.api = api;
    int i = 0;
    for (uint32_t r : regs) c.r[i++] = r;
    c.ret = ret;
    return c;
  };
  gs.OnCall(g.base(), call(Api::kCreateVertexBuffer, {0x100, 0}, vb));
  gs.OnCall(g.base(), call(Api::kSetRenderTarget, {dev, 0, rt}));
  gs.OnCall(g.base(), call(Api::kSetVertexDeclaration, {dev, decl}));
  gs.OnCall(g.base(), call(Api::kSetStreamSource, {dev, 0, vb, 0, 24}));
  gs.OnCall(g.base(), call(Api::kSetIndices, {dev, ib}));
  gs.OnCall(g.base(), call(Api::kSetTexture, {dev, 0, tex}));
  PipelineInfo p;
  p.valid = true;
  uint32_t vs_u[9] = {1u | (2u << 12) | (1u << 16), 0x1000u, 0, (31u << 20) | (2u << 25), 57u << 16, 6u};
  uint32_t ps_u[9] = {1u | (1u << 12) | (1u << 16), 0x1000u, 0, 1u | (0u << 20)};
  p.vertex = std::make_shared<const FetchUsage>(ScanFetches(vs_u, 9));
  p.pixel = std::make_shared<const FetchUsage>(ScanFetches(ps_u, 9));
  gs.OnPipeline(dev, p);
  Call draw = call(Api::kDrawIndexedVertices, {dev, 4, 0, 0, 6, 1});
  draw.cmd_before = cmd;
  draw.cmd_after = cmd + 20;
  gs.OnCall(g.base(), draw);
  Check(gs.Draws() == 1 && gs.CompleteDraws() == 1, "graphics: a draw with its stream, index buffer, texture and render target bound is complete");
  Check(gs.VerifyMismatches() == 0, "graphics: verify finds the tracked bindings, the fetch constants and the DRAW_INDX consistent");
  gs.OnCall(g.base(), call(Api::kSetTexture, {dev, 0, 0}));
  gs.OnCall(g.base(), draw);
  Check(gs.Draws() == 2 && gs.CompleteDraws() == 2, "graphics: SetTexture(NULL) leaves the GPU's fetch constant: the sampler still reads the previous texture, found by its base address");
  g.Put32(dev + layout::kDeviceFetchConstants, 0);  // a fetch constant that is not a texture either
  gs.OnCall(g.base(), draw);
  Check(gs.Draws() == 3 && gs.CompleteDraws() == 2, "graphics: a draw whose pixel shader reads a sampler with no texture on either side is not complete");
  const std::string line = gs.SummaryLine(1.0);
  Check(line.find("tex 1 vb 1") != std::string::npos, "graphics: the registry holds the texture and the vertex buffer", line);
}

}  // namespace

int main(int argc, char** argv) {
  TestFetchScan();
  TestGraphicsState();
  if (argc == 1) return g_failures == 0 ? 0 : 1;
  if (argc < 2) return std::fprintf(stderr, "usage: gh2_d3d_observer_selftest SHADER_CACHE_DIR [FX_XBOX_DIR]\n"), 2;
  const fs::path tmp = fs::temp_directory_path() / ("gh2_d3d_observer_selftest_" + std::to_string(::getpid()));
  fs::create_directories(tmp);
  fs::copy_file(fs::path(argv[1]) / "shaders.pak", tmp / "shaders.pak");

  Guest g;
  // Container layout used here: header (0..39), shader struct at 40: {physical offset, bytes}; the header says "shader offset 40".
  auto build = [&](uint32_t at, bool vertex) {
    g.Put32(at + container::kFlags, 0x102A1100u | (vertex ? 1u : 0u));
    g.Put32(at + container::kVirtualSize, 64);
    g.Put32(at + container::kPhysicalSize, 48);
    g.Put32(at + container::kShaderOffset, 40);
    g.Put32(at + 28, 0);
    g.Put32(at + 32, 0);
    g.Put32(at + 40, kUcodeOffset);   // physical offset of the microcode
    g.Put32(at + 44, kUcodeBytes);    // its size in bytes
    for (uint32_t i = 0; i < 48; i += 4) g.Put32(at + 64 + i, 0xA0000000u + (vertex ? 0x100u : 0x200u) + i);  // the physical part: microcode words
  };
  build(kContainer, true);          // vertex container
  build(kContainer + 0x400, false); // pixel container

  // shader objects as the library builds them: virtual part copied into the object, microcode copied to a separate allocation
  const uint32_t vs_obj = 0x20000, ps_obj = 0x30000, vs_ucode_mem = 0x40000, ps_ucode_mem = 0x50000, device = 0x60000, cmd = 0x70000;
  for (uint32_t i = 0; i < 64; i += 4) {
    g.Put32(vs_obj + layout::kVertexContainerCopy + i, g.Get32(kContainer + i));
    g.Put32(ps_obj + layout::kPixelContainerCopy + i, g.Get32(kContainer + 0x400 + i));
  }
  for (uint32_t i = 0; i < 48; i += 4) {
    g.Put32(vs_ucode_mem + i, g.Get32(kContainer + 64 + i));
    g.Put32(ps_ucode_mem + i, g.Get32(kContainer + 0x400 + 64 + i));
  }
  g.Put32(vs_obj + layout::kVertexUcodePointer, vs_ucode_mem);
  g.Put32(ps_obj + layout::kPixelUcodePointer, ps_ucode_mem);

  auto observer = MakeShaderObserver(tmp, tmp / "report.json");
  Check(observer != nullptr, "the observer opens the shader cache");
  if (!observer) return 1;
  observer->OnShaderCreated(g.base(), Stage::kVertex, kContainer, vs_obj);
  observer->OnShaderCreated(g.base(), Stage::kPixel, kContainer + 0x400, ps_obj);
  observer->OnShaderCreated(g.base(), Stage::kPixel, 0, 0);          // a failed creation is ignored
  observer->OnShaderCreated(g.base(), Stage::kPixel, 0x1000, 0x9999);  // not a container
  g.Put32(device + layout::kDeviceVertexShader, vs_obj);
  g.Put32(device + layout::kDevicePixelShader, ps_obj);

  // the library patches the vertex microcode (two words) and writes two IM_LOAD packets: pixel (type 1) then vertex (type 0)
  g.Put32(vs_ucode_mem + kUcodeOffset + 4, 0xDEADBEEF);
  g.Put32(vs_ucode_mem + kUcodeOffset + 12, 0xCAFEF00D);
  auto phys = [](uint32_t virt) { return (virt & 0x1FFFFFFEu) + (((virt >> 20) + 0x200u) & 0x1000u); };
  uint32_t p = cmd;  // "last written dword" convention: the first dword goes to p + 4
  auto emit = [&](uint32_t v) {
    p += 4;
    g.Put32(p, v);
  };
  const uint32_t before = p;
  emit(0xC0012700u);  // IM_LOAD, 2 dwords
  emit(phys(ps_ucode_mem + kUcodeOffset) | 1u);
  emit(kUcodeBytes / 4);
  emit(0xC0012700u);
  emit(phys(vs_ucode_mem + kUcodeOffset));
  emit(kUcodeBytes / 4);
  const uint32_t after = p;
  observer->OnDrawStateCommit(g.base(), device, before, after);
  // a second draw that loads nothing
  observer->OnDrawStateCommit(g.base(), device, after, after);
  // a draw with a pixel shader the creation hooks never saw
  g.Put32(device + layout::kDevicePixelShader, 0x31234);
  observer->OnDrawStateCommit(g.base(), device, after, after);


  // ---- the whole chain with real containers: one vertex and one pixel container from the extracted set, laid out like the library lays them out
  if (argc > 2) {
    std::vector<uint8_t> real[2];  // [0] vertex, [1] pixel
    for (const auto& e : fs::directory_iterator(argv[2])) {
      std::ifstream in(e.path(), std::ios::binary);
      std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      size_t size = 0;
      gh2gfx::ShaderStage stage;
      if (!gh2gfx::ShaderCache::ParseContainerHeader(bytes.data(), bytes.size(), &size, &stage)) continue;
      auto& slot = real[stage == gh2gfx::ShaderStage::kVertex ? 0 : 1];
      if (slot.empty() && size == bytes.size()) slot = bytes;
      if (!real[0].empty() && !real[1].empty()) break;
    }
    Check(!real[0].empty() && !real[1].empty(), "found a real vertex and a real pixel container in the extracted set");
    if (!real[0].empty() && !real[1].empty()) {
      Guest rg;
      auto be = [&](uint32_t at, uint32_t off) { return rg.Get32(at + off); };
      const uint32_t at[2] = {0x10000, 0x20000}, obj[2] = {0x30000, 0x40000}, ucode[2] = {0x50000, 0x60000}, dev = 0x70000;
      const uint32_t copy[2] = {layout::kVertexContainerCopy, layout::kPixelContainerCopy};
      const uint32_t ucode_field[2] = {layout::kVertexUcodePointer, layout::kPixelUcodePointer};
      for (int s = 0; s < 2; ++s) {
        std::memcpy(&rg.mem[at[s]], real[s].data(), real[s].size());
        const uint32_t vsize = be(at[s], container::kVirtualSize), psize = be(at[s], container::kPhysicalSize);
        std::memcpy(&rg.mem[obj[s] + copy[s]], &rg.mem[at[s]], vsize);                    // the virtual part, into the object
        std::memcpy(&rg.mem[ucode[s]], &rg.mem[at[s] + vsize], psize);                    // the microcode, into its own allocation
        rg.Put32(obj[s] + ucode_field[s], ucode[s]);
      }
      auto real_observer = MakeShaderObserver(tmp, tmp / "report_real.json");
      real_observer->OnShaderCreated(rg.base(), Stage::kVertex, at[0], obj[0]);
      real_observer->OnShaderCreated(rg.base(), Stage::kPixel, at[1], obj[1]);
      rg.Put32(dev + layout::kDeviceVertexShader, obj[0]);
      rg.Put32(dev + layout::kDevicePixelShader, obj[1]);
      const BoundShaders bound = real_observer->Resolve(rg.base(), dev);
      auto magic_ok = [](const BoundShader& b) { return b.registered && b.spirv && b.spirv->spirv[0] == 0x07230203u && b.spirv->size_bytes > 100; };
      Check(magic_ok(bound.vertex) && bound.vertex.spirv->stage == gh2gfx::ShaderStage::kVertex, "Resolve(): the bound vertex shader comes back as its SPIR-V module",
            std::to_string(bound.vertex.spirv ? bound.vertex.spirv->size_bytes : 0) + " bytes, hash " + std::to_string(bound.vertex.container_hash));
      Check(magic_ok(bound.pixel) && bound.pixel.spirv->stage == gh2gfx::ShaderStage::kPixel, "Resolve(): the bound pixel shader comes back as its SPIR-V module",
            std::to_string(bound.pixel.spirv ? bound.pixel.spirv->size_bytes : 0) + " bytes, hash " + std::to_string(bound.pixel.container_hash));
      rg.Put32(dev + layout::kDevicePixelShader, 0x31234);
      const BoundShaders partial = real_observer->Resolve(rg.base(), dev);
      Check(partial.vertex.spirv && !partial.pixel.registered && !partial.pixel.spirv && partial.pixel.object == 0x31234, "Resolve(): an unknown bound object has no module and is flagged");
    }
  }

  // read the report back
  observer.reset();
  std::ifstream f(tmp / "report.json");
  std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto has = [&](const char* needle) { return json.find(needle) != std::string::npos; };
  auto count = [&](const std::string& needle) {
    size_t n = 0;
    for (size_t at = json.find(needle); at != std::string::npos; at = json.find(needle, at + 1)) ++n;
    return n;
  };
  Check(has("\"commits\": 3"), "three draw-state commits seen");
  Check(has("\"bad_containers\": 1"), "the pointer that is not a container is counted, not registered");
  Check(has("\"unregistered_objects\": 1") && has("\"unregistered_draws\": 1"), "a bound object the hooks never saw is reported once");
  Check(has("\"pixel\": {\"created\": 1,") && has("\"ucode_identical\": 1, \"diff_words_buckets\": {\"0\": 1}}"), "pixel microcode identical to its container is recognised");
  Check(has("\"vertex\": {\"created\": 1,") && has("\"diff_words_buckets\": {\"1-2\": 1}}"), "vertex microcode with two patched words lands in the 1-2 bucket");
  Check(count("\"im_load_address_ok\": 1,") == 2, "the IM_LOAD address equals the one computed from the object (both stages)");
  Check(count("\"ucode_layout_ok\": 1,") == 2, "microcode location and size read from the object match the container (both stages)");
  fs::remove_all(tmp);
  std::printf("selftest: %s\n", g_failures ? "FAILED" : "PASSED");
  return g_failures ? 1 : 0;
}
