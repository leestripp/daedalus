// SPDX-License-Identifier: BSL-1.0
// src/tools/websearch/WebSearchProvider.cpp
//
// §WEB-SEARCH-CORE (2026-10-10): provider implementations + failover chain
// for the unified `web_search` tool. See WebSearchProvider.h for the
// design notes. HTTP via OmniNet::HTTP::Client (same client the legacy
// SerperSearchTool / SearxngSearchTool used); JSON via nlohmann.

#include "tools/websearch/WebSearchProvider.h"

#include "daedalus/paths.h"

#include <OmniNet.h>
#include <http/HttpClient.h>
#include <http/HttpResponse.h>
#include <utils/Utils.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cctype>
#include <cstring>
#include <sstream>
#include <utility>

using nlohmann::json;

namespace daedalus {

namespace {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

struct HttpResp {
    int status = 0;
    std::string body;
};

//! Blocking HTTP request; every failure mode is returned, never thrown.
std::expected<HttpResp, std::string> http_send(
    std::string_view method, const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body) {
    try {
        OmniNet::HTTP::ClientRequest req;
        req.url(url);
        req.method(std::string(method));
        if (!body.empty()) req.body(body);
        for (const auto& [k, v] : headers) req.header(k, v);

        OmniNet::HTTP::ClientConfig cfg;
        cfg.connect_timeout = std::chrono::seconds(10);
        cfg.request_timeout = std::chrono::seconds(30);
        cfg.follow_redirects = true;
        cfg.verify_tls       = true;

        auto result = OmniNet::HTTP::Client::get_instance().send_blocking(req, cfg);
        if (!result.has_value()) {
            return std::unexpected(
                std::string("transport error: ") +
                OmniNet::HTTP::clientErrorName(result.error()));
        }
        return HttpResp{result->statusCode(), result->bodyAsString()};
    } catch (const std::exception& e) {
        return std::unexpected(std::string("request build failed: ") + e.what());
    }
}

//! Extract {title,url,snippet} triples from a JSON array under provider-
//! specific field names. Items missing url or title are skipped.
void append_json_items(const json& arr, std::size_t max_results,
                       const char* f_title, const char* f_url,
                       const char* f_snippet, WebSearchResults& out) {
    if (!arr.is_array()) return;
    for (const auto& item : arr) {
        if (out.size() >= max_results) break;
        WebSearchResult r;
        if (item.contains(f_title) && item[f_title].is_string())
            r.title = item[f_title].get<std::string>();
        if (item.contains(f_url) && item[f_url].is_string())
            r.url = item[f_url].get<std::string>();
        if (item.contains(f_snippet) && item[f_snippet].is_string())
            r.snippet = item[f_snippet].get<std::string>();
        if (!r.url.empty() && !r.title.empty()) out.push_back(std::move(r));
    }
}

//! Parse `body` as JSON and locate the result array at a dotted path
//! ("web.results", "data.web", "results", ...). Throws on bad JSON.
const json* find_json_array(const json& root, std::string_view path) {
    const json* node = &root;
    std::size_t pos = 0;
    while (pos < path.size()) {
        const auto dot = path.find('.', pos);
        const auto key = path.substr(
            pos, dot == std::string_view::npos ? path.size() - pos : dot - pos);
        if (!node->is_object() || !node->contains(std::string(key)))
            return nullptr;
        node = &(*node)[std::string(key)];
        if (dot == std::string_view::npos) break;
        pos = dot + 1;
    }
    return node->is_array() ? node : nullptr;
}

//! Common driver for JSON providers: locate the array, map fields.
WebSearchResults parse_json_results(std::string_view body,
                                    std::size_t max_results,
                                    std::string_view array_path,
                                    const char* f_title, const char* f_url,
                                    const char* f_snippet) {
    WebSearchResults out;
    const json root = json::parse(std::string(body));
    if (const json* arr = find_json_array(root, array_path)) {
        append_json_items(*arr, max_results, f_title, f_url, f_snippet, out);
    }
    return out;
}

//! DuckDuckGo: percent-decode a URL component (uddg targets are encoded).
std::string ddg_url_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            try {
                out += static_cast<char>(
                    std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
                i += 2;
                continue;
            } catch (...) {
            }
        }
        if (s[i] == '+') {
            out += ' ';
            continue;
        }
        out += s[i];
    }
    return out;
}

