// SPDX-License-Identifier: BSL-1.0
// tests/test_web_search.cpp
//
// §WEB-SEARCH-CORE (2026-10-10): unified web_search provider registry.
// Hermetic: no network (parsers run on canned fixtures; the failover
// chain is exercised with injected fake providers).
//
// Covers:
//   1. web_search_html_to_text — tags, entities, whitespace.
//   2. parse_duckduckgo_html — result__a anchors, uddg decoding, snippets.
//   3. The 7 JSON response parsers (brave/tavily/exa/firecrawl/serper/
//      serpapi/searxng) on canned fixtures.
//   4. Registry: all 8 provider ids construct, unknown id -> nullptr,
//      is_configured contracts.
//   5. run_web_search_chain: first-success wins, error/empty fall through,
//      disabled + unconfigured skipped, all-fail aggregates failures.
//   6. ToolFactory: exactly one web_search, no serper_search/searxng_search.
//   7. SettingsStore schema v8: fresh DB seeds 8 rows (duckduckgo
//      enabled), accessor roundtrip, legacy serper/searxng rows migrate.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tools/websearch/WebSearchProvider.h"
#include "tools/ToolFactory.h"

#include "daedalus/settings.h"

#include <db/Database.h>

#include <filesystem>
#include <map>
#include <string>

namespace fs = std::filesystem;
using namespace daedalus;

