// Claude — Date 06/19/2026
// Minimal stderr logger for the early milestones. Under systemd, stderr is
// captured by journald, so this is deliberately dumb for now. The batched /
// rate-limited, SD-card-friendly logger described in the plan replaces this
// later (util/log proper) — see castleplusplus plan, "Hardening".
#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace castle {

enum class LogLevel { 
    Info, //information
    Warn, //warnings
    Error //errors
};

// Claude — Date 06/19/2026
// Single mutex so interleaved lines from the per-core worker threads don't
// shred each other. Cheap; logging is not on the hot path once steady-state.
inline std::mutex& log_mutex() {
    static std::mutex m;
    return m;
}

inline const char* level_tag(LogLevel l) {
    switch (l) {
        case LogLevel::Info:  return "[INFO]~";
        case LogLevel::Warn:  return "[WARN]~";
        case LogLevel::Error: return "[ERROR]~";
    }
    return "?";
}

inline void log_msg(LogLevel level, const char* fmt, ...) {
    char ts[32];
    std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&now, &tm_buf);
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    std::lock_guard<std::mutex> lock(log_mutex());
    std::fprintf(stderr, "[%s] %-5s ", ts, level_tag(level));
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
}

}  // namespace castle

#define LOG_INFO(...)  ::castle::log_msg(::castle::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...)  ::castle::log_msg(::castle::LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) ::castle::log_msg(::castle::LogLevel::Error, __VA_ARGS__)
