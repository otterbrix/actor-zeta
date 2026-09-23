#pragma once

#include <mutex>
#include <sstream>
#include <iostream>
#include <thread>
#include <string>

namespace dispatcher_test {

inline std::string thread_id_str() {
    std::ostringstream oss;
    oss << std::this_thread::get_id();
    return oss.str();
}

class thread_logger {
public:
    void set_enabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }

    void log(const std::string& msg) {
        if (!enabled_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        std::cerr << msg << std::endl;
    }

    template<typename... Args>
    void log(const char* fmt, Args&&... args) {
        if (!enabled_) return;
        std::ostringstream oss;
        format_impl(oss, fmt, std::forward<Args>(args)...);
        log(oss.str());
    }

private:
    bool enabled_ = false;
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

inline thread_logger g_log;

} // namespace dispatcher_test