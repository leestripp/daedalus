/**
 * @file src/acp/src/AcpTransport.cpp
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

} // anonymous namespace

namespace acp {

// ============================================================================
// CurlTransport implementation
// ============================================================================

class CurlTransport : public IAcpTransport {
public:
    CurlTransport(const std::string& base_url)
        : base_url_(base_url)
        , curl_(curl_easy_init())
        , headers_(nullptr)
    {
        if (curl_) {
            // Common options
            curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_callback);
            curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, header_callback);
            curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 30L);
            curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 10L);
            
            // Headers for JSON
            headers_ = curl_slist_append(headers_, "Content-Type: application/json");
            headers_ = curl_slist_append(headers_, "Accept: application/json");
        }
    }
    
    ~CurlTransport() override {
        if (headers_) {
            curl_slist_free_all(headers_);
        }
        if (curl_) {
            curl_easy_cleanup(curl_);
        }
    }
    
    nlohmann::json get(const std::string& path) override {
        std::string response;
        std::string headers;
        long http_code = 0;
        
        CURL* curl = curl_easy_duphandle(curl_);
        if (!curl) return {};
        
        std::string url = base_url_ + path;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers_);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);
        
        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);
        
        if (res != CURLE_OK) {
            std::cerr << "[CurlTransport] GET " << path << " failed: " << curl_easy_strerror(res) << "\n";
            return {};
        }
        
        if (http_code >= 400) {
            std::cerr << "[CurlTransport] GET " << path << " HTTP " << http_code << "\n";
            return {};
        }
        
        try {
            return nlohmann::json::parse(response);
        } catch (const nlohmann::json::parse_error& e) {
            std::cerr << "[CurlTransport] JSON parse error: " << e.what() << "\n";
            return {};
        }
    }
    
    nlohmann::json post(const std::string& path, const nlohmann::json& body) override {
        std::string response;
        std::string headers_str;
        long http_code = 0;
        
        CURL* curl = curl_easy_duphandle(curl_);
        if (!curl) return {};
        
        std::string url = base_url_ + path;
        std::string body_str = body.dump();
        
        // POST data setup (no need to modify shared headers list)
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers_);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body_str.size());
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers_str);
        
        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);
        
        if (res != CURLE_OK) {
            std::cerr << "[CurlTransport] POST " << path << " failed: " << curl_easy_strerror(res) << "\n";
            return {};
        }
        
        if (http_code >= 400) {
            std::cerr << "[CurlTransport] POST " << path << " HTTP " << http_code << "\n";
            return {};
        }
        
        try {
            return nlohmann::json::parse(response);
        } catch (const nlohmann::json::parse_error& e) {
            std::cerr << "[CurlTransport] JSON parse error: " << e.what() << "\n";
            return {};
        }
    }
    
    void stream_events(const RunId& run_id, IRunEventHandler& handler) override {
        std::cerr << "[CurlTransport] Stream events for run " << run_id << "\n";
        // TODO: Implement SSE streaming with libcurl
        // For now, this is a stub
    }
    
    bool is_connected() const override {
        CURL* curl = curl_easy_init();
        if (!curl) return false;
        
        std::string url = base_url_ + "/v1/ping";
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
        
        CURLcode res = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(curl);
        
        return res == CURLE_OK && code < 500;
    }

private:
    std::string base_url_;
    CURL* curl_;
    struct curl_slist* headers_;
};

// ============================================================================
// Transport factory
// ============================================================================

std::unique_ptr<IAcpTransport> create_curl_transport(const std::string& base_url) {
    return std::make_unique<CurlTransport>(base_url);
}

} // namespace acp
