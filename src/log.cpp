#include "cliplite/log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace cliplite::log {
namespace {

struct State {
    std::mutex mutex;
    std::string path;
    std::size_t max_size = 1024 * 1024;
    int max_files = 3;
    bool enabled = false;
    std::ofstream stream;
    std::size_t current_size = 0;
};

State g_state;

std::string timestamp_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_MSC_VER)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));
    return std::string(buf);
}

// Assumes g_state.mutex is held by the caller.
void rotate() {
    g_state.stream.close();
    for (int i = g_state.max_files - 1; i >= 1; --i) {
        const std::string from = g_state.path + "." + std::to_string(i - 1);
        const std::string to = g_state.path + "." + std::to_string(i);
        std::error_code ec;
        std::filesystem::remove(to, ec);
        std::filesystem::rename(from, to, ec);
    }
    std::error_code ec;
    std::filesystem::rename(g_state.path, g_state.path + ".1", ec);
    g_state.current_size = 0;
    g_state.stream.open(g_state.path, std::ios::out | std::ios::app);
    if (g_state.stream) {
        g_state.stream.seekp(0, std::ios::end);
        g_state.current_size = static_cast<std::size_t>(g_state.stream.tellp());
    }
}

// Assumes g_state.mutex is held by the caller.
void write_line(const std::string& line) {
    if (!g_state.enabled || !g_state.stream.is_open()) return;
    if (g_state.current_size > 0 && g_state.current_size + line.size() >= g_state.max_size) {
        rotate();
    }
    g_state.stream << line;
    g_state.current_size += line.size();
    g_state.stream.flush();
}

}  // namespace

void init(const std::string& file_path, std::size_t max_size, int max_files) {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.path = file_path;
    g_state.max_size = max_size;
    g_state.max_files = max_files < 1 ? 1 : max_files;
    if (!file_path.empty()) {
        std::error_code ec;
        const auto parent = std::filesystem::path(file_path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        g_state.stream.open(file_path, std::ios::out | std::ios::app);
        g_state.enabled = g_state.stream.is_open();
        if (g_state.enabled) {
            g_state.stream.seekp(0, std::ios::end);
            g_state.current_size = static_cast<std::size_t>(g_state.stream.tellp());
        }
    }
}

void shutdown() {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.stream.close();
    g_state.enabled = false;
}

const char* level_name(Level level) {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
    }
    return "UNKNOWN";
}

void log(Level level, const char* component, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (!g_state.enabled) return;
    const std::string line = timestamp_now() + " [" + level_name(level) + "] [" +
                             component + "] " + message + "\n";
    write_line(line);
}

}  // namespace cliplite::log
