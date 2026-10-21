// "Save problem report": one text file with what the maintainers need to understand a bug report: the install, the system, the
// settings in use, the controllers, the game's frame rate right now, and the tails of the game's logs. The user's home folder
// is replaced by "~" so the file is safe to attach to a public GitHub issue.
#pragma once

#include <string>

// Provided by the menu: saves the game's own picture (not the desktop, not other windows) as a PNG at `path`; false if it cannot.
using Gh2ScreenshotFn = bool (*)(const std::string& path);
void Gh2SetScreenshotFn(Gh2ScreenshotFn fn);

// Writes logs/report-<date>-<time>.txt next to the game (and, if asked, report-<date>-<time>.png with the game's picture).
// Returns the absolute path of the text file, or "" and sets `error`. `screenshot_saved` tells whether the picture was written.
std::string Gh2WriteProblemReport(std::string& error, bool with_screenshot = false, bool* screenshot_saved = nullptr);
