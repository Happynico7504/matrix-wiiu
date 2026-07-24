#include "app.h"
#include "../matrix/net_mutex.h"
#include <whb/proc.h>
#include <whb/log.h>
#include <coreinit/time.h>
#include <coreinit/thread.h>
#include <SDL2/SDL_image.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <sys/stat.h>

using namespace Matrix;

namespace UI {

// Font search paths: SD card first, then the font bundled into the WUHB's
// content/ directory. WUT mounts a running app's content path at
// /vol/content/ — "romfs:/" is the 3DS/libctru convention and doesn't exist
// under WUT, so that path was silently never matching.
static const char *FONT_PATHS[] = {
    "/vol/external01/wiiu/matrix_wiiu/font.ttf",
    "/vol/external01/wiiu/font.ttf",
    "/vol/content/font.ttf",
    nullptr
};
static const char *SESSION_PATH = "/vol/external01/wiiu/matrix_wiiu/session.txt";
static const char *AVATAR_CACHE_DIR = "/vol/external01/wiiu/matrix_wiiu/avatars";

// ---- avatar decode helper (file-scope) ---------------------------------------

// Decodes any SDL2_image-supported format (PNG/JPEG/WebP/...) to an RGBA
// surface with a circular alpha mask applied — used for room icons and
// member avatars. Matrix thumbnails can be JPEG or PNG depending on the
// homeserver, unlike Discord's always-PNG CDN, hence SDL2_image over libpng.
static SDL_Surface *decode_avatar(const void *data, size_t size) {
    SDL_RWops *rw = SDL_RWFromConstMem(data, (int)size);
    if (!rw) return nullptr;
    SDL_Surface *raw = IMG_Load_RW(rw, 1); // frees rw
    if (!raw) return nullptr;

    SDL_Surface *surf = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(raw);
    if (!surf) return nullptr;

    SDL_LockSurface(surf);
    int w = surf->w, h = surf->h;
    int cx = w / 2, cy = h / 2, r2 = std::min(cx, cy);
    r2 = r2 * r2;
    for (int py = 0; py < h; py++) {
        uint8_t *row = static_cast<uint8_t *>(surf->pixels) + py * surf->pitch;
        for (int px = 0; px < w; px++) {
            int dx = px - cx, dy = py - cy;
            if (dx * dx + dy * dy > r2) row[px * 4 + 3] = 0;
        }
    }
    SDL_UnlockSurface(surf);
    return surf;
}

// Map SDL keycode + modifier to printable ASCII character.
// Used as fallback when SDL_TEXTINPUT events are not generated (Wii U port).
static char keycode_to_char(SDL_Keycode k, SDL_Keymod mod) {
    bool shift = (mod & KMOD_SHIFT) != 0;
    bool caps  = (mod & KMOD_CAPS)  != 0;
    bool upper = shift ^ caps;
    if (k >= SDLK_a && k <= SDLK_z)
        return upper ? (char)('A' + (k - SDLK_a)) : (char)('a' + (k - SDLK_a));
    if (!shift) {
        if (k >= SDLK_0 && k <= SDLK_9) return (char)k;
        switch (k) {
            case SDLK_SPACE:        return ' ';
            case SDLK_MINUS:        return '-';
            case SDLK_EQUALS:       return '=';
            case SDLK_LEFTBRACKET:  return '[';
            case SDLK_RIGHTBRACKET: return ']';
            case SDLK_BACKSLASH:    return '\\';
            case SDLK_SEMICOLON:    return ';';
            case SDLK_QUOTE:        return '\'';
            case SDLK_COMMA:        return ',';
            case SDLK_PERIOD:       return '.';
            case SDLK_SLASH:        return '/';
            case SDLK_BACKQUOTE:    return '`';
            default: break;
        }
    } else {
        switch (k) {
            case SDLK_1: return '!'; case SDLK_2: return '@';
            case SDLK_3: return '#'; case SDLK_4: return '$';
            case SDLK_5: return '%'; case SDLK_6: return '^';
            case SDLK_7: return '&'; case SDLK_8: return '*';
            case SDLK_9: return '('; case SDLK_0: return ')';
            case SDLK_MINUS:        return '_';
            case SDLK_EQUALS:       return '+';
            case SDLK_LEFTBRACKET:  return '{';
            case SDLK_RIGHTBRACKET: return '}';
            case SDLK_BACKSLASH:    return '|';
            case SDLK_SEMICOLON:    return ':';
            case SDLK_QUOTE:        return '"';
            case SDLK_COMMA:        return '<';
            case SDLK_PERIOD:       return '>';
            case SDLK_SLASH:        return '?';
            case SDLK_BACKQUOTE:    return '~';
            default: break;
        }
    }
    return 0;
}

// ---- lifecycle -----------------------------------------------------------------

App::App() {
    login_fields_ = {
        { "Homeserver", "matrix.org", false },
        { "Username",   "",           false },
        { "Password",   "",           true  },
    };
    wire_callbacks();
}

App::~App() { teardown(); }

void App::wire_callbacks() {
    client_.on_ready = [this]() {
        if (state_ == AppState::LOADING) state_ = AppState::ROOM_LIST;
    };
    client_.on_typing = [this](const Matrix::TypingEvent &ev) {
        if (ev.room_id != client_.state().selected_room_id) return;
        Uint32 expiry = SDL_GetTicks() + 8000;
        typing_users_.clear();
        for (auto &uid : ev.user_ids) {
            if (uid != client_.state().self_user_id) typing_users_[uid] = expiry;
        }
    };
}

bool App::setup_sdl() {
    window_ = SDL_CreateWindow(
        "Matrix Wii U (Unofficial)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_W, SCREEN_H,
        SDL_WINDOW_SHOWN);
    if (!window_) {
        WHBLogPrintf("SDL_CreateWindow: %s", SDL_GetError());
        return false;
    }

    renderer_ = SDL_CreateRenderer(window_, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) {
        WHBLogPrintf("SDL_CreateRenderer: %s", SDL_GetError());
        return false;
    }

    SDL_RenderSetLogicalSize(renderer_, L_W, L_H);
    draw_.init(renderer_);
    return true;
}

bool App::load_fonts(const char *path) {
    draw_.font_sm   = TTF_OpenFont(path, FONT_SIZE_SM);
    draw_.font_md   = TTF_OpenFont(path, FONT_SIZE_MD);
    draw_.font_lg   = TTF_OpenFont(path, FONT_SIZE_LG);
    draw_.font_bold = TTF_OpenFont(path, FONT_SIZE_MD);
    if (draw_.font_bold) TTF_SetFontStyle(draw_.font_bold, TTF_STYLE_BOLD);
    return draw_.font_md != nullptr;
}

// Static UI icons bundled in content/icons/ (see gen_ui_icons.py) — loaded
// once via IMG_LoadTexture, unlike avatars which are downloaded async.
// Missing/failed loads are non-fatal: draw_icon() already no-ops on nullptr.
void App::load_icons() {
    icon_lock_    = IMG_LoadTexture(renderer_, "/vol/content/icons/lock.png");
    icon_send_    = IMG_LoadTexture(renderer_, "/vol/content/icons/send.png");
    icon_chevron_ = IMG_LoadTexture(renderer_, "/vol/content/icons/chevron_right.png");
    icon_brand_   = IMG_LoadTexture(renderer_, "/vol/content/icons/brand.png");
    if (!icon_lock_ || !icon_send_ || !icon_chevron_ || !icon_brand_) {
        WHBLogPrintf("App: one or more UI icons failed to load: %s", IMG_GetError());
    }
    for (SDL_Texture *tex : { icon_lock_, icon_send_, icon_chevron_, icon_brand_ }) {
        if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    }
}

void App::teardown() {
    client_.shutdown();

    avatar_stop_.store(true);
    if (avatar_thread_.joinable()) avatar_thread_.join();

    {
        std::lock_guard<std::mutex> lock(avatar_mutex_);
        while (!avatar_done_.empty()) {
            auto [id, surf] = avatar_done_.front();
            avatar_done_.pop();
            if (surf) SDL_FreeSurface(surf);
        }
    }
    for (auto &[id, tex] : avatar_cache_) {
        if (tex) SDL_DestroyTexture(tex);
    }
    avatar_cache_.clear();

    for (SDL_Texture *tex : { icon_lock_, icon_send_, icon_chevron_, icon_brand_ }) {
        if (tex) SDL_DestroyTexture(tex);
    }
    icon_lock_ = icon_send_ = icon_chevron_ = icon_brand_ = nullptr;

    draw_.destroy();
    if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
    if (window_)   { SDL_DestroyWindow(window_);     window_   = nullptr; }
}

// ---- session persistence -------------------------------------------------------

bool App::load_session() {
    FILE *f = fopen(SESSION_PATH, "r");
    if (!f) return false;

    char line1[256] = {0}, line2[512] = {0}, line3[256] = {0};
    bool ok = fgets(line1, sizeof(line1), f) && fgets(line2, sizeof(line2), f) &&
              fgets(line3, sizeof(line3), f);
    fclose(f);
    if (!ok) return false;

    auto trim = [](char *s) {
        size_t n = strlen(s);
        while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = '\0';
    };
    trim(line1); trim(line2); trim(line3);
    if (!line1[0] || !line2[0] || !line3[0]) return false;

    client_.resume_session(line1, line2, line3);
    return true;
}

void App::save_session() {
    mkdir("/vol/external01/wiiu", 0755);
    mkdir("/vol/external01/wiiu/matrix_wiiu", 0755);
    FILE *f = fopen(SESSION_PATH, "w");
    if (!f) { WHBLogPrint("WARNING: could not save session to SD card"); return; }
    fprintf(f, "%s\n%s\n%s\n",
            client_.homeserver().c_str(), client_.access_token().c_str(), client_.self_user_id().c_str());
    fclose(f);
}

// ---- main run loop -----------------------------------------------------------

void App::run() {
    if (!setup_sdl()) return;

    bool have_session = load_session();

    bool fonts_ok = false;
    for (int i = 0; FONT_PATHS[i]; i++) {
        if (load_fonts(FONT_PATHS[i])) { fonts_ok = true; break; }
    }
    load_icons();

    avatar_stop_.store(false);
    avatar_thread_ = std::thread(&App::avatar_worker, this);

    if (!fonts_ok) {
        WHBLogPrint("No font found. Place font.ttf at /vol/external01/wiiu/matrix_wiiu/font.ttf");
        state_ = AppState::ERROR;
        error_msg_ = "Missing font.ttf\nPlace font at:\n/vol/external01/wiiu/matrix_wiiu/font.ttf";
    } else if (have_session) {
        if (client_.init()) {
            state_ = AppState::LOADING;
        } else {
            state_ = AppState::ERROR;
            error_msg_ = client_.last_error();
        }
    } else {
        state_ = AppState::LOGIN;
    }

    SDL_StopTextInput();  // ensure keyboard is hidden at startup
    last_frame_ = SDL_GetTicks();

    while (WHBProcIsRunning()) {
        update();
        render();

        Uint32 now = SDL_GetTicks();
        int elapsed = (int)(now - last_frame_);
        if (elapsed < 16) SDL_Delay(16 - elapsed);
        last_frame_ = SDL_GetTicks();
    }

    teardown();
}

// ---- update ------------------------------------------------------------------

void App::update() {
    if (state_ == AppState::LOGIN) {
        handle_sdl_events();
        handle_vpad();
        Uint32 now = SDL_GetTicks();
        if (now - cursor_tick_ > 500) { input_cursor_blink_ = !input_cursor_blink_; cursor_tick_ = now; }
        return;
    }

    client_.poll();
    client_.drain_backfill_result();
    drain_avatar_results();

    handle_sdl_events();
    handle_vpad();

    Uint32 now = SDL_GetTicks();
    for (auto it = typing_users_.begin(); it != typing_users_.end(); ) {
        if (now > it->second) it = typing_users_.erase(it);
        else ++it;
    }
    if (now - cursor_tick_ > 500) { input_cursor_blink_ = !input_cursor_blink_; cursor_tick_ = now; }
}

// ---- SDL event handling ------------------------------------------------------

void App::handle_sdl_events() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            WHBProcStopRunning();
            return;
        }

        if (ev.type == SDL_KEYDOWN) {
            SDL_Keycode k  = ev.key.keysym.sym;
            SDL_Keymod  md = (SDL_Keymod)ev.key.keysym.mod;

            if (state_ == AppState::TYPING) {
                std::string &target = active_text_target();
                if (k == SDLK_BACKSPACE) {
                    if (!target.empty()) {
                        size_t len = target.size();
                        while (len > 0 && (target[len-1] & 0xC0) == 0x80) len--;
                        if (len > 0) len--;
                        target.resize(len);
                    }
                } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    confirm_typing();
                } else if (k == SDLK_ESCAPE) {
                    cancel_typing();
                } else {
                    char c = keycode_to_char(k, md);
                    if (c) target += c;
                }
            } else if (state_ == AppState::CHAT) {
                if (k == SDLK_RETURN) enter_typing_for_chat();
                if (k == SDLK_UP)       scroll_messages(3);
                if (k == SDLK_DOWN)     scroll_messages(-3);
                if (k == SDLK_PAGEUP)   scroll_messages(10);
                if (k == SDLK_PAGEDOWN) scroll_messages(-10);
            } else if (state_ == AppState::LOGIN) {
                if (k == SDLK_RETURN) {
                    if (login_focus_ < 3) enter_typing_for_login_field(login_focus_);
                    else attempt_login();
                } else if (k == SDLK_DOWN) {
                    login_focus_ = std::min(3, login_focus_ + 1);
                } else if (k == SDLK_UP) {
                    login_focus_ = std::max(0, login_focus_ - 1);
                }
            }
        }
    }
}

