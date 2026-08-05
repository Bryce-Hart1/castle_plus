// Claude — Date 06/19/2026 last changed: 07/05/2026 by: Bryce Hart
// castle's logging front-end. Everything goes to stderr (captured by journald
// under systemd). WARN/ERROR are ALSO mirrored into Bryce's file logger
// (bstd::store::Logger in util/log.hpp) for later retrieval — enabled with
// --log-file. That logger isn't thread-safe and can throw, so we only ever
// touch it under log_mutex() and inside try/catch: a logging failure degrades
// to stderr-only, it never takes down the server.
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "util/log.hpp"  // bstd::store::Logger + IndexedFileReader

namespace castle {

enum class LogLevel {
    Info,   // information
    Warn,   // warnings
    Error   // errors
};
// Single mutex so interleaved lines from the per-core worker threads don't
// shred each other — and so the (non-thread-safe) file Logger is only ever
// touched by one thread at a time. Cheap; logging is not on the hot path.
inline std::mutex& log_mutex() {
    static std::mutex m;
    return m;
}

// The optional file logger. Null until log_init() succeeds. Only read/written
// while holding log_mutex().
inline std::unique_ptr<bstd::store::Logger>& file_logger() {
    static std::unique_ptr<bstd::store::Logger> logger;
    return logger;
}

// Our own read-side view of the same log file, for incremental "pull new"
// retrieval. Independent of the Logger (which owns the write side) so the bstd
// class stays untouched. Only touched under log_mutex().
inline std::unique_ptr<bstd::store::IndexedFileReader>& log_reader() {
    static std::unique_ptr<bstd::store::IndexedFileReader> reader;
    return reader;
}

// Index of the next unread line — the "last pulled" watermark. Starts at 0, so
// the first pull returns the whole file and each later pull returns only what's
// arrived since. In-memory (resets on restart).
inline std::size_t& log_cursor() {
    static std::size_t cursor = 0;
    return cursor;
}

// Turn on file logging to `path` (WARN/ERROR are mirrored there). Call once from
// main before threads start; empty path leaves file logging off. A failure to
// open just logs to stderr and continues.
inline void log_init(const std::string& path) {
    if (path.empty()) return;
    std::lock_guard<std::mutex> lock(log_mutex());
    try {
        // Logger first (it creates the file), then our reader over the same path.
        file_logger() = std::make_unique<bstd::store::Logger>(path, true, true);
        log_reader() = std::make_unique<bstd::store::IndexedFileReader>(path);
        log_cursor() = 0;
    } catch (const std::exception& e) {
        file_logger().reset();
        log_reader().reset();
        std::fprintf(stderr, "[log] file logging disabled (%s)\n", e.what());
    }
}

// Return every log line written since the previous pull, advancing the cursor.
// First call returns the whole file; later calls return only new lines. Empty
// if file logging is off or nothing new. Thread-safe.
inline std::vector<std::string> log_pull_new() {
    std::lock_guard<std::mutex> lock(log_mutex());
    std::vector<std::string> out;
    auto& reader = log_reader();
    if (!reader) return out;

    reader->reindex();  // pick up lines appended since we last looked
    std::size_t n = reader->line_count();
    std::size_t& cursor = log_cursor();
    if (cursor > n) cursor = n;  // file shrank (e.g. clearLast) — don't over-read
    out.reserve(n - cursor);
    for (std::size_t i = cursor; i < n; ++i) out.push_back(reader->get_line(i));
    cursor = n;
    return out;
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

    // Render once so stderr and the file logger get identical text.
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(log_mutex());
    std::fprintf(stderr, "[%s] %-5s %s\n", ts, level_tag(level), msg);

    // Mirror WARN/ERROR into the file logger (Warn->WARNING, Error->HIGH).
    if (level != LogLevel::Info && file_logger()) {
        try {
            file_logger()->write(msg, level == LogLevel::Error
                ? bstd::store::Logger::HIGH //high
                : bstd::store::Logger::WARNING); //else warning
        }catch(const std::exception& e){
            std::fprintf(stderr, "[log] file write failed (%s)\n", e.what());
        }
    }
}

}  // namespace castle

#define LOG_INFO(...)  ::castle::log_msg(::castle::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...)  ::castle::log_msg(::castle::LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) ::castle::log_msg(::castle::LogLevel::Error, __VA_ARGS__)
