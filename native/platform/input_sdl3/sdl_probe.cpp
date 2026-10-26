// gh2_sdl_probe - read-only look at what the SDK's SDL3 makes of the connected controllers (type, ids, GUID, mapping, idle state).
//
//   gh2_sdl_probe [--watch SECONDS]     one listing; with --watch, prints every state change of every device for that long
//
// Nothing is written to a device. Used to design the guitar classifier of the native input thread (guitar_classifier.cpp).
#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

const char* TypeName(SDL_JoystickType t) {
  switch (t) {
    case SDL_JOYSTICK_TYPE_GAMEPAD: return "GAMEPAD";
    case SDL_JOYSTICK_TYPE_WHEEL: return "WHEEL";
    case SDL_JOYSTICK_TYPE_ARCADE_STICK: return "ARCADE_STICK";
    case SDL_JOYSTICK_TYPE_FLIGHT_STICK: return "FLIGHT_STICK";
    case SDL_JOYSTICK_TYPE_DANCE_PAD: return "DANCE_PAD";
    case SDL_JOYSTICK_TYPE_GUITAR: return "GUITAR";
    case SDL_JOYSTICK_TYPE_DRUM_KIT: return "DRUM_KIT";
    case SDL_JOYSTICK_TYPE_ARCADE_PAD: return "ARCADE_PAD";
    case SDL_JOYSTICK_TYPE_THROTTLE: return "THROTTLE";
    default: return "UNKNOWN";
  }
}

const char* GamepadTypeName(SDL_GamepadType t) {
  switch (t) {
    case SDL_GAMEPAD_TYPE_XBOX360: return "XBOX360";
    case SDL_GAMEPAD_TYPE_XBOXONE: return "XBOXONE";
    case SDL_GAMEPAD_TYPE_PS3: return "PS3";
    case SDL_GAMEPAD_TYPE_PS4: return "PS4";
    case SDL_GAMEPAD_TYPE_PS5: return "PS5";
    case SDL_GAMEPAD_TYPE_STANDARD: return "STANDARD";
    case SDL_GAMEPAD_TYPE_UNKNOWN: return "UNKNOWN";
    default: return "other";
  }
}

std::string Guid(SDL_JoystickID id) {
  char buf[64] = {};
  SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), buf, sizeof(buf));
  return buf;
}

}  // namespace

int main(int argc, char** argv) {
  double watch = 0;
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], "--watch")) watch = std::atof(argv[i + 1]);
  std::printf("SDL %d.%d.%d (header %d.%d.%d)\n", SDL_GetVersion() / 1000000, (SDL_GetVersion() / 1000) % 1000, SDL_GetVersion() % 1000,
              SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);
  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) return std::fprintf(stderr, "SDL_InitSubSystem: %s\n", SDL_GetError()), 1;
  // Give the evdev/udev enumeration and HIDAPI a moment.
  for (int i = 0; i < 20; ++i) {
    SDL_UpdateJoysticks();
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  int n = 0;
  SDL_JoystickID* ids = SDL_GetJoysticks(&n);
  std::printf("joysticks: %d\n", n);
  std::vector<SDL_Joystick*> opened;
  for (int i = 0; i < n; ++i) {
    const SDL_JoystickID id = ids[i];
    std::printf("[%d] id=%u name='%s'\n", i, id, SDL_GetJoystickNameForID(id));
    std::printf("    path=%s vid=%04x pid=%04x version=%04x guid=%s\n", SDL_GetJoystickPathForID(id) ? SDL_GetJoystickPathForID(id) : "-",
                SDL_GetJoystickVendorForID(id), SDL_GetJoystickProductForID(id), SDL_GetJoystickProductVersionForID(id), Guid(id).c_str());
    std::printf("    SDL_JoystickType=%s  is_gamepad=%d  gamepad_type=%s  player_index=%d\n", TypeName(SDL_GetJoystickTypeForID(id)),
                SDL_IsGamepad(id), GamepadTypeName(SDL_GetGamepadTypeForID(id)), SDL_GetJoystickPlayerIndexForID(id));
    if (SDL_IsGamepad(id)) {
      char* m = SDL_GetGamepadMappingForID(id);
      std::printf("    mapping: %s\n", m ? m : "(none)");
      SDL_free(m);
    }
    SDL_Joystick* js = SDL_OpenJoystick(id);
    opened.push_back(js);
    if (js) {
      std::printf("    axes=%d buttons=%d hats=%d  connection=%d  has_rumble=%d\n", SDL_GetNumJoystickAxes(js), SDL_GetNumJoystickButtons(js),
                  SDL_GetNumJoystickHats(js), static_cast<int>(SDL_GetJoystickConnectionState(js)), SDL_RumbleJoystick(js, 0, 0, 0));
      std::printf("    idle axes:");
      for (int a = 0; a < SDL_GetNumJoystickAxes(js); ++a) std::printf(" %d", SDL_GetJoystickAxis(js, a));
      std::printf("\n    idle buttons down:");
      for (int b = 0; b < SDL_GetNumJoystickButtons(js); ++b)
        if (SDL_GetJoystickButton(js, b)) std::printf(" %d", b);
      std::printf("\n");
    }
    if (SDL_IsGamepad(id)) {
      SDL_Gamepad* gp = SDL_OpenGamepad(id);
      if (gp) {
        std::printf("    gamepad idle: LX=%d LY=%d RX=%d RY=%d LT=%d RT=%d buttons down:", SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX),
                    SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY), SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX),
                    SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY), SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER),
                    SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
          if (SDL_GetGamepadButton(gp, static_cast<SDL_GamepadButton>(b))) std::printf(" %s", SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(b)));
        std::printf("\n");
        SDL_CloseGamepad(gp);
      }
    }
  }
  if (watch > 0 && !opened.empty()) {
    std::printf("watching %.0f s: press / move things on the device(s)\n", watch);
    std::vector<std::vector<int>> last(opened.size());
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < watch) {
      SDL_UpdateJoysticks();
      for (size_t i = 0; i < opened.size(); ++i) {
        SDL_Joystick* js = opened[i];
        if (!js) continue;
        std::vector<int> cur;
        for (int a = 0; a < SDL_GetNumJoystickAxes(js); ++a) cur.push_back(SDL_GetJoystickAxis(js, a));
        for (int b = 0; b < SDL_GetNumJoystickButtons(js); ++b) cur.push_back(SDL_GetJoystickButton(js, b));
        for (int h = 0; h < SDL_GetNumJoystickHats(js); ++h) cur.push_back(SDL_GetJoystickHat(js, h));
        if (cur != last[i]) {
          std::printf("%7.3f dev%zu:", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), i);
          for (size_t k = 0; k < cur.size(); ++k)
            if (last[i].size() != cur.size() || cur[k] != last[i][k]) std::printf(" [%zu]=%d", k, cur[k]);
          std::printf("\n");
          last[i] = cur;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  for (auto* js : opened)
    if (js) SDL_CloseJoystick(js);
  SDL_free(ids);
  SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  return 0;
}
