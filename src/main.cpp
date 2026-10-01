#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

#include "esee_replay.h"
#include "playback.h"
#include "ui.h"

#include <SDL2/SDL.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <openssl/evp.h>

#include <librtmp/amf.h>
#include <librtmp/log.h>
#include <librtmp/rtmp.h>

volatile std::sig_atomic_t programme_actif = 1;
bool lecture_nvr_demandee = false;

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
        return "";

    const bool succes =
        EVP_DigestInit_ex(contexte, EVP_md5(), nullptr) == 1 &&
        EVP_DigestUpdate(contexte, texte.data(), texte.size()) == 1 &&
        EVP_DigestFinal_ex(contexte, resultat, &longueur) == 1;

    EVP_MD_CTX_free(contexte);

    if (!succes)
        return "";

    std::ostringstream sortie;
    sortie << std::hex << std::setfill('0');

    for (unsigned int i = 0; i < longueur; ++i)
    {
        sortie << std::setw(2)
               << static_cast<int>(resultat[i]);
    }

    return sortie.str();
}

bool attendre_challenge(RTMP* rtmp, std::string& challenge)
{
    RTMPPacket paquet = {0};

    while (
        programme_actif &&
        RTMP_IsConnected(rtmp) &&
        RTMP_ReadPacket(rtmp, &paquet)
    )
    {
        if (!RTMPPacket_IsReady(&paquet))
            continue;

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
            const std::size_t position = contenu.find(marqueur);

            if (
                position != std::string::npos &&
                position + marqueur.size() + 32 <= contenu.size()
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
            RTMP_ClientPacket(rtmp, &paquet);

        RTMPPacket_Free(&paquet);

        if (trouve)
            return true;
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
    paquet.m_headerType = RTMP_PACKET_SIZE_MEDIUM;
    paquet.m_packetType = RTMP_PACKET_TYPE_INVOKE;
    paquet.m_nTimeStamp = 0;
    paquet.m_nInfoField2 = 0;
    paquet.m_hasAbsTimestamp = 0;
    paquet.m_body = tampon + RTMP_MAX_HEADER_SIZE;

    char* encodage = paquet.m_body;

    encodage = AMF_EncodeString(
        encodage,
        fin,
        &nom_methode
    );

    if (!encodage)
        return false;

    encodage = AMF_EncodeNumber(
        encodage,
        fin,
        ++rtmp->m_numInvokes
    );

    if (!encodage || encodage >= fin)
        return false;

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
            continue;

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
                contenu.find("_result") != std::string::npos;

            erreur =
                contenu.find("_error") != std::string::npos;
        }

        RTMP_ClientPacket(rtmp, &paquet);
        RTMPPacket_Free(&paquet);

        if (succes)
            return true;

        if (erreur)
            return false;
    }

    return false;
}

struct SourceRTMP
{
    RTMP* rtmp = nullptr;
};

struct Enregistrement
{
    AVFormatContext* format = nullptr;
    AVStream* flux = nullptr;
    int64_t origine = AV_NOPTS_VALUE;
    bool entete_ecrite = false;
    std::string chemin;
};

std::string chemin_enregistrement(int numero_camera)
{
    const char* dossier_personnel =
        std::getenv("HOME");

    std::filesystem::path dossier =
        dossier_personnel
            ? std::filesystem::path(dossier_personnel) / "Videos" / "GigaPaTChat"
            : std::filesystem::path(".") / "GigaPaTChat";

    std::error_code erreur;
    std::filesystem::create_directories(
        dossier,
        erreur
    );

    const std::time_t maintenant =
        std::time(nullptr);

    std::tm heure_locale = {};
    localtime_r(
        &maintenant,
        &heure_locale
    );

    char horodatage[32];

    std::strftime(
        horodatage,
        sizeof(horodatage),
        "%Y%m%d-%H%M%S",
        &heure_locale
    );

    const std::string nom =
        "GigaPaTChat-camera" +
        std::to_string(numero_camera + 1) +
        "-" +
        horodatage +
        ".mkv";

    return (dossier / nom).string();
}

void arreter_enregistrement(
    Enregistrement& enregistrement
)
{
    if (!enregistrement.format)
        return;

    if (enregistrement.entete_ecrite)
        av_write_trailer(enregistrement.format);

    if (
        !(enregistrement.format->oformat->flags &
          AVFMT_NOFILE) &&
        enregistrement.format->pb
    )
    {
        avio_closep(
            &enregistrement.format->pb
        );
    }

    avformat_free_context(
        enregistrement.format
    );

    if (enregistrement.entete_ecrite)
    {
        std::cout
            << "Enregistrement terminé : "
            << enregistrement.chemin
            << "\n";
    }

    enregistrement = Enregistrement{};
}

