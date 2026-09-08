#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;

struct SessionNVR
{
    std::string numero;
    int canal = 0;
    int type = 0;
    std::int64_t debut = 0;
    std::int64_t fin = 0;
};

bool rechercher_archives_nvr(
    const std::string& adresse_nvr,
    const std::string& utilisateur,
    const std::string& mot_de_passe,
    int canaux,
    int types,
    const std::string& date,
    const std::string& debut,
    const std::string& fin,
    std::vector<SessionNVR>& sessions,
    int& total
);

bool lire_session_archive_nvr(
    const std::string& adresse_nvr,
    const std::string& utilisateur,
    const std::string& mot_de_passe,
    const SessionNVR& session,
    SDL_Window* fenetre,
    SDL_Renderer* rendu
);

bool menu_lecture_nvr(
    const std::string& adresse_nvr,
    const std::string& utilisateur,
    const std::string& mot_de_passe,
    SDL_Window* fenetre,
    SDL_Renderer* rendu
);
