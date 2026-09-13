#pragma once

#include <string>

namespace cliplite::detection {

// Derives a human-readable application name. Priority:
//   1. Version resource FileDescription
//   2. Version resource ProductName
//   3. Window title cleanup (fallback to executable base name)
std::string name_application(const std::wstring& exe_path, const std::wstring& window_title,
                             const std::string& exe_name);

}  // namespace cliplite::detection
