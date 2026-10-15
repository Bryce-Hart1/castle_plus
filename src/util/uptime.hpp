// Bryce Hart — Date 10/15/2026
// Process start instant + the duration/timestamp formatting shared by the
// supervisor's `backendstatus` and the HTTP `/_castle/uptime` endpoint.
//
// Uptime is never a running timer: we stamp the start once and subtract it from
// now() whenever someone asks. steady_clock for the arithmetic (an NTP step
// can't make uptime jump), system_clock only for the printable "since" time.
#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace castle {

// First call stamps the instant; main() calls both right at startup so they
// mark when castle came up. Function-local statics are thread-safe to init.
inline std::chrono::steady_clock::time_point process_start() {
    static const auto t = std::chrono::steady_clock::now();
    return t;
}

inline std::chrono::system_clock::time_point process_start_wall() {
    static const auto t = std::chrono::system_clock::now();
    return t;
}

// Compact, human-scannable duration. Resolution drops as the span grows
inline std::string human_duration(long long secs) {
    constexpr u_int32_t minute = 60;
    constexpr u_int32_t hour = minute * minute;
    constexpr u_int32_t day = hour * 24;
    if (secs < 0) secs = 0;
    char buf[32];
    if (secs < minute) {
        std::snprintf(buf, sizeof(buf), "%llds", secs);
    } else if (secs < hour) {
        std::snprintf(buf, sizeof(buf), "%lldm %02llds", secs / 60, secs % 60);
    } else if (secs < day) {
        std::snprintf(buf, sizeof(buf), "%lldh %02lldm", secs / 3600, (secs % 3600) / 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%lldd %02lldh", secs / 86400, (secs % 86400) / 3600);
    }
    return buf;
}

inline std::string wall_timestamp(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
    ::localtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return buf;
}

}  // namespace castle