// ---- VPAD (GamePad) input ----------------------------------------------------

void App::handle_vpad() {
    static VPADStatus vpad_prev;
    static bool vpad_first = true;

    VPADStatus vpad;
    VPADReadError err = VPAD_READ_SUCCESS;
    VPADRead(VPAD_CHAN_0, &vpad, 1, &err);
    if (err != VPAD_READ_SUCCESS && err != VPAD_READ_NO_SAMPLES) return;

    if (vpad_first) { vpad_prev = vpad; vpad_first = false; return; }

    uint32_t pressed = vpad.trigger;

    if (state_ == AppState::TYPING) {
        if (pressed & VPAD_BUTTON_B) cancel_typing();
        if (pressed & VPAD_BUTTON_PLUS) confirm_typing();

        VPADTouchData tp;
        VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_1280X720, &tp, &vpad.tpFiltered1);
        bool touching = tp.touched != 0 && tp.validity == VPAD_VALID;
        if (touching && !vpad_touching_) {
            handle_keyboard_touch((int)tp.x / SCALE, (int)tp.y / SCALE);
        }
        vpad_touching_ = touching;

        vpad_prev = vpad;
        return;
    }
    vpad_touching_ = false;

    if (state_ == AppState::LOGIN) {
        if (pressed & VPAD_BUTTON_UP)   login_focus_ = std::max(0, login_focus_ - 1);
        if (pressed & VPAD_BUTTON_DOWN) login_focus_ = std::min(3, login_focus_ + 1);
        if (pressed & VPAD_BUTTON_A) {
            if (login_focus_ < 3) enter_typing_for_login_field(login_focus_);
            else attempt_login();
        }
        vpad_prev = vpad;
        return;
    }

    if (state_ == AppState::LOADING || state_ == AppState::ERROR) { vpad_prev = vpad; return; }

    const auto &rooms = client_.state().rooms;

    if (pressed & VPAD_BUTTON_UP) {
        if (state_ == AppState::ROOM_LIST) room_sel_ = std::max(0, room_sel_ - 1);
        else if (state_ == AppState::CHAT) scroll_messages(3);
    }
    if (pressed & VPAD_BUTTON_DOWN) {
        if (state_ == AppState::ROOM_LIST) room_sel_ = std::min((int)rooms.size() - 1, room_sel_ + 1);
        else if (state_ == AppState::CHAT) scroll_messages(-3);
    }
    if (pressed & VPAD_BUTTON_A) {
        if (state_ == AppState::ROOM_LIST && !rooms.empty()) select_room(room_sel_);
    }
    if (pressed & VPAD_BUTTON_B) {
        if (state_ == AppState::CHAT) state_ = AppState::ROOM_LIST;
    }
    if (pressed & VPAD_BUTTON_Y && state_ == AppState::CHAT) {
        enter_typing_for_chat();
    }

    if (vpad.hold & VPAD_BUTTON_ZL) scroll_messages(3);
    if (vpad.hold & VPAD_BUTTON_ZR) scroll_messages(-3);

    if (pressed & VPAD_BUTTON_LEFT) {
        if (state_ == AppState::CHAT) state_ = AppState::ROOM_LIST;
    }
    if (pressed & VPAD_BUTTON_RIGHT) {
        if (state_ == AppState::ROOM_LIST && !client_.state().selected_room_id.empty())
            state_ = AppState::CHAT;
    }

    vpad_prev = vpad;
}

