#include "core/utils.h"
#include "core/Utf8Utils.h"
#include "core/Logger.h"  // §Phase 5.1: LOG_DEBUG / LOG_WARNING for diagnostic output
#include <iostream>
#include <sstream>
#include <iomanip>
#include <set>
#include <string>
#include <vector>
#include <limits>
#include <algorithm>
#include <cstring>
#include <OmniNet.h>
#include <http/HttpClient.h>
#include <http/HttpResponse.h>


// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR calls via the
// existing Logger. Same formatted text routed through the Logger
// instead of writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _daedalus_msg;                   \
        _daedalus_msg << expr;                               \
        LOG_##level(_daedalus_msg.str());                    \
    } while (0)
// Base64 encoding table
static const char* base64_chars = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

// Base64 decode table
static const int base64_decode_table[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
    -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

// ============================================================================
// UTF-8 Display Width Helper
// ============================================================================

// Calculate the display width of a UTF-8 string for terminal output
// - ASCII characters (0x00-0x7F): width 1
// - CJK characters (Chinese, Japanese, Korean): width 2
// - Emoji and other 4-byte sequences: width 2
// - Most other Unicode: width 1
static size_t utf8_display_width(const std::string& str) {
    size_t width = 0;
    size_t pos = 0;
    const size_t len = str.size();
    
    while (pos < len) {
        uint8_t byte = static_cast<uint8_t>(str[pos]);
        
        if ((byte & 0x80) == 0) {
            // ASCII (0x00-0x7F) - width 1
            width += 1;
            pos += 1;
        } else if ((byte & 0xE0) == 0xC0) {
            // 2-byte sequence (0xC0-0xDF) - width 1
            width += 1;
            pos += 2;
        } else if ((byte & 0xF0) == 0xE0 && pos + 2 < len) {
            // 3-byte sequence (0xE0-0xEF)
            // Check for CJK range: U+3000-U+9FFF, U+FF00-U+FFEF, U+2E80-U+2EFF
            // For simplicity, treat most 3-byte as width 2 (covers CJK)
            uint32_t codepoint = ((byte & 0x0F) << 12) |
                                 ((static_cast<uint8_t>(str[pos + 1]) & 0x3F) << 6) |
                                 (static_cast<uint8_t>(str[pos + 2]) & 0x3F);
            
            // CJK Unified Ideographs and related ranges
            if ((codepoint >= 0x3000 && codepoint <= 0x9FFF) ||
                (codepoint >= 0xFF00 && codepoint <= 0xFFEF) ||
                (codepoint >= 0x2E80 && codepoint <= 0x2EFF) ||
                (codepoint >= 0xAC00 && codepoint <= 0xD7AF)) {  // Korean Hangul syllables
                width += 2;
            } else {
                width += 1;
            }
            pos += 3;
        } else if ((byte & 0xF0) == 0xE0) {
            // §fix (2026-07-18): truncated 3-byte sequence at the end
            // of the string — treat as width 1 instead of reading
            // str[pos+1]/str[pos+2] out of bounds.
            width += 1;
            pos += 1;
        } else if ((byte & 0xF8) == 0xF0) {
            // 4-byte sequence (0xF0-0xF7) - typically emoji, width 2
            width += 2;
            pos += 4;
        } else {
            // Invalid or continuation - treat as width 1
            width += 1;
            pos += 1;
        }
    }
    
    return width;
}

// Truncate string by display width, adding "..." if truncated
static std::string utf8_truncate_by_width(const std::string& str, size_t max_width) {
    if (utf8_display_width(str) <= max_width) {
        return str;
    }
    
    size_t width = 0;
    size_t pos = 0;
    const size_t len = str.size();
    
    while (pos < len) {
        uint8_t byte = static_cast<uint8_t>(str[pos]);
        size_t char_width;
        size_t char_len;
        
        if ((byte & 0x80) == 0) {
            char_width = 1;
            char_len = 1;
        } else if ((byte & 0xE0) == 0xC0) {
            char_width = 1;
            char_len = 2;
        } else if ((byte & 0xF0) == 0xE0 && pos + 2 < len) {
            uint32_t codepoint = ((byte & 0x0F) << 12) |
                                 ((static_cast<uint8_t>(str[pos + 1]) & 0x3F) << 6) |
                                 (static_cast<uint8_t>(str[pos + 2]) & 0x3F);
            if ((codepoint >= 0x3000 && codepoint <= 0x9FFF) ||
                (codepoint >= 0xFF00 && codepoint <= 0xFFEF) ||
                (codepoint >= 0x2E80 && codepoint <= 0x2EFF) ||
                (codepoint >= 0xAC00 && codepoint <= 0xD7AF)) {
                char_width = 2;
            } else {
                char_width = 1;
            }
            char_len = 3;
        } else if ((byte & 0xF0) == 0xE0) {
            // §fix (2026-07-18): truncated 3-byte sequence at the end
            // of the string — treat as width 1 instead of reading
            // str[pos+1]/str[pos+2] out of bounds.
            char_width = 1;
            char_len = 1;
        } else if ((byte & 0xF8) == 0xF0) {
            char_width = 2;
            char_len = 4;
        } else {
            char_width = 1;
            char_len = 1;
        }
        
        if (width + char_width > max_width - 3) {  // Leave room for "..."
            break;
        }
        
        width += char_width;
        pos += char_len;
    }
    
    return str.substr(0, pos) + "...";
}

std::string base64_encode(const std::string& input) {
    return base64_encode(reinterpret_cast<const unsigned char*>(input.data()), input.size());
}

std::string base64_encode(const unsigned char* data, size_t size) {
    std::string result;
    result.reserve(((size + 2) / 3) * 4);
    
    for (size_t i = 0; i < size; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < size) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < size) n |= static_cast<uint32_t>(data[i + 2]);
        
        result.push_back(base64_chars[(n >> 18) & 0x3F]);
        result.push_back(base64_chars[(n >> 12) & 0x3F]);
        
        if (i + 1 < size) {
            result.push_back(base64_chars[(n >> 6) & 0x3F]);
        } else {
            result.push_back('=');
        }
        
        if (i + 2 < size) {
            result.push_back(base64_chars[n & 0x3F]);
        } else {
            result.push_back('=');
        }
    }
    
    return result;
}

