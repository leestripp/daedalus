#include "monkeybyte/http/ApiClient.h"

#include <curl/curl.h>

namespace monkeybyte {

namespace {

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

} // namespace

ApiClient::ApiClient(std::string base_url) : base_url_(std::move(base_url)) {
    while (!base_url_.empty() && base_url_.back() == '/') base_url_.pop_back();
}

std::expected<ApiClient::Response, std::string>
ApiClient::get(const std::string& path) const { return request("GET", path, nullptr); }

std::expected<ApiClient::Response, std::string>
ApiClient::post(const std::string& path, const std::string& json_body) const {
    return request("POST", path, &json_body);
}

std::expected<ApiClient::Response, std::string>
ApiClient::put(const std::string& path, const std::string& json_body) const {
    return request("PUT", path, &json_body);
}

std::expected<ApiClient::Response, std::string>
ApiClient::del(const std::string& path) const { return request("DELETE", path, nullptr); }

std::expected<ApiClient::Response, std::string>
ApiClient::request(const std::string& method, const std::string& path,
                   const std::string* body) const {
    CURL* curl = curl_easy_init();
    if (!curl) return std::unexpected("curl_easy_init failed");

    Response resp;
    curl_easy_setopt(curl, CURLOPT_URL, (base_url_ + path).c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    curl_slist* headers = nullptr;
    if (body) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(body->size()));
    }

    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
        return std::unexpected(std::string("HTTP error: ") + curl_easy_strerror(rc));
    return resp;
}

} // namespace monkeybyte
