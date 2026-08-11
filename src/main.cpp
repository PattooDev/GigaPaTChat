#include <cstdio>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

#include <openssl/evp.h>
#include <librtmp/amf.h>
#include <librtmp/log.h>
#include <librtmp/rtmp.h>

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

int main()
{
    std::signal(SIGPIPE, SIG_IGN);

    std::cout << "=====================================\n";
    std::cout << "      GigaPaTChat Open Client\n";
    std::cout << "            Version 0.9\n";
    std::cout << "=====================================\n\n";

    const std::string serveur =
        "rtmp://192.168.1.114:80/";

    const std::string flux =
        "ch0_1.264";

    const std::string utilisateur =
        "admin";

    const std::string nonce_initial = "";

    char* saisie =
        getpass("Mot de passe du NVR : ");

    if (!saisie)
    {
        std::cerr
            << "Erreur : lecture du mot de passe impossible.\n";
        return 1;
    }

    const std::string mot_de_passe = saisie;

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
            << "Erreur : RTMP_Alloc() a échoué.\n";
        return 1;
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

    std::cout << "Serveur : " << serveur << "\n";
    std::cout << "Flux    : " << flux << "\n";
    std::cout << "Compte  : " << utilisateur << "\n";

    if (!RTMP_SetupURL(rtmp, url))
    {
        std::cerr
            << "Erreur : préparation RTMP impossible.\n";
        RTMP_Free(rtmp);
        return 1;
    }

    rtmp->Link.timeout = 5;
    RTMP_SetBufferMS(rtmp, 3000);

    std::cout << "Connexion initiale au NVR...\n";

    if (!RTMP_Connect(rtmp, nullptr))
    {
        std::cerr
            << "Erreur : connexion RTMP impossible.\n";
        RTMP_Close(rtmp);
        RTMP_Free(rtmp);
        return 1;
    }

    std::string challenge;

    std::cout
        << "Attente du challenge d'authentification...\n";

    if (!attendre_challenge(rtmp, challenge))
    {
        std::cerr
            << "Erreur : challenge introuvable.\n";
        RTMP_Close(rtmp);
        RTMP_Free(rtmp);
        return 1;
    }

    std::cout << "Challenge reçu.\n";

    const std::string digest =
        md5(challenge + ":" + mot_de_passe);

    const std::string methode_login =
        "login?method=md5"
        "&nonce=" + challenge +
        "&username=" + utilisateur +
        "&digest=" + digest;

    std::cout
        << "Envoi de la réponse d'authentification...\n";

    if (!envoyer_login(rtmp, methode_login))
    {
        std::cerr
            << "Erreur : envoi du login impossible.\n";
        RTMP_Close(rtmp);
        RTMP_Free(rtmp);
        return 1;
    }

    if (!attendre_reponse_login(rtmp))
    {
        std::cerr
            << "Erreur : authentification refusée.\n";
        RTMP_Close(rtmp);
        RTMP_Free(rtmp);
        return 1;
    }

    std::cout << "Authentification RTMP réussie.\n";
    std::cout << "Ouverture du flux vidéo...\n";

    if (!RTMP_SendCreateStream(rtmp))
    {
        std::cerr
            << "Erreur : création du flux impossible.\n";
        RTMP_Close(rtmp);
        RTMP_Free(rtmp);
        return 1;
    }

    if (!RTMP_ConnectStream(rtmp, 0))
    {
        std::cerr
            << "Erreur : ouverture du flux impossible.\n";
        RTMP_Close(rtmp);
        RTMP_Free(rtmp);
        return 1;
    }

    std::cout << "Flux vidéo ouvert avec succès.\n";
    std::cout << "Lancement de FFplay...\n";

    FILE* lecteur = popen(
        "ffplay -loglevel warning "
        "-fflags nobuffer "
        "-flags low_delay "
        "-framedrop "
        "-i pipe:0",
        "w"
    );

    if (!lecteur)
    {
        std::cerr
            << "Erreur : lancement de FFplay impossible.\n";
        RTMP_Close(rtmp);
        RTMP_Free(rtmp);
        return 1;
    }

    std::cout
        << "Vidéo en cours. Ferme la fenêtre pour arrêter.\n";

    char tampon_video[8192];

    while (RTMP_IsConnected(rtmp))
    {
        const int octets_lus =
            RTMP_Read(
                rtmp,
                tampon_video,
                sizeof(tampon_video)
            );

        if (octets_lus <= 0)
        {
            break;
        }

        const std::size_t octets_ecrits =
            std::fwrite(
                tampon_video,
                1,
                octets_lus,
                lecteur
            );

        std::fflush(lecteur);

        if (
            octets_ecrits !=
            static_cast<std::size_t>(octets_lus)
        )
        {
            break;
        }
    }

    pclose(lecteur);

    RTMP_Close(rtmp);
    RTMP_Free(rtmp);

    std::cout << "Lecture terminée.\n";
    return 0;
}
