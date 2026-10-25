#pragma once

#include "monkeybyte/backend/SettingsStore.h"

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace monkeybyte {

struct HttpRequest {
    std::string method;
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string content_type;
    std::string body; // bytes; may contain NULs
    int connect_timeout_seconds = 3;
    int timeout_seconds = 60;
};

struct HttpResponse {
    long status = 0;
    std::string content_type;
    std::vector<std::uint8_t> body;
};

using HttpTransport =
    std::function<HttpResponse(const HttpRequest&, std::stop_token)>;

class ChimpFlowError : public std::runtime_error {
public:
    ChimpFlowError(long status, const std::string& message);
    long status() const noexcept { return status_; }

private:
    long status_;
};

// Validates configuration and returns the loopback origin including the port.
// server_url must not contain its own port/path; the Settings UI has a separate
// port field. Remote hosts and HTTPS are deliberately rejected.
std::string chimpflow_base_url(const ChimpFlowSettings& settings);
std::string resolved_chimpflow_api_key(const ChimpFlowSettings& settings);

class ChimpFlowClient {
public:
    explicit ChimpFlowClient(HttpTransport transport = {});

    void health(const ChimpFlowSettings&, std::stop_token = {}) const;
    std::vector<std::string> list_models(const ChimpFlowSettings&,
                                         std::stop_token = {}) const;
    std::vector<std::string> list_voices(const ChimpFlowSettings&,
                                         std::stop_token = {}) const;
    std::vector<std::uint8_t> synthesize(const ChimpFlowSettings&,
                                         const std::string& text,
                                         const std::string& voice,
                                         std::stop_token = {}) const;
    std::string transcribe(const ChimpFlowSettings&,
                           const std::vector<std::uint8_t>& wav,
                           std::stop_token = {}) const;

private:
    HttpResponse exchange(const ChimpFlowSettings&, const std::string& path,
                          bool authenticated, std::string method,
                          std::string content_type, std::string body,
                          int timeout_seconds, std::stop_token) const;

    HttpTransport transport_;
};

} // namespace monkeybyte
