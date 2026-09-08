#include "ui.h"
#include "playback.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <future>
#include <string>
#include <vector>

#include <SDL2/SDL_ttf.h>

namespace
{

constexpr int HAUTEUR_BARRE = 80;
constexpr int MARGE = 8;
constexpr int NB_BOUTONS = 5;

TTF_Font* police = nullptr;
bool ttf_initialise = false;

std::array<SDL_Rect, NB_BOUTONS> rectangles_boutons(
    SDL_Window* fenetre
)
{
    int largeur = 960;
    int hauteur = 620;

    SDL_GetWindowSize(
        fenetre,
        &largeur,
        &hauteur
    );

    const int largeur_disponible =
        std::max(
            1,
            largeur - (NB_BOUTONS + 1) * MARGE
        );

    const int largeur_bouton =
        std::max(
            1,
            largeur_disponible / NB_BOUTONS
        );

    const int y =
        std::max(
            0,
            hauteur - HAUTEUR_BARRE + 34
        );

    const int hauteur_bouton =
        std::max(
            1,
            HAUTEUR_BARRE - 42
        );

    std::array<SDL_Rect, NB_BOUTONS> resultat = {};

    for (int i = 0; i < NB_BOUTONS; ++i)
    {
        resultat[static_cast<std::size_t>(i)] =
        {
            MARGE + i * (largeur_bouton + MARGE),
            y,
            largeur_bouton,
            hauteur_bouton
        };
    }

    return resultat;
}

bool contient(
    const SDL_Rect& rectangle,
    int x,
    int y
)
{
    return
        x >= rectangle.x &&
        x < rectangle.x + rectangle.w &&
        y >= rectangle.y &&
        y < rectangle.y + rectangle.h;
}

void dessiner_texte(
    SDL_Renderer* rendu,
    const std::string& texte,
    int x,
    int y,
    SDL_Color couleur,
    int largeur_max = 0
)
{
    if (!police || texte.empty())
        return;

    SDL_Surface* surface =
        TTF_RenderUTF8_Blended(
            police,
            texte.c_str(),
            couleur
        );

    if (!surface)
        return;

    SDL_Texture* texture =
        SDL_CreateTextureFromSurface(
            rendu,
            surface
        );

    if (texture)
    {
        int largeur = surface->w;
        int hauteur = surface->h;

        if (
            largeur_max > 0 &&
            largeur > largeur_max
        )
        {
            const double facteur =
                static_cast<double>(largeur_max) /
                static_cast<double>(largeur);

            largeur = largeur_max;
            hauteur =
                std::max(
                    1,
                    static_cast<int>(hauteur * facteur)
                );
        }

        const SDL_Rect destination =
        {
            x,
            y,
            largeur,
            hauteur
        };

        SDL_RenderCopy(
            rendu,
            texture,
            nullptr,
            &destination
        );

        SDL_DestroyTexture(texture);
    }

    SDL_FreeSurface(surface);
}

void dessiner_bouton(
    SDL_Renderer* rendu,
    const SDL_Rect& rectangle,
    const std::string& libelle,
    bool actif,
    bool alerte,
    bool autorise
)
{
    SDL_Color fond =
        autorise
            ? SDL_Color{58, 63, 70, 255}
            : SDL_Color{38, 41, 46, 255};

    if (actif)
        fond = SDL_Color{43, 105, 151, 255};

    if (alerte && actif)
        fond = SDL_Color{170, 46, 46, 255};

    SDL_SetRenderDrawColor(
        rendu,
        fond.r,
        fond.g,
        fond.b,
        fond.a
    );

    SDL_RenderFillRect(
        rendu,
        &rectangle
    );

    SDL_SetRenderDrawColor(
        rendu,
        105,
        112,
        122,
        255
    );

    SDL_RenderDrawRect(
        rendu,
        &rectangle
    );

    const SDL_Color couleur =
        autorise
            ? SDL_Color{242, 242, 242, 255}
            : SDL_Color{125, 125, 125, 255};

    int largeur_texte = 0;
    int hauteur_texte = 0;

    if (
        !police ||
        TTF_SizeUTF8(
            police,
            libelle.c_str(),
            &largeur_texte,
            &hauteur_texte
        ) != 0
    )
    {
        return;
    }

    const int x =
        rectangle.x +
        std::max(
            4,
            (rectangle.w - largeur_texte) / 2
        );

    const int y =
        rectangle.y +
        std::max(
            2,
            (rectangle.h - hauteur_texte) / 2
        );

    dessiner_texte(
        rendu,
        libelle,
        x,
        y,
        couleur,
        rectangle.w - 8
    );
}

} // namespace