//! DDG wraps outbound links as //duckduckgo.com/l/?uddg=<encoded>&rut=… —
//! decode the uddg parameter; scheme-relative //… gets an https: prefix.
std::string ddg_resolve_href(std::string_view href) {
    if (const auto pos = href.find("uddg="); pos != std::string_view::npos) {
        auto uddg = href.substr(pos + 5);
        if (const auto amp = uddg.find('&'); amp != std::string_view::npos)
            uddg = uddg.substr(0, amp);
        return ddg_url_decode(uddg);
    }
    if (href.starts_with("//")) return "https:" + std::string(href);
    return std::string(href);
}

//! Extract the href="…" attribute of the anchor tag spanning lt..gt.
std::string ddg_href_of(std::string_view html, std::size_t lt, std::size_t gt) {
    const auto href = html.find("href=", lt);
    if (href == std::string_view::npos || href > gt) return "";
    std::size_t val = href + 5;
    if (val < html.size() && (html[val] == '"' || html[val] == '\'')) {
        const char quote = html[val++];
        const auto end = html.find(quote, val);
        if (end == std::string_view::npos || end > gt) return "";
        return std::string(html.substr(val, end - val));
    }
    const auto end = html.find_first_of(" \t>", val);
    return std::string(html.substr(
        val, end == std::string_view::npos ? gt : end - val));
}

//! Inner HTML between gt+1 and the next occurrence of `closer`.
std::string_view ddg_inner_html(std::string_view html, std::size_t gt,
                                std::string_view closer) {
    const auto end = html.find(closer, gt);
    if (end == std::string_view::npos) return {};
    return html.substr(gt + 1, end - gt - 1);
}

// ---------------------------------------------------------------------------
// Provider base boilerplate
// ---------------------------------------------------------------------------

class ApiKeyProvider : public WebSearchProvider {
public:
    bool is_configured(const WebSearchProviderSettings& cfg) const override {
        return !cfg.api_key.empty();
    }
};

class BraveProvider final : public ApiKeyProvider {
public:
    std::string name() const override { return "brave"; }
    std::string display_name() const override { return "Brave Search"; }

    std::expected<WebSearchResults, std::string>
    search(const std::string& query, std::size_t max_results,
           const WebSearchProviderSettings& cfg) const override {
        const std::string url =
            "https://api.search.brave.com/res/v1/web/search?q=" +
            OmniNet::Utils::urlEncode(query) + "&count=" +
            std::to_string(max_results);
        auto resp = http_send("GET", url,
                              {{"Accept", "application/json"},
                               {"X-Subscription-Token", cfg.api_key},
                               {"User-Agent", "daedalus/1.0"}},
                              "");
        if (!resp) return std::unexpected(resp.error());
        if (resp->status != 200)
            return std::unexpected("Brave API returned HTTP " +
                                   std::to_string(resp->status));
        try {
            return parse_brave_response(resp->body, max_results);
        } catch (const std::exception& e) {
            return std::unexpected(std::string("Brave response parse: ") + e.what());
        }
    }
};

class TavilyProvider final : public ApiKeyProvider {
public:
    std::string name() const override { return "tavily"; }
    std::string display_name() const override { return "Tavily"; }

    std::expected<WebSearchResults, std::string>
    search(const std::string& query, std::size_t max_results,
           const WebSearchProviderSettings& cfg) const override {
        const json body = {{"query", query},
                           {"max_results", static_cast<int>(max_results)}};
        auto resp = http_send("POST", "https://api.tavily.com/search",
                              {{"Content-Type", "application/json"},
                               {"Authorization", "Bearer " + cfg.api_key},
                               {"User-Agent", "daedalus/1.0"}},
                              body.dump());
        if (!resp) return std::unexpected(resp.error());
        if (resp->status != 200)
            return std::unexpected("Tavily API returned HTTP " +
                                   std::to_string(resp->status));
        try {
            return parse_tavily_response(resp->body, max_results);
        } catch (const std::exception& e) {
            return std::unexpected(std::string("Tavily response parse: ") + e.what());
        }
    }
};

