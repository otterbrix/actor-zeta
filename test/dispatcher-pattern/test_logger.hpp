#pragma once

/// @file test_logger.hpp
/// @brief Thread-safe logging utility for tests
///
/// Provides formatted logging with thread identification.
/// Usage:
///   g_log.log("Message");
///   g_log.log("[%] value=%", "tag", 42);  // % is placeholder

#include <mutex>
#include <sstream>
#include <iostream>
#include <thread>
#include <string>

namespace dispatcher_test {

/// @brief Get current thread ID as string
inline std::string thread_id_str() {
    std::ostringstream oss;
    oss << std::this_thread::get_id();
    return oss.str();
}

/// @brief Thread-safe logger with printf-style formatting using % placeholders
class thread_logger {
public:
    /// @brief Log a simple message
    void log(const std::string& msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::cerr << msg << std::endl;
    }

    /// @brief Log with format string (% placeholders)
    /// @example g_log.log("[%::method] thread=% value=%", name_, tid, val);
    template<typename... Args>
    void log(const char* fmt, Args&&... args) {
        std::ostringstream oss;
        format_impl(oss, fmt, std::forward<Args>(args)...);
        log(oss.str());
    }

private:
    void format_impl(std::ostringstream& oss, const char* fmt) {
        oss << fmt;
    }

    template<typename T, typename... Args>
    void format_impl(std::ostringstream& oss, const char* fmt, T&& val, Args&&... args) {
        while (*fmt) {
            if (*fmt == '%') {
                oss << std::forward<T>(val);
                format_impl(oss, fmt + 1, std::forward<Args>(args)...);
                return;
            }
            oss << *fmt++;
        }
    }

    std::mutex mutex_;
};

/// @brief Global logger instance
inline thread_logger g_log;

} // namespace dispatcher_test