namespace {

fs::path make_temp_dir(const std::string& tag) {
    const auto dir = fs::temp_directory_path() /
        ("daedalus_ws_test_" + tag + "_" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    return dir;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. html_to_text
// ---------------------------------------------------------------------------

TEST_CASE("web_search_html_to_text: tags, entities, whitespace") {
    CHECK(web_search_html_to_text("GTK4 <b>Documentation</b>") ==
          "GTK4 Documentation");
    CHECK(web_search_html_to_text("a &amp; b &lt;c&gt; &#39;q&#39;") ==
          "a & b <c> 'q'");
    CHECK(web_search_html_to_text("  one\n\n  two\t three  ") ==
          "one two three");
    CHECK(web_search_html_to_text("x&nbsp;y") == "x y");
    CHECK(web_search_html_to_text("plain") == "plain");
    CHECK(web_search_html_to_text("<unterminated") == "");
}

// ---------------------------------------------------------------------------
// 2. DuckDuckGo HTML parser (fixture ported from the old monkeybyte test)
// ---------------------------------------------------------------------------

TEST_CASE("parse_duckduckgo_html: anchors, uddg redirect, snippets") {
    const std::string html = R"HTML(
<html><body>
<div class="result results_links results_links_deep web-result">
  <div class="links_main links_deep result__body">
    <h2 class="result__title">
      <a rel="nofollow" class="result__a"
         href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fgtk4&amp;rut=abc123">
         GTK4 <b>Documentation</b></a>
    </h2>
    <a class="result__snippet"
       href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fgtk4&amp;rut=abc123">
       GTK4 is the latest <b>major</b> release of the toolkit.</a>
  </div>
</div>
<div class="result results_links results_links_deep web-result">
  <div class="links_main links_deep result__body">
    <h2 class="result__title">
      <a rel="nofollow" class="result__a"
         href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.org%2Fmd4c%3Flang%3Den&amp;rut=def456">
         MD4C Markdown parser</a>
    </h2>
  </div>
</div>
</body></html>)HTML";

    const auto results = parse_duckduckgo_html(html, 10);
    REQUIRE(results.size() == 2);
    CHECK(results[0].title == "GTK4 Documentation");
    CHECK(results[0].url == "https://example.com/gtk4");
    CHECK(results[0].snippet ==
          "GTK4 is the latest major release of the toolkit.");
    CHECK(results[1].url == "https://example.org/md4c?lang=en");
    CHECK(results[1].snippet.empty());

    CHECK(parse_duckduckgo_html(html, 1).size() == 1);
    CHECK(parse_duckduckgo_html("<html><body>nothing here</body></html>", 10)
              .empty());
}

// ---------------------------------------------------------------------------
// 3. JSON response parsers
// ---------------------------------------------------------------------------

TEST_CASE("JSON parsers: provider-specific shapes normalise") {
    SUBCASE("brave: web.results / description") {
        const std::string body = R"({"web":{"results":[
            {"title":"T1","url":"https://a.example","description":"d1"},
            {"title":"T2","url":"https://b.example","description":"d2"}]}})";
        const auto r = parse_brave_response(body, 10);
        REQUIRE(r.size() == 2);
        CHECK(r[0].title == "T1");
        CHECK(r[0].url == "https://a.example");
        CHECK(r[0].snippet == "d1");
    }
    SUBCASE("tavily: results / content") {
        const auto r = parse_tavily_response(
            R"({"results":[{"title":"T","url":"https://a.example","content":"c"}]})",
            10);
        REQUIRE(r.size() == 1);
        CHECK(r[0].snippet == "c");
    }
    SUBCASE("exa: results / text") {
        const auto r = parse_exa_response(
            R"({"results":[{"title":"T","url":"https://a.example","text":"t"}]})",
            10);
        REQUIRE(r.size() == 1);
        CHECK(r[0].snippet == "t");
    }
    SUBCASE("firecrawl: data.web / description") {
        const auto r = parse_firecrawl_response(
            R"({"success":true,"data":{"web":[
                {"title":"T","url":"https://a.example","description":"d"}]}})",
            10);
        REQUIRE(r.size() == 1);
        CHECK(r[0].url == "https://a.example");
    }
    SUBCASE("serper: organic / link + snippet") {
        const auto r = parse_serper_response(
            R"({"organic":[{"title":"T","link":"https://a.example","snippet":"s"}]})",
            10);
        REQUIRE(r.size() == 1);
        CHECK(r[0].url == "https://a.example");
        CHECK(r[0].snippet == "s");
    }
    SUBCASE("serpapi: organic_results / link") {
        const auto r = parse_serpapi_response(
            R"({"organic_results":[{"title":"T","link":"https://a.example","snippet":"s"}]})",
            10);
        REQUIRE(r.size() == 1);
        CHECK(r[0].snippet == "s");
    }
    SUBCASE("searxng: results / content") {
        const auto r = parse_searxng_response(
            R"({"results":[{"title":"T","url":"https://a.example","content":"c","engine":"google"}]})",
            10);
        REQUIRE(r.size() == 1);
        CHECK(r[0].snippet == "c");
    }
    SUBCASE("shared behaviour") {
        // max_results honored
        CHECK(parse_tavily_response(
                  R"({"results":[
                    {"title":"1","url":"https://1.example","content":""},
                    {"title":"2","url":"https://2.example","content":""}]})",
                  1)
                  .size() == 1);
        // missing result array -> empty (not an error, not a crash)
        CHECK(parse_brave_response(R"({"query":{"original":"x"}})", 10).empty());
        // items missing url or title are skipped
        const auto r = parse_tavily_response(
            R"({"results":[
              {"title":"","url":"https://x.example","content":""},
              {"title":"ok","url":"https://ok.example","content":""}]})",
            10);
        REQUIRE(r.size() == 1);
        CHECK(r[0].title == "ok");
        // malformed JSON propagates as an exception (provider converts it
        // to an error string — the pure parser is allowed to throw)
        CHECK_THROWS(parse_serper_response("{not json", 10));
    }
}

// ---------------------------------------------------------------------------
// 4. Registry
// ---------------------------------------------------------------------------

TEST_CASE("registry: all 8 ids construct; unknown -> nullptr") {
    CHECK(web_search_provider_ids().size() == 8);
    for (const auto& id : web_search_provider_ids()) {
        auto p = make_web_search_provider(id);
        REQUIRE(p != nullptr);
        CHECK(p->name() == id);
        CHECK(!p->display_name().empty());
    }
    CHECK(make_web_search_provider("nope") == nullptr);

    WebSearchProviderSettings cfg;
    cfg.name = "brave";
    CHECK(!make_web_search_provider("brave")->is_configured(cfg));
    cfg.api_key = "KEY";
    CHECK(make_web_search_provider("brave")->is_configured(cfg));

    WebSearchProviderSettings sx;
    sx.name = "searxng";
    CHECK(!make_web_search_provider("searxng")->is_configured(sx));
    sx.base_url = "http://localhost:8888";
    CHECK(make_web_search_provider("searxng")->is_configured(sx));

    CHECK(make_web_search_provider("hybrid")->is_configured({}));
}

