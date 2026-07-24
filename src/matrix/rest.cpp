#include "rest.h"
#include "net_mutex.h"
#include <cstring>
#include <mutex>
#include <whb/log.h>

namespace Matrix {

std::string url_encode(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

RestClient::RestClient(const std::string &base_url) : base_url_(base_url) {
    curl_ = curl_easy_init();
}

RestClient::~RestClient() {
    if (curl_) curl_easy_cleanup(curl_);
}

size_t RestClient::write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *buf = static_cast<std::string *>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string RestClient::request(const std::string &method,
                                const std::string &endpoint,
                                const std::string &body,
                                long timeout_secs) {
    if (!curl_) return {};

    std::string url = base_url_ + endpoint;
    std::string response;
    std::string content_type = "Content-Type: application/json";

    curl_easy_reset(curl_);

    struct curl_slist *headers = nullptr;
    if (!token_.empty()) {
        std::string auth_header = "Authorization: Bearer " + token_;
        headers = curl_slist_append(headers, auth_header.c_str());
    }
    headers = curl_slist_append(headers, content_type.c_str());
    headers = curl_slist_append(headers, "User-Agent: matrix-wiiu/0.1.0");

    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, timeout_secs);

    if (method == "POST") {
        curl_easy_setopt(curl_, CURLOPT_POST, 1L);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, (long)body.size());
    } else if (method == "PUT") {
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, (long)body.size());
    }

    CURLcode res;
    {
        // Every full request (connect + TLS handshake + transfer) must be
        // serialized — see net_mutex.h for why.
        std::lock_guard<std::mutex> net(g_http_mutex);
        res = curl_easy_perform(curl_);
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &last_http_code_);
    }
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        WHBLogPrintf("REST %s %s failed: %s", method.c_str(), endpoint.c_str(), curl_easy_strerror(res));
        last_http_code_ = 0;
        return {};
    }

    return response;
}

std::string RestClient::get(const std::string &endpoint, long timeout_secs) {
    return request("GET", endpoint, {}, timeout_secs);
}

std::string RestClient::post(const std::string &endpoint, const std::string &body, long timeout_secs) {
    return request("POST", endpoint, body, timeout_secs);
}

std::string RestClient::put(const std::string &endpoint, const std::string &body, long timeout_secs) {
    return request("PUT", endpoint, body, timeout_secs);
}

} // namespace Matrix
