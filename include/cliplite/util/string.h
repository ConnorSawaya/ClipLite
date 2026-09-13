#pragma once

#include <string>

namespace cliplite::util {

// Replaces Windows-illegal filename characters and control chars with '_',
// trims trailing spaces/dots, and guarantees a non-empty result.
std::string sanitize_filename(const std::string& input);

// Returns the base name (no directory, no extension) of an executable path.
std::string exe_base_name(const std::string& exe_path);

// Derives a human-readable game name from a window title, falling back to the
// executable base name when the title is empty or reduces to nothing.
std::string clean_game_name(const std::string& window_title, const std::string& exe_name);

}  // namespace cliplite::util
