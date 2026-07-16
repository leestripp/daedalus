/**
 * @file demos/daedalus-cli/acp/src/AcpTransport.cpp
 * @brief ACP Transport implementation using libcurl
 */

#include "acp/AcpClient.h"
#include <curl/curl.h>
#include <cstring>
#include <sstream>
#include <iostream>

namespace {

// Helper class for curl easy handle RAII
class CurlGlobal {
public:
    CurlGlobal() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    ~CurlGlobal() {
        curl_global_cleanup();
    }
};

// Static initialization
static CurlGlobal g_curl_global;

size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    std::string* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), realsize);
    return realsize;
}

size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t len = size * nitems;
    std::string* headers = static_cast<std::string*>(userdata);
    headers->append(buffer, len);
    return len;
}

/// RAII wrapper for CURL* handles — calls curl_easy_cleanup() in dtor.
struct CurlHandle {
    CURL* handle;
    explicit CurlHandle(CURL* h = nullptr) noexcept : handle(h) {}
    ~CurlHandle() { if (handle) curl_easy_cleanup(handle); }
    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
    CurlHandle(CurlHandle&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    CurlHandle& operator=(CurlHandle&& other) noexcept {
        if (this != &other) {
            if (handle) curl_easy_cleanup(handle);
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    operator CURL*() const noexcept { return handle; }
};

/// Build a fresh handle pre-configured with the common options (timeouts,
/// write callback, follow redirects, JSON headers). One per request —
/// simpler than curl_easy_duphandle and matches what is_connected already
/// does. The returned handle is owned by the caller.
CurlHandle make_request_handle(const std::string& url,
                               std::string& response,
                               std::string& response_headers,
                               struct curl_slist* headers) {
    CurlHandle h(curl_easy_init());
    if (!h) return h;
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &response_headers);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 10L);
    return h;
}

/// Perform a request, returning parsed JSON or empty on any failure.
/// Logs a one-line diagnostic on transport error / non-2xx / parse error.
nlohmann::json perform_and_parse(CURL* curl,
                                 const std::string& method_path,
                                 const std::string& response) {
    long http_code = 0;
    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (res != CURLE_OK) {
        std::cerr << "[CurlTransport] " << method_path
                  << " failed: " << curl_easy_strerror(res) << "\n";
        return {};
    }
    if (http_code < 200 || http_code >= 300) {
        std::cerr << "[CurlTransport] " << method_path
                  << " HTTP " << http_code << "\n";
        return {};
    }
    try {
        return nlohmann::json::parse(response);
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "[CurlTransport] " << method_path
                  << " JSON parse error: " << e.what() << "\n";
        return {};
    }
}

} // anonymous namespace

namespace acp {

// ============================================================================
// CurlTransport implementation
// ============================================================================

class CurlTransport : public IAcpTransport {
public:
    CurlTransport(const std::string& base_url)
        : base_url_(base_url)
    {
        // Shared JSON headers; passed by pointer into each request handle.
        // We own this slist and free it on destruction.
        headers_ = curl_slist_append(nullptr, "Content-Type: application/json");
        headers_ = curl_slist_append(headers_,  "Accept: application/json");
    }

    ~CurlTransport() override {
        if (headers_) curl_slist_free_all(headers_);
    }

    nlohmann::json get(const std::string& path) override {
        std::string url      = base_url_ + path;
        std::string response;
        std::string headers;
        auto h = make_request_handle(url, response, headers, headers_);
        if (!h) return {};
        return perform_and_parse(h, "GET " + path, response);
    }

    nlohmann::json post(const std::string& path, const nlohmann::json& body) override {
        std::string url      = base_url_ + path;
        std::string response;
        std::string headers;
        auto h = make_request_handle(url, response, headers, headers_);
        if (!h) return {};

        std::string body_str = body.dump();
        curl_easy_setopt(h, CURLOPT_POST, 1L);
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body_str.c_str());
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_str.size()));
        return perform_and_parse(h, "POST " + path, response);
    }

    void stream_events(const RunId& run_id, IRunEventHandler& handler) override {
        std::cerr << "[CurlTransport] Stream events for run " << run_id << "\n";
        // TODO: Implement SSE streaming with libcurl
        // For now, this is a stub
        (void)handler;
    }

    bool is_connected() const override {
        CurlHandle h(curl_easy_init());
        if (!h) return false;

        std::string url = base_url_ + "/v1/ping";
        std::string response;
        std::string headers;
        curl_easy_setopt(h, CURLOPT_URL, url.c_str());
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers_);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(h, CURLOPT_HEADERDATA, &headers);
        curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(h, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 3L);

        CURLcode res = curl_easy_perform(h);
        long code = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);

        return res == CURLE_OK && code >= 200 && code < 300;
    }

private:
    std::string base_url_;
    struct curl_slist* headers_ = nullptr;
};

// ============================================================================
// Transport factory
// ============================================================================

std::unique_ptr<IAcpTransport> create_curl_transport(const std::string& base_url) {
    return std::make_unique<CurlTransport>(base_url);
}

} // namespace acp