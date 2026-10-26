#include "uinput_guitar.h"

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>

#include "input_core.h"

namespace gh2plat {

namespace {

bool Ioctl(int fd, unsigned long req, long arg) { return ioctl(fd, req, arg) == 0; }

// xpad's mapping of an Xbox 360 pad: A B X Y are BTN_A BTN_B BTN_X BTN_Y; the left shoulder is the orange fret; the d-pad is a hat.
int KeyOf(UinputGuitar::Button b) {
  switch (b) {
    case UinputGuitar::kGreen: return BTN_A;
    case UinputGuitar::kRed: return BTN_B;
    case UinputGuitar::kYellow: return BTN_Y;
    case UinputGuitar::kBlue: return BTN_X;
    case UinputGuitar::kOrange: return BTN_TL;
    case UinputGuitar::kStart: return BTN_START;
    case UinputGuitar::kBack: return BTN_SELECT;
    case UinputGuitar::kGuide: return BTN_MODE;
  }
  return BTN_A;
}

}  // namespace

UinputGuitar::~UinputGuitar() { Destroy(); }

bool UinputGuitar::Create(const std::string& name, uint16_t vendor, uint16_t product, uint16_t version) {
  fd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd_ < 0) {
    error_ = std::string("open /dev/uinput: ") + strerror(errno);
    return false;
  }
  Ioctl(fd_, UI_SET_EVBIT, EV_KEY);
  for (int k : {BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR})
    Ioctl(fd_, UI_SET_KEYBIT, k);
  Ioctl(fd_, UI_SET_EVBIT, EV_ABS);
  struct Axis { int code; int min, max, fuzz, flat, value; };
  // idle values are those read from the real X-plorer: triggers ~mid-range (garbage), whammy and tilt at their minimum
  const Axis axes[] = {
      {ABS_X, -32768, 32767, 16, 128, 0},   {ABS_Y, -32768, 32767, 16, 128, 0},   {ABS_Z, 0, 255, 0, 0, 125},
      {ABS_RX, -32768, 32767, 16, 128, -32768}, {ABS_RY, -32768, 32767, 16, 128, -32768}, {ABS_RZ, 0, 255, 0, 0, 132},
      {ABS_HAT0X, -1, 1, 0, 0, 0},          {ABS_HAT0Y, -1, 1, 0, 0, 0},
  };
  for (const auto& a : axes) {
    Ioctl(fd_, UI_SET_ABSBIT, a.code);
    uinput_abs_setup s = {};
    s.code = static_cast<__u16>(a.code);
    s.absinfo.minimum = a.min;
    s.absinfo.maximum = a.max;
    s.absinfo.fuzz = a.fuzz;
    s.absinfo.flat = a.flat;
    s.absinfo.value = a.value;
    if (ioctl(fd_, UI_ABS_SETUP, &s) != 0) {
      error_ = std::string("UI_ABS_SETUP: ") + strerror(errno);
      Destroy();
      return false;
    }
  }
  uinput_setup us = {};
  us.id.bustype = BUS_USB;
  us.id.vendor = vendor;
  us.id.product = product;
  us.id.version = version;
  std::strncpy(us.name, name.c_str(), UINPUT_MAX_NAME_SIZE - 1);
  if (ioctl(fd_, UI_DEV_SETUP, &us) != 0 || ioctl(fd_, UI_DEV_CREATE) != 0) {
    error_ = std::string("UI_DEV_SETUP/CREATE: ") + strerror(errno);
    Destroy();
    return false;
  }
  char sysname[64] = {};
  if (ioctl(fd_, UI_GET_SYSNAME(sizeof(sysname)), sysname) > 0) {  // returns the length copied
    // /sys/devices/virtual/input/inputN/eventM  ->  /dev/input/eventM
    for (int i = 0; i < 100 && event_path_.empty(); ++i) {
      std::error_code ec;
      for (const auto& e : std::filesystem::directory_iterator(std::string("/sys/devices/virtual/input/") + sysname, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("event", 0) == 0) event_path_ = "/dev/input/" + n;
      }
      if (event_path_.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  return true;
}

void UinputGuitar::Destroy() {
  if (fd_ < 0) return;
  ioctl(fd_, UI_DEV_DESTROY);
  close(fd_);
  fd_ = -1;
}

uint64_t UinputGuitar::Write(int type, int code, int value) {
  input_event ev[2] = {};
  ev[0].type = static_cast<__u16>(type);
  ev[0].code = static_cast<__u16>(code);
  ev[0].value = value;
  ev[1].type = EV_SYN;
  ev[1].code = SYN_REPORT;
  const uint64_t t = InputCore::NowNs();
  if (write(fd_, ev, sizeof(ev)) != static_cast<ssize_t>(sizeof(ev))) error_ = std::string("write: ") + strerror(errno);
  return t;
}

uint64_t UinputGuitar::SetButton(Button b, bool down) { return Write(EV_KEY, KeyOf(b), down ? 1 : 0); }
uint64_t UinputGuitar::SetStrum(int dir) { return Write(EV_ABS, ABS_HAT0Y, dir); }
uint64_t UinputGuitar::SetWhammy(int16_t v) { return Write(EV_ABS, ABS_RX, v); }

}  // namespace gh2plat
