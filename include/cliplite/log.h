#pragma once

#include <cstddef>
#include <string>

namespace cliplite::log {

enum class Level { Trace = 0, Debug, Info, Warn, Error };

// Initializes a rotating file logger. If file_path is empty, logging is disabled.
// max_size is the per-file byte threshold before rotation; max_files keeps at most
// that many rotated backups (.1, .2, ...).
void init(const std::string& file_path, std::size_t max_size = 1024 * 1024, int max_files = 3);
void shutdown();
void log(Level level, const char* component, const std::string& message);

const char* level_name(Level level);

}  // namespace cliplite::log

#define CL_TRACE(component, msg) ::cliplite::log::log(::cliplite::log::Level::Trace, component, msg)
#define CL_DEBUG(component, msg) ::cliplite::log::log(::cliplite::log::Level::Debug, component, msg)
#define CL_INFO(component, msg)  ::cliplite::log::log(::cliplite::log::Level::Info, component, msg)
#define CL_WARN(component, msg)  ::cliplite::log::log(::cliplite::log::Level::Warn, component, msg)
#define CL_ERROR(component, msg) ::cliplite::log::log(::cliplite::log::Level::Error, component, msg)
