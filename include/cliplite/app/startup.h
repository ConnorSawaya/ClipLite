#pragma once

namespace cliplite::app {

// Adds/removes the app from HKCU\...\Run so it starts with Windows. Uses the
// current executable path (quoted). Returns false on failure.
bool set_startup_enabled(bool enabled);
bool startup_enabled();

}  // namespace cliplite::app
