#include "client.h"
#include "json_scan.h"
#include <whb/log.h>
#include <coreinit/time.h>
#include <coreinit/thread.h>
#include <algorithm>
#include <cstdio>

namespace Matrix {

// ---- helpers -----------------------------------------------------------------

static std::string normalize_base_url(const std::string &input) {
    std::string s = input;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) start++;
    s = s.substr(start);
    if (s.rfind("http://", 0) != 0 && s.rfind("https://", 0) != 0) {
        s = "https://" + s;
    }
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// ---- lifecycle -----------------------------------------------------------------

Client::Client() {}
Client::~Client() { shutdown(); }

bool Client::login(const std::string &homeserver, const std::string &username, const std::string &password) {
    std::string base = normalize_base_url(homeserver);
    rest_ = std::make_unique<RestClient>(base);

    std::string body = "{\"type\":\"m.login.password\",\"identifier\":{\"type\":\"m.id.user\",\"user\":\""
                      + json_escape(username) + "\"},\"password\":\"" + json_escape(password)
                      + "\",\"initial_device_display_name\":\"Wii U\"}";

    std::string resp = rest_->post("/_matrix/client/v3/login", body);
    long code = rest_->last_http_code();

    if (code != 200) {
        std::string err = json_str_field(resp.c_str(), resp.c_str() + resp.size(), "error");
        last_error_ = !err.empty() ? err : ("login failed (HTTP " + std::to_string(code) + ")");
        WHBLogPrintf("Client: login failed: %s", last_error_.c_str());
        return false;
    }

    std::string token = json_str_field(resp.c_str(), resp.c_str() + resp.size(), "access_token");
    std::string uid   = json_str_field(resp.c_str(), resp.c_str() + resp.size(), "user_id");
    if (token.empty() || uid.empty()) {
        last_error_ = "malformed login response";
        return false;
    }

    homeserver_   = base;
    access_token_ = token;
    self_user_id_ = uid;
    rest_->set_access_token(access_token_);
    state_.self_user_id = self_user_id_;
    WHBLogPrintf("Client: logged in as %s on %s", self_user_id_.c_str(), homeserver_.c_str());
    return true;
}

void Client::resume_session(const std::string &homeserver, const std::string &access_token,
                             const std::string &user_id) {
    homeserver_   = normalize_base_url(homeserver);
    access_token_ = access_token;
    self_user_id_ = user_id;
    state_.self_user_id = self_user_id_;
    rest_ = std::make_unique<RestClient>(homeserver_);
    rest_->set_access_token(access_token_);
}

bool Client::init() {
    if (homeserver_.empty() || access_token_.empty()) {
        last_error_ = "not logged in";
        return false;
    }
    sync_worker_ = std::make_unique<SyncWorker>(homeserver_, access_token_);
    sync_worker_->start(event_queue_);
    action_stop_.store(false);
    action_thread_ = std::thread(&Client::action_worker, this);
    return true;
}

void Client::shutdown() {
    if (sync_worker_) sync_worker_->stop();
    action_stop_.store(true);
    if (action_thread_.joinable()) action_thread_.join();
}

void Client::poll() {
    SyncEvent ev;
    while (event_queue_.try_pop(ev)) {
        if (ev.type == SyncEventType::SYNC_READY) {
            std::string raw;
            if (sync_worker_->drain_result(raw)) {
                handle_sync_json(raw);
            }
        } else if (ev.type == SyncEventType::ERROR) {
            WHBLogPrintf("Client: sync error: %s", ev.message.c_str());
        }
    }
}

// ---- /sync response handling (main thread only; raw scanners, no cJSON — ------
// ---- see json_scan.h for why this matters once worker threads are alive) ------

Room &Client::find_or_create_room(const std::string &room_id) {
    for (auto &r : state_.rooms) if (r.id == room_id) return r;
    Room r;
    r.id   = room_id;
    r.name = room_id; // fallback display name until m.room.name/canonical_alias resolves
    state_.rooms.push_back(std::move(r));
    return state_.rooms.back();
}

void Client::apply_state_event(Room &room, const char *ev_s, const char *ev_e) {
    std::string type = json_str_field(ev_s, ev_e, "type");
    const char *cs = nullptr, *ce = nullptr;
    bool has_content = json_value_bounds(ev_s, ev_e, "content", &cs, &ce);
    if (!has_content) return;

    if (type == "m.room.name") {
        std::string n = json_str_field(cs, ce, "name");
        if (!n.empty()) room.name = n;
    } else if (type == "m.room.topic") {
        room.topic = json_str_field(cs, ce, "topic");
    } else if (type == "m.room.avatar") {
        room.avatar_mxc = json_str_field(cs, ce, "url");
    } else if (type == "m.room.encryption") {
        room.encrypted = true;
    } else if (type == "m.room.canonical_alias") {
        if (room.name.empty() || room.name == room.id) {
            std::string alias = json_str_field(cs, ce, "alias");
            if (!alias.empty()) room.name = alias;
        }
    } else if (type == "m.room.member") {
        std::string user_id = json_str_field(ev_s, ev_e, "state_key");
        if (user_id.empty()) return;
        std::string membership = json_str_field(cs, ce, "membership");
        if (membership == "join" || membership == "invite") {
            Member &m = room.members[user_id];
            m.user_id = user_id;
            std::string dn = json_str_field(cs, ce, "displayname");
            if (!dn.empty()) m.display_name = dn;
            std::string av = json_str_field(cs, ce, "avatar_url");
            if (!av.empty()) m.avatar_mxc = av;
        } else {
            room.members.erase(user_id);
        }
    }
}

Event Client::parse_timeline_event(const std::string &room_id, const char *ev_s, const char *ev_e) {
    Event ev;
    ev.room_id = room_id;
    ev.event_id = json_str_field(ev_s, ev_e, "event_id");
    ev.sender   = json_str_field(ev_s, ev_e, "sender");
    ev.origin_server_ts = (uint64_t)json_int_field(ev_s, ev_e, "origin_server_ts", 0);

    std::string type = json_str_field(ev_s, ev_e, "type");
    if (type == "m.room.encrypted") {
        ev.msgtype = "m.room.encrypted"; // placeholder — E2EE decryption is a future pass
        return ev;
    }

    const char *cs = nullptr, *ce = nullptr;
    if (json_value_bounds(ev_s, ev_e, "content", &cs, &ce)) {
        ev.msgtype = json_str_field(cs, ce, "msgtype");
        ev.body    = json_str_field(cs, ce, "body");
    }
    if (ev.msgtype.empty()) ev.msgtype = "m.text";
    return ev;
}

void Client::reorder_rooms_by_activity() {
    std::sort(state_.rooms.begin(), state_.rooms.end(),
              [](const Room &a, const Room &b) { return a.last_activity_ts > b.last_activity_ts; });
}

void Client::handle_sync_json(const std::string &raw_json) {
    const char *p = raw_json.c_str();
    const char *end = p + raw_json.size();

    const char *rooms_s = nullptr, *rooms_e = nullptr;
    if (json_value_bounds(p, end, "rooms", &rooms_s, &rooms_e)) {
        const char *join_s = nullptr, *join_e = nullptr;
        if (json_value_bounds(rooms_s, rooms_e, "join", &join_s, &join_e)) {
            const char *cursor = join_s;
            ObjectEntry entry;
            while (next_object_entry(&cursor, join_e, entry)) {
                std::string room_id = entry.key;
                const char *robj_s = entry.val_s;
                const char *robj_e = entry.val_e;

                // Collected while state_mutex_ is held below, fired only after
                // it's released — callbacks run UI code that must never be
                // invoked while this (non-recursive) mutex is locked.
                std::vector<Event> new_messages;
                std::vector<TypingEvent> new_typing;

                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    Room &room = find_or_create_room(room_id);
                    bool is_selected = (room_id == state_.selected_room_id);

                    // state events
                    const char *state_s = nullptr, *state_e = nullptr;
                    if (json_value_bounds(robj_s, robj_e, "state", &state_s, &state_e)) {
                        const char *ev_arr_s, *ev_arr_e;
                        if (json_value_bounds(state_s, state_e, "events", &ev_arr_s, &ev_arr_e)) {
                            const char *c2 = ev_arr_s;
                            const char *es, *ee;
                            while (next_array_element(&c2, ev_arr_e, &es, &ee)) {
                                apply_state_event(room, es, ee);
                            }
                        }
                    }

                    // timeline events
                    const char *tl_s = nullptr, *tl_e = nullptr;
                    if (json_value_bounds(robj_s, robj_e, "timeline", &tl_s, &tl_e)) {
                        std::string pb = json_str_field(tl_s, tl_e, "prev_batch");
                        if (!pb.empty() && room.prev_batch.empty()) room.prev_batch = pb;

                        const char *ev_arr_s, *ev_arr_e;
                        if (json_value_bounds(tl_s, tl_e, "events", &ev_arr_s, &ev_arr_e)) {
                            const char *c2 = ev_arr_s;
                            const char *es, *ee;
                            while (next_array_element(&c2, ev_arr_e, &es, &ee)) {
                                // A join/leave that arrives as a timeline event is still state.
                                if (json_has_key(es, ee, "state_key")) {
                                    apply_state_event(room, es, ee);
                                }
                                std::string type = json_str_field(es, ee, "type");
                                if (type == "m.room.message" || type == "m.room.encrypted") {
                                    Event tev = parse_timeline_event(room_id, es, ee);
                                    if (tev.origin_server_ts > room.last_activity_ts)
                                        room.last_activity_ts = tev.origin_server_ts;
                                    // Cache on the room itself — this is what makes history
                                    // from the initial /sync (before any room is selected)
                                    // and from rooms the user isn't currently viewing survive
                                    // until they open that room. See select_room().
                                    room.timeline.push_back(tev);
                                    if (room.timeline.size() > 200)
                                        room.timeline.erase(room.timeline.begin());
                                    if (is_selected) state_.timeline.push_back(tev);
                                    new_messages.push_back(tev);
                                }
                            }
                        }
                    }

                    // authoritative unread count from the server
                    const char *un_s = nullptr, *un_e = nullptr;
                    if (json_value_bounds(robj_s, robj_e, "unread_notifications", &un_s, &un_e)) {
                        room.unread_count = (int)json_int_field(un_s, un_e, "notification_count", room.unread_count);
                    }

                    // ephemeral (typing)
                    const char *eph_s = nullptr, *eph_e = nullptr;
                    if (json_value_bounds(robj_s, robj_e, "ephemeral", &eph_s, &eph_e)) {
                        const char *ev_arr_s, *ev_arr_e;
                        if (json_value_bounds(eph_s, eph_e, "events", &ev_arr_s, &ev_arr_e)) {
                            const char *c2 = ev_arr_s;
                            const char *es, *ee;
                            while (next_array_element(&c2, ev_arr_e, &es, &ee)) {
                                std::string type = json_str_field(es, ee, "type");
                                if (type != "m.typing") continue;
                                const char *cs, *ce;
                                if (!json_value_bounds(es, ee, "content", &cs, &ce)) continue;
                                const char *arr_s, *arr_e;
                                TypingEvent tev;
                                tev.room_id = room_id;
                                if (json_value_bounds(cs, ce, "user_ids", &arr_s, &arr_e)) {
                                    const char *c3 = arr_s;
                                    const char *us, *ue;
                                    while (next_array_element(&c3, arr_e, &us, &ue)) {
                                        const char *sp = us;
                                        tev.user_ids.push_back(decode_json_string(sp, ue));
                                    }
                                }
                                new_typing.push_back(std::move(tev));
                            }
                        }
                    }
                } // state_mutex_ released here

                for (auto &tev : new_messages) if (on_room_message) on_room_message(tev);
                for (auto &tev : new_typing)   if (on_typing) on_typing(tev);
                if (on_room_update) on_room_update(room_id);
            }
        }
        // rooms.invite / rooms.leave are intentionally ignored in v1 — see plan's
        // deferred-work list (no invite/join-new-room flow yet).
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        reorder_rooms_by_activity();
    }