bool initialiser_interface()
{
    if (ttf_initialise && police)
        return true;

    if (TTF_Init() != 0)
        return false;

    ttf_initialise = true;

    const std::array<const char*, 5> chemins =
    {{
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/opentype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf"
    }};

    for (const char* chemin : chemins)
    {
        police = TTF_OpenFont(chemin, 18);

        if (police)
            break;
    }

    if (!police)
    {
        TTF_Quit();
        ttf_initialise = false;
        return false;
    }

    return true;
}

void fermer_interface()
{
    if (police)
    {
        TTF_CloseFont(police);
        police = nullptr;
    }

    if (ttf_initialise)
    {
        TTF_Quit();
        ttf_initialise = false;
    }
}

int hauteur_interface()
{
    return HAUTEUR_BARRE;
}

void ajuster_fenetre_video(
    SDL_Window* fenetre,
    int largeur_video,
    int hauteur_video
)
{
    if (
        !fenetre ||
        largeur_video <= 0 ||
        hauteur_video <= 0
    )
    {
        return;
    }

    const double facteur_largeur =
        1280.0 /
        static_cast<double>(largeur_video);

    const double facteur_hauteur =
        720.0 /
        static_cast<double>(hauteur_video);

    const double facteur =
        std::min(
            1.0,
            std::min(
                facteur_largeur,
                facteur_hauteur
            )
        );

    const int largeur =
        std::max(
            640,
            static_cast<int>(largeur_video * facteur)
        );

    const int hauteur =
        std::max(
            360,
            static_cast<int>(hauteur_video * facteur)
        );

    SDL_SetWindowSize(
        fenetre,
        largeur,
        hauteur + HAUTEUR_BARRE
    );
}

SDL_Rect zone_video(
    SDL_Window* fenetre,
    int largeur_video,
    int hauteur_video
)
{
    int largeur_fenetre = 960;
    int hauteur_fenetre = 620;

    SDL_GetWindowSize(
        fenetre,
        &largeur_fenetre,
        &hauteur_fenetre
    );

    const int hauteur_disponible =
        std::max(
            1,
            hauteur_fenetre - HAUTEUR_BARRE
        );

    if (
        largeur_video <= 0 ||
        hauteur_video <= 0
    )
    {
        return
        {
            0,
            0,
            largeur_fenetre,
            hauteur_disponible
        };
    }

    const double ratio_video =
        static_cast<double>(largeur_video) /
        static_cast<double>(hauteur_video);

    const double ratio_zone =
        static_cast<double>(largeur_fenetre) /
        static_cast<double>(hauteur_disponible);

    int largeur = largeur_fenetre;
    int hauteur = hauteur_disponible;

    if (ratio_video > ratio_zone)
    {
        hauteur =
            std::max(
                1,
                static_cast<int>(largeur / ratio_video)
            );
    }
    else
    {
        largeur =
            std::max(
                1,
                static_cast<int>(hauteur * ratio_video)
            );
    }

    return
    {
        (largeur_fenetre - largeur) / 2,
        (hauteur_disponible - hauteur) / 2,
        largeur,
        hauteur
    };
}

