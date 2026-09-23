// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-manager/verdict.h
//
// Verdict parser: extracts "VERDICT: APPROVED" / "VERDICT: REJECTED: <issues>"
// from the validator's last assistant text. Extracted from main.cpp to keep it under 300 lines.

#pragma once

#include <string>

// =============================================================================
// parse_verdict — extract "VERDICT: APPROVED" / "VERDICT: REJECTED: <reasons>"
// Returns:
//   approved = true  + empty issues    → done
//   approved = false + non-empty issues → loop with feedback
// =============================================================================

struct Verdict {
    bool        approved = false;
    std::string summary;     // the full line
    std::string issues;      // comma-separated (when rejected)
};

inline Verdict parse_verdict(std::string_view text) {
    Verdict v;
    v.summary = std::string(text);
    auto upper = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::toupper((unsigned char)c));
        return s;
    };
    auto hay = upper(std::string(text));
    auto pos = hay.find("VERDICT:");
    if (pos == std::string::npos) {
        v.approved = true;
        return v;
    }
    auto rest = std::string(text.substr(pos + 8));
    while (!rest.empty() && std::isspace((unsigned char)rest.front())) rest.erase(rest.begin());
    auto ru = upper(rest);
    if (ru.rfind("APPROVED", 0) == 0) {
        v.approved = true;
        return v;
    }
    if (ru.rfind("REJECTED", 0) == 0) {
        v.approved = false;
        auto after = rest;
        if (after.size() >= 8) after = after.substr(8);
        if (!after.empty() && after.front() == ':') after.erase(after.begin());
        while (!after.empty() && std::isspace((unsigned char)after.front())) after.erase(after.begin());
        v.issues = after;
        return v;
    }
    v.approved = true;
    return v;
}
