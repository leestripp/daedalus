#include "monkeybyte/backend/ChimpFlowClient.h"

#include "monkeybyte/backend/Wav.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string_view>

namespace monkeybyte {

namespace {

using Json = nlohmann::json;

std::string trim(std::string_view value) {
    const auto whitespace = [](unsigned char c) { return std::isspace(c) != 0; };
    const auto first = std::find_if_not(value.begin(), value.end(), whitespace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), whitespace).base();
    if (first >= last) return {};
    return std::string(first, last);
}

std::string lower(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string body_text(const HttpResponse& response) {
    if (response.body.empty()) return {};
    return std::string(reinterpret_cast<const char*>(response.body.data()),
                       response.body.size());
}

[[noreturn]] void throw_response(const std::string& operation,
                                 const HttpResponse& response) {
    std::string detail;
    try {
        const auto json = Json::parse(body_text(response));
        if (json.contains("error") && json["error"].is_string())
            detail = json["error"].get<std::string>();
    } catch (...) {
        // The status-specific fallback below is more useful than raw HTML/bytes.
    }
    if (detail.empty()) {
        if (response.status == 401) detail = "invalid or missing API key";
        else if (response.status == 403) detail = "API key lacks the required scope";
        else if (response.status == 502) detail = "a ChimpFlow engine is unavailable";
        else if (response.status == 503) detail = "ChimpFlow catalog is unavailable";
        else detail = "HTTP " + std::to_string(response.status);
    }
    throw ChimpFlowError(response.status,
                         "ChimpFlow " + operation + " failed: " + detail);
}

void require_success(const std::string& operation, const HttpResponse& response) {
    if (response.status < 200 || response.status >= 300)
        throw_response(operation, response);
}

Json json_response(const std::string& operation, const HttpResponse& response) {
    require_success(operation, response);
    try {
        return Json::parse(body_text(response));
    } catch (const std::exception& e) {
        throw ChimpFlowError(response.status,
                             "ChimpFlow " + operation +
                                 " returned invalid JSON: " + e.what());
    }
}

std::vector<std::string> parse_ids(const std::string& operation,
                                   const HttpResponse& response) {
    const auto json = json_response(operation, response);
    if (!json.contains("data") || !json["data"].is_array())
        throw ChimpFlowError(response.status,
                             "ChimpFlow " + operation + " response has no data list");
    std::vector<std::string> ids;
    for (const auto& item : json["data"]) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string())
            throw ChimpFlowError(response.status,
                                 "ChimpFlow " + operation + " entry has no string id");
        ids.push_back(item["id"].get<std::string>());
    }
    return ids;
}

size_t append_body(char* data, size_t size, size_t count, void* userdata) {
    const std::size_t bytes = size * count;
    auto& body = *static_cast<std::vector<std::uint8_t>*>(userdata);
    const auto* first = reinterpret_cast<const std::uint8_t*>(data);
    body.insert(body.end(), first, first + static_cast<std::ptrdiff_t>(bytes));
    return bytes;
}

size_t append_header(char* data, size_t size, size_t count, void* userdata) {
    const std::size_t bytes = size * count;
    const std::string_view line(data, bytes);
    if (line.size() >= 13 && lower(line.substr(0, 12)) == "content-type") {
        const auto colon = line.find(':');
        if (colon != std::string_view::npos)
            *static_cast<std::string*>(userdata) = trim(line.substr(colon + 1));
    }
    return bytes;
}

int check_cancelled(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto& token = *static_cast<std::stop_token*>(userdata);
    return token.stop_requested() ? 1 : 0;
}

HttpResponse curl_transport(const HttpRequest& request, std::stop_token token) {
    static std::once_flag init_once;
    std::call_once(init_once, [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw std::runtime_error("curl_global_init failed");
    });

    using Curl = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    Curl curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    curl_slist* raw_headers = nullptr;
    auto headers = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>(
        nullptr, &curl_slist_free_all);
    for (const auto& [name, value] : request.headers) {
        const std::string line = name + ": " + value;
        raw_headers = curl_slist_append(raw_headers, line.c_str());
        if (!raw_headers) throw std::bad_alloc();
    }
    headers.reset(raw_headers);
    if (!request.content_type.empty()) {
        const std::string line = "Content-Type: " + request.content_type;
        raw_headers = curl_slist_append(headers.get(), line.c_str());
        if (!raw_headers) throw std::bad_alloc();
        headers.release();
        headers.reset(raw_headers);
    }

    HttpResponse response;
    char error_buffer[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl.get(), CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT,
                     static_cast<long>(request.connect_timeout_seconds));
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT,
                     static_cast<long>(request.timeout_seconds));
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &append_body);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, &append_header);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &response.content_type);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, &check_cancelled);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &token);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    if (request.method == "POST") {
        curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(request.body.size()));
    } else if (request.method != "GET") {
        curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, request.method.c_str());
    }

    const auto result = curl_easy_perform(curl.get());
    if (result != CURLE_OK) {
        if (token.stop_requested()) throw ChimpFlowError(0, "ChimpFlow request cancelled");
        const std::string detail = error_buffer[0] != '\0'
                                       ? error_buffer
                                       : curl_easy_strerror(result);
        throw ChimpFlowError(0, "ChimpFlow connection failed: " + detail);
    }
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.status);
    return response;
}

} // namespace

