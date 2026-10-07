#pragma once

#include <iostream>
#include <mutex>

inline std::mutex& LogMutex() {
    static std::mutex mutex;
    return mutex;
}

#define LOG_INFO(msg) do { std::lock_guard<std::mutex> lock(LogMutex()); std::cout << "[INFO] " << msg << '\n'; } while (0)
#define LOG_ERROR(msg) do { std::lock_guard<std::mutex> lock(LogMutex()); std::cout << "[ERROR] " << msg << '\n' << std::flush; } while (0)