std::string base64_decode(const std::string& input) {
    std::vector<unsigned char> decoded = base64_decode_binary(input);
    return std::string(decoded.begin(), decoded.end());
}

std::vector<unsigned char> base64_decode_binary(const std::string& input) {
    std::vector<unsigned char> result;
    
    // Remove padding characters
    // §fix (2026-07-18): size()-2 underflows when input.size() == 1.
    size_t pad_count = 0;
    if (!input.empty() && input[input.size() - 1] == '=') pad_count++;
    if (input.size() >= 2 && input[input.size() - 2] == '=') pad_count++;
    
    size_t len = input.size() - pad_count;
    result.reserve((len * 3) / 4);
    
    for (size_t i = 0; i < len; i += 4) {
        // §fix (2026-07-18): bounds-check every character — a truncated
        // input (length not a multiple of 4) used to read past the end.
        int n0 = base64_decode_table[static_cast<unsigned char>(input[i])];
        int n1 = (i + 1 < len) ? base64_decode_table[static_cast<unsigned char>(input[i + 1])] : 0;
        int n2 = (i + 2 < len) ? base64_decode_table[static_cast<unsigned char>(input[i + 2])] : 0;
        int n3 = (i + 3 < len) ? base64_decode_table[static_cast<unsigned char>(input[i + 3])] : 0;
        
        uint32_t n = (static_cast<uint32_t>(n0) << 18) |
                     (static_cast<uint32_t>(n1) << 12) |
                     (static_cast<uint32_t>(n2) << 6)  |
                     static_cast<uint32_t>(n3);
        
        result.push_back(static_cast<unsigned char>((n >> 16) & 0xFF));
        if (i + 2 < len) result.push_back(static_cast<unsigned char>((n >> 8) & 0xFF));
        if (i + 3 < len) result.push_back(static_cast<unsigned char>(n & 0xFF));
    }
    
    return result;
}

// Supported image extensions
static const std::set<std::string> supported_image_extensions = {
    "jpg", "jpeg", "png", "gif", "webp", "bmp"
};

