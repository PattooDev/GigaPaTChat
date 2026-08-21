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
            ? std::filesystem::path(dossier_personnel) / "Videos"
            : std::filesystem::path(".");

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

    const std::string flux =
        "ch" + std::to_string(numero_camera) + "_1.264";

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

    RTMP_SetBufferMS(
        rtmp,
        1000
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

    while (
        programme_actif &&
        camera_demandee == numero_camera
    )
    {
        SDL_Event evenement;

        while (SDL_PollEvent(&evenement))
        {
            if (evenement.type == SDL_QUIT)
            {
                programme_actif = 0;
            }
            else if (
                evenement.type ==
                SDL_KEYDOWN
            )
            {
                if (
                    evenement.key.keysym.sym ==
                    SDLK_ESCAPE
                )
                {
                    programme_actif = 0;
                }
                else if (
                    evenement.key.keysym.sym ==
                    SDLK_1
                )
                {
                    camera_demandee = 0;
                }
                else if (
                    evenement.key.keysym.sym ==
                    SDLK_2
                )
                {
                    camera_demandee = 1;
                }
                else if (
                    evenement.key.keysym.sym ==
                    SDLK_r
                )
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
                }
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

                        SDL_SetWindowSize(
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

                    SDL_RenderClear(rendu);

                    SDL_RenderCopy(
                        rendu,
                        texture,
                        nullptr,
                        nullptr
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
        << "           Version 1.5.1\n"
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
            540,
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

    std::cout
        << "Fenêtre vidéo native SDL2 active.\n"
        << "Touche 1 : caméra 1\n"
        << "Touche 2 : caméra 2\n"
        << "Touche R : démarrer/arrêter l'enregistrement\n"
        << "Échap : quitter\n";

    int camera_demandee = 0;

    while (programme_actif)
    {
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

        if (
            !programme_actif
        )
        {
            break;
        }

        if (
            !succes &&
            camera_demandee ==
                camera_actuelle
        )
        {
            break;
        }
    }

    SDL_DestroyRenderer(rendu);
    SDL_DestroyWindow(fenetre);
    SDL_Quit();

    std::cout
        << "\nGigaPaTChat arrêté.\n";

    return 0;
}
