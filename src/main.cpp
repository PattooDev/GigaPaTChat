#include <chrono>
#include <cstdio>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

#include <openssl/evp.h>
#include <librtmp/amf.h>
#include <librtmp/log.h>
#include <librtmp/rtmp.h>

volatile std::sig_atomic_t programme_actif = 1;

void arreter_programme(int)
{
    programme_actif = 0;
}

std::string md5(const std::string& texte)
{
    unsigned char resultat[EVP_MAX_MD_SIZE];
    unsigned int longueur = 0;

    EVP_MD_CTX* contexte = EVP_MD_CTX_new();

    if (!contexte)
    {
        return "";
    }

    const bool succes =
        EVP_DigestInit_ex(contexte, EVP_md5(), nullptr) == 1 &&
        EVP_DigestUpdate(
            contexte,
            texte.data(),
            texte.size()
        ) == 1 &&
        EVP_DigestFinal_ex(
            contexte,
            resultat,
            &longueur
        ) == 1;

    EVP_MD_CTX_free(contexte);

    if (!succes)
    {
        return "";
    }

    std::ostringstream sortie;
    sortie << std::hex << std::setfill('0');

    for (unsigned int i = 0; i < longueur; ++i)
    {
        sortie << std::setw(2)
               << static_cast<int>(resultat[i]);
    }

    return sortie.str();
}

bool attendre_challenge(
    RTMP* rtmp,
    std::string& challenge
)
{
    RTMPPacket paquet = {0};

    while (
        programme_actif &&
        RTMP_IsConnected(rtmp) &&
        RTMP_ReadPacket(rtmp, &paquet)
    )
    {
        if (!RTMPPacket_IsReady(&paquet))
        {
            continue;
        }

        bool trouve = false;

        if (
            paquet.m_packetType == RTMP_PACKET_TYPE_INVOKE &&
            paquet.m_nBodySize > 0
        )
        {
            const std::string contenu(
                paquet.m_body,
                paquet.m_nBodySize
            );

            const std::string marqueur = "challenge=";
            const std::size_t position =
                contenu.find(marqueur);

            if (
                position != std::string::npos &&
                position + marqueur.size() + 32 <=
                    contenu.size()
            )
            {
                challenge = contenu.substr(
                    position + marqueur.size(),
                    32
                );

                trouve = true;
            }
        }

        if (!trouve)
        {
            RTMP_ClientPacket(rtmp, &paquet);
        }

        RTMPPacket_Free(&paquet);

        if (trouve)
        {
            return true;
        }
    }

    return false;
}

bool envoyer_login(
    RTMP* rtmp,
    const std::string& methode
)
{
    RTMPPacket paquet = {0};
    char tampon[2048];
    char* fin = tampon + sizeof(tampon);

    AVal nom_methode;
    nom_methode.av_val =
        const_cast<char*>(methode.c_str());
    nom_methode.av_len =
        static_cast<int>(methode.size());

    paquet.m_nChannel = 0x03;
    paquet.m_headerType =
        RTMP_PACKET_SIZE_MEDIUM;
    paquet.m_packetType =
        RTMP_PACKET_TYPE_INVOKE;
    paquet.m_nTimeStamp = 0;
    paquet.m_nInfoField2 = 0;
    paquet.m_hasAbsTimestamp = 0;
    paquet.m_body =
        tampon + RTMP_MAX_HEADER_SIZE;

    char* encodage = paquet.m_body;

    encodage = AMF_EncodeString(
        encodage,
        fin,
        &nom_methode
    );

    if (!encodage)
    {
        return false;
    }

    encodage = AMF_EncodeNumber(
        encodage,
        fin,
        ++rtmp->m_numInvokes
    );

    if (!encodage || encodage >= fin)
    {
        return false;
    }

    *encodage++ = AMF_NULL;

    paquet.m_nBodySize =
        encodage - paquet.m_body;

    return RTMP_SendPacket(
        rtmp,
        &paquet,
        TRUE
    ) != 0;
}

bool attendre_reponse_login(RTMP* rtmp)
{
    RTMPPacket paquet = {0};

    while (
        programme_actif &&
        RTMP_IsConnected(rtmp) &&
        RTMP_ReadPacket(rtmp, &paquet)
    )
    {
        if (!RTMPPacket_IsReady(&paquet))
        {
            continue;
        }

        bool succes = false;
        bool erreur = false;

        if (
            paquet.m_packetType == RTMP_PACKET_TYPE_INVOKE &&
            paquet.m_nBodySize > 0
        )
        {
            const std::string contenu(
                paquet.m_body,
                paquet.m_nBodySize
            );

            succes =
                contenu.find("_result") !=
                std::string::npos;

            erreur =
                contenu.find("_error") !=
                std::string::npos;
        }

        RTMP_ClientPacket(rtmp, &paquet);
        RTMPPacket_Free(&paquet);

        if (succes)
        {
            return true;
        }

        if (erreur)
        {
            return false;
        }
    }

    return false;
}