    if (!state_.ready) {
        state_.ready = true;
        if (on_ready) on_ready();
    }
}

void Client::select_room(const std::string &room_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (state_.selected_room_id == room_id) return;
    state_.selected_room_id = room_id;
    state_.timeline.clear();
    for (auto &r : state_.rooms) {
        if (r.id == room_id) {
            r.unread_count = 0;
            state_.timeline = r.timeline; // restore this room's cached history
            break;
        }
    }
}

std::string Client::mxc_to_http(const std::string &mxc_uri, int w, int h) const {
    if (mxc_uri.rfind("mxc://", 0) != 0) return {};
    std::string rest = mxc_uri.substr(6); // server/mediaId
    size_t slash = rest.find('/');
    if (slash == std::string::npos) return {};
    std::string server   = rest.substr(0, slash);
    std::string media_id = rest.substr(slash + 1);
    char qs[64];
    snprintf(qs, sizeof(qs), "?width=%d&height=%d&method=crop", w, h);
    return homeserver_ + "/_matrix/media/v3/thumbnail/" + url_encode(server) + "/" + url_encode(media_id) + qs;
}

// ---- async actions (action worker thread) --------------------------------------

void Client::send_message(const std::string &room_id, const std::string &body) {
    std::lock_guard<std::mutex> lock(action_mutex_);
    pending_send_room_ = room_id;
    pending_send_body_ = body;
}

