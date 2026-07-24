#pragma once
#include <string>
#include <curl/curl.h>

namespace Matrix {

// Percent-encodes a string for safe embedding in a URL path segment or query
// value. Matrix room/user/event IDs (e.g. "!abc:matrix.org", "@bob:matrix.org")
// and opaque sync tokens routinely contain characters that must be escaped.
std::string url_encode(const std::string &s);

// Thin wrapper around libcurl for the Matrix Client-Server API.
// All calls are synchronous and internally serialized on g_http_mutex
// (see net_mutex.h) — call from a worker thread if you don't want to block.
class RestClient {
public:
    // base_url e.g. "https://matrix.org" (no trailing slash, no /_matrix suffix).
    explicit RestClient(const std::string &base_url);
    ~RestClient();

    void set_access_token(const std::string &token) { token_ = token; }
    const std::string &base_url() const { return base_url_; }

    // Returns response body; empty string on transport failure.
    // Use last_http_code() to distinguish "empty 200 body" from failure.
    // timeout_secs covers the whole request — callers doing a Matrix /sync
    // long-poll should pass a value comfortably above their `timeout=` query param.
    std::string get(const std::string &endpoint, long timeout_secs = 10);
    std::string post(const std::string &endpoint, const std::string &json_body, long timeout_secs = 10);
    std::string put(const std::string &endpoint, const std::string &json_body, long timeout_secs = 10);

    long last_http_code() const { return last_http_code_; }

private:
    static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata);
    std::string request(const std::string &method,
                        const std::string &endpoint,
                        const std::string &body,
                        long timeout_secs);

    std::string base_url_;
    std::string token_;         // empty before login
    long        last_http_code_ = 0;
    CURL       *curl_ = nullptr;
};

} // namespace Matrix
