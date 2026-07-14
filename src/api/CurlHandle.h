/**
 * @file CurlHandle.h
 * @brief RAII wrapper for libcurl easy handles
 *
 * Why this exists:
 * - libcurl easy handles have manual init/cleanup (curl_easy_init/curl_easy_cleanup).
 *   Forgetting cleanup = handle leak. Multiple owners = undefined behavior.
 * - libcurl easy handles are NOT safe to use concurrently from multiple threads
 *   (libcurl is thread-safe but each handle is single-threaded). This wrapper
 *   documents that constraint at the type level (move-only, not copyable).
 * - Prepare for Phase 2 CurlMultiSession refactor: we want each subsystem to
 *   own its CURL* via this wrapper for clarity.
 *
 * Thread-safety: Each CurlHandle is single-threaded. Don't share between
 * threads. You may pass it between threads, but only one thread may use
 * it at a time.
 *
 * Usage:
 * @code
 *   CurlHandle curl;
 *   if (!curl) { ... }
 *   curl.set_url("https://api.example.com");
 *   curl.set_post_body(payload);
 *   auto response = curl.perform();
 * @endcode
 */

#pragma once

#include <curl/curl.h>
#include <string>
#include <utility>

namespace daedalus::net {

/**
 * @brief RAII wrapper for CURL* (libcurl easy handle)
 *
 * Move-only. Owns one CURL*. Cleans up on destruction.
 */
class CurlHandle {
public:
    CurlHandle() : handle_(curl_easy_init()) {}

    ~CurlHandle() {
        if (handle_) {
            curl_easy_cleanup(handle_);
        }
    }

    // Move-only (CURL* is single-owner)
    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;

    CurlHandle(CurlHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    CurlHandle& operator=(CurlHandle&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                curl_easy_cleanup(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    // Check if init succeeded
    explicit operator bool() const noexcept { return handle_ != nullptr; }
    bool valid() const noexcept { return handle_ != nullptr; }

    // Raw access (for advanced use; use the helpers below for common ops)
    CURL* get() const noexcept { return handle_; }
    CURL* release() noexcept {
        CURL* h = handle_;
        handle_ = nullptr;
        return h;
    }

    // ========================================================================
    // Common configuration helpers
    // ========================================================================

    CurlHandle& set_url(const std::string& url) {
        curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
        return *this;
    }

    CurlHandle& set_post(bool enable = true) {
        curl_easy_setopt(handle_, CURLOPT_POST, enable ? 1L : 0L);
        return *this;
    }

    CurlHandle& set_post_fields(const std::string& body) {
        curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        return *this;
    }

    CurlHandle& set_timeout(long seconds) {
        curl_easy_setopt(handle_, CURLOPT_TIMEOUT, seconds);
        return *this;
    }

    CurlHandle& set_follow_location(bool enable = true) {
        curl_easy_setopt(handle_, CURLOPT_FOLLOWLOCATION, enable ? 1L : 0L);
        return *this;
    }

    CurlHandle& set_write_callback(size_t (*cb)(void*, size_t, size_t, void*), void* userdata) {
        curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, cb);
        curl_easy_setopt(handle_, CURLOPT_WRITEDATA, userdata);
        return *this;
    }

    CurlHandle& set_header_callback(size_t (*cb)(void*, size_t, size_t, void*), void* userdata) {
        curl_easy_setopt(handle_, CURLOPT_HEADERFUNCTION, cb);
        curl_easy_setopt(handle_, CURLOPT_HEADERDATA, userdata);
        return *this;
    }

    CurlHandle& add_header(const std::string& header, curl_slist*& list) {
        list = curl_slist_append(list, header.c_str());
        return *this;
    }

    CurlHandle& set_headers(curl_slist* list) {
        curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, list);
        return *this;
    }

    CurlHandle& set_bearer_token(const std::string& token) {
        // Note: Authorization header should be set via add_header() since
        // bearer tokens are passed as "Authorization: Bearer <token>".
        // This method is here for API completeness; the cleanest path is:
        //   curl_slist* headers = nullptr;
        //   curl.add_header("Authorization: Bearer " + token, headers);
        //   curl.set_headers(headers);
        (void)token;
        return *this;
    }

    // ========================================================================
    // Perform and info
    // ========================================================================

    /**
     * @brief Execute the request synchronously
     * @return CURLcode result
     */
    CURLcode perform() {
        return curl_easy_perform(handle_);
    }

    /**
     * @brief Get HTTP response code (call after perform)
     */
    long get_response_code() const {
        long code = 0;
        curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &code);
        return code;
    }

    /**
     * @brief Reset the handle for reuse
     *
     * Resets options but keeps the connection pool (HTTP keep-alive).
     * Use this to reuse the same handle for multiple sequential requests.
     */
    CurlHandle& reset() {
        curl_easy_reset(handle_);
        return *this;
    }

private:
    CURL* handle_;
};

} // namespace daedalus::net
