// gh2gfx::d3d shader observer: correlates what the game's D3D library binds with the validated shader cache (native/gfx/shader_cache).
//
// For every shader object the library creates it hashes the container the engine handed over (the cache's own key) and keeps the container's
// microcode; at every draw-state commit it looks at the shader objects now bound, reads the microcode the library left in memory (patched to the
// current vertex declaration for vertex shaders), parses the IM_LOAD packets the commit wrote and compares them with the object's microcode.
// Observation only: the game runs as before. Enabled with GH2_D3D_OBSERVE=<dir of shaders.pak> (GH2_D3D_REPORT=<json file>, a summary line goes
// to stderr every 10 s).
//
// Plain C++ (no guest macros, no SDK); links native/gfx/shader_cache.
#pragma once

#include <gh2gfx/guest_d3d.h>

#include <filesystem>
#include <memory>
#include <optional>

#include "shader_cache.h"

namespace gh2gfx::d3d {

// One of the two shaders bound on a device, as the renderer will need it at draw time.
struct BoundShader {
  bool registered = false;                   // the object was returned by one of the hooked creation functions
  uint32_t object = 0;                       // guest address of the shader object (0 = nothing bound)
  uint64_t container_hash = 0;               // the shader cache's key
  std::optional<gh2gfx::ShaderBlob> spirv;   // the validated module when the cache has it (valid while the observer lives)
};
struct BoundShaders {
  BoundShader vertex, pixel;
};

class ShaderObserver : public Observer {
 public:
  // What the library has bound on `device` right now, with the SPIR-V modules from the cache: the answer the renderer asks for at every draw.
  // Meant for the guest thread that draws (right after OnDrawStateCommit); thread-safe.
  virtual BoundShaders Resolve(const uint8_t* base, uint32_t device) = 0;
};

std::unique_ptr<ShaderObserver> MakeShaderObserver(const std::filesystem::path& cache_dir, const std::filesystem::path& report_json);

// Reads GH2_D3D_OBSERVE / GH2_D3D_REPORT and, when asked for and the cache opens, registers an observer for the life of the process.
// Returns whether one was installed.
bool InstallObserverFromEnvironment();

}  // namespace gh2gfx::d3d
