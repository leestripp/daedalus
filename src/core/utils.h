#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

/**
 * @brief Base64 encode a string
 * @param input The string to encode
 * @return Base64 encoded string
 */
std::string base64_encode(const std::string& input);

/**
 * @brief Base64 decode a string
 * @param input The base64 encoded string
 * @return Decoded string
 */
std::string base64_decode(const std::string& input);

/**
 * @brief Base64 encode binary data
 * @param data Pointer to binary data
 * @param size Size of data in bytes
 * @return Base64 encoded string
 */
std::string base64_encode(const unsigned char* data, size_t size);

/**
 * @brief Base64 decode to binary data
 * @param input The base64 encoded string
 * @return Vector of decoded bytes
 */
std::vector<unsigned char> base64_decode_binary(const std::string& input);

/**
 * @brief Validate if a string is a valid image URL
 * @param url The URL to validate
 * @return true if valid image URL
 */
bool is_valid_image_url(const std::string& url);

/**
 * @brief Extract file extension from URL or path
 * @param url_or_path The URL or file path
 * @return Lowercase file extension (without dot)
 */
std::string get_image_extension(const std::string& url_or_path);

/**
 * @brief Download image from URL and return as base64
 * @param url The image URL
 * @return Base64 encoded image data with data URI prefix, or empty string on failure
 */
std::string download_image_as_base64(const std::string& url);

/**
 * @brief Formats a tool result header with bordered box.
 * @param tool_name The name of the tool being executed.
 * @param info_lines Vector of info lines to display.
 * @param width The width of the bordered box (default 62 characters).
 * @return The formatted string. §Phase 5.1: was `print_tool_header`
 *         writing to std::cout — now returns the string so the
 *         host can route it (typically via sink_->on_log).
 */
std::string format_tool_header(const std::string& tool_name,
                                const std::vector<std::string>& info_lines,
                                int width = 62);

/**
 * @brief Formats a separator line for tool output.
 * @param width The width of the separator (default 62 characters).
 */
std::string format_tool_separator(int width = 62);

/**
 * @brief Formats a summary of tool execution results.
 * @param result The JSON response from the tool handler.
 * @return The formatted string. §Phase 5.1: was `print_tool_result`.
 */
std::string format_tool_result(const nlohmann::json& result);

/**
 * @brief Formats a tool result as a string for logging purposes.
 * @param result The JSON response from the tool handler.
 * @return A formatted string suitable for logging.
 */
std::string format_tool_result_for_log(const nlohmann::json& result);

/**
 * @brief Trim whitespace from both ends of a string.
 * @param str The string to trim.
 * @return The trimmed string.
 */
std::string trim(const std::string& str);

/**
 * @brief Truncate a string for display, adding "..." if truncated.
 * Uses UTF-8 aware truncation to prevent splitting multi-byte characters.
 * @param str The string to truncate.
 * @param max_len Maximum length in bytes.
 * @return Truncated string with "..." appended if truncated.
 */
std::string truncate_for_display(const std::string& str, size_t max_len);

/**
 * @brief Formats tool header BEFORE execution starts.
 * §Phase 5.1: was `print_tool_header_before` writing to std::cout.
 */
std::string format_tool_header_before(
    const std::string& tool_name,
    const std::string& action_description,
    const nlohmann::json& args = nlohmann::json::object(),
    int width = 62);