void Client::set_typing(const std::string &room_id, bool typing) {
    std::lock_guard<std::mutex> lock(action_mutex_);
    pending_typing_room_  = room_id;
    pending_typing_state_ = typing ? 1 : 0;
}

void Client::start_backfill(const std::string &room_id) {
    std::lock_guard<std::mutex> lock(action_mutex_);
    pending_backfill_room_ = room_id;
}

bool Client::drain_backfill_result() {
    std::string room_id, raw;
    {
        std::lock_guard<std::mutex> lock(backfill_mutex_);
        if (!backfill_ready_) return false;
        backfill_ready_ = false;
        room_id = std::move(backfill_room_id_);
        raw     = std::move(backfill_raw_json_);
        backfill_room_id_.clear();
        backfill_raw_json_.clear();
    }

    const char *p = raw.c_str();
    const char *end = p + raw.size();
    std::string end_token = json_str_field(p, end, "end");

    std::vector<Event> older;
    const char *arr_s = nullptr, *arr_e = nullptr;
    if (json_value_bounds(p, end, "chunk", &arr_s, &arr_e)) {
        const char *c = arr_s;
        const char *es, *ee;
        while (next_array_element(&c, arr_e, &es, &ee)) {
            std::string type = json_str_field(es, ee, "type");
            if (type == "m.room.message" || type == "m.room.encrypted") {
                older.push_back(parse_timeline_event(room_id, es, ee));
            }
        }
    }
    // dir=b returns newest-first; flip to chronological order before prepending.
    std::reverse(older.begin(), older.end());

    std::lock_guard<std::mutex> lock(state_mutex_);
    for (auto &r : state_.rooms) {
        if (r.id == room_id) {
            r.has_more_history = !older.empty();
            if (!end_token.empty()) r.prev_batch = end_token;
            if (!older.empty()) r.timeline.insert(r.timeline.begin(), older.begin(), older.end());
            break;
        }
    }
    if (room_id == state_.selected_room_id && !older.empty()) {
        state_.timeline.insert(state_.timeline.begin(), older.begin(), older.end());
    }
    return true;
}