// ---------------------------------------------------------------------------
// 5. Failover chain (injected fakes — no network)
// ---------------------------------------------------------------------------

namespace {

struct FakeBehaviour {
    enum class Kind { Ok, Error, Empty } kind = Kind::Ok;
    std::string error;
    bool configured = true;
};

std::map<std::string, FakeBehaviour>& fake_map() {
    static std::map<std::string, FakeBehaviour> m;
    return m;
}

class FakeProvider final : public WebSearchProvider {
public:
    explicit FakeProvider(std::string name) : name_(std::move(name)) {}

    std::string name() const override { return name_; }
    std::string display_name() const override { return name_; }

    bool is_configured(const WebSearchProviderSettings&) const override {
        return fake_map()[name_].configured;
    }

    std::expected<WebSearchResults, std::string>
    search(const std::string&, std::size_t,
           const WebSearchProviderSettings&) const override {
        const auto& b = fake_map()[name_];
        switch (b.kind) {
            case FakeBehaviour::Kind::Error:
                return std::unexpected(b.error);
            case FakeBehaviour::Kind::Empty:
                return WebSearchResults{};
            case FakeBehaviour::Kind::Ok:
                break;
        }
        return WebSearchResults{{"T", "https://a.example", "s"}};
    }

private:
    std::string name_;
};

std::unique_ptr<WebSearchProvider> fake_factory(std::string_view id) {
    if (fake_map().contains(std::string(id)))
        return std::make_unique<FakeProvider>(std::string(id));
    return nullptr;
}

WebSearchProviderSettings row(const std::string& name, bool enabled = true,
                              int priority = 0) {
    WebSearchProviderSettings p;
    p.name = name;
    p.enabled = enabled;
    p.priority = priority;
    return p;
}

}  // namespace

TEST_CASE("run_web_search_chain: failover semantics") {
    SUBCASE("first success wins, later providers not called") {
        fake_map() = {{"a", {}}, {"b", {}}};
        const auto out = run_web_search_chain({row("a"), row("b")}, "q", 5,
                                              &fake_factory);
        CHECK(out.ok);
        CHECK(out.provider == "a");
        CHECK(out.results.size() == 1);
        CHECK(out.failures.empty());
    }
    SUBCASE("error falls through to the next provider") {
        fake_map() = {{"a", {FakeBehaviour::Kind::Error, "HTTP 429"}},
                      {"b", {}}};
        const auto out = run_web_search_chain({row("a"), row("b")}, "q", 5,
                                              &fake_factory);
        CHECK(out.ok);
        CHECK(out.provider == "b");
        REQUIRE(out.failures.size() == 1);
        CHECK(out.failures[0].provider == "a");
        CHECK(out.failures[0].error == "HTTP 429");
    }
    SUBCASE("empty result set falls through") {
        fake_map() = {{"a", {FakeBehaviour::Kind::Empty}}, {"b", {}}};
        const auto out = run_web_search_chain({row("a"), row("b")}, "q", 5,
                                              &fake_factory);
        CHECK(out.ok);
        CHECK(out.provider == "b");
    }
    SUBCASE("all fail -> not ok, failures recorded in order") {
        fake_map() = {{"a", {FakeBehaviour::Kind::Error, "e1"}},
                      {"b", {FakeBehaviour::Kind::Error, "e2"}}};
        const auto out = run_web_search_chain({row("a"), row("b")}, "q", 5,
                                              &fake_factory);
        CHECK(!out.ok);
        REQUIRE(out.failures.size() == 2);
        CHECK(out.failures[0].provider == "a");
        CHECK(out.failures[1].provider == "b");
    }
    SUBCASE("disabled rows skipped; unconfigured skipped silently") {
        fake_map() = {{"off", {}},
                      {"nokey", {{}, "", /*configured=*/false}},
                      {"good", {}}};
        const auto out = run_web_search_chain(
            {row("off", /*enabled=*/false), row("nokey"), row("good")},
            "q", 5, &fake_factory);
        CHECK(out.ok);
        CHECK(out.provider == "good");
        CHECK(out.failures.empty());  // skipped != failed
    }
    SUBCASE("unknown provider id recorded as a failure") {
        fake_map() = {{"good", {}}};
        const auto out = run_web_search_chain({row("ghost"), row("good")},
                                              "q", 5, &fake_factory);
        CHECK(out.ok);
        CHECK(out.provider == "good");
        REQUIRE(out.failures.size() == 1);
        CHECK(out.failures[0].provider == "ghost");
    }
}