// ---- navigation / actions ------------------------------------------------------

void App::select_room(int index) {
    const auto &rooms = client_.state().rooms;
    if (index < 0 || index >= (int)rooms.size()) return;

    if (!client_.state().selected_room_id.empty())
        client_.set_typing(client_.state().selected_room_id, false);

    state_ = AppState::CHAT;
    msg_scroll_ = 0;
    at_history_top_ = false;
    typing_users_.clear();
    client_.select_room(rooms[index].id);
}

void App::scroll_messages(int delta) {
    const auto &msgs = client_.state().timeline;
    if (msgs.empty()) return;
    msg_scroll_ = std::clamp(msg_scroll_ + delta, 0, (int)msgs.size() - 1);

    bool at_top = (msg_scroll_ == (int)msgs.size() - 1);
    if (at_top && !at_history_top_) {
        for (auto &r : client_.state().rooms) {
            if (r.id == client_.state().selected_room_id && r.has_more_history) {
                client_.start_backfill(r.id);
                break;
            }
        }
    }
    at_history_top_ = at_top;
}

void App::attempt_login() {
    const std::string &hs   = login_fields_[0].text;
    const std::string &user = login_fields_[1].text;
    const std::string &pass = login_fields_[2].text;
    if (hs.empty() || user.empty() || pass.empty()) {
        error_msg_ = "Please fill in all fields";
        return;
    }

    error_msg_.clear();
    if (!client_.login(hs, user, pass)) {
        error_msg_ = client_.last_error();
        return;
    }

    save_session();
    login_fields_[2].text.clear(); // don't keep the password in memory longer than needed

    if (client_.init()) {
        state_ = AppState::LOADING;
    } else {
        error_msg_ = client_.last_error();
        state_ = AppState::ERROR;
    }
}

void App::send_current_input() {
    if (input_text_.empty()) return;
    const std::string &room = client_.state().selected_room_id;
    if (room.empty()) return;

    client_.send_message(room, input_text_);
    client_.set_typing(room, false);
    input_text_.clear();
    state_ = AppState::CHAT;
    msg_scroll_ = 0;
}

void App::enter_typing_for_chat() {
    state_ = AppState::TYPING;
    typing_login_ = false;
    input_text_.clear();
    kb_caps_ = false;
    kb_layer_ = 0;
    const std::string &room = client_.state().selected_room_id;
    if (!room.empty()) client_.set_typing(room, true);
}

void App::enter_typing_for_login_field(int field_index) {
    state_ = AppState::TYPING;
    typing_login_ = true;
    login_focus_ = field_index;
    kb_caps_ = false;
    kb_layer_ = 0;
}

void App::confirm_typing() {
    if (typing_login_) {
        state_ = AppState::LOGIN;   // just closes the keyboard, doesn't submit the form
        return;
    }
    send_current_input();
}

void App::cancel_typing() {
    if (typing_login_) {
        state_ = AppState::LOGIN;
        return;
    }
    state_ = AppState::CHAT;
    input_text_.clear();
    const std::string &room = client_.state().selected_room_id;
    if (!room.empty()) client_.set_typing(room, false);
}

std::string &App::active_text_target() {
    if (typing_login_) return login_fields_[login_focus_].text;
    return input_text_;
}

// ---- avatar loading ----------------------------------------------------------

void App::request_avatar(const std::string &key, const std::string &mxc_uri) {
    if (mxc_uri.empty()) return;
    avatar_cache_[key] = nullptr; // mark in-progress so we don't re-queue
    std::string url = client_.mxc_to_http(mxc_uri, 64, 64);
    if (url.empty()) return;
    std::lock_guard<std::mutex> lock(avatar_mutex_);
    avatar_queue_.push({key, url});
}

void App::drain_avatar_results() {
    std::queue<std::pair<std::string, SDL_Surface*>> done;
    {
        std::lock_guard<std::mutex> lock(avatar_mutex_);
        std::swap(done, avatar_done_);
    }
    while (!done.empty()) {
        auto [key, surf] = done.front();
        done.pop();
        if (surf) {
            SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer_, surf);
            SDL_FreeSurface(surf);
            if (tex) {
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                avatar_cache_[key] = tex;
            }
        }
    }
}

