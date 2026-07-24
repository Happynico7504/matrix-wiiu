FROM devkitpro/devkitppc:latest

RUN dkp-pacman -Syu --noconfirm \
        wut \
        wiiu-sdl2 \
        wiiu-sdl2_ttf \
        wiiu-sdl2_image \
        wiiu-curl \
        wiiu-mbedtls \
    && dkp-pacman -Sc --noconfirm