bool demarrer_enregistrement(
    Enregistrement& enregistrement,
    AVStream* flux_source,
    int numero_camera
)
{
    enregistrement.chemin =
        chemin_enregistrement(numero_camera);

    if (
        avformat_alloc_output_context2(
            &enregistrement.format,
            nullptr,
            "matroska",
            enregistrement.chemin.c_str()
        ) < 0 ||
        !enregistrement.format
    )
    {
        std::cerr
            << "Erreur : création du fichier MKV impossible.\n";

        enregistrement = Enregistrement{};
        return false;
    }

    enregistrement.flux =
        avformat_new_stream(
            enregistrement.format,
            nullptr
        );

    if (!enregistrement.flux)
    {
        std::cerr
            << "Erreur : création du flux d'enregistrement impossible.\n";

        arreter_enregistrement(enregistrement);
        return false;
    }

    if (
        avcodec_parameters_copy(
            enregistrement.flux->codecpar,
            flux_source->codecpar
        ) < 0
    )
    {
        std::cerr
            << "Erreur : copie des paramètres vidéo impossible.\n";

        arreter_enregistrement(enregistrement);
        return false;
    }

    enregistrement.flux->codecpar->codec_tag = 0;
    enregistrement.flux->time_base =
        flux_source->time_base;

    if (
        !(enregistrement.format->oformat->flags &
          AVFMT_NOFILE) &&
        avio_open(
            &enregistrement.format->pb,
            enregistrement.chemin.c_str(),
            AVIO_FLAG_WRITE
        ) < 0
    )
    {
        std::cerr
            << "Erreur : ouverture du fichier MKV impossible.\n";

        arreter_enregistrement(enregistrement);
        return false;
    }

    if (
        avformat_write_header(
            enregistrement.format,
            nullptr
        ) < 0
    )
    {
        std::cerr
            << "Erreur : écriture de l'en-tête MKV impossible.\n";

        arreter_enregistrement(enregistrement);
        return false;
    }

    enregistrement.entete_ecrite = true;
    enregistrement.origine = AV_NOPTS_VALUE;

    std::cout
        << "Enregistrement démarré : "
        << enregistrement.chemin
        << "\n";

    return true;
}

bool ecrire_paquet_enregistrement(
    Enregistrement& enregistrement,
    AVStream* flux_source,
    const AVPacket* paquet_source
)
{
    if (
        !enregistrement.format ||
        !enregistrement.flux ||
        !enregistrement.entete_ecrite
    )
    {
        return false;
    }

    AVPacket paquet = {};

    if (
        av_packet_ref(
            &paquet,
            paquet_source
        ) < 0
    )
    {
        return false;
    }

    if (
        enregistrement.origine == AV_NOPTS_VALUE
    )
    {
        enregistrement.origine =
            paquet.dts != AV_NOPTS_VALUE
                ? paquet.dts
                : paquet.pts;
    }

    if (
        enregistrement.origine != AV_NOPTS_VALUE
    )
    {
        if (paquet.pts != AV_NOPTS_VALUE)
            paquet.pts -= enregistrement.origine;

        if (paquet.dts != AV_NOPTS_VALUE)
            paquet.dts -= enregistrement.origine;
    }

    av_packet_rescale_ts(
        &paquet,
        flux_source->time_base,
        enregistrement.flux->time_base
    );

    paquet.stream_index =
        enregistrement.flux->index;

    paquet.pos = -1;

    const bool succes =
        av_interleaved_write_frame(
            enregistrement.format,
            &paquet
        ) >= 0;

    av_packet_unref(&paquet);

    return succes;
}

int lire_rtmp_ffmpeg(
    void* opaque,
    uint8_t* tampon,
    int taille
)
{
    SourceRTMP* source =
        static_cast<SourceRTMP*>(opaque);

    if (
        !programme_actif ||
        !source ||
        !source->rtmp ||
        !RTMP_IsConnected(source->rtmp)
    )
    {
        return AVERROR_EOF;
    }

    const int lus =
        RTMP_Read(
            source->rtmp,
            reinterpret_cast<char*>(tampon),
            taille
        );

    if (lus <= 0)
        return AVERROR_EOF;

    return lus;
}

