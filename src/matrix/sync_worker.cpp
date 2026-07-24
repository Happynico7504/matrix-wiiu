#include "sync_worker.h"
#include <coreinit/time.h>
#include <coreinit/thread.h>
#include <whb/log.h>

namespace Matrix {

// Inline sync filter, URL-encoded ahead of time: {"room":{"timeline":{"limit":20}}}
// Caps the initial/each sync's per-room timeline size so a large account
// doesn't blow past what Wii U's heap can comfortably hold; deeper history
// is fetched on demand via /messages backfill (see Client::start_backfill).
static const char *FILTER_QS = "%7B%22room%22%3A%7B%22timeline%22%3A%7B%22limit%22%3A20%7D%7D%7D";

// Plain substring scan for the top-level "next_batch" field — deliberately
// not cJSON, so this can run safely on the worker thread (see sync_worker.h).
static std::string extract_next_batch(const std::string &json) {
    static const std::string key = "\"next_batch\":\"";
    size_t pos = json.find(key);
    if (pos == std::string::npos) return {};
    pos += key.size();
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return {};
    return json.substr(pos, end - pos);
}

SyncWorker::SyncWorker(const std::string &base_url, const std::string &access_token)
    : rest_(base_url) {
    rest_.set_access_token(access_token);
}

SyncWorker::~SyncWorker() {
    stop();
}

void SyncWorker::start(EventQueue &queue) {
    stop_ = false;
    thread_ = std::thread(&SyncWorker::loop, this, &queue);
}

void SyncWorker::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

bool SyncWorker::drain_result(std::string &raw_json) {
    std::lock_guard<std::mutex> lock(result_mutex_);
    if (!result_ready_) return false;
    raw_json = std::move(result_raw_json_);
    result_raw_json_.clear();
    result_ready_ = false;
    return true;
}

void SyncWorker::loop(EventQueue *queue) {
    while (!stop_.load()) {
        std::string endpoint = "/_matrix/client/v3/sync?timeout=12000&filter=" + std::string(FILTER_QS);
        if (!since_.empty()) {
            endpoint += "&since=" + url_encode(since_);
        }

        // Server-side timeout is 12s; give the transfer a comfortable margin.
        std::string body = rest_.get(endpoint, 20);
        long code = rest_.last_http_code();

        if (stop_.load()) break;

        if (code == 200 && !body.empty()) {
            std::string next_batch = extract_next_batch(body);
            if (!next_batch.empty()) since_ = next_batch;

            {
                std::lock_guard<std::mutex> lock(result_mutex_);
                result_raw_json_ = std::move(body);
                result_ready_ = true;
            }
            queue->push(SyncEvent{SyncEventType::SYNC_READY, {}});
        } else {
            WHBLogPrintf("Matrix /sync failed (http %ld), retrying in 2s", code);
            queue->push(SyncEvent{SyncEventType::ERROR, "sync request failed"});
            OSSleepTicks(OSMillisecondsToTicks(2000));
        }
    }
}

} // namespace Matrix
