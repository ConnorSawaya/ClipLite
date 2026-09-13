#pragma once

#include <windows.h>

#include <string>

namespace cliplite::library {

// Decodes one frame from a clip and returns a scaled RGB thumbnail bitmap
// (target width x height). Returns NULL on failure. The caller owns the HBITMAP.
// Results are cached on disk under %LOCALAPPDATA%\ClipLite\thumbs, keyed by the
// source path + last-write-time + target size, so repeat library scans skip the
// Media Foundation decode entirely.
HBITMAP generate_thumbnail(const std::wstring& path, int width, int height);

// Absolute path of the on-disk cached thumbnail for this source, or an empty
// string when none exists yet (generate_thumbnail writes it).
std::wstring cached_thumbnail_file(const std::wstring& path, int width, int height);

// Seeks to time_ms and returns that frame as a bitmap (same ownership/cache
// rules as generate_thumbnail, keyed with the timestamp). Powers the editor
// filmstrip; each call decodes at most a short GOP, never the whole clip.
HBITMAP generate_thumbnail_at(const std::wstring& path, int width, int height,
                              int64_t time_ms);

// Cached-file twin of generate_thumbnail_at (no decode when present).
std::wstring cached_thumbnail_file_at(const std::wstring& path, int width, int height,
                                      int64_t time_ms);

// The directory backing the thumbnail disk cache (created on demand). Suitable
// for a WebView2 SetVirtualHostNameToFolderMapping so the UI can load thumbs by
// URL instead of receiving megabytes of base64 data URLs.
std::wstring thumbnail_cache_dir();

}  // namespace cliplite::library