bool lire_camera(
    int numero_camera,
    const std::string& adresse_nvr,
    const std::string& utilisateur,
    const std::string& mot_de_passe,
    SDL_Window* fenetre,
    SDL_Renderer* rendu,
    int& camera_demandee
)
{
    const std::string serveur =
        "rtmp://" + adresse_nvr + ":80/";

    const char* profil_env = std::getenv("GIGAPATCHAT_STREAM");

    const std::string profil =
        (profil_env && std::string(profil_env) == "0")
        ? "0"
        : "1";

    const std::string flux =
        "ch" + std::to_string(numero_camera) +
        "_" + profil + ".264";

    std::cerr
        << "[RTMP] Flux demandé : "
        << flux
        << "\n";

    const std::string nonce_initial = "";

    const std::string digest_initial =
        md5(
            nonce_initial +
            ":" +
            mot_de_passe
        );

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
    RTMP_LogSetLevel(RTMP_LOGDEBUG);

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

    rtmp->Link.timeout = 30;

    const int buffer_rtmp_ms =
        (profil == "0") ? 200 : 1000;

    std::cerr
        << "[RTMP] Buffer demandé : "
        << buffer_rtmp_ms
        << " ms\n";

    RTMP_SetBufferMS(
        rtmp,
        buffer_rtmp_ms
    );

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
        md5(
            challenge +
            ":" +
            mot_de_passe
        );

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
            << "Erreur : authentification RTMP refusée.\n";

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
        << " connectée.\n";

    SourceRTMP source;
    source.rtmp = rtmp;

    constexpr int taille_avio = 32768;

    unsigned char* tampon_avio =
        static_cast<unsigned char*>(
            av_malloc(taille_avio)
        );

    if (!tampon_avio)
    {
        RTMP_Close(rtmp);
        RTMP_Free(rtmp);
        return false;
    }

    AVIOContext* avio =
        avio_alloc_context(
            tampon_avio,
            taille_avio,
            0,
            &source,
            lire_rtmp_ffmpeg,
            nullptr,
            nullptr
        );

    if (!avio)
    {
        av_free(tampon_avio);
        RTMP_Close(rtmp);
        RTMP_Free(rtmp);
        return false;
    }

    AVFormatContext* format =
        avformat_alloc_context();

    if (!format)
    {
        avio_context_free(&avio);
        RTMP_Close(rtmp);
        RTMP_Free(rtmp);
        return false;
    }

    format->pb = avio;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;

    const AVInputFormat* flv =
        av_find_input_format("flv");

    if (
        avformat_open_input(
            &format,
            nullptr,
            flv,
            nullptr
        ) < 0
    )
    {
        std::cerr
            << "Erreur : FFmpeg ne reconnaît pas le flux.\n";

        avformat_free_context(format);
        avio_context_free(&avio);

        RTMP_Close(rtmp);
        RTMP_Free(rtmp);

        return false;
    }

    if (
        avformat_find_stream_info(
            format,
            nullptr
        ) < 0
    )
    {
        std::cerr
            << "Erreur : informations vidéo introuvables.\n";

        avformat_close_input(&format);
        avio_context_free(&avio);

        RTMP_Close(rtmp);
        RTMP_Free(rtmp);

        return false;
    }

    const int flux_video =
        av_find_best_stream(
            format,
            AVMEDIA_TYPE_VIDEO,
            -1,
            -1,
            nullptr,
            0
        );

    if (flux_video < 0)
    {
        std::cerr
            << "Erreur : aucun flux vidéo trouvé.\n";

        avformat_close_input(&format);
        avio_context_free(&avio);

        RTMP_Close(rtmp);
        RTMP_Free(rtmp);

        return false;
    }

    AVStream* stream =
        format->streams[flux_video];

    const AVCodec* codec =
        avcodec_find_decoder(
            stream->codecpar->codec_id
        );

    if (!codec)
    {
        std::cerr
            << "Erreur : décodeur vidéo introuvable.\n";

        avformat_close_input(&format);
        avio_context_free(&avio);

        RTMP_Close(rtmp);
        RTMP_Free(rtmp);

        return false;
    }

    AVCodecContext* decodeur =
        avcodec_alloc_context3(codec);

    if (!decodeur)
    {
        avformat_close_input(&format);
        avio_context_free(&avio);

        RTMP_Close(rtmp);
        RTMP_Free(rtmp);

        return false;
    }

    avcodec_parameters_to_context(
        decodeur,
        stream->codecpar
    );

    decodeur->flags |=
        AV_CODEC_FLAG_LOW_DELAY;

    if (
        avcodec_open2(
            decodeur,
            codec,
            nullptr
        ) < 0
    )
    {
        std::cerr
            << "Erreur : ouverture du décodeur impossible.\n";

        avcodec_free_context(&decodeur);
        avformat_close_input(&format);
        avio_context_free(&avio);

        RTMP_Close(rtmp);
        RTMP_Free(rtmp);

        return false;
    }

    AVFrame* image =
        av_frame_alloc();

    AVFrame* image_yuv =
        av_frame_alloc();

    AVPacket* paquet =
        av_packet_alloc();

    SDL_Texture* texture = nullptr;
    SwsContext* conversion = nullptr;

    int largeur = 0;
    int hauteur = 0;

    bool succes = true;
    bool enregistrement_demande = false;
    Enregistrement enregistrement;

    auto basculer_enregistrement = [&]()
    {
        if (
            enregistrement.format ||
            enregistrement_demande
        )
        {
            enregistrement_demande = false;
            arreter_enregistrement(
                enregistrement
            );
        }
        else
        {
            enregistrement_demande = true;

            std::cout
                << "Démarrage demandé : attente d'une image clé...\n";
        }
    };

    while (
        programme_actif &&
        camera_demandee == numero_camera
    )
    {
        SDL_Event evenement;

        while (SDL_PollEvent(&evenement))
        {
            const CommandeInterface commande =
                commande_interface(
                    evenement,
                    fenetre
                );

            if (commande == CommandeInterface::Camera1)
            {
                camera_demandee = 0;
            }
            else if (commande == CommandeInterface::Camera2)
            {
                camera_demandee = 1;
            }
            else if (commande == CommandeInterface::Enregistrement)
            {
                basculer_enregistrement();
            }
            else if (commande == CommandeInterface::Lecture)
            {
                lecture_nvr_demandee = true;
                camera_demandee = -1;

                std::cout
                    << "Ouverture de la lecture NVR...\n";
            }
            else if (commande == CommandeInterface::Quitter)
            {
                programme_actif = 0;
            }
        }

        if (
            !programme_actif ||
            camera_demandee != numero_camera
        )
        {
            break;
        }

        const int lecture =
            av_read_frame(
                format,
                paquet
            );

        if (lecture < 0)
        {
            succes = false;
            break;
        }

        if (
            paquet->stream_index ==
            flux_video
        )
        {
            if (
                enregistrement_demande &&
                !enregistrement.format &&
                (paquet->flags & AV_PKT_FLAG_KEY)
            )
            {
                if (
                    !demarrer_enregistrement(
                        enregistrement,
                        stream,
                        numero_camera
                    )
                )
                {
                    enregistrement_demande = false;
                }
            }

            if (enregistrement.format)
            {
                if (
                    !ecrire_paquet_enregistrement(
                        enregistrement,
                        stream,
                        paquet
                    )
                )
                {
                    std::cerr
                        << "Erreur : écriture de l'enregistrement interrompue.\n";

                    enregistrement_demande = false;
                    arreter_enregistrement(
                        enregistrement
                    );
                }
            }

            if (
                avcodec_send_packet(
                    decodeur,
                    paquet
                ) == 0
            )
            {
                while (
                    avcodec_receive_frame(
                        decodeur,
                        image
                    ) == 0
                )
                {
                    if (
                        image->width != largeur ||
                        image->height != hauteur
                    )
                    {
                        largeur = image->width;
                        hauteur = image->height;
std::cout << "Résolution reçue : " << largeur << "x" << hauteur << std::endl;

                        if (texture)
                            SDL_DestroyTexture(texture);

                        texture =
                            SDL_CreateTexture(
                                rendu,
                                SDL_PIXELFORMAT_IYUV,
                                SDL_TEXTUREACCESS_STREAMING,
                                largeur,
                                hauteur
                            );

                        if (conversion)
                            sws_freeContext(conversion);

                        conversion =
                            sws_getContext(
                                largeur,
                                hauteur,
                                static_cast<AVPixelFormat>(
                                    image->format
                                ),
                                largeur,
                                hauteur,
                                AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR,
                                nullptr,
                                nullptr,
                                nullptr
                            );

                        av_frame_unref(
                            image_yuv
                        );

                        image_yuv->format =
                            AV_PIX_FMT_YUV420P;

                        image_yuv->width =
                            largeur;

                        image_yuv->height =
                            hauteur;

                        av_frame_get_buffer(
                            image_yuv,
                            32
                        );

                        ajuster_fenetre_video(
                            fenetre,
                            largeur,
                            hauteur
                        );
                    }

                    if (
                        !texture ||
                        !conversion
                    )
                    {
                        succes = false;
                        break;
                    }

                    av_frame_make_writable(
                        image_yuv
                    );

                    sws_scale(
                        conversion,
                        image->data,
                        image->linesize,
                        0,
                        hauteur,
                        image_yuv->data,
                        image_yuv->linesize
                    );

                    SDL_UpdateYUVTexture(
                        texture,
                        nullptr,
                        image_yuv->data[0],
                        image_yuv->linesize[0],
                        image_yuv->data[1],
                        image_yuv->linesize[1],
                        image_yuv->data[2],
                        image_yuv->linesize[2]
                    );

                    SDL_SetRenderDrawColor(
                        rendu,
                        0,
                        0,
                        0,
                        255
                    );

                    SDL_RenderClear(rendu);

                    const SDL_Rect destination =
                        zone_video(
                            fenetre,
                            largeur,
                            hauteur
                        );

                    SDL_RenderCopy(
                        rendu,
                        texture,
                        nullptr,
                        &destination
                    );

                    EtatInterface etat;
                    etat.camera = numero_camera;
                    etat.camera_disponible = true;
                    etat.enregistrement =
                        enregistrement.format ||
                        enregistrement_demande;
                    etat.lecture_autorisee = true;

                    dessiner_interface(
                        rendu,
                        fenetre,
                        etat
                    );

                    SDL_RenderPresent(rendu);
                }
            }
        }

        av_packet_unref(paquet);
    }

    arreter_enregistrement(
        enregistrement
    );

    if (conversion)
        sws_freeContext(conversion);

    if (texture)
        SDL_DestroyTexture(texture);

    av_packet_free(&paquet);

    av_frame_free(&image_yuv);
    av_frame_free(&image);

    avcodec_free_context(&decodeur);

    avformat_close_input(&format);
    avio_context_free(&avio);

    RTMP_Close(rtmp);
    RTMP_Free(rtmp);

    return succes;
}