class ExaProvider final : public ApiKeyProvider {
public:
    std::string name() const override { return "exa"; }
    std::string display_name() const override { return "Exa"; }

    std::expected<WebSearchResults, std::string>
    search(const std::string& query, std::size_t max_results,
           const WebSearchProviderSettings& cfg) const override {
        const json body = {
            {"query", query},
            {"numResults", static_cast<int>(max_results)},
            {"contents", {{"text", {{"maxCharacters", 500}}}}},
        };
        auto resp = http_send("POST", "https://api.exa.ai/search",
                              {{"Content-Type", "application/json"},
                               {"x-api-key", cfg.api_key},
                               {"User-Agent", "daedalus/1.0"}},
                              body.dump());
        if (!resp) return std::unexpected(resp.error());
        if (resp->status != 200)
            return std::unexpected("Exa API returned HTTP " +
                                   std::to_string(resp->status));
        try {
            return parse_exa_response(resp->body, max_results);
        } catch (const std::exception& e) {
            return std::unexpected(std::string("Exa response parse: ") + e.what());
        }
    }
};

class FirecrawlProvider final : public ApiKeyProvider {
public:
    std::string name() const override { return "firecrawl"; }
    std::string display_name() const override { return "Firecrawl"; }

    std::expected<WebSearchResults, std::string>
    search(const std::string& query, std::size_t max_results,
           const WebSearchProviderSettings& cfg) const override {
        const json body = {{"query", query},
                           {"limit", static_cast<int>(max_results)}};
        auto resp = http_send("POST", "https://api.firecrawl.dev/v2/search",
                              {{"Content-Type", "application/json"},
                               {"Authorization", "Bearer " + cfg.api_key},
                               {"User-Agent", "daedalus/1.0"}},
                              body.dump());
        if (!resp) return std::unexpected(resp.error());
        if (resp->status != 200)
            return std::unexpected("Firecrawl API returned HTTP " +
                                   std::to_string(resp->status));
        try {
            return parse_firecrawl_response(resp->body, max_results);
        } catch (const std::exception& e) {
            return std::unexpected(std::string("Firecrawl response parse: ") + e.what());
        }
    }
};

class SerperProvider final : public ApiKeyProvider {
public:
    std::string name() const override { return "serper"; }
    std::string display_name() const override { return "Serper (Google)"; }

    std::expected<WebSearchResults, std::string>
    search(const std::string& query, std::size_t max_results,
           const WebSearchProviderSettings& cfg) const override {
        const json body = {{"q", query},
                           {"num", static_cast<int>(max_results)}};
        auto resp = http_send("POST", "https://google.serper.dev/search",
                              {{"Content-Type", "application/json"},
                               {"X-API-Key", cfg.api_key},
                               {"User-Agent", "daedalus/1.0"}},
                              body.dump());
        if (!resp) return std::unexpected(resp.error());
        if (resp->status != 200)
            return std::unexpected("Serper API returned HTTP " +
                                   std::to_string(resp->status));
        try {
            return parse_serper_response(resp->body, max_results);
        } catch (const std::exception& e) {
            return std::unexpected(std::string("Serper response parse: ") + e.what());
        }
    }
};

class SerpapiProvider final : public ApiKeyProvider {
public:
    std::string name() const override { return "serpapi"; }
    std::string display_name() const override { return "SerpAPI"; }

    std::expected<WebSearchResults, std::string>
    search(const std::string& query, std::size_t max_results,
           const WebSearchProviderSettings& cfg) const override {
        const std::string url =
            "https://serpapi.com/search.json?q=" +
            OmniNet::Utils::urlEncode(query) + "&num=" +
            std::to_string(max_results) + "&api_key=" +
            OmniNet::Utils::urlEncode(cfg.api_key);
        auto resp = http_send("GET", url,
                              {{"Accept", "application/json"},
                               {"User-Agent", "daedalus/1.0"}},
                              "");
        if (!resp) return std::unexpected(resp.error());
        if (resp->status != 200)
            return std::unexpected("SerpAPI returned HTTP " +
                                   std::to_string(resp->status));
        try {
            return parse_serpapi_response(resp->body, max_results);
        } catch (const std::exception& e) {
            return std::unexpected(std::string("SerpAPI response parse: ") + e.what());
        }
    }
};