void Client::action_worker() {
    RestClient bg_rest(homeserver_);
    bg_rest.set_access_token(access_token_);

    while (!action_stop_.load()) {
        std::string send_room, send_body, backfill_room, typing_room;
        int typing_state = -1;
        {
            std::lock_guard<std::mutex> lock(action_mutex_);
            if (!pending_send_room_.empty()) {
                send_room = std::move(pending_send_room_);
                send_body = std::move(pending_send_body_);
                pending_send_room_.clear();
            } else if (pending_typing_state_ >= 0) {
                typing_room   = std::move(pending_typing_room_);
                typing_state  = pending_typing_state_;
                pending_typing_state_ = -1;
            } else if (!pending_backfill_room_.empty()) {
                backfill_room = std::move(pending_backfill_room_);
                pending_backfill_room_.clear();
            }
        }

        if (!send_room.empty()) {
            std::string body = "{\"msgtype\":\"m.text\",\"body\":\"" + json_escape(send_body) + "\"}";
            std::string txn = std::to_string(++txn_counter_) + "-" + std::to_string((long long)OSGetTime());
            std::string ep = "/_matrix/client/v3/rooms/" + url_encode(send_room)
                            + "/send/m.room.message/" + url_encode(txn);
            bg_rest.put(ep, body);
            if (bg_rest.last_http_code() != 200) {
                WHBLogPrintf("Client: send_message failed (HTTP %ld)", bg_rest.last_http_code());
            }
        } else if (typing_state >= 0) {
            std::string ep = "/_matrix/client/v3/rooms/" + url_encode(typing_room)
                            + "/typing/" + url_encode(self_user_id_);
            std::string body = typing_state ? "{\"typing\":true,\"timeout\":8000}" : "{\"typing\":false}";
            bg_rest.put(ep, body);
        } else if (!backfill_room.empty()) {
            std::string from;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                for (auto &r : state_.rooms) {
                    if (r.id == backfill_room) { from = r.prev_batch; break; }
                }
            }
            if (!from.empty()) {
                std::string ep = "/_matrix/client/v3/rooms/" + url_encode(backfill_room)
                                + "/messages?dir=b&limit=30&from=" + url_encode(from);
                std::string json = bg_rest.get(ep);
                if (bg_rest.last_http_code() == 200 && !json.empty()) {
                    std::lock_guard<std::mutex> lock(backfill_mutex_);
                    backfill_room_id_  = backfill_room;
                    backfill_raw_json_ = std::move(json);
                    backfill_ready_    = true;
                }
            }
        } else {
            OSSleepTicks(OSMillisecondsToTicks(50));
        }
    }
}

} // namespace Matrix
