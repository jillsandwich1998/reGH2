#include "guitar_classifier.h"

#include <cctype>
#include <cstddef>

namespace gh2plat {

namespace {

struct UsbEntry {
  uint16_t vendor;
  uint16_t product;
  DeviceKind kind;
  const char* what;
};

// Instruments SDL 3.4 itself lists (src/joystick/controller_list.h and usb_ids.h), by the protocol they speak. Only a fallback:
// when SDL's HIDAPI driver recognised the device, its own type (evidence 1) has already decided.
constexpr UsbEntry kUsbTable[] = {
    // Xbox 360 wire protocol (xpad / HIDAPI Xbox 360): SDL types these XBOX360 gamepads, never guitars
    {0x1430, 0x4748, DeviceKind::kGuitar, "RedOctane Guitar Hero X-plorer (Xbox 360)"},
    {0x1bad, 0x0002, DeviceKind::kGuitar, "Harmonix Rock Band Guitar (Xbox 360)"},
    {0x1bad, 0x0003, DeviceKind::kDrumKit, "Harmonix Rock Band Drumkit (Xbox 360)"},
    // PS3 protocol (Sony licensed vendor 0x12ba, Harmonix 0x1bad for the Wii ones)
    {0x12ba, 0x0100, DeviceKind::kGuitar, "PS3 Guitar Hero Guitar"},
    {0x12ba, 0x0120, DeviceKind::kDrumKit, "PS3 Guitar Hero Drums"},
    {0x12ba, 0x0200, DeviceKind::kGuitar, "PS3 Rock Band Guitar"},
    {0x12ba, 0x0210, DeviceKind::kDrumKit, "PS3 Rock Band Drums"},
    {0x12ba, 0x0218, DeviceKind::kDrumKit, "PS3 Midi Pro Adapter, drums mode"},
    {0x12ba, 0x074b, DeviceKind::kGuitar, "PS3 / Wii U Guitar Hero Live Guitar"},
    {0x12ba, 0x2430, DeviceKind::kGuitar, "PS3 Mustang Guitar"},
    {0x12ba, 0x2438, DeviceKind::kGuitar, "PS3 Midi Pro Adapter, Mustang mode"},
    {0x12ba, 0x2530, DeviceKind::kGuitar, "PS3 Squire Guitar"},
    {0x12ba, 0x2538, DeviceKind::kGuitar, "PS3 Midi Pro Adapter, Squire mode"},
    {0x1bad, 0x0004, DeviceKind::kGuitar, "Wii RB1 Guitar (PS3 protocol)"},
    {0x1bad, 0x0005, DeviceKind::kDrumKit, "Wii RB1 Drums (PS3 protocol)"},
    {0x1bad, 0x3010, DeviceKind::kGuitar, "Wii RB2 Guitar (PS3 protocol)"},
    {0x1bad, 0x3110, DeviceKind::kDrumKit, "Wii RB2 Drums (PS3 protocol)"},
    {0x1bad, 0x3138, DeviceKind::kDrumKit, "Wii RB3 Midi Pro Adapter, drums mode"},
    {0x1bad, 0x3430, DeviceKind::kGuitar, "Wii RB3 Mustang Guitar"},
    {0x1bad, 0x3438, DeviceKind::kGuitar, "Wii RB3 Midi Pro Adapter, Mustang mode"},
    {0x1bad, 0x3530, DeviceKind::kGuitar, "Wii RB3 Squire Guitar"},
    {0x1bad, 0x3538, DeviceKind::kGuitar, "Wii RB3 Midi Pro Adapter, Squire mode"},
    // Xbox One / GIP (HIDAPI GIP driver); SDL tags these itself when it can open them
    {0x0e6f, 0x0170, DeviceKind::kGuitar, "PDP Xbox One Jaguar Guitar"},
    {0x0e6f, 0x0248, DeviceKind::kGuitar, "PDP Xbox One Riffmaster Guitar"},
    {0x0738, 0x4161, DeviceKind::kGuitar, "Mad Catz Xbox One Stratocaster Guitar"},
    {0x1430, 0x0170, DeviceKind::kGuitar, "RedOctane Xbox One Guitar Hero Live Guitar"},
    {0x1430, 0x4161, DeviceKind::kGuitar, "RedOctane Xbox One Stage Tour Guitar"},
};

bool ContainsCI(std::string_view haystack, std::string_view needle) {
  if (needle.empty() || haystack.size() < needle.size()) return false;
  for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    size_t j = 0;
    while (j < needle.size() &&
           std::tolower(static_cast<unsigned char>(haystack[i + j])) == std::tolower(static_cast<unsigned char>(needle[j])))
      ++j;
    if (j == needle.size()) return true;
  }
  return false;
}

uint8_t SubtypeOf(DeviceKind kind) {
  switch (kind) {
    case DeviceKind::kGuitar: return kSubtypeGuitar;
    case DeviceKind::kDrumKit: return kSubtypeDrumKit;
    case DeviceKind::kWheel: return kSubtypeWheel;
    case DeviceKind::kArcadeStick: return kSubtypeArcadeStick;
    case DeviceKind::kFlightStick: return kSubtypeFlightStick;
    case DeviceKind::kDancePad: return kSubtypeDancePad;
    case DeviceKind::kArcadePad: return kSubtypeArcadePad;
    case DeviceKind::kGamepad: break;
  }
  return kSubtypeGamepad;
}

Classification Make(DeviceKind kind, Evidence evidence, const char* detail) { return {kind, SubtypeOf(kind), evidence, detail}; }

// Name words, checked in this order. Drums come before guitars so that "Guitar Hero World Tour Drums" is a drum kit; the rest
// follows the SDK driver's order (its names come from the XInput backend: "XInput Guitar #1", "XInput ArcadeStick #1", ...).
struct NameRule {
  const char* word;
  DeviceKind kind;
};
constexpr NameRule kNameRules[] = {
    {"drum", DeviceKind::kDrumKit},
    {"guitar", DeviceKind::kGuitar},
    {"stratocaster", DeviceKind::kGuitar},
    {"x-plorer", DeviceKind::kGuitar},
    {"xplorer", DeviceKind::kGuitar},
    {"riffmaster", DeviceKind::kGuitar},
    {"wheel", DeviceKind::kWheel},
    {"dancepad", DeviceKind::kDancePad},
    {"dance pad", DeviceKind::kDancePad},
    {"flightstick", DeviceKind::kFlightStick},
    {"flight stick", DeviceKind::kFlightStick},
    {"hotas", DeviceKind::kFlightStick},
    {"arcadepad", DeviceKind::kArcadePad},
    {"arcade pad", DeviceKind::kArcadePad},
    {"arcade", DeviceKind::kArcadeStick},
};

}  // namespace

