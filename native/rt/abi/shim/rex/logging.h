// Shim of <rex/logging.h>: the only logging the generated code does is ppc_trap() in gh2_pch.h (REXCPU_DEBUG / REXCPU_WARN
// with "{}" placeholders). Formatting is done here with no dependency (no fmt/spdlog); the message goes to gh2abi::LogMessage.
#pragma once

#include <gh2abi/abi.h>

#include <string>
#include <string_view>
#include <type_traits>

namespace gh2abi::detail {

inline void AppendArg(std::string& out, std::string_view v) { out.append(v); }
inline void AppendArg(std::string& out, const char* v) { out.append(v ? v : "(null)"); }
inline void AppendArg(std::string& out, const std::string& v) { out.append(v); }
inline void AppendArg(std::string& out, bool v) { out.append(v ? "true" : "false"); }
template <class T>
  requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
inline void AppendArg(std::string& out, T v) {
  out.append(std::to_string(v));
}
template <class T>
  requires std::is_floating_point_v<T>
inline void AppendArg(std::string& out, T v) {
  out.append(std::to_string(v));
}

template <class... Args>
inline void Log(LogLevel level, std::string_view fmt, const Args&... args) {
  std::string out;
  out.reserve(fmt.size() + 16 * sizeof...(Args));
  size_t pos = 0;
  auto next = [&](const auto& arg) {
    const size_t brace = fmt.find("{}", pos);
    if (brace == std::string_view::npos) return;
    out.append(fmt.substr(pos, brace - pos));
    AppendArg(out, arg);
    pos = brace + 2;
  };
  (next(args), ...);
  out.append(fmt.substr(pos));
  LogMessage(level, out);
}

}  // namespace gh2abi::detail

#define REXCPU_DEBUG(...) ::gh2abi::detail::Log(::gh2abi::LogLevel::kDebug, __VA_ARGS__)
#define REXCPU_WARN(...) ::gh2abi::detail::Log(::gh2abi::LogLevel::kWarn, __VA_ARGS__)