class SearxngProvider final : public WebSearchProvider {
public:
    std::string name() const override { return "searxng"; }
    std::string display_name() const override { return "SearXNG (self-hosted)"; }

    bool is_configured(const WebSearchProviderSettings& cfg) const override {
        return cfg.base_url.starts_with("http://") ||
               cfg.base_url.starts_with("https://");
    }

    std::expected<WebSearchResults, std::string>
    search(const std::string& query, std::size_t max_results,
           const WebSearchProviderSettings& cfg) const override {
        std::string base = cfg.base_url;
        while (!base.empty() && base.back() == '/') base.pop_back();
        const std::string url = base + "/search?q=" +
                                OmniNet::Utils::urlEncode(query) +
                                "&format=json";

        std::vector<std::pair<std::string, std::string>> headers = {
            {"Accept", "application/json"},
            {"User-Agent", "daedalus/1.0"},
        };
        // Auth, in priority order:
        //  1. username set → HTTP Basic auth (classic protected instance)
        //  2. api_key set → Bearer + X-API-Key headers (instance behind a
        //     token gate, e.g. a reverse proxy in front of SearXNG —
        //     SearXNG itself has no API-key concept)
        //  3. neither → unauthenticated (e.g. localhost-only instance)
        if (!cfg.username.empty()) {
            headers.emplace_back(
                "Authorization",
                "Basic " + OmniNet::Utils::base64Encode(
                               cfg.username + ":" + cfg.password));
        } else if (!cfg.api_key.empty()) {
            headers.emplace_back("Authorization", "Bearer " + cfg.api_key);
            headers.emplace_back("X-API-Key", cfg.api_key);
        }

        auto resp = http_send("GET", url, headers, "");
        if (!resp) return std::unexpected(resp.error());
        if (resp->status == 401 || resp->status == 403)
            return std::unexpected(
                "SearXNG instance rejected the request (HTTP " +
                std::to_string(resp->status) +
                ") — check username/password in Settings");
        if (resp->status != 200)
            return std::unexpected("SearXNG instance returned HTTP " +
                                   std::to_string(resp->status));
        try {
            return parse_searxng_response(resp->body, max_results);
        } catch (const std::exception& e) {
            return std::unexpected(
                std::string("SearXNG response parse (is JSON format "
                            "enabled on the instance?): ") +
                e.what());
        }
    }
};

// §WEB-SEARCH-HYBRID (2026-10-11): the key-less fallback is no longer a
// single engine. DuckDuckGo's HTML endpoint answers HTTP 202 (anti-bot
// challenge) to most datacenter traffic and Mojeek serves a Captcha page,
// so "hybrid" walks a small ordered list of key-less engines — plain
// browser-UA GETs, same as web_fetch on a crafted search URL — and the
// first engine to return results wins. Verified live 2026-10-11:
// bing 200 (works), mojeek Captcha (dropped), ddg 202 (kept last — it
// still works from some residential IPs).
class HybridProvider final : public WebSearchProvider {
public:
    std::string name() const override { return "hybrid"; }
    std::string display_name() const override {
        return "Web HTML (Bing/DuckDuckGo, key-less)";
    }

    bool is_configured(const WebSearchProviderSettings&) const override {
        return true;  // no key, no config — the always-available fallback
    }