Classification Classify(const DeviceIdentity& d) {
  switch (d.sdl_kind) {
    case SdlKind::kGuitar: return Make(DeviceKind::kGuitar, Evidence::kSdlType, "SDL_JOYSTICK_TYPE_GUITAR");
    case SdlKind::kDrumKit: return Make(DeviceKind::kDrumKit, Evidence::kSdlType, "SDL_JOYSTICK_TYPE_DRUM_KIT");
    case SdlKind::kWheel: return Make(DeviceKind::kWheel, Evidence::kSdlType, "SDL_JOYSTICK_TYPE_WHEEL");
    case SdlKind::kArcadeStick: return Make(DeviceKind::kArcadeStick, Evidence::kSdlType, "SDL_JOYSTICK_TYPE_ARCADE_STICK");
    case SdlKind::kFlightStick: return Make(DeviceKind::kFlightStick, Evidence::kSdlType, "SDL_JOYSTICK_TYPE_FLIGHT_STICK");
    case SdlKind::kDancePad: return Make(DeviceKind::kDancePad, Evidence::kSdlType, "SDL_JOYSTICK_TYPE_DANCE_PAD");
    case SdlKind::kArcadePad: return Make(DeviceKind::kArcadePad, Evidence::kSdlType, "SDL_JOYSTICK_TYPE_ARCADE_PAD");
    case SdlKind::kGamepad:
    case SdlKind::kThrottle:
    case SdlKind::kUnknown: break;
  }
  for (const auto& e : kUsbTable)
    if (e.vendor == d.vendor && e.product == d.product) return Make(e.kind, Evidence::kUsbId, e.what);
  for (const auto& r : kNameRules)
    if (ContainsCI(d.name, r.word)) return Make(r.kind, Evidence::kName, r.word);
  return Make(DeviceKind::kGamepad, Evidence::kDefault, "no instrument evidence");
}

const char* KindName(DeviceKind kind) {
  switch (kind) {
    case DeviceKind::kGamepad: return "Gamepad";
    case DeviceKind::kGuitar: return "Guitar";
    case DeviceKind::kDrumKit: return "Drum kit";
    case DeviceKind::kWheel: return "Wheel";
    case DeviceKind::kArcadeStick: return "Arcade stick";
    case DeviceKind::kFlightStick: return "Flight stick";
    case DeviceKind::kDancePad: return "Dance pad";
    case DeviceKind::kArcadePad: return "Arcade pad";
  }
  return "?";
}

const char* EvidenceName(Evidence evidence) {
  switch (evidence) {
    case Evidence::kSdlType: return "sdl-type";
    case Evidence::kUsbId: return "usb-id";
    case Evidence::kName: return "name";
    case Evidence::kDefault: return "default";
  }
  return "?";
}

}  // namespace gh2plat
