#pragma once

#include <expected>
#include <string>

namespace monkeybyte {

// Synchronous HTTP(S) GET for the web_fetch agent tool (design §6), built
// on OmniNet's HttpClient (the project's HTTP stack — no separate libcurl
// usage here).
class WebFetch {
public:
    struct Result {
        long status = 0;
        std::string body;
        std::string content_type; // lowercase, parameters stripped
    };

    static constexpr std::size_t kMaxBody = 2 * 1024 * 1024; // 2 MB

    // Follows redirects, 10 s total timeout, body capped at kMaxBody.
    // Sends User-Agent "daedalus-monkeybyte/<version>". Only http:// and
    // https:// URLs are accepted; non-2xx statuses are errors.
    static std::expected<Result, std::string> get(const std::string& url);
};

} // namespace monkeybyte
