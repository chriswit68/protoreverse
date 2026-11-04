#include "log.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <string>

namespace pr {
namespace {

std::mutex g_log_mutex;
LogLevel g_log_level = LogLevel::Info;

const char* level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO ";
    case LogLevel::Warn:
        return "WARN ";
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Off:
        return "OFF  ";
    }
    return "?????";
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) %
                    1000;

    std::tm tm{};
    localtime_r(&time, &tm);

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d",
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms.count()));
    return buffer;
}

} // namespace

void set_log_level(LogLevel level) {
    std::lock_guard lock(g_log_mutex);
    g_log_level = level;
}

LogLevel log_level() {
    std::lock_guard lock(g_log_mutex);
    return g_log_level;
}

bool parse_log_level(std::string_view text, LogLevel& out) {
    std::string lower;
    lower.reserve(text.size());
    for (char c : text) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (lower == "trace") {
        out = LogLevel::Trace;
    } else if (lower == "debug") {
        out = LogLevel::Debug;
    } else if (lower == "info") {
        out = LogLevel::Info;
    } else if (lower == "warn" || lower == "warning") {
        out = LogLevel::Warn;
    } else if (lower == "error") {
        out = LogLevel::Error;
    } else if (lower == "off" || lower == "none") {
        out = LogLevel::Off;
    } else {
        return false;
    }
    return true;
}

void log_message(LogLevel level, std::string_view category, std::string_view message) {
    std::lock_guard lock(g_log_mutex);
    if (g_log_level == LogLevel::Off || static_cast<int>(level) < static_cast<int>(g_log_level)) {
        return;
    }

    std::printf("%s [%s] %-11.*s %.*s\n",
                timestamp().c_str(),
                level_name(level),
                static_cast<int>(category.size()), category.data(),
                static_cast<int>(message.size()), message.data());
    std::fflush(stdout);
}

} // namespace pr