std::string get_image_extension(const std::string& url_or_path) {
    // Find the last dot
    size_t dot_pos = url_or_path.rfind('.');
    if (dot_pos == std::string::npos || dot_pos == url_or_path.size() - 1) {
        return "";
    }
    
    // Extract extension and convert to lowercase
    std::string ext = url_or_path.substr(dot_pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    return ext;
}

bool is_valid_image_url(const std::string& url) {
    if (url.empty()) {
        return false;
    }
    
    // Check for http or https scheme
    bool has_valid_scheme = (url.substr(0, 7) == "http://" || 
                             url.substr(0, 8) == "https://");
    if (!has_valid_scheme) {
        // Also check for data URLs (base64 encoded images)
        if (url.substr(0, 5) == "data:") {
            return true;
        }
        return false;
    }
    
    // Check for valid image extension
    std::string ext = get_image_extension(url);
    if (ext.empty()) {
        return false;
    }
    
    return supported_image_extensions.count(ext) > 0;
}

std::string download_image_as_base64(const std::string& url) {
    if (url.empty()) {
        LOG_DEBUG("[download_image] URL is empty");
        return "";
    }

    // Check if it's already a data URL
    if (url.substr(0, 5) == "data:") {
        // Already base64 encoded, just return as-is
        return url;
    }

    // §NC-5 (2026-07-16): migrated from HttpSession to OmniNet.
    // Sync GET via OmniNet::HTTP::Client::send_blocking. Content-Type
    // is pulled from the response headers map.
    LOG_DEBUG("[download_image] Fetching URL: " + url);

    OmniNet::HTTP::ClientRequest req;
    req.url(url);
    req.method("GET");
    req.header("User-Agent", "daedalus/1.0");

    OmniNet::HTTP::ClientConfig cfg;
    cfg.connect_timeout = std::chrono::seconds(10);
    cfg.request_timeout = std::chrono::seconds(30);
    cfg.follow_redirects = true;

    auto result = OmniNet::HTTP::Client::get_instance().send_blocking(req, cfg);

    if (!result.has_value()) {
        LOG_WARNING("[download_image] Request failed for URL: " + url);
        return "";
    }
    const auto& resp = result.value();
    const int code = resp.statusCode();
    if (code < 200 || code >= 300) {
        LOG_WARNING("[download_image] HTTP error: response code " +
                    std::to_string(code) + " for URL: " + url);
        return "";
    }
    const std::string body = resp.bodyAsString();
    if (body.empty()) {
        LOG_WARNING("[download_image] No data received from URL: " + url);
        return "";
    }

    LOG_DEBUG("[download_image] HTTP response code: " + std::to_string(code));
    LOG_DEBUG("[download_image] Data size received: " +
              std::to_string(body.size()) + " bytes");

    // Detect MIME type from URL extension (or use Content-Type from response)
    std::string ext = get_image_extension(url);
    std::string mime_type;
    if (ext == "png") mime_type = "image/png";
    else if (ext == "gif") mime_type = "image/gif";
    else if (ext == "webp") mime_type = "image/webp";
    else if (ext == "bmp") mime_type = "image/bmp";
    else {
        // Use server-provided Content-Type when available; strip any
        // parameters (e.g. "; charset=utf-8") so the data URL stays valid.
        const auto ctype = resp.header("Content-Type");
        if (!ctype.empty()) {
            const auto semi = ctype.find(';');
            mime_type = (semi == std::string::npos) ? ctype
                                                   : ctype.substr(0, semi);
            // Trim trailing whitespace.
            while (!mime_type.empty() &&
                   (mime_type.back() == ' ' || mime_type.back() == '\t'))
                mime_type.pop_back();
        } else {
            mime_type = "image/jpeg"; // Default to JPEG
        }
    }

    // Encode to base64
    std::string base64_data = base64_encode(
        reinterpret_cast<const unsigned char*>(body.data()),
        body.size()
    );

    // Return as data URL
    return "data:" + mime_type + ";base64," + base64_data;
}

// §Phase 5: linenoise helpers removed from the library. The library
// has no concept of where user input comes from — the host owns that
// entirely (see Agent::submit_user_message).
// Hosts that want line-editing + history can bring their own (e.g. drop
// a copy of linenoise into their host's own source tree). The CLI demo
// uses std::getline as the minimal example.

std::string trim(const std::string& str) {
    size_t start = 0;
    while (start < str.length() && (str[start] == ' ' || str[start] == '\t' || str[start] == '\n' || str[start] == '\r')) {
        start++;
    }
    
    size_t end = str.length();
    while (end > start && (str[end - 1] == ' ' || str[end - 1] == '\t' || str[end - 1] == '\n' || str[end - 1] == '\r')) {
        end--;
    }
    
    return str.substr(start, end - start);
}

/**
 * @brief Prints a tool result header with left-bordered format (no right border).
 * @param tool_name The name of the tool being executed.
 * @param info_lines Vector of info lines to display (e.g., "Query: search", "Files: 5")
 * @param width The width of the bordered box (default 62 characters).
 */
std::string format_tool_header(const std::string& tool_name, const std::vector<std::string>& info_lines, int width) {
    std::ostringstream out;
    // Top border with cap on LEFT only (┌)
    out << "\n" << "\u250C";
    for (int i = 1; i < width; i++) out << "\u2500";
    out << "\n";

    // Tool name line with wrench emoji (🔧)
    out << "\u2502 \xF0\x9F\x94\xA7 TOOL: " << tool_name;

    // Calculate padding using display width for proper UTF-8 alignment
    size_t tool_display_width = utf8_display_width(tool_name);
    int tool_line_len = 9 + tool_display_width; // " TOOL: " = 7 chars + emoji
    int padding = width - 4 - tool_line_len;
    for (int i = 0; i < padding && padding > 0; i++) out << " ";
    out << "\n";

    // Info lines
    for (const auto& info : info_lines) {
        out << "\u2502    " << info;
        int info_padding = width - 4 - 4 - static_cast<int>(utf8_display_width(info));
        for (int i = 0; i < info_padding && info_padding > 0; i++) out << " ";
        out << "\n";
    }

    // Bottom border with cap on LEFT only (└)
    out << "\u2514";
    for (int i = 1; i < width; i++) out << "\u2500";
    out << "\n";
    return out.str();
}

/**
 * @brief Formats a separator line for tool output.
 * §Phase 5.1: was `print_tool_separator` writing to std::cout.
 */
std::string format_tool_separator(int width) {
    std::ostringstream out;
    out << "\n";
    for (int i = 0; i < width; i++) out << "\u2500"; // ─
    out << "\n\n";
    return out.str();
}

/**
 * @brief Prints tool header BEFORE execution starts.
 * @param tool_name The name of the tool being executed.
 * @param action_description A brief description of the action being performed.
 * @param width The width of the bordered box (default 62 characters).
 */
std::string format_tool_header_before(const std::string& tool_name, const std::string& action_description, const json& args, int width) {
    std::ostringstream out;
    // Top border with cap on LEFT only (┌)
    out << "\n" << "\u250C";
    for (int i = 1; i < width; i++) out << "\u2500";
    out << "\n";
    
    // Tool name line with wrench emoji (🔧)
    out << "\u2502 \xF0\x9F\x94\xA7 TOOL CALL: " << tool_name;
    
    // Calculate padding using display width for proper UTF-8 alignment
    size_t tool_display_width = utf8_display_width(tool_name);
    int tool_line_len = 12 + tool_display_width; // " TOOL CALL: " = 12 chars + emoji
    int padding = width - 4 - tool_line_len;
    for (int i = 0; i < padding && padding > 0; i++) out << " ";
    out << "\n";
    
    // Action description line if provided
    if (!action_description.empty()) {
        out << "\u2502 \xF0\x9F\x93\x9D Action: " << action_description;
        size_t desc_width = utf8_display_width(action_description);
        int desc_padding = width - 4 - 11 - static_cast<int>(desc_width); // " 📝 Action: " = 11 chars + emoji
        for (int i = 0; i < desc_padding && desc_padding > 0; i++) out << " ";
        out << "\n";
    }
    
    // Display all arguments if provided
    if (!args.is_null() && args.is_object() && !args.empty()) {
        for (auto& [key, val] : args.items()) {
            std::string val_str;
            if (val.is_string()) {
                val_str = val.get<std::string>();
            } else if (val.is_number()) {
                val_str = std::to_string(val.get<double>());
            } else if (val.is_boolean()) {
                val_str = val.get<bool>() ? "true" : "false";
            } else if (val.is_array() || val.is_object()) {
                val_str = "[" + std::to_string(val.size()) + " items]";
            } else {
                val_str = "[unknown type]";
            }
            
            // Replace newlines and carriage returns with spaces to preserve box layout
            for (char& c : val_str) {
                if (c == '\n' || c == '\r') {
                    c = ' ';
                }
            }
            
            if (val_str.length() > 50) {
                val_str = val_str.substr(0, 47) + "...";
            }
            
            out << "\u2502    " << key << ": " << val_str;
            size_t line_len = 6 + utf8_display_width(key) + 2 + utf8_display_width(val_str);
            int arg_padding = width - 4 - line_len;
            for (int i = 0; i < arg_padding && arg_padding > 0; i++) out << " ";
            out << "\n";
        }
    }
    
    // Bottom border with cap on LEFT only (└)
    out << "\u2514";
    for (int i = 1; i < width; i++) out << "\u2500";
    out << "\n";
    return out.str();
}

/**
 * @brief Helper to truncate long strings for display.
 * 
 * Uses UTF-8 aware truncation to prevent splitting multi-byte characters.
 * Falls back to simple truncation if the string contains invalid UTF-8
 * (which preserves original behavior for non-UTF-8 content).
 */
std::string truncate_for_display(const std::string& str, size_t max_len) {
    if (str.length() <= max_len) return str;
    if (max_len < 3) return "";  // Can't fit "...", return empty
    
    // Try UTF-8 aware truncation first using display width
    if (is_valid_utf8(str)) {
        return utf8_truncate_by_width(str, max_len);
    }
    
    // Fallback for non-UTF-8 content (preserves original behavior)
    return str.substr(0, max_len - 3) + "...";
}

/**
 * @brief Extract unique files from grep results.
 */
std::set<std::string> extract_files_from_results(const json& results) {
    std::set<std::string> files;
    if (results.is_array()) {
        for (const auto& r : results) {
            if (r.is_string()) {
                std::string line = r.get<std::string>();
                size_t colon_pos = line.find(':');
                if (colon_pos != std::string::npos) {
                    files.insert(line.substr(0, colon_pos));
                }
            }
        }
    }
    return files;
}

/**
 * @brief Prints a formatted summary of tool execution results (UNIFIED for ALL tools).
 * @param result The JSON response from the tool handler.
 */
std::string format_tool_result(const json& result) {
    std::ostringstream out;

    std::string status = result.value("status", "unknown");
    std::string message = result.value("message", "");
    std::string tool_name = result.value("tool_name", "Tool");
    
    // Build info lines based on tool type
    std::vector<std::string> info_lines;
    
    // ==== BASH COMMAND ====
    if (result.contains("output") && result.contains("command")) {
        std::string command = result.value("command", "");
        int exit_code = result.value("exit_code", -1);
        
        info_lines.push_back("Command: " + truncate_for_display(command, 45));
        // Exit code -1 is not a failure - it can occur due to fork() usage in build tools like cmake
        if (exit_code == 0) {
            info_lines.push_back("\u2705 Exit: 0");
        } else if (exit_code == -1) {
            info_lines.push_back("\u26A0 Exit: -1 (fork/exec signal)");
        } else {
            info_lines.push_back("\u274C Exit: " + std::to_string(exit_code));
        }
        
        // Header already printed by print_tool_header_before()
        
        // Show output
        std::string output = result.value("output", "");
        if (!output.empty()) {
            std::istringstream output_stream(output);
            std::string line;
            std::vector<std::string> all_lines;
            while (std::getline(output_stream, line)) {
                all_lines.push_back(line);
            }
            
            const int max_lines = 30;
            if (all_lines.size() <= static_cast<size_t>(max_lines)) {
                for (const auto& l : all_lines) {
                    out << "  " << l << "\n";
                }
            } else {
                // Head + tail display
                for (int i = 0; i < 15 && i < static_cast<int>(all_lines.size()); i++) {
                    out << "  " << all_lines[i] << "\n";
                }
                int skipped = all_lines.size() - 30;
                out << "  \x1b[33m... (" << skipped << " lines skipped, "
                          << output.length() << " bytes) ...\x1b[0m\n";
                for (int i = all_lines.size() - 15; i < static_cast<int>(all_lines.size()); i++) {
                    if (i >= 0) out << "  " << all_lines[i] << "\n";
                }
            }
        } else {
            out << "  (no output)\n";
        }
        
        // Separator not needed after output
        return out.str();
    }
    
    // ==== BACKGROUND_PROCESS ====
    if (tool_name == "background_process") {
        std::string command = result.value("command", "");
        std::string process_id = result.value("process_id", "");
        
        if (!command.empty()) {
            info_lines.push_back("Command: " + truncate_for_display(command, 45));
        }
        if (!process_id.empty()) {
            info_lines.push_back("Process ID: " + process_id);
        }
        if (result.contains("pid")) {
            info_lines.push_back("PID: " + std::to_string(result["pid"].get<int>()));
        }
        
        if (!message.empty()) {
            info_lines.push_back(truncate_for_display(message, 45));
        }
        
        // Header already printed by print_tool_header_before()
        
        if (status == "success") {
            out << "  \u2705 Done\n";
        } else if (status == "error") {
            out << "  \u2717 Error\n";
        }
        
        // Separator not needed after output
        return out.str();
    }

// §Phase 11: removed 'if (tool_name == "subagent")' branch — was a
    // pretty-printer for the (now-deleted) SubAgentTool.

    // ==== GREP_SEARCH ====
    if (tool_name == "grep_search" && result.contains("results") && result.contains("query")) {
        std::string query = result.value("query", "");
        int count = result.value("count", 0);
        bool truncated = result.value("truncated", false);
        const auto& results = result["results"];
        auto files = extract_files_from_results(results);
        
        info_lines.push_back("Query: \"" + truncate_for_display(query, 40) + "\"");
        info_lines.push_back("Found: " + std::to_string(count) + " matches in " + 
                           std::to_string(files.size()) + " file(s)" + 
                           (truncated ? " (truncated)" : ""));
        
        // Header already printed by print_tool_header_before()
        
        // Show file list (up to 8)
        int shown = 0;
        for (const auto& file : files) {
            if (shown >= 8) break;
            out << "  " << file << "\n";
            shown++;
        }
        if (files.size() > 8) {
            out << "  \x1b[90m... and " << (files.size() - 8) << " more files\x1b[0m\n";
        }
        
        // Separator not needed after output
        return out.str();
    }
    
    // ==== WIKI_SEARCH / SERPER_SEARCH (Native tools - grep_search also has "results"+"query" but different format) ====
    if (result.contains("results") && result.contains("query") && tool_name != "grep_search") {
        std::string query = result.value("query", "");
        int count = result.value("count", result["results"].size());
        
        info_lines.push_back("Query: \"" + truncate_for_display(query, 40) + "\"");
        info_lines.push_back("Found: " + std::to_string(count) + " result(s)");
        
        // Header already printed by print_tool_header_before()
        
        // Show results
        const auto& results = result["results"];
        if (results.is_array()) {
            int shown = 0;
            for (const auto& r : results) {
                if (shown >= 5) break;
                if (r.is_object() && r.contains("title")) {
                    out << "  \x1b[36m" << r["title"].get<std::string>() << "\x1b[0m\n";
                    shown++;
                }
            }
            if (results.size() > 5) {
                out << "  \x1b[90m... and " << (results.size() - 5) << " more\x1b[0m\n";
            }
        }
        
        // Separator not needed after output
        return out.str();
    }

    // ==== MCP TOOL RESULTS (server + tool + result) ====
    // Check for MCP-style results: has result field, server field, and tool field
    if (result.contains("result") && result.contains("server") && result.contains("tool")) {
        std::string server = result.value("server", "");
        std::string tool = result.value("tool", "");
        
        info_lines.push_back("\u2705 Server: " + server);
        info_lines.push_back("Tool: " + tool);
        
        // Header already printed by print_tool_header_before()
        
        // Pretty print the result JSON
        if (result.contains("result")) {
            const auto& result_content = result["result"];
            
            // Try to parse and pretty-print if it is a JSON string
            if (result_content.is_string()) {
                std::string result_str = result_content.get<std::string>();
                // Try to parse as JSON for pretty printing
                try {
                    json parsed = json::parse(result_str);
                    out << "  \x1b[36m--- Result ---\x1b[0m\n";
                    // Print pretty-printed JSON with indent
                    std::istringstream iss(parsed.dump(2));
                    std::string line;
                    int line_count = 0;
                    const int max_lines = 50;
                    while (std::getline(iss, line) && line_count < max_lines) {
                        out << "  " << line << "\n";
                        line_count++;
                    }
                    if (line_count >= max_lines) {
                        out << "  \x1b[90m... (output truncated)\x1b[0m\n";
                    }
                } catch (...) {
                    // Not JSON, just print as-is
                    out << "  \x1b[36m--- Result ---\x1b[0m\n";
                    out << "  " << truncate_for_display(result_str, 100) << "\n";
                }
            } else if (result_content.is_object() || result_content.is_array()) {
                // Already a JSON object/array
                out << "  \x1b[36m--- Result ---\x1b[0m\n";
                std::istringstream iss(result_content.dump(2));
                std::string line;
                int line_count = 0;
                const int max_lines = 50;
                while (std::getline(iss, line) && line_count < max_lines) {
                    out << "  " << line << "\n";
                    line_count++;
                }
                if (line_count >= max_lines) {
                    out << "  \x1b[90m... (output truncated)\x1b[0m\n";
                }
            }
        }
        
        return out.str();

    }
    
    // ==== FILE OPERATIONS ====
    std::string path = result.value("path", "");
    if (!path.empty()) {
        // Show full path
        info_lines.push_back("File: " + truncate_for_display(path, 45));
        
        // Add line range if available
        if (result.contains("start_line") && result.contains("end_line")) {
            int start = result["start_line"].get<int>();
            int end = result["end_line"].get<int>();
            info_lines.push_back("Lines: " + std::to_string(start) + "-" + std::to_string(end));
        }
        
        // Add content size and line count
        if (result.contains("content")) {
            std::string file_content = result["content"].get<std::string>();
            size_t line_count = std::count(file_content.begin(), file_content.end(), '\n') + 1;
            info_lines.push_back("Size: " + std::to_string(file_content.length()) + " chars, " + std::to_string(line_count) + " lines");
        }
        
        // Header already printed by print_tool_header_before()
        
        // Show patch diff for edit_file_patch
        std::string old_str = result.value("old_str", "");
        std::string new_str = result.value("new_str", "");
        
        if (!old_str.empty() || !new_str.empty()) {
            if (!old_str.empty()) {
                std::istringstream old_stream(old_str);
                std::string line;
                while (std::getline(old_stream, line)) {
                    out << "\x1b[33m- " << line << "\x1b[0m\n";
                }
            }
            if (!new_str.empty()) {
                std::istringstream new_stream(new_str);
                std::string line;
                while (std::getline(new_stream, line)) {
                    out << "\x1b[34m+ " << line << "\x1b[0m\n";
                }
            }
        }
        
        // Show PDF info
        if (result.contains("num_pages")) {
            int num_pages = result["num_pages"].get<int>();
            int pages_extracted = result.value("pages_extracted", 0);
            out << "  PDF: " << num_pages << " page(s)";
            if (pages_extracted > 0) {
                out << " (extracted " << pages_extracted << ")";
            }
            out << "\n";
        }
        
        // Show status
        if (status == "success") {
            out << "  \u2713 Done\n";
        } else if (status == "error") {
            out << "  \u2717 Error: " << message << "\n";
        }
        
        // Separator not needed after output
        return out.str();
    }
    
    // ==== GIT OPERATIONS ====
    if (tool_name.find("git_") == 0) {
        if (result.contains("branch")) {
            info_lines.push_back("Branch: " + result["branch"].get<std::string>());
        }
        if (result.contains("current_branch")) {
            info_lines.push_back("Current: " + result["current_branch"].get<std::string>());
        }
        if (result.contains("modified_files")) {
            const auto& files = result["modified_files"];
            if (files.is_array()) {
                info_lines.push_back("Files: " + std::to_string(files.size()) + " changed");
            }
        }
        
        // Header already printed by print_tool_header_before()
        
        // Show commit message
        if (result.contains("message") && !message.empty()) {
            out << "  " << message << "\n";
        }
        
        // Separator not needed after output
        return out.str();
    }
    
    // ==== LIST_DIRECTORY_TREE ====
    if (result.contains("tree") || result.contains("entries")) {
        std::string dir = result.value("path", result.value("directory", "."));
        int count = 0;
        if (result.contains("entries") && result["entries"].is_array()) {
            count = result["entries"].size();
        } else if (result.contains("count")) {
            count = result["count"].get<int>();
        }
        
        info_lines.push_back("Path: " + truncate_for_display(dir, 45));
        if (count > 0) {
            info_lines.push_back("Entries: " + std::to_string(count));
        }
        
        // Header already printed by print_tool_header_before()
        // Separator not needed after output
        return out.str();
    }
    
    // ==== STOP TOOL ====
    if (tool_name == "stop") {
        // Show summary prominently inside the box
        std::string summary = result.value("summary", "");
        if (!summary.empty()) {
            // Print summary as displayed lines
            out << "  \u2713 Task Complete\n\n";
            std::istringstream summary_stream(summary);
            std::string line;
            bool first_line = true;
            while (std::getline(summary_stream, line)) {
                if (first_line) {
                    out << line << "\n";
                    first_line = false;
                } else {
                    out << "  " << line << "\n";
                }
            }
        }
        // Separator not needed after output
        return out.str();
    }
    
    // ==== USER TOOLS (tell_user, compress_context, stop) ====
    if (tool_name.find("tell_") == 0 ||
        tool_name.find("compress_") == 0 ||
        tool_name.find("stop") == 0) {
        if (!message.empty()) {
            info_lines.push_back(truncate_for_display(message, 50));
        }
        
        // Header already printed by print_tool_header_before()
        
        if (status == "success") {
            out << "  \u2713 Done\n";
        }
        
        // Separator not needed after output
        return out.str();
    }
    
    // ==== FALLBACK: Generic tool with no specific format ====
    // Header already printed by print_tool_header_before()
    // Just show status inline - no second header needed
    if (status == "success") {
        out << "  \u2713 Success\n";
    } else if (status == "error") {
        out << "  \u2717 Error: " << truncate_for_display(message, 40) << "\n";
    } else if (!message.empty()) {
        out << "  " << truncate_for_display(message, 50) << "\n";
    }
    // Note: No format_tool_header() or format_tool_separator() here
    // The first header was already produced by
    // format_tool_header_before().
    return out.str();
}

std::string format_tool_result_for_log(const json& result) {
    std::ostringstream ss;
    ss << "===========================================\n";
    
    std::string status = result.value("status", "unknown");
    std::string message = result.value("message", "");
    std::string tool_name = result.value("tool_name", "Tool");
    
    ss << "Tool: " << tool_name << "\n";
    ss << "Status: " << status << "\n";
    
    if (!message.empty()) {
        ss << "Message: " << message << "\n";
    }
    
    // Tool-specific fields
    if (result.contains("output")) {
        std::string output = result.value("output", "");
        ss << "Output: " << output << "\n";
    }
    
    if (result.contains("exit_code")) {
        ss << "Exit code: " << result.value("exit_code", 0) << "\n";
    }
    
    if (result.contains("command")) {
        ss << "Command: " << result.value("command", "") << "\n";
    }
    
    if (result.contains("query")) {
        ss << "Query: " << result.value("query", "") << "\n";
    }
    
    if (result.contains("count")) {
        ss << "Count: " << result.value("count", 0) << "\n";
    }
    
    if (result.contains("path")) {
        ss << "Path: " << result.value("path", "") << "\n";
    }
    
    if (result.contains("branch")) {
        ss << "Branch: " << result.value("branch", "") << "\n";
    }
    
    if (result.contains("current_branch")) {
        ss << "Current branch: " << result.value("current_branch", "") << "\n";
    }
    
    if (result.contains("modified_files")) {
        ss << "Modified files: " << result["modified_files"].size() << "\n";
    }
    
    if (result.contains("commands_executed")) {
        ss << "Commands executed: " << result["commands_executed"].size() << "\n";
    }
    
    if (result.contains("summary")) {
        ss << "Summary: " << result.value("summary", "") << "\n";
    }
    
    ss << "===========================================\n";
    
    return ss.str();
}