void App::avatar_worker() {
    mkdir("/vol/external01/wiiu", 0755);
    mkdir("/vol/external01/wiiu/matrix_wiiu", 0755);
    mkdir(AVATAR_CACHE_DIR, 0755);

    // Matrix media thumbnail/download endpoints live under /_matrix/client/
    // and require the same Bearer auth as every other client call (see
    // Client::mxc_to_http) — going through RestClient gets that header and
    // the g_http_mutex locking for free instead of hand-rolling curl here.
    //
    // This thread is started from App::run() before login happens (so it's
    // ready the instant the first avatar is requested), which means
    // client_.homeserver()/access_token() are still empty at that point for
    // a fresh login. Building RestClient here up front baked in those empty
    // values forever (RestClient has no way to change its base URL after
    // construction) — every avatar request silently hit "" + endpoint and
    // failed. Deferred construction below, on the first actual job, is safe
    // because a job can only ever be enqueued after login succeeds (avatars
    // are only requested while rendering ROOM_LIST/CHAT, which requires a
    // populated room list, which requires a completed post-login sync).
    std::unique_ptr<Matrix::RestClient> rest;

    while (!avatar_stop_.load()) {
        std::pair<std::string, std::string> job;
        {
            std::lock_guard<std::mutex> lock(avatar_mutex_);
            if (!avatar_queue_.empty()) {
                job = std::move(avatar_queue_.front());
                avatar_queue_.pop();
            }
        }

        if (job.first.empty()) {
            OSSleepTicks(OSMillisecondsToTicks(50));
            continue;
        }

        // Cached by our internal key (room:<id> / user:<id>), not by content
        // hash — a changed avatar won't be picked up until next relaunch,
        // an acceptable v1 simplification.
        char cache_path[320];
        snprintf(cache_path, sizeof(cache_path), "%s/%s.img", AVATAR_CACHE_DIR, job.first.c_str());
        for (char *p = cache_path; *p; p++) if (*p == ':') *p = '_';

        std::string raw;
        FILE *f = fopen(cache_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            rewind(f);
            if (sz > 0) { raw.resize((size_t)sz); fread(&raw[0], 1, (size_t)sz, f); }
            fclose(f);
        }

        if (raw.empty()) {
            if (!rest) {
                rest = std::make_unique<Matrix::RestClient>(client_.homeserver());
            }
            rest->set_access_token(client_.access_token());

            // job.second is a full mxc_to_http() URL, already rooted at our
            // own homeserver — pass just the path+query part as the endpoint.
            std::string url = job.second;
            std::string endpoint = url;
            size_t path_start = url.find("/_matrix/");
            if (path_start != std::string::npos) endpoint = url.substr(path_start);
            raw = rest->get(endpoint, 15);
            long code = rest->last_http_code();
            if (code != 200) {
                WHBLogPrintf("Avatar: GET %s failed (HTTP %ld)", endpoint.c_str(), code);
                raw.clear();
            }

            {
                std::lock_guard<std::mutex> lock(avatar_mutex_);
                avatar_debug_ = job.first + " -> HTTP " + std::to_string(code)
                              + " (" + std::to_string(raw.size()) + "B) " + endpoint;
            }

            if (!raw.empty()) {
                FILE *wf = fopen(cache_path, "wb");
                if (wf) { fwrite(raw.data(), 1, raw.size(), wf); fclose(wf); }
            }
        }

        SDL_Surface *surf = raw.empty() ? nullptr : decode_avatar(raw.data(), raw.size());
        if (!raw.empty() && !surf) {
            std::lock_guard<std::mutex> lock(avatar_mutex_);
            avatar_debug_ = job.first + " -> downloaded " + std::to_string(raw.size())
                          + "B but decode_avatar() failed";
        }
        {
            std::lock_guard<std::mutex> lock(avatar_mutex_);
            avatar_done_.push({job.first, surf});
        }
    }
}

// ---- render --------------------------------------------------------------------

void App::render() {
    SDL_SetRenderDrawColor(renderer_, COL_BG_MAIN.r, COL_BG_MAIN.g, COL_BG_MAIN.b, 255);
    SDL_RenderClear(renderer_);

    switch (state_) {
        case AppState::LOGIN:
            render_login();
            break;
        case AppState::LOADING:
        case AppState::ERROR:
            render_loading();
            break;
        case AppState::TYPING:
            if (typing_login_) {
                render_login();
                render_keyboard();
            } else {
                render_room_list();
                render_header();
                render_chat();
                render_keyboard();
                render_input_box();
            }
            break;
        default: // ROOM_LIST, CHAT
            render_room_list();
            render_header();
            render_chat();
            render_input_box();
            break;
    }

    // TEMPORARY: on-screen avatar-fetch diagnostic (see avatar_debug_ in
    // app.h) — shows the last avatar HTTP attempt's outcome directly on
    // screen since neither Cemu nor real hardware surface WHBLogPrintf
    // without a separate UDP log listener. Remove once avatars are
    // confirmed working.
    {
        std::string dbg;
        {
            std::lock_guard<std::mutex> lock(avatar_mutex_);
            dbg = avatar_debug_;
        }
        if (!dbg.empty()) {
            while (draw_.text_width(dbg, draw_.font_sm) > L_W - 8 && dbg.size() > 4)
                dbg.resize(dbg.size() - 1);
            draw_.fill_rect(0, L_H - 11, L_W, 11, { 0, 0, 0, 255 });
            draw_.draw_text(4, L_H - 10, dbg, COL_MENTION, draw_.font_sm);
        }
    }

    SDL_RenderPresent(renderer_);
}

// ---- Login screen --------------------------------------------------------------