int main()
{
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, arreter_programme);

    std::cout
        << "=====================================\n"
        << "      GigaPaTChat Open Client\n"
        << "           Version 1.6.1\n"
        << "=====================================\n\n";

    std::string adresse_nvr;

    std::cout
        << "Adresse IP ou nom du NVR : ";

    if (
        !std::getline(std::cin, adresse_nvr) ||
        adresse_nvr.empty()
    )
    {
        std::cerr
            << "Erreur : adresse du NVR manquante.\n";

        return 1;
    }

    const char* mode_replay_esee =
        std::getenv("GIGAPATCHAT_ESEE_REPLAY");

    if (
        mode_replay_esee &&
        std::string(mode_replay_esee) == "1"
    )
    {
        std::string utilisateur_replay;

        std::cout
            << "Nom d'utilisateur du NVR : ";

        if (
            !std::getline(
                std::cin,
                utilisateur_replay
            ) ||
            utilisateur_replay.empty()
        )
        {
            std::cerr
                << "Erreur : nom d'utilisateur manquant.\n";

            return 1;
        }

        char* saisie_replay =
            getpass(
                "Mot de passe du NVR : "
            );

        if (!saisie_replay)
        {
            std::cerr
                << "Erreur : lecture du mot de passe impossible.\n";

            return 1;
        }

        const std::string mot_de_passe_replay =
            saisie_replay;

        const std::time_t maintenant_replay =
            std::time(nullptr);

        std::tm locale_replay = {};
        localtime_r(
            &maintenant_replay,
            &locale_replay
        );

        char date_replay[32] = {};

        std::strftime(
            date_replay,
            sizeof(date_replay),
            "%Y-%m-%d",
            &locale_replay
        );

        std::vector<SessionNVR> sessions_replay;
        int total_http_replay = 0;

        const bool http_replay_ok =
            rechercher_archives_nvr(
                adresse_nvr,
                utilisateur_replay,
                mot_de_passe_replay,
                3,
                15,
                date_replay,
                "00:00:00",
                "23:59:59",
                sessions_replay,
                total_http_replay
            );

        if (
            !http_replay_ok ||
            sessions_replay.empty()
        )
        {
            std::cerr
                << "[HTTP] Impossible de choisir une archive réelle pour REPLAY.\n";

            return 9;
        }

        const SessionNVR& reference_replay =
            sessions_replay.front();

        std::cout
            << "\n[HTTP] Archive choisie pour REPLAY natif :\n"
            << "[HTTP] caméra "
            << reference_replay.canal + 1
            << " | type "
            << reference_replay.type
            << " | début="
            << reference_replay.debut
            << " | fin="
            << reference_replay.fin
            << "\n";

        std::cout
            << "\n[EseeCloud] Test REPLAY START KP2P expérimental...\n";

        const ResultatReplayEsee resultat =
            tester_replay_start_esee(
                adresse_nvr,
                utilisateur_replay,
                mot_de_passe_replay,
                reference_replay.canal,
                reference_replay.type,
                reference_replay.debut,
                reference_replay.fin,
                10000,
                5000
            );

        std::cout
            << "[EseeCloud] WebSocket : "
            << (resultat.websocket ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] ARQ/KP2P   : "
            << (resultat.arq ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] IOT_OPEN   : "
            << (resultat.iot ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] AUTH       : "
            << (resultat.auth ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] REPLAY     : "
            << (resultat.demarrage ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] MEDIA      : "
            << (resultat.media ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] Code       : "
            << resultat.code
            << "\n"
            << "[EseeCloud] Détail     : "
            << resultat.detail
            << "\n";

        if (resultat.media)
        {
            std::cout
                << "[EseeCloud] Codec      : "
                << resultat.premiere_trame.codec
                << "\n"
                << "[EseeCloud] Payload    : "
                << resultat.premiere_trame.taille_payload
                << " octets\n"
                << "[EseeCloud] Timestamp  : "
                << resultat.premiere_trame.timestamp_ms
                << " ms\n"
                << "[EseeCloud] Champs     : "
                << resultat.premiere_trame.champ_120
                << " / "
                << resultat.premiere_trame.champ_124
                << " / "
                << resultat.premiere_trame.champ_128
                << "\n";
        }

        return
            resultat.demarrage
                ? 0
                : 10;
    }

    const char* mode_find_esee =
        std::getenv("GIGAPATCHAT_ESEE_FIND");

    if (
        mode_find_esee &&
        std::string(mode_find_esee) == "1"
    )
    {
        std::string utilisateur_find;

        std::cout
            << "Nom d'utilisateur du NVR : ";

        if (
            !std::getline(
                std::cin,
                utilisateur_find
            ) ||
            utilisateur_find.empty()
        )
        {
            std::cerr
                << "Erreur : nom d'utilisateur manquant.\n";

            return 1;
        }

        char* saisie_find =
            getpass(
                "Mot de passe du NVR : "
            );

        if (!saisie_find)
        {
            std::cerr
                << "Erreur : lecture du mot de passe impossible.\n";

            return 1;
        }

        const std::string mot_de_passe_find =
            saisie_find;

        std::time_t maintenant_find =
            std::time(nullptr);

        std::tm locale_find = {};
        localtime_r(
            &maintenant_find,
            &locale_find
        );

        char date_find[32] = {};

        std::strftime(
            date_find,
            sizeof(date_find),
            "%Y-%m-%d",
            &locale_find
        );

        std::vector<SessionNVR> sessions_find;
        int total_http_find = 0;

        const bool http_find_ok =
            rechercher_archives_nvr(
                adresse_nvr,
                utilisateur_find,
                mot_de_passe_find,
                3,
                15,
                date_find,
                "00:00:00",
                "23:59:59",
                sessions_find,
                total_http_find
            );

        int canal_find = 0;
        int type_find = 15;
        std::int64_t debut_find = 0;
        std::int64_t fin_find = 0;

        if (
            http_find_ok &&
            !sessions_find.empty()
        )
        {
            const SessionNVR& reference =
                sessions_find.front();

            canal_find = reference.canal;
            type_find = reference.type;
            debut_find = reference.debut;
            fin_find = reference.fin;

            std::cout
                << "\n[HTTP] Référence réelle choisie pour comparaison KP2P :\n"
                << "[HTTP] caméra "
                << reference.canal + 1
                << " | type "
                << reference.type
                << " | début="
                << reference.debut
                << " | fin="
                << reference.fin
                << "\n";
        }
        else
        {
            std::cout
                << "\n[HTTP] Aucune référence précise disponible ; "
                << "recherche KP2P large utilisée.\n";
        }

        std::cout
            << "\n[EseeCloud] Recherche REPLAY native KP2P expérimentale...\n";

        const ResultatRechercheEsee resultat =
            tester_recherche_replay_esee(
                adresse_nvr,
                utilisateur_find,
                mot_de_passe_find,
                canal_find,
                type_find,
                10000,
                3000,
                debut_find,
                fin_find
            );

        std::cout
            << "[EseeCloud] WebSocket : "
            << (resultat.websocket ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] ARQ/KP2P   : "
            << (resultat.arq ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] IOT_OPEN   : "
            << (resultat.iot ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] AUTH       : "
            << (resultat.auth ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] SEARCH     : "
            << (resultat.recherche ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] Code       : "
            << resultat.code
            << "\n"
            << "[EseeCloud] Détail     : "
            << resultat.detail
            << "\n";

        for (
            std::size_t i = 0;
            i < resultat.fichiers.size();
            ++i
        )
        {
            const FichierRechercheEsee& fichier =
                resultat.fichiers[i];

            std::cout
                << "  "
                << i + 1
                << " | caméra "
                << fichier.canal + 1
                << " | type "
                << fichier.type
                << " | "
                << fichier.debut
                << " -> "
                << fichier.fin
                << " | "
                << "qualité "
                << fichier.qualite
                << "\n";
        }

        return
            resultat.recherche
                ? 0
                : 8;
    }

    const char* mode_http_auth =
        std::getenv("GIGAPATCHAT_HTTP_AUTH_TEST");

    if (
        mode_http_auth &&
        std::string(mode_http_auth) == "1"
    )
    {
        std::string utilisateur_http;

        std::cout
            << "Nom d'utilisateur du NVR : ";

        if (
            !std::getline(
                std::cin,
                utilisateur_http
            ) ||
            utilisateur_http.empty()
        )
        {
            std::cerr
                << "Erreur : nom d'utilisateur manquant.\n";

            return 1;
        }

        char* saisie_http =
            getpass(
                "Mot de passe du NVR : "
            );

        if (!saisie_http)
        {
            std::cerr
                << "Erreur : lecture du mot de passe impossible.\n";

            return 1;
        }

        const std::string mot_de_passe_http =
            saisie_http;

        const std::time_t maintenant =
            std::time(nullptr);

        std::tm locale = {};
        localtime_r(
            &maintenant,
            &locale
        );

        char date_http[32] = {};

        std::strftime(
            date_http,
            sizeof(date_http),
            "%Y-%m-%d",
            &locale
        );

        std::vector<SessionNVR> sessions_http;
        int total_http = 0;

        std::cout
            << "\n[HTTP] Test des identifiants via recsearch...\n";

        const bool succes_http =
            rechercher_archives_nvr(
                adresse_nvr,
                utilisateur_http,
                mot_de_passe_http,
                3,
                15,
                date_http,
                "00:00:00",
                "23:59:59",
                sessions_http,
                total_http
            );

        std::cout
            << "[HTTP] RECSEARCH : "
            << (succes_http ? "OK" : "ECHEC")
            << "\n";

        if (succes_http)
        {
            std::cout
                << "[HTTP] Identifiants acceptés par l'API HTTP NVR.\n"
                << "[HTTP] Enregistrements aujourd'hui : "
                << total_http
                << "\n";
        }

        return
            succes_http
                ? 0
                : 7;
    }

    const char* mode_auth3_esee =
        std::getenv("GIGAPATCHAT_ESEE_AUTH3");

    if (
        mode_auth3_esee &&
        std::string(mode_auth3_esee) == "1"
    )
    {
        std::string utilisateur_auth3;

        std::cout
            << "Nom d'utilisateur du NVR : ";

        if (
            !std::getline(
                std::cin,
                utilisateur_auth3
            ) ||
            utilisateur_auth3.empty()
        )
        {
            std::cerr
                << "Erreur : nom d'utilisateur manquant.\n";

            return 1;
        }

        char* saisie_auth3 =
            getpass(
                "Mot de passe du NVR : "
            );

        if (!saisie_auth3)
        {
            std::cerr
                << "Erreur : lecture du mot de passe impossible.\n";

            return 1;
        }

        const std::string mot_de_passe_auth3 =
            saisie_auth3;

        std::cout
            << "\n[EseeCloud] Test AUTH3 KP2P expérimental...\n";

        const ResultatAuthEsee resultat =
            tester_auth3_esee(
                adresse_nvr,
                utilisateur_auth3,
                mot_de_passe_auth3,
                10000,
                3000
            );

        std::cout
            << "[EseeCloud] WebSocket : "
            << (resultat.websocket ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] ARQ/KP2P   : "
            << (resultat.arq ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] IOT_OPEN   : "
            << (resultat.iot ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] AUTH3      : "
            << (resultat.auth ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] Code       : "
            << resultat.code
            << "\n"
            << "[EseeCloud] Détail     : "
            << resultat.detail
            << "\n";

        return
            resultat.auth
                ? 0
                : 6;
    }

    const char* mode_auth_esee =
        std::getenv("GIGAPATCHAT_ESEE_AUTH");

    if (
        mode_auth_esee &&
        std::string(mode_auth_esee) == "1"
    )
    {
        std::string utilisateur_auth;

        std::cout
            << "Nom d'utilisateur du NVR : ";

        if (
            !std::getline(
                std::cin,
                utilisateur_auth
            ) ||
            utilisateur_auth.empty()
        )
        {
            std::cerr
                << "Erreur : nom d'utilisateur manquant.\n";

            return 1;
        }

        char* saisie_auth =
            getpass(
                "Mot de passe du NVR : "
            );

        if (!saisie_auth)
        {
            std::cerr
                << "Erreur : lecture du mot de passe impossible.\n";

            return 1;
        }

        const std::string mot_de_passe_auth =
            saisie_auth;

        std::cout
            << "\n[EseeCloud] Test d'authentification KP2P expérimental...\n";

        const ResultatAuthEsee resultat =
            tester_auth_esee(
                adresse_nvr,
                utilisateur_auth,
                mot_de_passe_auth,
                10000,
                3000
            );

        std::cout
            << "[EseeCloud] WebSocket : "
            << (resultat.websocket ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] ARQ/KP2P   : "
            << (resultat.arq ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] IOT_OPEN   : "
            << (resultat.iot ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] AUTH       : "
            << (resultat.auth ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] Code       : "
            << resultat.code
            << "\n"
            << "[EseeCloud] Détail     : "
            << resultat.detail
            << "\n";

        return
            resultat.auth
                ? 0
                : 5;
    }

    const char* mode_iot_esee =
        std::getenv("GIGAPATCHAT_ESEE_IOT");

    if (
        mode_iot_esee &&
        std::string(mode_iot_esee) == "1"
    )
    {
        std::cout
            << "\n[EseeCloud] Test d'ouverture IOT expérimental...\n";

        const ResultatSessionIotEsee resultat =
            tester_session_iot_esee(
                adresse_nvr,
                10000,
                3000
            );

        std::cout
            << "[EseeCloud] WebSocket : "
            << (resultat.websocket ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] ARQ/KP2P   : "
            << (resultat.arq ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] IOT_OPEN   : "
            << (resultat.iot ? "OK" : "ECHEC")
            << "\n"
            << "[EseeCloud] Détail     : "
            << resultat.detail
            << "\n";

        return
            resultat.iot
                ? 0
                : 4;
    }

    const char* mode_handshake_esee =
        std::getenv("GIGAPATCHAT_ESEE_HANDSHAKE");

    if (
        mode_handshake_esee &&
        std::string(mode_handshake_esee) == "1"
    )
    {
        std::cout
            << "\n[EseeCloud] Test WebSocket/KP2P expérimental...\n";

        const ResultatHandshakeEsee resultat =
            tester_handshake_esee(
                adresse_nvr,
                10000,
                3000
            );

        std::cout
            << "[EseeCloud] WebSocket : "
            << (
                resultat.websocket
                    ? "OK"
                    : "ECHEC"
            )
            << "\n"
            << "[EseeCloud] ARQ/KP2P   : "
            << (
                resultat.arq
                    ? "OK"
                    : "ECHEC"
            )
            << "\n"
            << "[EseeCloud] Détail     : "
            << resultat.detail
            << "\n";

        return
            resultat.arq
                ? 0
                : 3;
    }

    const char* mode_sonde_esee =
        std::getenv("GIGAPATCHAT_ESEE_PROBE");

    if (
        mode_sonde_esee &&
        std::string(mode_sonde_esee) == "1"
    )
    {
        std::cout
            << "\n[EseeCloud] Sonde expérimentale du service natif...\n";

        const ResultatSondeEsee resultat =
            sonder_service_esee(
                adresse_nvr,
                10000,
                2000
            );

        std::cout
            << "[EseeCloud] "
            << adresse_nvr
            << ":"
            << resultat.port
            << " -> "
            << (
                resultat.joignable
                    ? "JOIGNABLE"
                    : "INJOIGNABLE"
            )
            << " ("
            << resultat.detail
            << ")\n";

        return
            resultat.joignable
                ? 0
                : 2;
    }

    std::string utilisateur;

    std::cout
        << "Nom d'utilisateur du NVR : ";

    if (
        !std::getline(std::cin, utilisateur) ||
        utilisateur.empty()
    )
    {
        std::cerr
            << "Erreur : nom d'utilisateur manquant.\n";

        return 1;
    }

    char* saisie =
        getpass(
            "Mot de passe du NVR : "
        );

    if (!saisie)
    {
        std::cerr
            << "Erreur : lecture du mot de passe impossible.\n";

        return 1;
    }

    const std::string mot_de_passe =
        saisie;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");

    if (
        SDL_Init(
            SDL_INIT_VIDEO |
            SDL_INIT_EVENTS
        ) != 0
    )
    {
        std::cerr
            << "Erreur SDL : "
            << SDL_GetError()
            << "\n";

        return 1;
    }

    SDL_Window* fenetre =
        SDL_CreateWindow(
            "GigaPaTChat",
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            960,
            620,
            SDL_WINDOW_RESIZABLE
        );

    if (!fenetre)
    {
        std::cerr
            << "Erreur : création de la fenêtre impossible.\n";

        SDL_Quit();
        return 1;
    }

    SDL_Renderer* rendu =
        SDL_CreateRenderer(
            fenetre,
            -1,
            SDL_RENDERER_ACCELERATED |
            SDL_RENDERER_PRESENTVSYNC
        );

    if (!rendu)
    {
        std::cerr
            << "Erreur : création du rendu SDL impossible.\n";

        SDL_DestroyWindow(fenetre);
        SDL_Quit();

        return 1;
    }

    if (!initialiser_interface())
    {
        std::cerr
            << "Erreur : initialisation de l'interface SDL2_ttf impossible.\n";

        SDL_DestroyRenderer(rendu);
        SDL_DestroyWindow(fenetre);
        SDL_Quit();

        return 1;
    }

    std::cout
        << "Fenêtre vidéo native SDL2 active.\n"
        << "Touche 1 : caméra 1\n"
        << "Touche 2 : caméra 2\n"
        << "Touche R : démarrer/arrêter l'enregistrement\n"
        << "Touche P : rechercher et lire les archives du NVR\n"
        << "Souris : boutons Caméra 1 / Caméra 2 / Enregistrer / Lecture NVR / Quitter\n"
        << "Échap : quitter\n";

    int camera_demandee = -1;
    std::string message_interface =
        "Prêt - choisissez une caméra ou Lecture NVR";

    while (programme_actif)
    {
        if (camera_demandee < 0)
        {
            const CommandeInterface commande =
                attendre_commande_interface(
                    rendu,
                    fenetre,
                    0,
                    message_interface,
                    true
                );

            if (commande == CommandeInterface::Camera1)
            {
                camera_demandee = 0;
                message_interface = "Connexion caméra 1...";
            }
            else if (commande == CommandeInterface::Camera2)
            {
                camera_demandee = 1;
                message_interface = "Connexion caméra 2...";
            }
            else if (commande == CommandeInterface::Lecture)
            {
                if (
                    !afficher_ecran_archives(
                        rendu,
                        fenetre,
                        adresse_nvr,
                        utilisateur,
                        mot_de_passe
                    )
                )
                {
                    programme_actif = 0;
                }

                message_interface =
                    "Retour des archives NVR - choisissez une caméra";
            }
            else if (commande == CommandeInterface::Quitter)
            {
                programme_actif = 0;
            }

            continue;
        }

        const int camera_actuelle =
            camera_demandee;

        const bool succes =
            lire_camera(
                camera_actuelle,
                adresse_nvr,
                utilisateur,
                mot_de_passe,
                fenetre,
                rendu,
                camera_demandee
            );

        if (lecture_nvr_demandee)
        {
            lecture_nvr_demandee = false;
            camera_demandee = -1;

            if (
                !afficher_ecran_archives(
                        rendu,
                        fenetre,
                        adresse_nvr,
                        utilisateur,
                        mot_de_passe
                    )
            )
            {
                programme_actif = 0;
            }

            message_interface =
                "Retour des archives NVR - choisissez une caméra";

            continue;
        }

        if (!programme_actif)
            break;

        if (
            !succes &&
            camera_demandee == camera_actuelle
        )
        {
            camera_demandee = -1;
            message_interface =
                "Caméra indisponible - connexion ou authentification refusée";
        }
    }

    fermer_interface();
    SDL_DestroyRenderer(rendu);
    SDL_DestroyWindow(fenetre);
    SDL_Quit();

    std::cout
        << "\nGigaPaTChat arrêté.\n";

    return 0;
}