// ---------------------------------------------------------------------------
// 6. ToolFactory roster
// ---------------------------------------------------------------------------

TEST_CASE("ToolFactory: web_search in, serper_search/searxng_search out") {
    ToolFactory factory;  // ctor registers the default core tools
    CHECK(factory.has_tool("web_search"));
    CHECK(!factory.has_tool("serper_search"));
    CHECK(!factory.has_tool("searxng_search"));
}

// ---------------------------------------------------------------------------
// 7. SettingsStore schema v8
// ---------------------------------------------------------------------------

TEST_CASE("schema v8: fresh DB seeds 8 providers, duckduckgo enabled") {
    const auto dir = make_temp_dir("fresh");
    auto store = SettingsStore::for_home(dir);
    const auto chain = store.get_web_search_providers();

    REQUIRE(chain.size() == 8);
    // priority order: brave first, hybrid last (§WEB-SEARCH-HYBRID)
    CHECK(chain.front().name == "brave");
    CHECK(chain.back().name == "hybrid");
    for (const auto& p : chain) {
        CHECK(p.enabled == (p.name == "hybrid"));
        CHECK(p.api_key.empty());
    }
}

TEST_CASE("schema v8: set/get roundtrip; re-open keeps edits, still 8 rows") {
    const auto dir = make_temp_dir("roundtrip");
    {
        auto store = SettingsStore::for_home(dir);
        auto brave = store.get_web_search_providers().front();
        REQUIRE(brave.name == "brave");
        brave.enabled = true;
        brave.api_key = "BRAVE-TEST-KEY";
        store.set_web_search_provider(brave);
    }
    auto store = SettingsStore::for_home(dir);
    const auto chain = store.get_web_search_providers();
    REQUIRE(chain.size() == 8);
    const auto it = std::find_if(chain.begin(), chain.end(),
                                 [](const auto& p) { return p.name == "brave"; });
    REQUIRE(it != chain.end());
    CHECK(it->enabled);
    CHECK(it->api_key == "BRAVE-TEST-KEY");
}

TEST_CASE("schema v8: legacy serper_config/searxng_config rows migrate") {
    const auto dir = make_temp_dir("migrate");
    const auto db_path = (dir / "daedalus.db").string();

    // Hand-build a v7-era DB: legacy tables + data + schema_version = 7.
    // SettingsStore::for_home() then runs kSchemaSql (idempotent) and the
    // v8 migration, which must copy these values into the new table.
    {
        OmniNet::DB::Database db;
        db.open(db_path);
        db.execute("CREATE TABLE schema_version (version INTEGER PRIMARY KEY)");
        db.execute("INSERT INTO schema_version(version) VALUES (7)");
        db.execute("CREATE TABLE serper_config ("
                   " id INTEGER PRIMARY KEY CHECK (id = 1),"
                   " api_key TEXT NOT NULL)");
        db.execute("INSERT INTO serper_config(id, api_key)"
                   " VALUES (1, 'LEGACY-SERPER-KEY')");
        db.execute("CREATE TABLE searxng_config ("
                   " id INTEGER PRIMARY KEY CHECK (id = 1),"
                   " base_url TEXT NOT NULL,"
                   " username TEXT NOT NULL DEFAULT '',"
                   " password TEXT NOT NULL DEFAULT '',"
                   " enabled INTEGER NOT NULL DEFAULT 1)");
        db.execute("INSERT INTO searxng_config(id, base_url, username,"
                   " password, enabled)"
                   " VALUES (1, 'https://search.example.com', 'u', 'p', 1)");
        db.close();
    }

    auto store = SettingsStore::for_home(dir);
    const auto chain = store.get_web_search_providers();
    REQUIRE(chain.size() == 8);

    const auto serper = std::find_if(
        chain.begin(), chain.end(),
        [](const auto& p) { return p.name == "serper"; });
    REQUIRE(serper != chain.end());
    CHECK(serper->enabled);
    CHECK(serper->api_key == "LEGACY-SERPER-KEY");

    const auto searxng = std::find_if(
        chain.begin(), chain.end(),
        [](const auto& p) { return p.name == "searxng"; });
    REQUIRE(searxng != chain.end());
    CHECK(searxng->enabled);
    CHECK(searxng->base_url == "https://search.example.com");
    CHECK(searxng->username == "u");
    CHECK(searxng->password == "p");
}

