#pragma once

#include <string>

#include <SDL2/SDL.h>

enum class CommandeInterface
{
    Aucune,
    Camera1,
    Camera2,
    Enregistrement,
    Lecture,
    Quitter
};

struct EtatInterface
{
    int camera = 0;
    bool camera_disponible = false;
    bool enregistrement = false;
    bool lecture_autorisee = true;
    std::string message;
};

bool initialiser_interface();
void fermer_interface();

int hauteur_interface();

void ajuster_fenetre_video(
    SDL_Window* fenetre,
    int largeur_video,
    int hauteur_video
);

SDL_Rect zone_video(
    SDL_Window* fenetre,
    int largeur_video,
    int hauteur_video
);

CommandeInterface commande_interface(
    const SDL_Event& evenement,
    SDL_Window* fenetre
);

void dessiner_interface(
    SDL_Renderer* rendu,
    SDL_Window* fenetre,
    const EtatInterface& etat
);

CommandeInterface attendre_commande_interface(
    SDL_Renderer* rendu,
    SDL_Window* fenetre,
    int camera,
    const std::string& message,
    bool lecture_autorisee
);

bool afficher_ecran_archives(
    SDL_Renderer* rendu,
    SDL_Window* fenetre,
    const std::string& adresse_nvr,
    const std::string& utilisateur,
    const std::string& mot_de_passe
);
