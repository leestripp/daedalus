#pragma once

#include <expected>
#include <map>
#include <string>

namespace monkeybyte {

// Thin synchronous libcurl wrapper for the /api/* contract (design §10).
class ApiClient {
public:
    struct Response {
        long status = 0;
        std::string body;
    };

    explicit ApiClient(std::string base_url); // e.g. "http://127.0.0.1:8080"

    std::expected<Response, std::string> get(const std::string& path) const;
    std::expected<Response, std::string> post(const std::string& path,
                                              const std::string& json_body) const;
    std::expected<Response, std::string> put(const std::string& path,
                                             const std::string& json_body) const;
    std::expected<Response, std::string> del(const std::string& path) const;

    const std::string& base_url() const { return base_url_; }

private:
    std::expected<Response, std::string> request(const std::string& method,
                                                 const std::string& path,
                                                 const std::string* body) const;
    std::string base_url_;
};

} // namespace monkeybyte