CommandeInterface commande_interface(
    const SDL_Event& evenement,
    SDL_Window* fenetre
)
{
    if (evenement.type == SDL_QUIT)
        return CommandeInterface::Quitter;

    if (evenement.type == SDL_KEYDOWN)
    {
        switch (evenement.key.keysym.sym)
        {
        case SDLK_1:
            return CommandeInterface::Camera1;
        case SDLK_2:
            return CommandeInterface::Camera2;
        case SDLK_r:
            return CommandeInterface::Enregistrement;
        case SDLK_p:
            return CommandeInterface::Lecture;
        case SDLK_ESCAPE:
            return CommandeInterface::Quitter;
        default:
            return CommandeInterface::Aucune;
        }
    }

    if (
        evenement.type != SDL_MOUSEBUTTONDOWN ||
        evenement.button.button != SDL_BUTTON_LEFT
    )
    {
        return CommandeInterface::Aucune;
    }

    const auto rectangles =
        rectangles_boutons(fenetre);

    const int x = evenement.button.x;
    const int y = evenement.button.y;

    if (contient(rectangles[0], x, y))
        return CommandeInterface::Camera1;
    if (contient(rectangles[1], x, y))
        return CommandeInterface::Camera2;
    if (contient(rectangles[2], x, y))
        return CommandeInterface::Enregistrement;
    if (contient(rectangles[3], x, y))
        return CommandeInterface::Lecture;
    if (contient(rectangles[4], x, y))
        return CommandeInterface::Quitter;

    return CommandeInterface::Aucune;
}

void dessiner_interface(
    SDL_Renderer* rendu,
    SDL_Window* fenetre,
    const EtatInterface& etat
)
{
    int largeur = 960;
    int hauteur = 620;

    SDL_GetWindowSize(
        fenetre,
        &largeur,
        &hauteur
    );

    const SDL_Rect barre =
    {
        0,
        std::max(0, hauteur - HAUTEUR_BARRE),
        largeur,
        HAUTEUR_BARRE
    };

    SDL_SetRenderDrawColor(
        rendu,
        27,
        30,
        34,
        255
    );

    SDL_RenderFillRect(
        rendu,
        &barre
    );

    SDL_SetRenderDrawColor(
        rendu,
        78,
        83,
        91,
        255
    );

    SDL_RenderDrawLine(
        rendu,
        0,
        barre.y,
        largeur,
        barre.y
    );

    std::string statut = etat.message;

    if (statut.empty())
    {
        statut =
            etat.camera_disponible
                ? "Direct - caméra " +
                    std::to_string(etat.camera + 1)
                : "Prêt";
    }

    if (etat.enregistrement)
        statut += "   |   REC";

    dessiner_texte(
        rendu,
        statut,
        MARGE,
        barre.y + 5,
        SDL_Color{230, 230, 230, 255},
        std::max(1, largeur - 2 * MARGE)
    );

    const auto rectangles =
        rectangles_boutons(fenetre);

    dessiner_bouton(
        rendu,
        rectangles[0],
        "Caméra 1",
        etat.camera_disponible && etat.camera == 0,
        false,
        true
    );

    dessiner_bouton(
        rendu,
        rectangles[1],
        "Caméra 2",
        etat.camera_disponible && etat.camera == 1,
        false,
        true
    );

    dessiner_bouton(
        rendu,
        rectangles[2],
        etat.enregistrement ? "Arrêter REC" : "Enregistrer",
        etat.enregistrement,
        true,
        etat.camera_disponible
    );

    dessiner_bouton(
        rendu,
        rectangles[3],
        "Lecture NVR",
        false,
        false,
        etat.lecture_autorisee
    );

    dessiner_bouton(
        rendu,
        rectangles[4],
        "Quitter",
        false,
        false,
        true
    );
}

CommandeInterface attendre_commande_interface(
    SDL_Renderer* rendu,
    SDL_Window* fenetre,
    int camera,
    const std::string& message,
    bool lecture_autorisee
)
{
    while (true)
    {
        SDL_SetRenderDrawColor(
            rendu,
            8,
            10,
            12,
            255
        );

        SDL_RenderClear(rendu);

        EtatInterface etat;
        etat.camera = camera;
        etat.camera_disponible = false;
        etat.enregistrement = false;
        etat.lecture_autorisee = lecture_autorisee;
        etat.message = message;

        dessiner_interface(
            rendu,
            fenetre,
            etat
        );

        SDL_RenderPresent(rendu);

        SDL_Event evenement;

        if (!SDL_WaitEvent(&evenement))
            continue;

        const CommandeInterface commande =
            commande_interface(
                evenement,
                fenetre
            );

        if (commande == CommandeInterface::Enregistrement)
            continue;

        if (
            commande == CommandeInterface::Lecture &&
            !lecture_autorisee
        )
        {
            continue;
        }

        if (commande != CommandeInterface::Aucune)
            return commande;
    }
}

