#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace Matrix {

struct Member {
    std::string user_id;
    std::string display_name;   // empty = fall back to user_id
    std::string avatar_mxc;     // mxc:// URI; empty = no avatar
};

// A message/timeline event. Encrypted events are represented with
// msgtype == "m.room.encrypted" and rendered as a placeholder — the seam
// for a future E2EE (Olm/Megolm) decryption pass.
struct Event {
    std::string event_id;
    std::string room_id;
    std::string sender;
    std::string body;
    std::string msgtype;        // "m.text", "m.image", "m.file", "m.room.encrypted", ...
    uint64_t    origin_server_ts = 0;
    bool        redacted = false;
};

struct Room {
    std::string id;
    std::string name;           // resolved m.room.name, or fallback
    std::string topic;
    std::string avatar_mxc;
    bool        encrypted = false;
    int         unread_count = 0;
    std::string prev_batch;     // token for /messages backfill (oldest known point)
    bool        has_more_history = true;
    uint64_t    last_activity_ts = 0;  // most recent timeline event ts, for room-list ordering
    std::unordered_map<std::string, Member> members;

    // This room's own message cache, independent of which room is currently
    // selected — populated from every /sync response (including the initial
    // one, before any room has ever been selected) and from /messages
    // backfill. ClientState::timeline is just a live copy of whichever
    // room's cache is selected; see Client::select_room / handle_sync_json.
    std::vector<Event> timeline;
};

struct TypingEvent {
    std::string room_id;
    std::vector<std::string> user_ids;
};

} // namespace Matrix