void App::render_login() {
    draw_.fill_rect(0, 0, L_W, L_H, COL_BG_DARK);

    draw_.fill_rect(0, 0, L_W, HEADER_H, COL_BG_MED);
    {
        const char *title = "Sign in to Matrix";
        int tw = draw_.text_width(title, draw_.font_lg);
        int th = draw_.text_height(draw_.font_lg);
        int brand_sz = HEADER_H - 6;
        int gap = 6;
        int total_w = brand_sz + gap + tw;
        int start_x = (L_W - total_w) / 2;
        draw_.draw_icon(icon_brand_, start_x, (HEADER_H - brand_sz) / 2, brand_sz, brand_sz);
        draw_.draw_text(start_x + brand_sz + gap, (HEADER_H - th) / 2, title, COL_TEXT, draw_.font_lg);
    }
    draw_.fill_rect(0, HEADER_H - 1, L_W, 1, COL_BG_DARK);

    {
        const char *disclaimer = "Unofficial client - not affiliated with Matrix.org";
        int dw = draw_.text_width(disclaimer, draw_.font_sm);
        draw_.draw_text((L_W - dw) / 2, HEADER_H + 4, disclaimer, COL_TEXT_MUTED, draw_.font_sm);
    }

    const int form_w = 280;
    const int form_x = (L_W - form_w) / 2;
    int y = HEADER_H + 4 + draw_.text_height(draw_.font_sm) + 10;

    for (int i = 0; i < 3; i++) {
        bool focused = (login_focus_ == i);
        bool editing = (state_ == AppState::TYPING && typing_login_ && login_focus_ == i);

        draw_.draw_text(form_x, y, login_fields_[i].label, COL_TEXT_MUTED, draw_.font_sm);
        int box_y = y + draw_.text_height(draw_.font_sm) + 2;

        draw_.fill_rounded_rect(form_x, box_y, form_w, 22, 3, focused ? COL_BG_SELECT : COL_BG_INPUT);
        if (focused) draw_.draw_rect(form_x, box_y, form_w, 22, COL_ACCENT, 1);

        std::string display = login_fields_[i].mask
            ? std::string(login_fields_[i].text.size(), '*')
            : login_fields_[i].text;
        if (editing && input_cursor_blink_) display += "|";

        SDL_Rect clip = { form_x + 6, box_y, form_w - 12, 22 };
        SDL_RenderSetClipRect(renderer_, &clip);
        draw_.draw_text(form_x + 6, box_y + (22 - draw_.text_height(draw_.font_md)) / 2,
                        display, COL_TEXT, draw_.font_md);
        SDL_RenderSetClipRect(renderer_, nullptr);

        y = box_y + 22 + 12;
    }

    {
        bool focused = (login_focus_ == 3);
        draw_.fill_rounded_rect(form_x, y, form_w, 26, 4, focused ? COL_ACCENT : COL_BG_INPUT);
        const char *lbl = "Log In";
        int lw = draw_.text_width(lbl, draw_.font_md);
        draw_.draw_text(form_x + (form_w - lw) / 2, y + (26 - draw_.text_height(draw_.font_md)) / 2,
                        lbl, focused ? COL_WHITE : COL_TEXT, draw_.font_md);
        y += 26 + 12;
    }

    if (!error_msg_.empty()) {
        auto lines = draw_.wrap_text(error_msg_, form_w, draw_.font_sm);
        for (auto &line : lines) {
            int ew = draw_.text_width(line, draw_.font_sm);
            draw_.draw_text(form_x + (form_w - ew) / 2, y, line, COL_ERROR, draw_.font_sm);
            y += draw_.text_height(draw_.font_sm) + 2;
        }
    }

    std::string hint = "D-Pad: Move   A: Edit / Submit";
    int hw = draw_.text_width(hint, draw_.font_sm);
    draw_.draw_text((L_W - hw) / 2, L_H - 16, hint, COL_TEXT_MUTED, draw_.font_sm);

    draw_.fill_rect(0, L_H - 4, L_W, 4, COL_ACCENT);
}

// ---- Loading / error screen --------------------------------------------------