    std::expected<WebSearchResults, std::string>
    search(const std::string& query, std::size_t max_results,
           const WebSearchProviderSettings&) const override {
        static const std::string kUa =
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, "
            "like Gecko) Chrome/126.0 Safari/537.36";
        const std::string q = OmniNet::Utils::urlEncode(query);

        struct Engine {
            std::string label;
            std::string url;
            WebSearchResults (*parse)(std::string_view, std::size_t);
        };
        const std::vector<Engine> engines = {
            {"Bing", "https://www.bing.com/search?q=" + q, &parse_bing_html},
            {"DuckDuckGo", "https://html.duckduckgo.com/html/?q=" + q,
             &parse_duckduckgo_html},
        };

        std::string errors;
        for (const auto& e : engines) {
            auto resp = http_send("GET", e.url, {{"User-Agent", kUa}}, "");
            if (!resp) {
                errors += (errors.empty() ? "" : "; ") + e.label + ": " +
                          resp.error();
                continue;
            }
            if (resp->status != 200) {
                errors += (errors.empty() ? "" : "; ") + e.label +
                          ": HTTP " + std::to_string(resp->status);
                continue;
            }
            auto results = e.parse(resp->body, max_results);
            if (!results.empty()) return results;
            errors += (errors.empty() ? "" : "; ") + e.label +
                      ": no results parsed";
        }
        return std::unexpected(errors.empty()
                                   ? std::string("all key-less engines failed")
                                   : errors);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Pure parsers
// ---------------------------------------------------------------------------

std::string web_search_html_to_text(std::string_view html) {
    std::string out;
    out.reserve(html.size());
    bool last_space = true;  // collapse whitespace, trim leading
    for (std::size_t i = 0; i < html.size();) {
        const char c = html[i];
        if (c == '<') {
            const auto gt = html.find('>', i);
            if (gt == std::string_view::npos) break;
            // Drop script/style/noscript CONTENT, not just their tags.
            bool dropped = false;
            const auto tag = html.substr(i + 1, gt - i - 1);
            for (const auto drop : {"script", "style", "noscript"}) {
                if (tag.size() >= std::strlen(drop) &&
                    tag.compare(0, std::strlen(drop), drop) == 0 &&
                    tag[0] != '/' &&
                    (tag.size() == std::strlen(drop) ||
                     tag[std::strlen(drop)] == ' ' ||
                     tag[std::strlen(drop)] == '>')) {
                    const std::string close = "</" + std::string(drop);
                    const auto end = html.find(close, gt);
                    i = end == std::string_view::npos
                            ? html.size()
                            : html.find('>', end) + 1;
                    dropped = true;
                    break;
                }
            }
            if (!dropped) {
                i = gt + 1;
                if (!last_space) {
                    out += ' ';
                    last_space = true;
                }
            }
            continue;
        }
        if (c == '&') {
            const auto semi = html.find(';', i);
            if (semi != std::string_view::npos && semi - i < 12) {
                const auto ent = html.substr(i + 1, semi - i - 1);
                std::string rep;
                if (ent == "amp") rep = "&";
                else if (ent == "lt") rep = "<";
                else if (ent == "gt") rep = ">";
                else if (ent == "quot") rep = "\"";
                else if (ent == "apos" || ent == "#39") rep = "'";
                else if (ent == "nbsp") rep = " ";
                else if (!ent.empty() && ent[0] == '#') {
                    try {
                        const int n = std::stoi(std::string(ent.substr(1)));
                        if (n >= 32 && n < 127)
                            rep = static_cast<char>(n);
                    } catch (...) {
                    }
                }
                if (!rep.empty()) {
                    if (rep == " ") {
                        if (!last_space) {
                            out += ' ';
                            last_space = true;
                        }
                    } else {
                        out += rep;
                        last_space = false;
                    }
                    i = semi + 1;
                    continue;
                }
            }
            ++i;  // not a recognised entity — drop the '&'
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!last_space) {
                out += ' ';
                last_space = true;
            }
        } else {
            out += c;
            last_space = false;
        }
        ++i;
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

WebSearchResults parse_duckduckgo_html(std::string_view html,
                                       std::size_t max_results) {
    WebSearchResults out;
    std::size_t pos = 0;
    while (out.size() < max_results) {
        // Result anchors carry class="result__a".
        const auto cls = html.find("result__a", pos);
        if (cls == std::string_view::npos) break;
        const auto lt = html.rfind('<', cls);
        const auto gt = html.find('>', cls);
        if (lt == std::string_view::npos || gt == std::string_view::npos) break;

        WebSearchResult r;
        r.url = ddg_resolve_href(ddg_href_of(html, lt, gt));
        r.title = web_search_html_to_text(ddg_inner_html(html, gt, "</a>"));

        // The snippet follows the anchor within the same result block.
        const auto snip = html.find("result__snippet", gt);
        if (snip != std::string_view::npos) {
            const auto sgt = html.find('>', snip);
            if (sgt != std::string_view::npos) {
                // Snippet element is an <a> or <td>; both close plainly.
                auto text = ddg_inner_html(html, sgt, "</a>");
                if (text.empty()) text = ddg_inner_html(html, sgt, "</td>");
                r.snippet = web_search_html_to_text(text);
            }
        }

        if (!r.url.empty() && !r.title.empty()) out.push_back(std::move(r));
        pos = gt + 1;
    }
    return out;
}

WebSearchResults parse_brave_response(std::string_view body,
                                      std::size_t max_results) {
    return parse_json_results(body, max_results, "web.results",
                              "title", "url", "description");
}

WebSearchResults parse_tavily_response(std::string_view body,
                                       std::size_t max_results) {
    return parse_json_results(body, max_results, "results",
                              "title", "url", "content");
}

WebSearchResults parse_exa_response(std::string_view body,
                                    std::size_t max_results) {
    return parse_json_results(body, max_results, "results",
                              "title", "url", "text");
}

WebSearchResults parse_firecrawl_response(std::string_view body,
                                          std::size_t max_results) {
    return parse_json_results(body, max_results, "data.web",
                              "title", "url", "description");
}

WebSearchResults parse_serper_response(std::string_view body,
                                       std::size_t max_results) {
    return parse_json_results(body, max_results, "organic",
                              "title", "link", "snippet");
}

WebSearchResults parse_serpapi_response(std::string_view body,
                                        std::size_t max_results) {
    return parse_json_results(body, max_results, "organic_results",
                              "title", "link", "snippet");
}

WebSearchResults parse_searxng_response(std::string_view body,
                                        std::size_t max_results) {
    return parse_json_results(body, max_results, "results",
                              "title", "url", "content");
}

// §WEB-SEARCH-HYBRID: Bing HTML results. Each organic hit is a
// <li class="b_algo"> block holding <h2><a href="…">title</a></h2> plus a
// <p class="b_lineclamp…"> snippet. Bing wraps outbound links as
// /ck/a redirect URLs; the real URL rides in the u= query param as
// "a1" + base64url — decode it, fall back to the raw href when absent.
namespace {

std::string bing_decode_redirect(std::string href) {
    if (href.find("bing.com/ck/a") == std::string::npos) return href;
    // HTML may escape the query string separators.
    std::string::size_type pos = 0;
    while ((pos = href.find("&amp;", pos)) != std::string::npos) {
        href.replace(pos, 5, "&");
        pos += 1;
    }
    const auto u = href.find("u=a1");
    if (u == std::string::npos) return href;
    auto end = href.find('&', u);
    std::string b64 = href.substr(u + 4, end == std::string::npos
                                              ? end : end - (u + 4));
    // base64url → base64
    for (auto& c : b64) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (b64.size() % 4 != 0) b64 += '=';
    auto decoded = OmniNet::Utils::base64DecodeToString(b64);
    if (decoded && (decoded->starts_with("http://") ||
                    decoded->starts_with("https://"))) {
        return *decoded;
    }
    return href;
}

}  // namespace

WebSearchResults parse_bing_html(std::string_view html,
                                 std::size_t max_results) {
    WebSearchResults results;
    const std::string h(html);
    std::size_t pos = 0;
    while (results.size() < max_results) {
        const auto li = h.find("<li class=\"b_algo\"", pos);
        if (li == std::string::npos) break;
        const auto li_end = h.find("<li class=\"b_algo\"", li + 1);
        const std::string block = h.substr(
            li, li_end == std::string::npos ? li_end : li_end - li);

        const auto a = block.find("<h2><a");
        if (a == std::string::npos) { pos = li + 1; continue; }
        const auto href_at = block.find("href=\"", a);
        const auto href_end = block.find('\"', href_at + 6);
        if (href_at == std::string::npos || href_end == std::string::npos) {
            pos = li + 1;
            continue;
        }
        const std::string url =
            bing_decode_redirect(block.substr(href_at + 6,
                                              href_end - (href_at + 6)));
        const auto gt = block.find('>', href_end);
        const auto a_end = block.find("</a>", gt);
        if (gt == std::string::npos || a_end == std::string::npos) {
            pos = li + 1;
            continue;
        }
        const std::string title =
            web_search_html_to_text(block.substr(gt + 1, a_end - gt - 1));

        std::string snippet;
        const auto p = block.find("<p class=\"b_lineclamp");
        if (p != std::string::npos) {
            const auto p_gt = block.find('>', p);
            const auto p_end = block.find("</p>", p_gt);
            if (p_gt != std::string::npos && p_end != std::string::npos)
                snippet = web_search_html_to_text(
                    block.substr(p_gt + 1, p_end - p_gt - 1));
        }

        if (!title.empty() && !url.empty())
            results.push_back({title, url, snippet});
        pos = li + 1;
    }
    return results;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

const std::vector<std::string>& web_search_provider_ids() {
    static const std::vector<std::string> kIds = {
        "brave", "tavily", "exa", "firecrawl",
        "serper", "serpapi", "searxng", "hybrid",
    };
    return kIds;
}

std::unique_ptr<WebSearchProvider>
make_web_search_provider(std::string_view id) {
    if (id == "brave")      return std::make_unique<BraveProvider>();
    if (id == "tavily")     return std::make_unique<TavilyProvider>();
    if (id == "exa")        return std::make_unique<ExaProvider>();
    if (id == "firecrawl")  return std::make_unique<FirecrawlProvider>();
    if (id == "serper")     return std::make_unique<SerperProvider>();
    if (id == "serpapi")    return std::make_unique<SerpapiProvider>();
    if (id == "searxng")    return std::make_unique<SearxngProvider>();
    if (id == "hybrid")     return std::make_unique<HybridProvider>();
    return nullptr;
}

// ---------------------------------------------------------------------------
// Failover chain
// ---------------------------------------------------------------------------

WebSearchOutcome run_web_search_chain(
    const std::vector<WebSearchProviderSettings>& chain,
    const std::string& query, std::size_t max_results,
    std::unique_ptr<WebSearchProvider> (*make)(std::string_view)) {
    WebSearchOutcome out;
    if (!make) make = &make_web_search_provider;

    for (const auto& cfg : chain) {
        if (!cfg.enabled) continue;
        auto provider = make(cfg.name);
        if (!provider) {
            out.failures.push_back({cfg.name, "unknown provider id"});
            continue;
        }
        // Not an error: the user hasn't entered a key/URL yet — skip quietly.
        if (!provider->is_configured(cfg)) continue;

        auto results = provider->search(query, max_results, cfg);
        if (!results) {
            out.failures.push_back({cfg.name, results.error()});
            continue;
        }
        if (results->empty()) {
            out.failures.push_back({cfg.name, "returned 0 results"});
            continue;
        }
        out.ok = true;
        out.provider = cfg.name;
        out.results = std::move(*results);
        return out;
    }
    return out;
}

std::vector<WebSearchProviderSettings> load_web_search_chain() {
    const auto& paths = daedalus::Paths::global();
    try {
        auto store = daedalus::SettingsStore::for_home(paths.user_data_root());
        return store.get_web_search_providers();  // already priority-ordered
    } catch (...) {
        return {};
    }
}

std::string web_search_startup_status() {
    const auto chain = load_web_search_chain();
    std::ostringstream line;
    bool any = false;
    for (const auto& cfg : chain) {
        if (!cfg.enabled) continue;
        auto provider = make_web_search_provider(cfg.name);
        if (any) line << " -> ";
        line << cfg.name;
        if (!provider || !provider->is_configured(cfg)) line << " (unconfigured)";
        any = true;
    }
    if (!any) {
        return "[WEB-SEARCH] WARNING: no web_search providers enabled — "
               "configure one in Settings (hybrid works key-less)";
    }
    return "[WEB-SEARCH] failover chain: " + line.str();
}

}  // namespace daedalus
