#include <wut.h>
#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_udp.h>
#include <nn/ac.h>
#include <nsysnet/nssl.h>
#include <curl/curl.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>

#include "ui/app.h"

// Returns 0 on clean exit
static int run_app() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0) {
        WHBLogPrintf("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    if (TTF_Init() < 0) {
        WHBLogPrintf("TTF_Init: %s", TTF_GetError());
        SDL_Quit();
        return 1;
    }
    int img_flags = IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP;
    if ((IMG_Init(img_flags) & img_flags) == 0) {
        WHBLogPrintf("IMG_Init: %s", IMG_GetError());
        // Non-fatal — avatars/thumbnails just won't decode.
    }

    {
        UI::App app;
        app.run(); // handles login / session resume / the whole event loop internally
    }

    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
    return 0;
}

int main(int argc, char **argv) {
    WHBProcInit();
    WHBLogUdpInit();
    WHBLogPrint("Matrix Wii U v1.0.0 starting");

    // Network
    ACInitialize();
    ACConnect();

    // SSL
    NSSLInit();

    // curl
    curl_global_init(CURL_GLOBAL_ALL);

    int ret = run_app();

    curl_global_cleanup();
    NSSLFinish();
    ACFinalize();

    WHBLogPrint("Matrix Wii U exiting");
    WHBLogUdpDeinit();
    WHBProcShutdown();
    return ret;
}
