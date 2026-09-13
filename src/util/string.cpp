#include "cliplite/util/string.h"

#include <algorithm>
#include <cctype>

namespace cliplite::util {

namespace {

// Strips a trailing version token like "1.21.4" or "v1.2" (must contain a dot
// and a digit, and be separated by a space or 'v') only if a word remains.
std::string strip_trailing_version(std::string s) {
    size_t i = s.size();
    while (i > 0 && (std::isdigit(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '.')) {
        --i;
    }
    if (i == 0 || i == s.size()) return s;

    const std::string token = s.substr(i);
    const bool has_dot = token.find('.') != std::string::npos;
    if (!has_dot) return s;  // avoid stripping "Portal 2", "Left 4 Dead 2", etc.

    size_t sep = i;
    if (sep > 0 && (s[sep - 1] == 'v' || s[sep - 1] == 'V')) --sep;
    if (sep > 0 && s[sep - 1] == ' ') --sep;
    if (sep == i) return s;  // no separator

    std::string result = s.substr(0, sep);
    while (!result.empty() && result.back() == ' ') result.pop_back();
    bool has_letter = false;
    for (unsigned char c : result) {
        if (std::isalpha(c)) {
            has_letter = true;
            break;
        }
    }
    return has_letter ? result : s;
}

}  // namespace

std::string sanitize_filename(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (unsigned char c : input) {
        const bool illegal = (c < 0x20) || c == '<' || c == '>' || c == ':' || c == '"' ||
                             c == '/' || c == '\\' || c == '|' || c == '?' || c == '*';
        out.push_back(illegal ? '_' : static_cast<char>(c));
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    if (out.empty()) out = "clip";
    return out;
}

std::string exe_base_name(const std::string& exe_path) {
    const auto slash = exe_path.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? exe_path : exe_path.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    return name;
}

std::string clean_game_name(const std::string& window_title, const std::string& exe_name) {
    std::string name = window_title;
    // Remove decoration markers (e.g. Minecraft's "*" dirty flag).
    name.erase(std::remove(name.begin(), name.end(), '*'), name.end());

    // Truncate at common separators that precede engine/fps/version info.
    for (char sep : {'-', '|', '('}) {
        const auto pos = name.find(sep);
        if (pos != std::string::npos) {
            std::string candidate = name.substr(0, pos);
            while (!candidate.empty() && candidate.back() == ' ') candidate.pop_back();
            if (!candidate.empty()) name = candidate;
        }
    }
    while (!name.empty() && name.back() == ' ') name.pop_back();

    name = strip_trailing_version(name);

    if (name.empty()) return exe_base_name(exe_name);
    return name;
}

}  // namespace cliplite::util
