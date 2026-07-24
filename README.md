# Matrix Wii U (Unofficial)

A [Matrix](https://matrix.org/) protocol chat client for the Nintendo Wii U, running as an [Aroma](https://aroma.foryour.cafe/) homebrew application. Built with SDL2, libcurl, and the Matrix Client-Server API.

> **This is an unofficial, fan-made project.** It is not affiliated with, endorsed by, or supported by the Matrix.org Foundation, New Vector Ltd, or Element. "Matrix" is a trademark of The Matrix.org Foundation C.I.C. — this app just implements their open Client-Server API. Use at your own risk; see [Notes](#notes) for the security tradeoffs made to run on Wii U hardware.

## Features

- Username/password login against any homeserver (matrix.org, a self-hosted Synapse/Dendrite, etc.)
- Real-time messaging via Matrix `/sync` long-polling
- Room list with names, topics, and avatars
- Message history with senders, timestamps, and scroll
- Typing indicators (sent and received)
- On-screen touch keyboard (letters, numbers, symbols)
- Session persisted to SD card — no need to log in again on next launch
- Ships with a bundled font (DejaVu Sans) — works out of the box, no manual font setup required

### Not yet supported

- **Encrypted rooms** — shown in the room list and marked `[E]`, but messages are not decrypted (E2EE/Olm/Megolm is a planned future addition)
- Room invites / joining new rooms / creating rooms
- Reactions, message edits, read receipts, threads, Spaces
- QR code / SSO login

## Requirements

### Build dependencies

[devkitPro](https://devkitpro.org/wiki/Getting_Started) with the following packages:

```bash
(dkp-)pacman -S wut wiiu-sdl2 wiiu-sdl2_ttf wiiu-sdl2_image wiiu-curl wiiu-mbedtls
```

The full library chain pulled in transitively: `harfbuzz`, `freetype`, `libpng`, `libjpeg-turbo`, `libwebp`, `brotli`, `bzip2`, `zlib`.

### Runtime

- Nintendo Wii U running [Aroma CFW](https://aroma.foryour.cafe/)
- SD card
- USB keyboard (optional — the on-screen touch keyboard covers all input)
- A Matrix account on any homeserver

## Building

```bash
export DEVKITPRO=/opt/devkitpro
make
# Produces: matrix-wiiu.wuhb
```

Using Docker:

```bash
docker run --rm -v "$(pwd):/project" devkitpro/devkitppc:latest bash -c \
  "cd /project && \
   (dkp-)pacman -Syu --noconfirm && \
   (dkp-)pacman -S --noconfirm wut wiiu-sdl2 wiiu-sdl2_ttf wiiu-sdl2_image wiiu-curl wiiu-mbedtls && \
   make"
```

## Installation

1. **Copy the app** — place `matrix-wiiu.wuhb` at `SD:/wiiu/apps/matrix-wiiu/matrix-wiiu.wuhb`

2. Launch **Matrix Wii U** from the Aroma Homebrew Launcher.

The app ships with a bundled font ([DejaVu Sans](https://dejavu-fonts.github.io/), Bitstream Vera license — see [LICENSE](LICENSE)), so no font setup is needed. To use a different font instead, place a `.ttf` file at `SD:/wiiu/matrix_wiiu/font.ttf` — the app checks the SD card first and only falls back to the bundled font if that's absent.

### Logging in

On first launch you'll see a login form with three fields: **Homeserver**, **Username**, and **Password**. Use D-Pad Up/Down to move between fields, A to open the keyboard for the highlighted field (or GamePad touch), and A again on the **Log In** button to submit.

- **Homeserver** — just the domain is fine (e.g. `matrix.org`); `https://` is added automatically if omitted.
- **Username** — the local part only (e.g. `alice`, not `@alice:matrix.org`).

On success, your homeserver, access token, and user ID are saved to SD card so you won't need to log in again on future launches.

### SD card layout

```
SD:/
├── wiiu/
│   └── apps/
│       └── matrix-wiiu/
│           └── matrix-wiiu.wuhb
└── wiiu/
    └── matrix_wiiu/
        ├── font.ttf               ← optional — overrides the bundled font if present
        ├── session.txt            ← saved login (keep private — grants account access)
        └── avatars/               ← avatar/room-icon cache (auto-created)
```

> **Warning:** `session.txt` contains an access token with full account access. Keep it off any public repository or shared SD card.

## Controls

### Navigation

| Input | Action |
|-------|--------|
| D-Pad ↑ / ↓ | Move selection up / down |
| D-Pad ← | Focus room list from chat |
| D-Pad → | Focus chat from room list (if a room is open) |
| A | Select room / confirm |
| B | Go back one panel |
| ZL / ZR (hold) | Scroll message history |

### Typing

Press **Y** (or tap the input bar on the GamePad) to open the keyboard.

| Input | Action |
|-------|--------|
| GamePad touch | Type using the on-screen keyboard |
| USB keyboard | Type characters directly |
| **CAPS** | Toggle caps lock |
| **123** | Switch to numbers / punctuation layer |
| **SYM** | Switch to extended symbols layer |
| **ABC** | Return to letters layer |
| **DEL** | Delete one character (UTF-8 aware) |
| **SEND** / USB Enter | Send message (or confirm a login field) |
| B / USB Escape | Cancel and close input |

## Architecture

```
src/
├── main.cpp                    Entry point, system init
├── matrix/
│   ├── types.h                  Room, Member, Event structs
│   ├── json_scan.h              Zero/low-allocation JSON field scanners (see below)
│   ├── net_mutex.{h,cpp}        Global HTTP mutex (one curl transfer at a time)
│   ├── rest.{h,cpp}              Matrix Client-Server API via libcurl
│   ├── sync_worker.{h,cpp}      /sync long-polling loop (replaces a Discord-style gateway)
│   └── client.{h,cpp}           State manager, async action worker, /sync response handling
└── ui/
    ├── theme.h                  Colors, layout constants, font sizes
    ├── renderer.{h,cpp}          SDL2 drawing primitives + SDL_ttf text
    └── app.{h,cpp}               Main loop, all UI panels, touch keyboard
```

### Threading model

| Thread | Responsibility |
|--------|----------------|
| **Main** | SDL render loop, VPAD input, `client.poll()` |
| **Sync worker** | Blocking `GET /sync` long-poll loop + heartbeat-free retry |
| **Action worker** | Async send message / typing indicator / history backfill |
| **Avatar worker** | Background room-icon / member-avatar downloads and decoding |

The sync worker and action worker only perform network I/O and stage raw response bytes; **all JSON parsing happens on the main thread**, using hand-written scanners in `json_scan.h` rather than a general-purpose parser like cJSON. This mirrors a hard-won lesson from the sibling [discord-wii-u](https://github.com/happynico7504/discord-wii-u) project: a JSON parser doing many rapid small allocations races with other threads' allocations on the Wii U's non-thread-safe newlib heap. Every `curl_easy_perform()` call is also serialized through a single global mutex (`net_mutex.h`) for the same reason.

## Notes

- **SSL:** Peer verification is disabled (`CURLOPT_SSL_VERIFYPEER = 0`) for compatibility with the Wii U environment. Be cautious on untrusted networks.
- **Avatar/thumbnail decoding** uses SDL2_image (`IMG_Load_RW`) rather than a hand-rolled PNG decoder, since Matrix media can be PNG or JPEG depending on the homeserver.
- **Sync scope:** each `/sync` response is filtered to the 20 most recent timeline events per room to keep parsing and memory bounded on this hardware; older history loads on demand via `/messages` when you scroll to the top of a room.
- **Fresh sync on every launch:** the app always performs a full initial sync rather than persisting a `since` token across restarts, trading a few seconds of extra load time for simpler, more predictable behavior.

## License

MIT — see [LICENSE](LICENSE).
