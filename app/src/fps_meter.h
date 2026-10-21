// Frame rate of the game itself (the guest), measured where the game swaps frames (VdSwap). Unlike an overlay that hooks the
// host's Vulkan present calls, this does not repeat when the game is late: a slow game shows as a low number here.
#pragma once

constexpr int kGh2FpsHistory = 160;  // quarter-second samples: 40 s of history

struct Gh2FpsView {
  float fps = 0;        // frame rate over the last ~0.5 s
  float frame_ms = 0;   // average frame time over the same time
  float low1_fps = 0;   // "1% low": the frame rate of the slowest 1% of the last ~10 s
  float worst_ms = 0;   // slowest frame of the last ~5 s
  int history_n = 0;    // valid samples in `history`, oldest first
  float history[kGh2FpsHistory] = {};  // fps per quarter second
};

// Called by the game's thread once per frame with the time since the previous frame.
void Gh2FpsNoteFrame(double interval_ms);
// Called by the UI thread.
void Gh2FpsRead(Gh2FpsView& out);
