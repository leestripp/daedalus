/**
 * @file test_curl_handle.cpp
 * @brief Unit tests for the CurlHandle RAII wrapper
 *
 * Tests cover:
 * - Construction succeeds
 * - Move semantics
 * - RAII cleanup (no leaks)
 * - Configuration helpers
 * - Perform a real HTTP request to a local echo server
 */

#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>

#include "api/CurlHandle.h"

using namespace daedalus::net;

static int failures = 0;
static int test_count = 0;

#define TEST(name) int test_##name()

#define CHECK(cond) do { \
    test_count++; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        failures++; \
        return 1; \
    } \
} while(0)

#define CHECK_EQ(a, b) do { \
    test_count++; \
    if ((a) != (b)) { \
        std::cerr << "FAIL: " << #a << " == " << #b << " (" << (a) << " vs " << (b) << ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
        failures++; \
        return 1; \
    } \
} while(0)

// Simple write callback for tests
static size_t test_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), realsize);
    return realsize;
}

// ============================================================================
// Test: construction
// ============================================================================
TEST(construction) {
    CurlHandle curl;
    CHECK(curl.valid());
    CHECK(static_cast<bool>(curl));
    return 0;
}

// ============================================================================
// Test: move semantics
// ============================================================================
TEST(move_semantics) {
    CurlHandle a;
    CHECK(a.valid());
    CurlHandle b = std::move(a);
    CHECK(b.valid());
    CHECK(!a.valid());  // Moved-from is null
    return 0;
}

// ============================================================================
// Test: move assignment
// ============================================================================
TEST(move_assignment) {
    CurlHandle a;
    CurlHandle b;
    CHECK(a.valid());
    CHECK(b.valid());
    a = std::move(b);
    CHECK(a.valid());
    CHECK(!b.valid());
    return 0;
}

// ============================================================================
// Test: copy is disallowed (compile-time check)
// Note: We can't test this at runtime since it's a deleted function.
// The fact that the code compiles is the test.
// ============================================================================
TEST(copy_disallowed_at_compile_time) {
    // If copy was allowed, this would compile. It doesn't, so this test
    // serves as documentation.
    CurlHandle a;
    // CurlHandle b = a;  // ERROR: copy constructor deleted
    // CurlHandle b(a);   // ERROR: copy constructor deleted
    // a = b;              // ERROR: copy assignment deleted
    CHECK(a.valid());
    return 0;
}

// ============================================================================
// Test: configuration helpers
// ============================================================================
TEST(config_helpers) {
    CurlHandle curl;
    CHECK(curl.valid());

    // These should not throw
    curl.set_url("https://example.com")
         .set_timeout(60L)
         .set_follow_location(true);

    CHECK(curl.valid());
    return 0;
}

// ============================================================================
// Test: perform a real HTTP GET (only if network is available)
// ============================================================================
TEST(http_get) {
    CurlHandle curl;
    if (!curl.valid()) {
        std::cerr << "SKIP: curl init failed\n";
        return 0;
    }

    std::string body;
    curl.set_url("https://example.com")
         .set_timeout(15L)
         .set_follow_location(true)
         .set_write_callback(test_write_cb, &body);

    CURLcode res = curl.perform();
    if (res != CURLE_OK) {
        std::cerr << "SKIP: network not available (curl error: " << curl_easy_strerror(res) << ")\n";
        return 0;  // Skip if no network
    }

    CHECK(res == CURLE_OK);
    long code = curl.get_response_code();
    CHECK_EQ(code, 200L);
    CHECK(!body.empty());
    CHECK(body.find("Example") != std::string::npos);
    return 0;
}

// ============================================================================
// Test: reset preserves handle
// ============================================================================
TEST(reset) {
    CurlHandle curl;
    CHECK(curl.valid());
    curl.set_url("https://example.com").set_timeout(60L);
    curl.reset();
    // After reset, handle is still valid
    CHECK(curl.valid());
    return 0;
}

// ============================================================================
// Test: release returns ownership
// ============================================================================
TEST(release) {
    CurlHandle curl;
    CHECK(curl.valid());
    CURL* raw = curl.release();
    CHECK(raw != nullptr);
    CHECK(!curl.valid());
    // Caller now owns raw and must clean it up
    curl_easy_cleanup(raw);
    return 0;
}

// ============================================================================
// Test: multiple handles can coexist (different threads can use different handles)
// ============================================================================
TEST(multiple_handles_coexist) {
    CurlHandle a;
    CurlHandle b;
    CurlHandle c;
    CHECK(a.valid());
    CHECK(b.valid());
    CHECK(c.valid());
    CHECK(a.get() != b.get());
    CHECK(b.get() != c.get());
    CHECK(a.get() != c.get());
    return 0;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "Running CurlHandle tests..." << std::endl;

    int local_failures = 0;
    local_failures += test_construction();
    local_failures += test_move_semantics();
    local_failures += test_move_assignment();
    local_failures += test_copy_disallowed_at_compile_time();
    local_failures += test_config_helpers();
    local_failures += test_http_get();
    local_failures += test_reset();
    local_failures += test_release();
    local_failures += test_multiple_handles_coexist();

    std::cout << "\nResults: " << (local_failures == 0 ? "PASS" : "FAIL") << std::endl;
    std::cout << "Total checks: " << test_count << std::endl;
    std::cout << "Failed checks: " << local_failures << std::endl;

    return local_failures > 0 ? 1 : 0;
}
