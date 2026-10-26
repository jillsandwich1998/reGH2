// gh2plat guitar / instrument classifier: what kind of device is this, and which XINPUT_DEVSUBTYPE does the guest get?
//
// Pure C++ (no SDL, no SDK): the SDL side turns what SDL reports into a DeviceIdentity. Evidence, strongest first:
//   1. SDL's own verdict (SDL_GetJoystickType): the HIDAPI drivers for Xbox One / GIP, PS3, PS4 and PS5 devices tag guitars and
//      drums themselves. The Linux evdev path (xpad, hid-sony, ...) never does: an Xbox 360 guitar such as the X-plorer arrives
//      as "GAMEPAD" there (probed on the real device: SDL 3.4.14 says type GAMEPAD, XBOX360, path /dev/input/eventN).
//   2. USB vendor/product: the instruments SDL itself knows about (its controller_list.h and usb_ids.h).
//   3. The device name ("Guitar", "Stratocaster", "X-plorer", "Riffmaster", "Drum", ...): what the SDK's own driver relies on.
// Sub-types are the ones the SDK's SDL driver reports (so menus and the hooks see the same values): guitars are 6 here; the hook
// of XamInputGetCapabilities presents every guitar to the game as 7, which is what the game's pad routine wants.
#pragma once

#include <cstdint>
#include <string_view>

namespace gh2plat {

// SDL_JoystickType, mirrored so the classifier needs no SDL header.
enum class SdlKind : uint8_t { kUnknown, kGamepad, kWheel, kArcadeStick, kFlightStick, kDancePad, kGuitar, kDrumKit, kArcadePad, kThrottle };

enum class DeviceKind : uint8_t { kGamepad, kGuitar, kDrumKit, kWheel, kArcadeStick, kFlightStick, kDancePad, kArcadePad };

enum class Evidence : uint8_t { kSdlType, kUsbId, kName, kDefault };

// XINPUT_DEVSUBTYPE_*
constexpr uint8_t kSubtypeGamepad = 0x01;
constexpr uint8_t kSubtypeWheel = 0x02;
constexpr uint8_t kSubtypeArcadeStick = 0x03;
constexpr uint8_t kSubtypeFlightStick = 0x04;
constexpr uint8_t kSubtypeDancePad = 0x05;
constexpr uint8_t kSubtypeGuitar = 0x06;
constexpr uint8_t kSubtypeDrumKit = 0x08;
constexpr uint8_t kSubtypeArcadePad = 0x13;

struct DeviceIdentity {
  uint16_t vendor = 0;
  uint16_t product = 0;
  std::string_view name;
  SdlKind sdl_kind = SdlKind::kUnknown;
};

struct Classification {
  DeviceKind kind = DeviceKind::kGamepad;
  uint8_t xinput_subtype = kSubtypeGamepad;
  Evidence evidence = Evidence::kDefault;
  const char* detail = "";  // which table entry / name word decided (static string, for logs)
};

Classification Classify(const DeviceIdentity& device);

inline bool IsGuitar(const Classification& c) { return c.kind == DeviceKind::kGuitar; }
inline bool IsGuitarSubtype(uint8_t subtype) { return subtype == 0x06 || subtype == 0x07 || subtype == 0x0B; }  // as the app's assignment

const char* KindName(DeviceKind kind);
const char* EvidenceName(Evidence evidence);

}  // namespace gh2plat
