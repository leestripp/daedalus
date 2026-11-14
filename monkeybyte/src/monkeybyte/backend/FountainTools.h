#pragma once

#include <optional>
#include <string>

namespace monkeybyte::fountain {

// External Fountain (fountain.io) screenplay converters (Plan.md P4).
// MonkeyByte never parses/renders Fountain in-process — "wrap" (preferred)
// or "screenplain" (fallback) do the work; absence degrades gracefully.

// Path/name of the first converter found on PATH, "" when neither exists.
std::string find_converter();

// fountain source -> standalone styled HTML (screenplay layout).
// Empty string on failure or when no converter is installed.
std::string to_html(const std::string& fountain_source);

// Exports <fountain_path> to <pdf_path>. True on success; false + *error
// otherwise (including "no converter installed" with an install hint).
bool to_pdf(const std::string& fountain_path, const std::string& pdf_path,
            std::string* error = nullptr);

// User-facing hint when find_converter() == "".
inline constexpr const char* kInstallHint =
    "No Fountain converter found. Install 'wrap' (go install "
    "github.com/eprovst/wrap@latest) or 'screenplain' (pip install "
    "screenplain[PDF]) to enable screenplay preview and PDF export.";

} // namespace monkeybyte::fountain
