#pragma once
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include "types.h"
#include "event_queue.h"
#include "rest.h"
#include "sync_worker.h"

namespace Matrix {

struct ClientState {
    std::string        self_user_id;
    std::vector<Room>  rooms;              // joined rooms, most-recent-activity first
    std::vector<Event> timeline;           // timeline for selected_room_id only
    std::string        selected_room_id;
    bool               ready = false;
};

class Client {
public:
    Client();
    ~Client();

    // Synchronous — call once, before init(), with no other threads running yet.
    // On success, homeserver()/access_token()/self_user_id() are populated.
    bool login(const std::string &homeserver, const std::string &username, const std::string &password);

    // Restores a session persisted from a previous login(), skipping the network call.
    void resume_session(const std::string &homeserver, const std::string &access_token,
                         const std::string &user_id);

    // Starts the /sync worker and action worker threads. Call after login()/resume_session().
    bool init();
    void shutdown();

    // Call every frame from the main thread: drains the event queue and parses
    // any staged /sync response (raw-byte scanners only — see json_scan.h for why).
    void poll();

    // Queue async actions — return immediately, actual REST calls happen on
    // the action worker thread so the UI thread never blocks on the network.
    void send_message(const std::string &room_id, const std::string &body);
    void set_typing(const std::string &room_id, bool typing);
    void start_backfill(const std::string &room_id);
    bool drain_backfill_result();   // call from main thread; returns true if state changed

    void select_room(const std::string &room_id);

    const ClientState &state() const { return state_; }
    ClientState       &state()       { return state_; }

    const std::string &homeserver() const { return homeserver_; }
    const std::string &access_token() const { return access_token_; }
    const std::string &self_user_id() const { return self_user_id_; }
    const std::string &last_error() const { return last_error_; }

    // Resolves an "mxc://server/mediaId" URI to an HTTP(S) thumbnail URL.
    // Returns "" for anything else (already-http URLs, empty input).
    std::string mxc_to_http(const std::string &mxc_uri, int w = 64, int h = 64) const;

    std::function<void()>              on_ready;
    std::function<void(const Event &)> on_room_message;
    std::function<void(const std::string &room_id)> on_room_update;
    std::function<void(const TypingEvent &)> on_typing;

private:
    void handle_sync_json(const std::string &raw_json);
    Room &find_or_create_room(const std::string &room_id);
    void apply_state_event(Room &room, const char *ev_s, const char *ev_e);
    Event parse_timeline_event(const std::string &room_id, const char *ev_s, const char *ev_e);
    void reorder_rooms_by_activity();

    void action_worker();

    std::string homeserver_;
    std::string access_token_;
    std::string self_user_id_;
    std::string last_error_;

    std::unique_ptr<RestClient> rest_;   // main-thread-only: used by login() before any thread starts
    std::unique_ptr<SyncWorker> sync_worker_;
    EventQueue  event_queue_;

    ClientState state_;
    std::mutex  state_mutex_;

    // ---- action worker: single-pending-slot-per-kind, mirrors discord-wii-u's
    // channel_worker design. Each kind is handled by its own local RestClient
    // instance inside action_worker() (own CURL handle, own thread).
    std::thread        action_thread_;
    std::atomic<bool>  action_stop_{false};
    std::mutex         action_mutex_;
    std::string        pending_send_room_;
    std::string        pending_send_body_;
    std::string        pending_backfill_room_;
    std::string        pending_typing_room_;
    int                pending_typing_state_ = -1; // -1 none, 0 stop, 1 start
    long               txn_counter_ = 0;

    std::mutex   backfill_mutex_;
    bool         backfill_ready_ = false;
    std::string  backfill_room_id_;
    std::string  backfill_raw_json_;
};

} // namespace Matrix
