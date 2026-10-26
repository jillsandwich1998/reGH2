// Registration of the shader observer from the environment (separate from observer.cpp so that tests can use the observer without the registry,
// which lives with the hooks: d3d_hooks.cpp).
#include <cstdlib>

#include "observer.h"

namespace gh2gfx::d3d {

namespace {
std::unique_ptr<ShaderObserver> g_observer;
}

bool InstallObserverFromEnvironment() {
  const char* dir = getenv("GH2_D3D_OBSERVE");
  if (!dir || !*dir || g_observer) return false;
  const char* report = getenv("GH2_D3D_REPORT");
  g_observer = MakeShaderObserver(dir, report ? report : "");
  if (!g_observer) return false;
  SetObserver(g_observer.get());
  return true;
}

}  // namespace gh2gfx::d3d