bool afficher_ecran_archives(
    SDL_Renderer* rendu,
    SDL_Window* fenetre,
    const std::string& adresse_nvr,
    const std::string& utilisateur,
    const std::string& mot_de_passe
)
{
    if (!rendu || !fenetre)
        return false;

    const auto date_du_jour_ui = []()
    {
        const std::time_t maintenant =
            std::time(nullptr);

        std::tm locale = {};
        localtime_r(&maintenant, &locale);

        char tampon[32] = {};

        std::strftime(
            tampon,
            sizeof(tampon),
            "%Y-%m-%d",
            &locale
        );

        return std::string(tampon);
    };

    const auto heure_epoch_ui =
        [](std::int64_t valeur)
        {
            const std::time_t temps =
                static_cast<std::time_t>(valeur);

            /*
             * Le lecteur Web officiel du NVR affiche
             * ces valeurs avec getUTCHours().
             * Elles ne doivent donc pas subir le
             * décalage du fuseau horaire du PC.
             */
            std::tm utc = {};
            gmtime_r(&temps, &utc);

            char tampon[32] = {};

            std::strftime(
                tampon,
                sizeof(tampon),
                "%H:%M:%S",
                &utc
            );

            return std::string(tampon);
        };

    const auto nom_type_ui =
        [](int type)
        {
            switch (type)
            {
            case 1: return std::string("Time");
            case 2: return std::string("Motion");
            case 4: return std::string("Sensor");
            case 8: return std::string("Manual");
            default: return std::string("Inconnu");
            }
        };

    const std::string date =
        date_du_jour_ui();

    std::vector<SessionNVR> sessions;
    int total = 0;

    std::string message =
        "Prêt pour la recherche des archives du NVR.";

    bool continuer = true;

    while (continuer)
    {
        int largeur = 960;
        int hauteur = 620;

        SDL_GetWindowSize(
            fenetre,
            &largeur,
            &hauteur
        );

        SDL_SetRenderDrawColor(
            rendu,
            8,
            10,
            12,
            255
        );

        SDL_RenderClear(rendu);

        dessiner_texte(
            rendu,
            "Archives NVR",
            30,
            25,
            SDL_Color{242, 242, 242, 255}
        );

        dessiner_texte(
            rendu,
            "Date : " + date,
            30,
            70,
            SDL_Color{220, 220, 220, 255}
        );

        dessiner_texte(
            rendu,
            "Début : 00:00:00",
            30,
            105,
            SDL_Color{220, 220, 220, 255}
        );

        dessiner_texte(
            rendu,
            "Fin : 23:59:59",
            300,
            105,
            SDL_Color{220, 220, 220, 255}
        );

        dessiner_texte(
            rendu,
            "Caméras : toutes",
            30,
            140,
            SDL_Color{220, 220, 220, 255}
        );

        dessiner_texte(
            rendu,
            "Types : tous",
            300,
            140,
            SDL_Color{220, 220, 220, 255}
        );

        dessiner_texte(
            rendu,
            message,
            30,
            185,
            SDL_Color{180, 210, 235, 255},
            std::max(1, largeur - 60)
        );

        int y = 225;

        const std::size_t nombre_affiche =
            std::min<std::size_t>(
                sessions.size(),
                8
            );

        std::vector<SDL_Rect> zones_sessions;
        zones_sessions.reserve(nombre_affiche);

        for (
            std::size_t i = 0;
            i < nombre_affiche;
            ++i
        )
        {
            const SessionNVR& session =
                sessions[i];

            const std::string ligne =
                std::to_string(i + 1) +
                ". Caméra " +
                std::to_string(session.canal + 1) +
                "   " +
                nom_type_ui(session.type) +
                "   " +
                heure_epoch_ui(session.debut) +
                " -> " +
                heure_epoch_ui(session.fin);

            const SDL_Rect zone_session =
            {
                40,
                y,
                std::max(1, largeur - 80),
                30
            };

            zones_sessions.push_back(
                zone_session
            );

            dessiner_bouton(
                rendu,
                zone_session,
                ligne,
                false,
                false,
                true
            );

            y += 34;
        }

        if (sessions.size() > nombre_affiche)
        {
            dessiner_texte(
                rendu,
                "... autres résultats disponibles",
                45,
                y,
                SDL_Color{170, 170, 170, 255}
            );
        }

        const SDL_Rect bouton_rechercher =
        {
            30,
            std::max(500, hauteur - 70),
            220,
            45
        };

        const SDL_Rect bouton_retour =
        {
            270,
            std::max(500, hauteur - 70),
            180,
            45
        };

        dessiner_bouton(
            rendu,
            bouton_rechercher,
            "Rechercher",
            false,
            false,
            true
        );

        dessiner_bouton(
            rendu,
            bouton_retour,
            "Retour",
            false,
            false,
            true
        );

        SDL_RenderPresent(rendu);

        SDL_Event evenement;

        if (!SDL_WaitEvent(&evenement))
            continue;

        if (evenement.type == SDL_QUIT)
            return false;

        if (
            evenement.type == SDL_KEYDOWN &&
            evenement.key.keysym.sym == SDLK_ESCAPE
        )
        {
            return true;
        }

        if (
            evenement.type != SDL_MOUSEBUTTONDOWN ||
            evenement.button.button != SDL_BUTTON_LEFT
        )
        {
            continue;
        }

        const int x = evenement.button.x;
        const int y_clic = evenement.button.y;

        if (contient(bouton_retour, x, y_clic))
            return true;

        bool session_cliquee = false;

        for (
            std::size_t i = 0;
            i < zones_sessions.size();
            ++i
        )
        {
            if (
                !contient(
                    zones_sessions[i],
                    x,
                    y_clic
                )
            )
            {
                continue;
            }

            session_cliquee = true;

            message =
                "Lecture de l'archive en cours - Échap ou P pour revenir.";

            const bool lecture_ok =
                lire_session_archive_nvr(
                    adresse_nvr,
                    utilisateur,
                    mot_de_passe,
                    sessions[i],
                    fenetre,
                    rendu
                );

            message =
                lecture_ok
                    ? "Retour des archives - choisissez une séquence."
                    : "Erreur pendant la lecture de l'archive.";

            break;
        }

        if (session_cliquee)
            continue;

        if (!contient(bouton_rechercher, x, y_clic))
            continue;

        message =
            "Recherche sur le NVR en cours...";

        sessions.clear();
        total = 0;

        SDL_SetRenderDrawColor(
            rendu,
            8,
            10,
            12,
            255
        );

        SDL_RenderClear(rendu);

        dessiner_texte(
            rendu,
            "Archives NVR",
            30,
            25,
            SDL_Color{242, 242, 242, 255}
        );

        dessiner_texte(
            rendu,
            message,
            30,
            185,
            SDL_Color{180, 210, 235, 255}
        );

        SDL_RenderPresent(rendu);

        auto futur =
            std::async(
                std::launch::async,
                [&]()
                {
                    return rechercher_archives_nvr(
                        adresse_nvr,
                        utilisateur,
                        mot_de_passe,
                        3,
                        15,
                        date,
                        "00:00:00",
                        "23:59:59",
                        sessions,
                        total
                    );
                }
            );

        bool quitter_apres_recherche = false;
        bool fermeture_demandee = false;

        while (
            futur.wait_for(
                std::chrono::milliseconds(0)
            ) != std::future_status::ready
        )
        {
            SDL_Event attente;

            while (SDL_PollEvent(&attente))
            {
                if (attente.type == SDL_QUIT)
                {
                    fermeture_demandee = true;
                    quitter_apres_recherche = true;
                } 
                 

                if (
                    attente.type == SDL_KEYDOWN &&
                    attente.key.keysym.sym == SDLK_ESCAPE
                )
                {
                    quitter_apres_recherche = true;
                }
            }

            SDL_Delay(20);
        }

        const bool succes =
            futur.get();

        if (quitter_apres_recherche)
        return !fermeture_demandee;

        if (!succes)
        {
            sessions.clear();

            message =
                "Erreur pendant la recherche des archives NVR.";
        }
        else if (sessions.empty())
        {
            message =
                "Aucun enregistrement trouvé aujourd'hui.";
        }
        else
        {
            message =
                std::to_string(total) +
                " enregistrement(s) trouvé(s) - "
                "affichage des premiers résultats.";
        }
    }

    return true;
}
