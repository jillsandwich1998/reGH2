#include "fps_meter.h"

#include <algorithm>
#include <atomic>
#include <vector>

namespace {
constexpr int kFrames = 600;  // about 10 s of frame times
float g_interval[kFrames];
std::atomic<unsigned> g_interval_head{0};
float g_history[kGh2FpsHistory];
std::atomic<unsigned> g_history_head{0};
double g_acc_ms = 0;  // only touched by the game's thread
int g_acc_frames = 0;
}  // namespace

void Gh2FpsNoteFrame(double interval_ms) {
  const unsigned i = g_interval_head.load(std::memory_order_relaxed);
  g_interval[i % kFrames] = static_cast<float>(interval_ms);
  g_interval_head.store(i + 1, std::memory_order_release);
  g_acc_ms += interval_ms;
  ++g_acc_frames;
  if (g_acc_ms >= 250.0) {
    const unsigned h = g_history_head.load(std::memory_order_relaxed);
    g_history[h % kGh2FpsHistory] = static_cast<float>(g_acc_frames * 1000.0 / g_acc_ms);
    g_history_head.store(h + 1, std::memory_order_release);
    g_acc_ms = 0;
    g_acc_frames = 0;
  }
}

void Gh2FpsRead(Gh2FpsView& out) {
  const unsigned head = g_interval_head.load(std::memory_order_acquire);
  const int n = static_cast<int>(std::min<unsigned>(head, kFrames));
  std::vector<float> iv(n);
  for (int k = 0; k < n; ++k) iv[k] = g_interval[(head - n + k) % kFrames];  // oldest first
  if (n > 0) {
    const int recent = std::min(n, 30);  // ~0.5 s at 60 fps
    double sum = 0;
    for (int k = n - recent; k < n; ++k) sum += iv[k];
    out.frame_ms = static_cast<float>(sum / recent);
    out.fps = out.frame_ms > 0 ? 1000.0f / out.frame_ms : 0;
    out.worst_ms = *std::max_element(iv.end() - std::min(n, 300), iv.end());
    std::vector<float> sorted(iv);
    std::sort(sorted.begin(), sorted.end());
    const float p99 = sorted[std::min<size_t>(sorted.size() - 1, static_cast<size_t>(sorted.size() * 0.99))];
    out.low1_fps = p99 > 0 ? 1000.0f / p99 : 0;
  }
  const unsigned hh = g_history_head.load(std::memory_order_acquire);
  out.history_n = static_cast<int>(std::min<unsigned>(hh, kGh2FpsHistory));
  for (int k = 0; k < out.history_n; ++k) out.history[k] = g_history[(hh - out.history_n + k) % kGh2FpsHistory];
}
