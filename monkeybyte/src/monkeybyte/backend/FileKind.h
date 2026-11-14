#pragma once

#include <string>

namespace monkeybyte {

// True when the file is meaningful to open in the internal text editor:
// the GIO content type is text/* or a text/plain subtype (application/json,
// xml, shellscript, … via shared-mime-info subclassing), or — when the type
// is unknown — the first bytes sniff clean of NULs. Never throws.
bool is_text_like_file(const std::string& path);

// Open the file in the system's default application (freedesktop association
// DB via GIO — the in-process equivalent of xdg-open). Returns false with
// `error` set when no handler is registered or the launch fails.
bool launch_external(const std::string& path, std::string& error);

} // namespace monkeybyte
