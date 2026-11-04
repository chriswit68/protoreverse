#pragma once

#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace pr {

enum class LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5,
};

void set_log_level(LogLevel level);
LogLevel log_level();
bool parse_log_level(std::string_view text, LogLevel& out);

void log_message(LogLevel level, std::string_view category, std::string_view message);

template <typename... Args>
void log(LogLevel level, std::string_view category, Args&&... args) {
    if (static_cast<int>(level) < static_cast<int>(log_level()) && log_level() != LogLevel::Off) {
        return;
    }
    if (log_level() == LogLevel::Off) {
        return;
    }
    std::ostringstream out;
    (out << ... << std::forward<Args>(args));
    log_message(level, category, out.str());
}

template <typename... Args>
void log_info(std::string_view category, Args&&... args) {
    log(LogLevel::Info, category, std::forward<Args>(args)...);
}

template <typename... Args>
void log_debug(std::string_view category, Args&&... args) {
    log(LogLevel::Debug, category, std::forward<Args>(args)...);
}

template <typename... Args>
void log_warn(std::string_view category, Args&&... args) {
    log(LogLevel::Warn, category, std::forward<Args>(args)...);
}

template <typename... Args>
void log_error(std::string_view category, Args&&... args) {
    log(LogLevel::Error, category, std::forward<Args>(args)...);
}

} // namespace pr