bool lire_camera(
    int numero_camera,
    const std::string& utilisateur,
    const std::string& mot_de_passe,
    FILE* lecteur,
    bool premier_flux
)
{
    const std::string serveur =
        "rtmp://192.168.1.114:80/";

    const std::string flux =
        "ch" +
        std::to_string(numero_camera) +
        "_1.264";

    const std::string nonce_initial = "";

    const std::string digest_initial =
        md5(nonce_initial + ":" + mot_de_passe);

    const std::string configuration =
        serveur +
        " playpath=" + flux +
        " live=1"
        " conn=N:100"
        " conn=S:" + nonce_initial +
        " conn=S:" + utilisateur +
        " conn=S:" + digest_initial;

    RTMP* rtmp = RTMP_Alloc();

    if (!rtmp)
    {
        std::cerr
            << "Erreur : allocation RTMP impossible.\n";
        return false;
    }

    RTMP_Init(rtmp);
    RTMP_LogSetLevel(RTMP_LOGERROR);

    char url[1024];

    std::snprintf(
        url,
        sizeof(url),
        "%s",
        configuration.c_str()
    );

    std::cout
        << "\nConnexion à la caméra "
        << numero_camera + 1
        << "...\n";

    if (!RTMP_SetupURL(rtmp, url))
    {
        std::cerr
            << "Erreur : préparation RTMP impossible.\n";
        RTMP_Free(rtmp);
        return false;
    }

    rtmp->Link.timeout = 5;
    RTMP_SetBufferMS(rtmp, 3000);

    if (!RTMP_Connect(rtmp, nullptr))
    {
        std::cerr
            << "Erreur : connexion RTMP impossible.\n";
        RTMP_Close(rtmp);
        RTMP_Free(rtmp);
        return false;
    }

    std::string challenge;

    if (!attendre_challenge(rtmp, challenge))
    {
        std::cerr
            << "Erreur : challenge introuvable.\n";
        RTMP_Close(rtmp);
        RTMP_Free(rtmp);
        return false;
    }

    const std::string digest =
        md5(challenge + ":" + mot_de_passe);

    const std::string methode_login =
        "login?method=md5"
        "&nonce=" + challenge +
        "&username=" + utilisateur +
        "&digest=" + digest;

    if (
        !envoyer_login(rtmp, methode_login) ||
        !attendre_reponse_login(rtmp)
    )
    {
        std::cerr
            << "Erreur : authentification refusée.\n";
        RTMP_Close(rtmp);
        RTMP_Free(rtmp);
        return false;
    }

    if (
        !RTMP_SendCreateStream(rtmp) ||
        !RTMP_ConnectStream(rtmp, 0)
    )
    {
        std::cerr
            << "Erreur : ouverture du flux impossible.\n";
        RTMP_Close(rtmp);
        RTMP_Free(rtmp);
        return false;
    }

    std::cout
        << "Caméra "
        << numero_camera + 1
        << " en cours d'affichage.\n";

    char tampon_video[8192];

    std::size_t entete_a_ignorer =
        premier_flux ? 0 : 13;

    const auto debut =
        std::chrono::steady_clock::now();

    bool lecture_reussie = true;

    while (
        programme_actif &&
        RTMP_IsConnected(rtmp)
    )
    {
        const int octets_lus =
            RTMP_Read(
                rtmp,
                tampon_video,
                sizeof(tampon_video)
            );

        if (octets_lus <= 0)
        {
            lecture_reussie = false;
            break;
        }

        const char* donnees = tampon_video;
        std::size_t taille =
            static_cast<std::size_t>(octets_lus);

        if (entete_a_ignorer > 0)
        {
            const std::size_t a_ignorer =
                entete_a_ignorer < taille
                    ? entete_a_ignorer
                    : taille;

            donnees += a_ignorer;
            taille -= a_ignorer;
            entete_a_ignorer -= a_ignorer;
        }

        if (taille > 0)
        {
            const std::size_t octets_ecrits =
                std::fwrite(
                    donnees,
                    1,
                    taille,
                    lecteur
                );

            const int vidage =
                std::fflush(lecteur);

            if (
                octets_ecrits != taille ||
                vidage == EOF
            )
            {
                programme_actif = 0;
                lecture_reussie = false;
                break;
            }
        }

        const auto maintenant =
            std::chrono::steady_clock::now();

        const auto secondes =
            std::chrono::duration_cast<
                std::chrono::seconds
            >(maintenant - debut).count();

        if (secondes >= 15)
        {
            break;
        }
    }

    RTMP_Close(rtmp);
    RTMP_Free(rtmp);

    return lecture_reussie;
}

int main()
{
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, arreter_programme);

    std::cout << "=====================================\n";
    std::cout << "      GigaPaTChat Open Client\n";
    std::cout << "            Version 1.1\n";
    std::cout << "=====================================\n\n";

    const std::string utilisateur = "admin";

    char* saisie =
        getpass("Mot de passe du NVR : ");

    if (!saisie)
    {
        std::cerr
            << "Erreur : lecture du mot de passe impossible.\n";
        return 1;
    }

    const std::string mot_de_passe = saisie;

    FILE* lecteur = popen(
        "ffplay -loglevel warning "
        "-fflags nobuffer "
        "-flags low_delay "
        "-framedrop "
        "-window_title \"GigaPaTChat\" "
        "-i pipe:0",
        "w"
    );

    if (!lecteur)
    {
        std::cerr
            << "Erreur : lancement de FFplay impossible.\n";
        return 1;
    }

    std::cout
        << "Alternance automatique des deux caméras.\n";
    std::cout
        << "La fenêtre vidéo restera ouverte.\n";
    std::cout
        << "Appuie sur Ctrl+C pour arrêter.\n";

    int numero_camera = 0;
    bool premier_flux = true;

    while (programme_actif)
    {
        if (
            !lire_camera(
                numero_camera,
                utilisateur,
                mot_de_passe,
                lecteur,
                premier_flux
            )
        )
        {
            break;
        }

        premier_flux = false;

        std::this_thread::sleep_for(
            std::chrono::seconds(2)
        );

        numero_camera =
            numero_camera == 0 ? 1 : 0;
    }

    pclose(lecteur);

    std::cout << "\nGigaPaTChat arrêté.\n";
    return 0;
}
