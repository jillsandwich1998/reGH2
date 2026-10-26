// Registration point of the host services (see gh2rt/host_services.h). No SDK dependency.
#include <gh2rt/host_services.h>

#include <atomic>

namespace gh2rt {
namespace {

// Used until the application registers its own: every query answers "nothing special".
class InertServices final : public HostServices {
 public:
  Settings GetSettings() override { return {}; }
  void SetGuestTimeScalar(double) override {}
  KeyboardState PollKeyboard(uint32_t) override { return {}; }
  int KeyboardSlot() override { return -1; }
  void KeyboardNoteInSong() override {}
  bool UserIsGuitar(uint32_t) override { return false; }
  bool UserHasRealDevice(uint32_t) override { return false; }
  void NoteGuestFrame(double) override {}
  void DumpCvars() override {}
};

std::atomic<HostServices*> g_services{nullptr};

}  // namespace

void SetHostServices(HostServices* services) { g_services.store(services, std::memory_order_release); }

HostServices& Host() {
  static InertServices inert;
  HostServices* services = g_services.load(std::memory_order_acquire);
  return services ? *services : inert;
}

}  // namespace gh2rt
