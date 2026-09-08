#pragma once

#include <string>

struct SDL_Window;
struct SDL_Renderer;

bool lire_archive_hevc_nvr(
    const std::string& url,
    SDL_Window* fenetre,
    SDL_Renderer* rendu
);