void App::render_loading() {
    draw_.fill_rect(0, 0, L_W, L_H, COL_BG_DARK);

    std::string msg = error_msg_.empty() ? "Connecting to Matrix..." : error_msg_;
    int y = L_H / 2 - 15;
    size_t pos = 0;
    while (pos <= msg.size()) {
        size_t nl = msg.find('\n', pos);
        std::string line = (nl == std::string::npos) ? msg.substr(pos) : msg.substr(pos, nl - pos);
        int w = draw_.text_width(line, draw_.font_lg);
        draw_.draw_text((L_W - w) / 2, y, line, error_msg_.empty() ? COL_TEXT : COL_ERROR, draw_.font_lg);
        y += draw_.text_height(draw_.font_lg) + 4;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    draw_.fill_rect(0, L_H - 4, L_W, 4, COL_ACCENT);
}

// ---- Room list -----------------------------------------------------------------

SDL_Color App::user_color(const std::string &id) {
    static const SDL_Color PALETTE[] = {
        {255, 99, 71, 255}, {255, 165, 0, 255}, {144, 238, 144, 255},
        {135, 206, 250, 255}, {221, 160, 221, 255}, {255, 218, 185, 255},
    };
    if (id.empty()) return COL_ACCENT;
    uint32_t hash = 0;
    for (char c : id) hash = hash * 31 + (unsigned char)c;
    return PALETTE[hash % 6];
}

std::string App::sender_display_name(const Matrix::Room *room, const std::string &sender) {
    if (room) {
        auto it = room->members.find(sender);
        if (it != room->members.end() && !it->second.display_name.empty())
            return it->second.display_name;
    }
    return sender;
}

void App::draw_room_item(int x, int y, int w, const Matrix::Room &r, bool selected) {
    if (selected) {
        draw_.fill_rect(x, y, w, ROOM_ROW_H, COL_BG_SELECT);
        draw_.fill_rect(x, y, ROOM_SELECT_BAR_W, ROOM_ROW_H, COL_ACCENT);
    }

    int cx = x + 6 + ROOM_AVATAR_W / 2, cy = y + ROOM_ROW_H / 2;
    auto it = avatar_cache_.find("room:" + r.id);
    if (it != avatar_cache_.end() && it->second) {
        SDL_Rect dst = { x + 6, cy - ROOM_AVATAR_H / 2, ROOM_AVATAR_W, ROOM_AVATAR_H };
        SDL_RenderCopy(renderer_, it->second, nullptr, &dst);
    } else {
        SDL_Color c = user_color(r.id);
        draw_.fill_circle(cx, cy, ROOM_AVATAR_W / 2, c);
        std::string initial = r.name.empty() ? "?" : std::string(1, r.name[0]);
        int iw = draw_.text_width(initial, draw_.font_sm);
        int ih = draw_.text_height(draw_.font_sm);
        draw_.draw_text(cx - iw / 2, cy - ih / 2, initial, COL_WHITE, draw_.font_sm);

        if (it == avatar_cache_.end() && !r.avatar_mxc.empty())
            request_avatar("room:" + r.id, r.avatar_mxc);
    }

    SDL_Color text_col = selected ? COL_TEXT : COL_TEXT_MUTED;
    int text_x = x + 6 + ROOM_AVATAR_W + 6;

    if (r.encrypted) {
        draw_.draw_icon(icon_lock_, text_x, y + (ROOM_ROW_H - ICON_SM) / 2, ICON_SM, ICON_SM, text_col);
        text_x += ICON_SM + 4;
    }

    std::string name = r.name;
    int max_w = w - (text_x - x) - 20;
    while (draw_.text_width(name, draw_.font_sm) > max_w && name.size() > 3)
        name.resize(name.size() - 1);
    draw_.draw_text(text_x, y + (ROOM_ROW_H - draw_.text_height(draw_.font_sm)) / 2,
                    name, text_col, draw_.font_sm);

    if (r.unread_count > 0) {
        std::string badge = r.unread_count > 99 ? "99+" : std::to_string(r.unread_count);
        int bw = draw_.text_width(badge, draw_.font_sm) + 8;
        int bx = x + w - bw - 4, by = y + 4;
        draw_.fill_rounded_rect(bx, by, bw, 14, 7, COL_ACCENT);
        draw_.draw_text(bx + 4, by + 1, badge, COL_WHITE, draw_.font_sm);
    } else {
        draw_.draw_icon(icon_chevron_, x + w - ICON_SM - 6, y + (ROOM_ROW_H - ICON_SM) / 2,
                        ICON_SM, ICON_SM, COL_TEXT_MUTED);
    }
}

void App::render_room_list() {
    draw_.fill_rect(0, 0, ROOM_LIST_W, L_H, COL_BG_DARK);
    draw_.draw_text(6, 6, "Rooms", COL_TEXT, draw_.font_bold);
    draw_.fill_rect(0, HEADER_H - 1, ROOM_LIST_W, 1, COL_BG_MED);

    const auto &rooms = client_.state().rooms;
    int y = HEADER_H + 4;
    int bottom = L_H - 4;
    int visible = (bottom - y) / ROOM_ROW_H;
    int start_i = 0;
    if (room_sel_ >= visible) start_i = room_sel_ - visible + 1;

    if (rooms.empty()) {
        draw_.draw_text(8, y, "Loading rooms...", COL_TEXT_MUTED, draw_.font_sm);
        return;
    }
    for (int i = start_i; i < (int)rooms.size(); i++) {
        if (y + ROOM_ROW_H > bottom) break;
        bool sel = (state_ == AppState::ROOM_LIST && i == room_sel_) ||
                   rooms[i].id == client_.state().selected_room_id;
        draw_room_item(0, y, ROOM_LIST_W, rooms[i], sel);
        y += ROOM_ROW_H;
    }
}

// ---- Header bar --------------------------------------------------------------

void App::render_header() {
    int x = CHAT_X;
    draw_.fill_rect(x, 0, CHAT_W, HEADER_H, COL_BG_MAIN);
    draw_.fill_rect(x, HEADER_H - 1, CHAT_W, 1, COL_BG_DARK);

    const auto &rooms = client_.state().rooms;
    const std::string &sel = client_.state().selected_room_id;
    std::string name = "Select a room";
    std::string topic;
    bool encrypted = false;
    for (auto &r : rooms) {
        if (r.id == sel) {
            name = r.name;
            topic = r.topic;
            encrypted = r.encrypted;
            break;
        }
    }

    std::string hint = "Y:Type  B:Back  ZL/ZR:Scroll";
    int hw = draw_.text_width(hint, draw_.font_sm);

    // Budget name+icon+topic to whatever's left of the hint text (with a
    // small gap), instead of assuming a fixed width — a long room name was
    // previously drawn unclipped and could run straight into the hint text.
    int name_x = x + 8;
    int avail_w = CHAT_W - (name_x - x) - hw - 16;

    if (encrypted) {
        draw_.draw_icon(icon_lock_, name_x, (HEADER_H - ICON_MD) / 2, ICON_MD, ICON_MD, COL_TEXT);
        name_x += ICON_MD + 5;
        avail_w -= ICON_MD + 5;
    }

    while (draw_.text_width(name, draw_.font_bold) > avail_w && name.size() > 4)
        name.resize(name.size() - 1);
    int name_w = draw_.text_width(name, draw_.font_bold);
    draw_.draw_text(name_x, 5, name, COL_TEXT, draw_.font_bold);

    int topic_avail_w = avail_w - name_w - 6;
    if (!topic.empty() && topic_avail_w > 30) {
        std::string t = topic;
        while (draw_.text_width(t, draw_.font_sm) > topic_avail_w && t.size() > 4)
            t.resize(t.size() - 1);
        if (t != topic) t += "...";
        draw_.draw_text(name_x + name_w + 6, 6, t, COL_TEXT_MUTED, draw_.font_sm);
    }

    draw_.draw_text(x + CHAT_W - hw - 4, 8, hint, COL_TEXT_MUTED, draw_.font_sm);
}

// ---- Chat messages -----------------------------------------------------------

static std::string body_for_event(const Matrix::Event &ev, const std::string &dispname) {
    if (ev.msgtype == "m.room.encrypted") return "Encrypted message (not yet supported)";
    if (ev.msgtype == "m.emote")           return "* " + dispname + " " + ev.body;
    if (ev.msgtype == "m.image")           return "[image] " + ev.body;
    if (ev.msgtype == "m.file" || ev.msgtype == "m.video" || ev.msgtype == "m.audio")
        return "[file] " + ev.body;
    return ev.body;
}

void App::draw_message(int x, int &y, const Matrix::Event &ev, bool group) {
    int avail_w = CHAT_W - MSG_PADDING * 2 - MSG_INDENT;

    const Matrix::Room *room = nullptr;
    for (auto &r : client_.state().rooms) {
        if (r.id == ev.room_id) { room = &r; break; }
    }
    std::string dispname = sender_display_name(room, ev.sender);

    if (!group) {
        auto av_it = avatar_cache_.find("user:" + ev.sender);
        if (av_it != avatar_cache_.end() && av_it->second) {
            SDL_Rect dst = { x + MSG_PADDING, y, MSG_AVATAR_W, MSG_AVATAR_H };
            SDL_RenderCopy(renderer_, av_it->second, nullptr, &dst);
        } else {
            SDL_Color acol = user_color(ev.sender);
            draw_.fill_circle(x + MSG_PADDING + MSG_AVATAR_W / 2, y + MSG_AVATAR_H / 2,
                              MSG_AVATAR_W / 2, acol);
            std::string letter = dispname;
            if (!letter.empty() && letter[0] == '@' && letter.size() > 1) letter = letter.substr(1);
            std::string initial = letter.empty() ? "?" : std::string(1, letter[0]);
            int iw = draw_.text_width(initial, draw_.font_sm);
            int ih = draw_.text_height(draw_.font_sm);
            draw_.draw_text(x + MSG_PADDING + MSG_AVATAR_W / 2 - iw / 2,
                            y + MSG_AVATAR_H / 2 - ih / 2, initial, COL_WHITE, draw_.font_sm);

            if (av_it == avatar_cache_.end() && room) {
                auto mit = room->members.find(ev.sender);
                if (mit != room->members.end() && !mit->second.avatar_mxc.empty())
                    request_avatar("user:" + ev.sender, mit->second.avatar_mxc);
            }
        }

        draw_.draw_text(x + MSG_PADDING + MSG_INDENT, y, dispname, user_color(ev.sender), draw_.font_bold);

        char tsbuf[8] = "--:--";
        if (ev.origin_server_ts > 0) {
            time_t t = (time_t)(ev.origin_server_ts / 1000);
            struct tm *g = gmtime(&t);
            if (g) snprintf(tsbuf, sizeof(tsbuf), "%02d:%02d", g->tm_hour, g->tm_min);
        }
        int uw = draw_.text_width(dispname, draw_.font_bold);
        draw_.draw_text(x + MSG_PADDING + MSG_INDENT + uw + 4, y + 1, tsbuf, COL_TEXT_MUTED, draw_.font_sm);
        y += draw_.text_height(draw_.font_bold) + 2;
    }

    std::string body = body_for_event(ev, dispname);
    SDL_Color body_col = (ev.msgtype == "m.room.encrypted") ? COL_ENCRYPTED : COL_TEXT;
    if (!body.empty()) {
        auto lines = draw_.wrap_text(body, avail_w, draw_.font_md);
        int line_h = draw_.text_height(draw_.font_md) + 1;

        // Subtle bubble behind the text block, sized to the widest wrapped
        // line rather than the full chat width — keeps the existing
        // avatar-left / flat-text layout, just visually separates messages.
        // Bounds exactly match the text block's own footprint below (no
        // padding overshoot) so this can't drift from render_chat's
        // separately-computed per-message height used for scroll layout.
        int max_lw = 0;
        for (auto &line : lines) max_lw = std::max(max_lw, draw_.text_width(line, draw_.font_md));
        int bubble_x = x + MSG_PADDING + MSG_INDENT - 6;
        int bubble_w = max_lw + 12;
        int bubble_h = (int)lines.size() * line_h;
        draw_.fill_rounded_rect(bubble_x, y, bubble_w, bubble_h, 6, COL_BUBBLE);

        for (auto &line : lines) {
            draw_.draw_text(x + MSG_PADDING + MSG_INDENT, y, line, body_col, draw_.font_md);
            y += line_h;
        }
    }
}

void App::render_chat() {
    int x = CHAT_X;
    int chat_y = CHAT_BODY_Y;
    int chat_h = CHAT_BODY_H - 4;

    draw_.fill_rect(x, chat_y, CHAT_W, chat_h, COL_BG_MAIN);

    const auto &msgs = client_.state().timeline;
    if (msgs.empty()) {
        std::string empty_msg = client_.state().selected_room_id.empty()
            ? "Select a room to start chatting" : "No messages yet";
        int w = draw_.text_width(empty_msg, draw_.font_md);
        draw_.draw_text(x + (CHAT_W - w) / 2, chat_y + chat_h / 2, empty_msg, COL_TEXT_MUTED, draw_.font_md);
        return;
    }

    const Matrix::Room *room = nullptr;
    for (auto &r : client_.state().rooms) {
        if (r.id == client_.state().selected_room_id) { room = &r; break; }
    }

    int total = (int)msgs.size();
    int end_idx = std::clamp(total - 1 - msg_scroll_, 0, total - 1);

    struct MsgHeight { int idx; int h; bool group; };
    std::vector<MsgHeight> visible;
    int total_h = 0;

    for (int i = end_idx; i >= 0; i--) {
        const auto &ev = msgs[i];
        bool group = i > 0 && msgs[i-1].sender == ev.sender;

        int h = 0;
        if (!group) h += draw_.text_height(draw_.font_bold) + 2 + 4;
        std::string body = body_for_event(ev, sender_display_name(room, ev.sender));
        auto lines = draw_.wrap_text(body, CHAT_W - MSG_PADDING * 2 - MSG_INDENT, draw_.font_md);
        h += (int)lines.size() * (draw_.text_height(draw_.font_md) + 1);

        total_h += h;
        visible.push_back({ i, h, group });
        if (total_h > chat_h + 100) break;
    }
    std::reverse(visible.begin(), visible.end());

    int y = chat_y + (chat_h - total_h);
    SDL_Rect clip = { x, chat_y, CHAT_W, chat_h };
    SDL_RenderSetClipRect(renderer_, &clip);
    for (auto &mh : visible) {
        if (y >= chat_y + chat_h) break;
        draw_message(x, y, msgs[mh.idx], mh.group);
        SDL_RenderSetClipRect(renderer_, &clip);
    }
    SDL_RenderSetClipRect(renderer_, nullptr);

    if (msg_scroll_ > 0) {
        std::string scroll_msg = "^ " + std::to_string(msg_scroll_) + " newer messages ^";
        int sw = draw_.text_width(scroll_msg, draw_.font_sm);
        draw_.fill_rounded_rect(x + (CHAT_W - sw) / 2 - 4, chat_y + chat_h - 14, sw + 8, 11, 2, COL_ACCENT);
        draw_.draw_text(x + (CHAT_W - sw) / 2, chat_y + chat_h - 12, scroll_msg, COL_WHITE, draw_.font_sm);
    }

    if (!typing_users_.empty()) {
        const Matrix::Room *room = nullptr;
        for (auto &r : client_.state().rooms) {
            if (r.id == client_.state().selected_room_id) { room = &r; break; }
        }
        std::string typing_str;
        int count = 0;
        for (auto &[uid, _] : typing_users_) {
            if (count > 0) typing_str += ", ";
            typing_str += sender_display_name(room, uid);
            if (++count >= 3) { typing_str += " and others"; break; }
        }
        typing_str += (count == 1 ? " is typing..." : " are typing...");
        draw_.draw_text(x + MSG_PADDING, chat_y + chat_h - 9, typing_str, COL_TEXT_MUTED, draw_.font_sm);
    }
}

// ---- Virtual keyboard --------------------------------------------------------
// Same three-layer design as the sibling discord-wii-u project (letters /
// numbers+punct / symbols), generalized to write into whichever field is
// currently active (active_text_target()) instead of a single fixed target,
// and to span the full screen width when editing a login field.

static const char * const KB_DATA[3][3][10] = {
    {
        { "q","w","e","r","t","y","u","i","o","p" },
        { "a","s","d","f","g","h","j","k","l", nullptr },
        { "z","x","c","v","b","n","m", nullptr, nullptr, nullptr },
    },
    {
        { "1","2","3","4","5","6","7","8","9","0" },
        { "!","@","#","$","%","^","&","*","(",")" },
        { "-","_","=","+",".",",","?","'","\"","/" },
    },
    {
        { ":",";","~","`","|","\\","<",">","[","]" },
        { "{","}","€","£","¥","©","®","™","→","✓" },
        { "♥","★","←","↑","↓","≠","≤","≥","×","÷" },
    },
};
static const int KB_ROW_LENS[3][3] = {
    { 10, 9, 7 },
    { 10, 10, 10 },
    { 10, 10, 10 },
};
static const int KB_KEY_W   = 44;
static const int KB_KEY_H   = 25;
static const int KB_KEY_GAP = 2;
static const int KB_ROW_H   = KB_KEY_H + KB_KEY_GAP;
static const int KB_SP_W    = 50;
static const int KB_TOTAL_H = 4 * KB_ROW_H + 6;
static const int KB_TOP_Y   = L_H - INPUT_H - KB_TOTAL_H;

static std::string kb_key_str(const char *raw, bool caps, int layer) {
    if (!raw) return {};
    if (layer == 0 && caps && raw[0] >= 'a' && raw[0] <= 'z' && raw[1] == '\0')
        return std::string(1, (char)(raw[0] - 'a' + 'A'));
    return raw;
}

void App::render_keyboard() {
    int kb_x = typing_login_ ? 0 : CHAT_X;
    int kb_w = typing_login_ ? L_W : CHAT_W;

    draw_.fill_rect(kb_x, KB_TOP_Y, kb_w, KB_TOTAL_H, { 28, 29, 33, 255 });

    for (int r = 0; r < 3; r++) {
        int n       = KB_ROW_LENS[kb_layer_][r];
        int total_w = n * KB_KEY_W + (n - 1) * KB_KEY_GAP;
        int start_x = kb_x + (kb_w - total_w) / 2;
        int ky      = KB_TOP_Y + 4 + r * KB_ROW_H;
        for (int i = 0; i < n; i++) {
            std::string label = kb_key_str(KB_DATA[kb_layer_][r][i], kb_caps_, kb_layer_);
            int kx = start_x + i * (KB_KEY_W + KB_KEY_GAP);
            draw_.fill_rounded_rect(kx, ky, KB_KEY_W, KB_KEY_H, 3, { 55, 57, 63, 255 });
            int tw = draw_.text_width(label.c_str(), draw_.font_sm);
            int th = draw_.text_height(draw_.font_sm);
            draw_.draw_text(kx + (KB_KEY_W - tw) / 2, ky + (KB_KEY_H - th) / 2,
                            label.c_str(), COL_TEXT, draw_.font_sm);
        }
    }

    int ky      = KB_TOP_Y + 4 + 3 * KB_ROW_H;
    int space_w = kb_w - 4 * KB_SP_W - 4 * KB_KEY_GAP - 8;
    int kx      = kb_x + 4;

    {
        const char *lbl = (kb_layer_ == 0) ? "CAPS" : "ABC";
        SDL_Color   bg  = (kb_layer_ == 0 && kb_caps_) ? COL_ACCENT : SDL_Color{ 70, 72, 80, 255 };
        draw_.fill_rounded_rect(kx, ky, KB_SP_W, KB_KEY_H, 3, bg);
        int tw = draw_.text_width(lbl, draw_.font_sm), th = draw_.text_height(draw_.font_sm);
        draw_.draw_text(kx + (KB_SP_W - tw) / 2, ky + (KB_KEY_H - th) / 2, lbl, COL_TEXT, draw_.font_sm);
    }
    kx += KB_SP_W + KB_KEY_GAP;

    {
        const char *lbl = (kb_layer_ == 1) ? "SYM" : "123";
        draw_.fill_rounded_rect(kx, ky, KB_SP_W, KB_KEY_H, 3, { 70, 72, 80, 255 });
        int tw = draw_.text_width(lbl, draw_.font_sm), th = draw_.text_height(draw_.font_sm);
        draw_.draw_text(kx + (KB_SP_W - tw) / 2, ky + (KB_KEY_H - th) / 2, lbl, COL_TEXT, draw_.font_sm);
    }
    kx += KB_SP_W + KB_KEY_GAP;

    draw_.fill_rounded_rect(kx, ky, space_w, KB_KEY_H, 3, { 55, 57, 63, 255 });
    {
        int tw = draw_.text_width("SPACE", draw_.font_sm), th = draw_.text_height(draw_.font_sm);
        draw_.draw_text(kx + (space_w - tw) / 2, ky + (KB_KEY_H - th) / 2, "SPACE", COL_TEXT_MUTED, draw_.font_sm);
    }
    kx += space_w + KB_KEY_GAP;

    draw_.fill_rounded_rect(kx, ky, KB_SP_W, KB_KEY_H, 3, { 80, 40, 40, 255 });
    {
        int tw = draw_.text_width("DEL", draw_.font_sm), th = draw_.text_height(draw_.font_sm);
        draw_.draw_text(kx + (KB_SP_W - tw) / 2, ky + (KB_KEY_H - th) / 2, "DEL", COL_TEXT, draw_.font_sm);
    }
    kx += KB_SP_W + KB_KEY_GAP;

    {
        draw_.fill_rounded_rect(kx, ky, KB_SP_W, KB_KEY_H, 3, COL_ACCENT);
        if (typing_login_) {
            const char *lbl = "DONE";
            int tw = draw_.text_width(lbl, draw_.font_sm), th = draw_.text_height(draw_.font_sm);
            draw_.draw_text(kx + (KB_SP_W - tw) / 2, ky + (KB_KEY_H - th) / 2, lbl, COL_WHITE, draw_.font_sm);
        } else {
            int isz = ICON_MD;
            draw_.draw_icon(icon_send_, kx + (KB_SP_W - isz) / 2, ky + (KB_KEY_H - isz) / 2, isz, isz, COL_WHITE);
        }
    }
}

void App::handle_keyboard_touch(int tx, int ty) {
    int kb_x = typing_login_ ? 0 : CHAT_X;
    int kb_w = typing_login_ ? L_W : CHAT_W;
    std::string &target = active_text_target();

    for (int r = 0; r < 3; r++) {
        int n       = KB_ROW_LENS[kb_layer_][r];
        int total_w = n * KB_KEY_W + (n - 1) * KB_KEY_GAP;
        int start_x = kb_x + (kb_w - total_w) / 2;
        int ky      = KB_TOP_Y + 4 + r * KB_ROW_H;
        if (ty < ky || ty >= ky + KB_KEY_H) continue;
        for (int i = 0; i < n; i++) {
            int kx = start_x + i * (KB_KEY_W + KB_KEY_GAP);
            if (tx >= kx && tx < kx + KB_KEY_W) {
                target += kb_key_str(KB_DATA[kb_layer_][r][i], kb_caps_, kb_layer_);
                return;
            }
        }
        return;
    }

    int ky      = KB_TOP_Y + 4 + 3 * KB_ROW_H;
    if (ty < ky || ty >= ky + KB_KEY_H) return;
    int space_w = kb_w - 4 * KB_SP_W - 4 * KB_KEY_GAP - 8;
    int kx      = kb_x + 4;

    if (tx >= kx && tx < kx + KB_SP_W) {
        if (kb_layer_ == 0) kb_caps_ = !kb_caps_;
        else kb_layer_ = 0;
        return;
    }
    kx += KB_SP_W + KB_KEY_GAP;

    if (tx >= kx && tx < kx + KB_SP_W) {
        if      (kb_layer_ == 0) kb_layer_ = 1;
        else if (kb_layer_ == 1) kb_layer_ = 2;
        else                     kb_layer_ = 1;
        return;
    }
    kx += KB_SP_W + KB_KEY_GAP;

    if (tx >= kx && tx < kx + space_w) { target += ' '; return; }
    kx += space_w + KB_KEY_GAP;

    if (tx >= kx && tx < kx + KB_SP_W) {
        if (!target.empty()) {
            size_t len = target.size();
            while (len > 0 && (target[len-1] & 0xC0) == 0x80) len--;
            if (len > 0) len--;
            target.resize(len);
        }
        return;
    }
    kx += KB_SP_W + KB_KEY_GAP;

    if (tx >= kx && tx < kx + KB_SP_W) { confirm_typing(); }
}

// ---- Input box ---------------------------------------------------------------

void App::render_input_box() {
    int x = CHAT_X;
    int y = L_H - INPUT_H;

    draw_.fill_rect(x, y, CHAT_W, INPUT_H, COL_BG_MAIN);

    if (state_ == AppState::TYPING || state_ == AppState::CHAT) {
        int bx = x + 8, by = y + 5, bw = CHAT_W - 16, bh = INPUT_H - 10;
        draw_.fill_rounded_rect(bx, by, bw, bh, 3, COL_BG_INPUT);

        if (state_ == AppState::TYPING && !typing_login_) {
            std::string display = input_text_;
            if (input_cursor_blink_) display += "|";
            SDL_Rect clip = { bx + 4, by, bw - 8, bh };
            SDL_RenderSetClipRect(renderer_, &clip);
            draw_.draw_text(bx + 8, by + (bh - draw_.text_height(draw_.font_md)) / 2,
                            display, COL_TEXT, draw_.font_md);
            SDL_RenderSetClipRect(renderer_, nullptr);
        } else {
            std::string placeholder = "Press Y or Enter to type a message...";
            for (auto &r : client_.state().rooms) {
                if (r.id == client_.state().selected_room_id) { placeholder = "Message " + r.name; break; }
            }
            draw_.draw_text(bx + 8, by + (bh - draw_.text_height(draw_.font_md)) / 2,
                            placeholder, COL_TEXT_MUTED, draw_.font_md);
        }
    }
}

} // namespace UI