ChimpFlowError::ChimpFlowError(long status, const std::string& message)
    : std::runtime_error(message), status_(status) {}

std::string chimpflow_base_url(const ChimpFlowSettings& settings) {
    if (settings.port < 1 || settings.port > 65535)
        throw ChimpFlowError(0, "ChimpFlow port must be between 1 and 65535");

    std::string url = trim(settings.server_url);
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (!url.starts_with("http://"))
        throw ChimpFlowError(0, "ChimpFlow requires a loopback http:// server URL");

    const std::string authority = url.substr(7);
    if (authority.empty() || authority.find_first_of("/?#@") != std::string::npos)
        throw ChimpFlowError(0, "ChimpFlow server URL must not contain a path or credentials");

    const bool ipv6_loopback = authority == "[::1]";
    const bool named_loopback = lower(authority) == "localhost" || authority == "127.0.0.1";
    if (!ipv6_loopback && !named_loopback) {
        if (authority.find(':') != std::string::npos)
            throw ChimpFlowError(0, "Put the ChimpFlow port in its separate Port field");
        throw ChimpFlowError(0, "ChimpFlow is localhost-only (localhost, 127.0.0.1, or [::1])");
    }
    return "http://" + authority + ":" + std::to_string(settings.port);
}

std::string resolved_chimpflow_api_key(const ChimpFlowSettings& settings) {
    if (!settings.api_key.empty()) return settings.api_key;
    if (const char* from_env = std::getenv("CHIMPFLOW_KEY"))
        return from_env;
    return {};
}

ChimpFlowClient::ChimpFlowClient(HttpTransport transport)
    : transport_(transport ? std::move(transport) : HttpTransport(&curl_transport)) {}

HttpResponse ChimpFlowClient::exchange(
    const ChimpFlowSettings& settings, const std::string& path, bool authenticated,
    std::string method, std::string content_type, std::string body,
    int timeout_seconds, std::stop_token token) const {
    HttpRequest request;
    request.method = std::move(method);
    request.url = chimpflow_base_url(settings) + path;
    request.content_type = std::move(content_type);
    request.body = std::move(body);
    request.timeout_seconds = timeout_seconds;
    if (authenticated) {
        const std::string key = resolved_chimpflow_api_key(settings);
        if (key.empty())
            throw ChimpFlowError(0,
                                 "ChimpFlow API key is required; enter a scoped key or set CHIMPFLOW_KEY");
        request.headers.emplace_back("Authorization", "Bearer " + key);
    }
    return transport_(request, token);
}

void ChimpFlowClient::health(const ChimpFlowSettings& settings,
                             std::stop_token token) const {
    require_success("health check",
                    exchange(settings, "/healthz", false, "GET", {}, {}, 5, token));
}

std::vector<std::string> ChimpFlowClient::list_models(
    const ChimpFlowSettings& settings, std::stop_token token) const {
    return parse_ids("model discovery",
                     exchange(settings, "/v1/models", true, "GET", {}, {}, 10, token));
}

std::vector<std::string> ChimpFlowClient::list_voices(
    const ChimpFlowSettings& settings, std::stop_token token) const {
    return parse_ids("voice discovery",
                     exchange(settings, "/v1/voices", true, "GET", {}, {}, 10, token));
}

std::vector<std::uint8_t> ChimpFlowClient::synthesize(
    const ChimpFlowSettings& settings, const std::string& text,
    const std::string& voice, std::stop_token token) const {
    if (text.empty()) throw ChimpFlowError(0, "Cannot synthesize empty text");
    if (voice.empty()) throw ChimpFlowError(0, "Select a ChimpFlow voice first");
    if (settings.tts_model.empty()) throw ChimpFlowError(0, "Select a ChimpFlow TTS model");
    const Json request = {{"model", settings.tts_model},
                          {"input", text},
                          {"voice", voice},
                          {"response_format", "wav"}};
    auto response = exchange(settings, "/v1/audio/speech", true, "POST",
                             "application/json", request.dump(), 90, token);
    require_success("speech synthesis", response);
    return std::move(response.body);
}

std::string ChimpFlowClient::transcribe(const ChimpFlowSettings& settings,
                                        const std::vector<std::uint8_t>& wav,
                                        std::stop_token token) const {
    if (wav.empty()) throw ChimpFlowError(0, "No recorded audio to transcribe");
    if (wav.size() > 10 * 1024 * 1024)
        throw ChimpFlowError(0, "Recording exceeds ChimpFlow's 10 MB transcription limit");
    constexpr std::string_view boundary = "----MonkeyByteChimpFlowBoundary";
    auto response = exchange(
        settings, "/v1/audio/transcriptions", true, "POST",
        "multipart/form-data; boundary=" + std::string(boundary),
        make_stt_multipart(boundary, settings.stt_model, wav), 60, token);
    const auto json = json_response("transcription", response);
    if (!json.contains("text") || !json["text"].is_string())
        throw ChimpFlowError(response.status, "ChimpFlow transcription response has no text");
    return trim(json["text"].get<std::string>());
}

} // namespace monkeybyte
