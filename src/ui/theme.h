#pragma once
#include <SDL2/SDL.h>

namespace UI {

// Matrix/Element-inspired dark theme colors
constexpr SDL_Color COL_BG_DARK    = { 21,  25,  30, 255 };  // #15191e room list
constexpr SDL_Color COL_BG_MED     = { 26,  29,  35, 255 };  // #1a1d23 room list hover row
constexpr SDL_Color COL_BG_MAIN    = { 37,  40,  46, 255 };  // #25282e chat area
constexpr SDL_Color COL_BG_INPUT   = { 45,  49,  57, 255 };  // #2d3139 input box
constexpr SDL_Color COL_BG_HOVER   = { 49,  53,  61, 255 };  // hover state
constexpr SDL_Color COL_BG_SELECT  = { 54,  59,  68, 255 };  // selected room
constexpr SDL_Color COL_ACCENT     = { 13, 189, 139, 255 };  // #0dbd8b matrix green
constexpr SDL_Color COL_ACCENT2    = { 10, 165, 122, 255 };  // accent hover/pressed
constexpr SDL_Color COL_TEXT       = {227, 227, 232, 255 };  // primary text
constexpr SDL_Color COL_TEXT_MUTED = {141, 151, 165, 255 };  // muted / timestamps
constexpr SDL_Color COL_TEXT_LINK  = { 54, 139, 214, 255 };  // #368bd6 links
constexpr SDL_Color COL_ONLINE     = { 13, 189, 139, 255 };  // reuse accent green
constexpr SDL_Color COL_OFFLINE    = {116, 127, 141, 255 };
constexpr SDL_Color COL_MENTION    = {250, 166,  26, 255 };  // highlight/mention
constexpr SDL_Color COL_ERROR      = {255,  75,  85, 255 };  // #ff4b55
constexpr SDL_Color COL_ENCRYPTED  = {141, 151, 165, 255 };  // lock placeholder text
constexpr SDL_Color COL_WHITE      = {255, 255, 255, 255 };
constexpr SDL_Color COL_TRANSPARENT= {  0,   0,   0,   0 };
constexpr SDL_Color COL_BUBBLE     = { 43,  47,  54, 255 };  // message background, between BG_MAIN/BG_INPUT

// Layout (TV: 1280x720 window, rendered at half resolution via SDL logical size)
constexpr int SCREEN_W       = 1280;
constexpr int SCREEN_H       = 720;
constexpr int SCALE          = 2;
constexpr int L_W            = SCREEN_W / SCALE;  // 640 – logical canvas width
constexpr int L_H            = SCREEN_H / SCALE;  // 360 – logical canvas height

// Matrix rooms are flat (no guild/server grouping in v1) — a single room-list
// column replaces Discord's guild-bar + channel-list pair.
constexpr int ROOM_LIST_W    = 130;
constexpr int CHAT_X         = ROOM_LIST_W;
constexpr int CHAT_W         = L_W - CHAT_X;
constexpr int HEADER_H       = 24;
constexpr int INPUT_H        = 28;
constexpr int CHAT_BODY_H    = L_H - HEADER_H - INPUT_H;
constexpr int CHAT_BODY_Y    = HEADER_H;

// Font sizes
constexpr int FONT_SIZE_SM   = 10;
constexpr int FONT_SIZE_MD   = 11;
constexpr int FONT_SIZE_LG   = 14;

// Message layout
constexpr int MSG_AVATAR_W   = 20;
constexpr int MSG_AVATAR_H   = 20;
constexpr int MSG_PADDING    = 8;
constexpr int MSG_INDENT     = MSG_AVATAR_W + 6;

// Room list row layout
constexpr int ROOM_ROW_H     = 28;
constexpr int ROOM_AVATAR_W  = 20;
constexpr int ROOM_AVATAR_H  = 20;
constexpr int ROOM_SELECT_BAR_W = 3;   // left accent bar on the selected row

// Icon sizes
constexpr int ICON_SM        = 10;
constexpr int ICON_MD        = 14;

} // namespace UI
