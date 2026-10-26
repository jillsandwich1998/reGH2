// What draws for --gpu=native: the Sink the command processor reports to. gpu_renderer.cpp picks it (the Vulkan renderer once it exists, otherwise a null sink that counts).
#pragma once

#include <gh2gpu/command_processor.h>

#include <memory>
#include <string>

namespace gh2rt::gpu {
std::unique_ptr<gh2gpu::Sink> CreateSink(const gh2gpu::GuestMemory& memory);
// The Vulkan renderer's counters for the periodic log line (empty with the null sink).
std::string RendererStatsLine();
}
