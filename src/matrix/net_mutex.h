#pragma once
#include <mutex>

namespace Matrix {
// Every curl_easy_perform() call (REST client, /sync worker, avatar/thumbnail
// worker) must hold this mutex for its full duration.
// mbedTLS on Wii U is not safe for concurrent use across threads, and unlike
// a persistent WebSocket, each Matrix HTTP call does its own connect+handshake
// every time — there is no "steady state" that's safe to leave unguarded.
extern std::mutex g_http_mutex;
}
