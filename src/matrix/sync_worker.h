#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include "event_queue.h"
#include "rest.h"

namespace Matrix {

// Runs Matrix's /sync long-polling loop on a dedicated thread. This replaces
// the WebSocket gateway a Discord-style client would use: Matrix has no
// persistent socket, just a repeated "GET /sync?since=<token>&timeout=..."
// that blocks server-side until something happens or the timeout elapses.
//
// The worker never parses JSON with cJSON — it only performs the HTTP call
// and a plain substring scan for "next_batch" (needed to drive the next
// request). The full raw response body is staged behind result_mutex_ and
// handed to the main thread via drain_result(); all real parsing happens
// there, single-threaded, to avoid concurrent cJSON allocation races.
class SyncWorker {
public:
    // Owns its own RestClient/CURL handle so it can run concurrently with
    // any other REST activity (e.g. Client's action worker sending messages).
    SyncWorker(const std::string &base_url, const std::string &access_token);
    ~SyncWorker();

    void start(EventQueue &queue);
    void stop();

    // Called by the main thread after receiving a SyncEventType::SYNC_READY.
    // Returns true and fills raw_json if a result was staged; clears the flag.
    bool drain_result(std::string &raw_json);

private:
    void loop(EventQueue *queue);

    RestClient        rest_;
    std::thread        thread_;
    std::atomic<bool>  stop_{false};
    std::string        since_;   // "" until the first successful /sync

    std::mutex   result_mutex_;
    bool         result_ready_ = false;
    std::string  result_raw_json_;
};

} // namespace Matrix
