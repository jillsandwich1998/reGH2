// A virtual Xbox 360 guitar for tests, made with /dev/uinput (needs write access to it; this machine grants the user an ACL).
// It reproduces what the kernel's xpad driver exposes for the RedOctane X-plorer (same ids, name, 11 keys, 6 axes, one hat,
// the idle values read from the real device), so SDL's evdev driver and the core see exactly the kind of device the player's is.
#pragma once

#include <cstdint>
#include <string>

namespace gh2plat {

class UinputGuitar {
 public:
  enum Button { kGreen, kRed, kYellow, kBlue, kOrange, kStart, kBack, kGuide };
  ~UinputGuitar();
  // `name` is what SDL reports; ids default to the real X-plorer's (1430:4748, version 0x3122).
  bool Create(const std::string& name, uint16_t vendor = 0x1430, uint16_t product = 0x4748, uint16_t version = 0x3122);
  void Destroy();
  // Each call is one input report (events + SYN_REPORT), written immediately. Returns the steady-clock ns taken right before write().
  uint64_t SetButton(Button b, bool down);
  uint64_t SetStrum(int dir);     // -1 up, 0 none, +1 down  (ABS_HAT0Y)
  uint64_t SetWhammy(int16_t v);  // ABS_RX
  const std::string& error() const { return error_; }
  std::string event_path() const { return event_path_; }

 private:
  uint64_t Write(int type, int code, int value);
  int fd_ = -1;
  std::string error_, event_path_;
};

}  // namespace gh2plat
