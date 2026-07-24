#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <vpad/input.h>
#include "renderer.h"
#include "theme.h"
#include "../matrix/client.h"
#include "../matrix/types.h"

namespace UI {

enum class AppState {
    LOGIN,
    LOADING,
    ROOM_LIST,
    CHAT,
    TYPING,
    ERROR,
};

class App {
public:
    App();
    ~App();

    void run();

private:
    // -- lifecycle --
    bool setup_sdl();
    bool load_fonts(const char *font_path);
    void teardown();
    void wire_callbacks();

    // -- session persistence (SD card) --
    bool load_session();   // true if a session file existed and resume_session() was called
    void save_session();

    // -- main loop --
    void update();
    void render();

    // -- input --
    void handle_sdl_events();
    void handle_vpad();

    // -- per-state render --
    void render_login();
    void render_loading();
    void render_room_list();
    void render_header();
    void render_chat();
    void render_keyboard();
    void render_input_box();

    // -- navigation / actions --
    void select_room(int index);
    void scroll_messages(int delta);
    void attempt_login();
    void send_current_input();
    void enter_typing_for_chat();
    void enter_typing_for_login_field(int field_index);
    void confirm_typing();          // SEND / Enter / Plus
    void cancel_typing();           // B / Escape
    std::string &active_text_target();
    void handle_keyboard_touch(int tx, int ty);

    // -- avatar loading (room icons + member avatars, keyed "room:<id>" / "user:<id>") --
    void request_avatar(const std::string &key, const std::string &mxc_uri);
    void drain_avatar_results();
    void avatar_worker();

    // -- draw helpers --
    SDL_Color user_color(const std::string &id);
    std::string sender_display_name(const Matrix::Room *room, const std::string &sender);
    void draw_room_item(int x, int y, int w, const Matrix::Room &r, bool selected);
    void draw_message(int x, int &y, const Matrix::Event &ev, bool group);

    // SDL
    SDL_Window   *window_   = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    Renderer      draw_;

    // Matrix client — owned directly; starts unauthenticated until login()/resume_session().
    Matrix::Client client_;

    // State
    AppState state_ = AppState::LOGIN;

    // Login form
    struct TextField { std::string label; std::string text; bool mask = false; };
    std::vector<TextField> login_fields_;   // 0=Homeserver 1=Username 2=Password
    int  login_focus_  = 0;                 // 0..2 = fields, 3 = "Log In" button
    bool typing_login_ = false;             // true when TYPING was entered from the login form

    // Chat compose box
    std::string input_text_;
    bool        input_cursor_blink_ = true;
    Uint32      cursor_tick_        = 0;

    // Room list / chat
    int  room_sel_         = 0;
    int  msg_scroll_        = 0;    // how many timeline entries scrolled up from the bottom
    bool at_history_top_    = false;

    // Virtual keyboard state
    bool kb_caps_       = false;
    bool vpad_touching_ = false;
    int  kb_layer_      = 0;   // 0=letters, 1=numbers/punct, 2=symbols

    // Typing indicators for the currently selected room (user_id -> expiry tick)
    std::unordered_map<std::string, Uint32> typing_users_;

    // Error / status text (login failures, missing font, init failures)
    std::string error_msg_;

    // Avatar loading: room icons + member avatars share one worker thread.
    // nullptr texture = in-progress or failed; no map entry = never requested.
    std::unordered_map<std::string, SDL_Texture*>     avatar_cache_;
    std::mutex                                         avatar_mutex_;
    std::queue<std::pair<std::string, std::string>>   avatar_queue_;  // (key, resolved URL)
    std::queue<std::pair<std::string, SDL_Surface*>>  avatar_done_;
    std::thread                                        avatar_thread_;
    std::atomic<bool>                                  avatar_stop_{false};

    Uint32 last_frame_ = 0;
};

} // namespace UI
