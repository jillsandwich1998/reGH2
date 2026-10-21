// "Save problem report": one text file with what the maintainers need to understand a bug report: the install, the system, the
// settings in use, the controllers, the game's frame rate right now, and the tails of the game's logs. The user's home folder
// is replaced by "~" so the file is safe to attach to a public GitHub issue.
#pragma once

#include <string>

// Writes logs/report-<date>-<time>.txt next to the game. Returns the absolute path, or "" and sets `error`.
std::string Gh2WriteProblemReport(std::string& error);