TEST_CASE("schema v9: duckduckgo row converts to hybrid (§WEB-SEARCH-HYBRID)") {
    const auto dir = make_temp_dir("v9migrate");
    const auto db_path = (dir / "daedalus.db").string();

    // Hand-build a v8-era DB: duckduckgo row disabled at priority 42 —
    // the migration must carry BOTH values to the new hybrid row.
    {
        OmniNet::DB::Database db;
        db.open(db_path);
        db.execute("CREATE TABLE schema_version (version INTEGER PRIMARY KEY)");
        db.execute("INSERT INTO schema_version(version) VALUES (8)");
        db.execute("CREATE TABLE web_search_providers ("
                   " name TEXT PRIMARY KEY,"
                   " enabled INTEGER NOT NULL DEFAULT 0,"
                   " api_key TEXT NOT NULL DEFAULT '',"
                   " base_url TEXT NOT NULL DEFAULT '',"
                   " username TEXT NOT NULL DEFAULT '',"
                   " password TEXT NOT NULL DEFAULT '',"
                   " priority INTEGER NOT NULL DEFAULT 0)");
        db.execute("INSERT INTO web_search_providers"
                   "(name, enabled, priority) VALUES ('duckduckgo', 0, 42)");
        db.close();
    }

    auto store = SettingsStore::for_home(dir);
    const auto chain = store.get_web_search_providers();

    const auto ddg = std::find_if(
        chain.begin(), chain.end(),
        [](const auto& p) { return p.name == "duckduckgo"; });
    CHECK(ddg == chain.end());  // old row gone

    const auto hybrid = std::find_if(
        chain.begin(), chain.end(),
        [](const auto& p) { return p.name == "hybrid"; });
    REQUIRE(hybrid != chain.end());
    CHECK(!hybrid->enabled);          // disabled state carried over
    CHECK(hybrid->priority == 42);    // priority carried over
}

TEST_CASE("parse_bing_html: b_algo blocks, ck/a redirect decode") {
    // Fixture mirrors www.bing.com/search markup. The first result's href
    // is a /ck/a redirect whose u= param is "a1" + base64url of
    // https://example.com/gtk4 ("aHR0cHM6Ly9leGFtcGxlLmNvbS9ndGs0").
    const std::string html = R"HTML(
<html><body><ol id="b_results">
<li class="b_algo" data-id iid=SERP.1>
  <h2><a href="https://www.bing.com/ck/a?!&amp;&amp;p=abc&amp;u=a1aHR0cHM6Ly9leGFtcGxlLmNvbS9ndGs0&amp;ntb=1">
  GTK4 <b>Documentation</b></a></h2>
  <p class="b_lineclamp2">GTK4 is the latest major release.</p>
</li>
<li class="b_algo" data-id iid=SERP.2>
  <h2><a href="https://direct.example.org/md4c">MD4C Markdown parser</a></h2>
</li>
</ol></body></html>)HTML";

    const auto results = parse_bing_html(html, 10);
    REQUIRE(results.size() == 2);
    CHECK(results[0].title == "GTK4 Documentation");
    CHECK(results[0].url == "https://example.com/gtk4");  // ck/a decoded
    CHECK(results[0].snippet == "GTK4 is the latest major release.");
    CHECK(results[1].url == "https://direct.example.org/md4c");  // direct kept
    CHECK(results[1].snippet.empty());

    CHECK(parse_bing_html(html, 1).size() == 1);
    CHECK(parse_bing_html("<html><body>nothing</body></html>", 10).empty());
}
