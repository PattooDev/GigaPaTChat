#include "playback_hevc.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include <curl/curl.h>
#include <SDL2/SDL.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

extern volatile std::sig_atomic_t programme_actif;

namespace
{

struct ParseurFlvHevc
{
    std::vector<std::uint8_t> tampon;

    bool entete_lu = false;

    std::size_t tags_video = 0;
    std::size_t nals_hevc = 0;
    std::size_t images_decodees = 0;
    std::size_t erreurs = 0;

    AVCodecContext* decodeur = nullptr;
    AVFrame* image = nullptr;
    AVFrame* image_yuv = nullptr;
    AVPacket* paquet = nullptr;

    SDL_Window* fenetre = nullptr;
    SDL_Renderer* rendu = nullptr;
    SDL_Texture* texture = nullptr;
    SwsContext* conversion = nullptr;

    int largeur = 0;
    int hauteur = 0;

    bool erreur_decode = false;
    bool arret_demande = false;

    bool horloge_initialisee = false;
    std::uint32_t premier_timestamp = 0;
    std::uint32_t dernier_timestamp = 0;

    std::chrono::steady_clock::time_point depart_reel;
};

std::uint32_t lire_u24(
    const std::uint8_t* p
)
{
    return
        (static_cast<std::uint32_t>(p[0]) << 16) |
        (static_cast<std::uint32_t>(p[1]) << 8) |
        static_cast<std::uint32_t>(p[2]);
}

std::uint32_t lire_u32(
    const std::uint8_t* p
)
{
    return
        (static_cast<std::uint32_t>(p[0]) << 24) |
        (static_cast<std::uint32_t>(p[1]) << 16) |
        (static_cast<std::uint32_t>(p[2]) << 8) |
        static_cast<std::uint32_t>(p[3]);
}

bool traiter_evenements_sdl(
    ParseurFlvHevc& parseur
)
{
    SDL_Event evenement;

    while (SDL_PollEvent(&evenement))
    {
        if (evenement.type == SDL_QUIT)
        {
            programme_actif = 0;
            parseur.arret_demande = true;
        }
        else if (
            evenement.type == SDL_KEYDOWN &&
            (
                evenement.key.keysym.sym == SDLK_ESCAPE ||
                evenement.key.keysym.sym == SDLK_p
            )
        )
        {
            parseur.arret_demande = true;
        }
    }

    return
        programme_actif &&
        !parseur.arret_demande;
}

bool attendre_timestamp_flv(
    ParseurFlvHevc& parseur,
    std::uint32_t timestamp
)
{
    using Horloge =
        std::chrono::steady_clock;

    if (!parseur.horloge_initialisee)
    {
        parseur.horloge_initialisee = true;
        parseur.premier_timestamp = timestamp;
        parseur.dernier_timestamp = timestamp;
        parseur.depart_reel = Horloge::now();

        return true;
    }

    parseur.dernier_timestamp = timestamp;

    /*
     * Soustraction non signée :
     * elle reste correcte même en cas de
     * débordement du timestamp FLV 32 bits.
     */
    const std::uint32_t ecoule_ms =
        timestamp -
        parseur.premier_timestamp;

    const auto cible =
        parseur.depart_reel +
        std::chrono::milliseconds(ecoule_ms);

    for (;;)
    {
        if (!traiter_evenements_sdl(parseur))
            return false;

        const auto maintenant =
            Horloge::now();

        if (maintenant >= cible)
            return true;

        const auto reste =
            std::chrono::duration_cast<
                std::chrono::milliseconds
            >(cible - maintenant).count();

        const Uint32 attente =
            static_cast<Uint32>(
                reste > 10 ? 10 : (reste > 0 ? reste : 1)
            );

        SDL_Delay(attente);
    }
}

bool afficher_image(
    ParseurFlvHevc& parseur
)
{
    AVFrame* image =
        parseur.image;

    if (
        image->width != parseur.largeur ||
        image->height != parseur.hauteur
    )
    {
        parseur.largeur = image->width;
        parseur.hauteur = image->height;

        if (parseur.texture)
        {
            SDL_DestroyTexture(parseur.texture);
            parseur.texture = nullptr;
        }

        if (parseur.conversion)
        {
            sws_freeContext(parseur.conversion);
            parseur.conversion = nullptr;
        }

        parseur.texture =
            SDL_CreateTexture(
                parseur.rendu,
                SDL_PIXELFORMAT_IYUV,
                SDL_TEXTUREACCESS_STREAMING,
                parseur.largeur,
                parseur.hauteur
            );

        parseur.conversion =
            sws_getContext(
                parseur.largeur,
                parseur.hauteur,
                static_cast<AVPixelFormat>(
                    image->format
                ),
                parseur.largeur,
                parseur.hauteur,
                AV_PIX_FMT_YUV420P,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr
            );

        av_frame_unref(parseur.image_yuv);

        parseur.image_yuv->format =
            AV_PIX_FMT_YUV420P;

        parseur.image_yuv->width =
            parseur.largeur;

        parseur.image_yuv->height =
            parseur.hauteur;

        if (
            !parseur.texture ||
            !parseur.conversion ||
            av_frame_get_buffer(
                parseur.image_yuv,
                32
            ) < 0
        )
        {
            std::cerr
                << "Erreur : préparation affichage SDL HEVC impossible.\n";

            return false;
        }

        /*
         * On ne force pas ici la fenêtre à 1920x1080.
         * SDL adaptera l'image à la taille déjà utilisée
         * par GigaPaTChat.
         */
    }

    if (
        av_frame_make_writable(
            parseur.image_yuv
        ) < 0
    )
    {
        return false;
    }

    sws_scale(
        parseur.conversion,
        image->data,
        image->linesize,
        0,
        parseur.hauteur,
        parseur.image_yuv->data,
        parseur.image_yuv->linesize
    );

    if (
        SDL_UpdateYUVTexture(
            parseur.texture,
            nullptr,
            parseur.image_yuv->data[0],
            parseur.image_yuv->linesize[0],
            parseur.image_yuv->data[1],
            parseur.image_yuv->linesize[1],
            parseur.image_yuv->data[2],
            parseur.image_yuv->linesize[2]
        ) != 0
    )
    {
        return false;
    }

    SDL_RenderClear(parseur.rendu);

    SDL_RenderCopy(
        parseur.rendu,
        parseur.texture,
        nullptr,
        nullptr
    );

    SDL_RenderPresent(parseur.rendu);

    return traiter_evenements_sdl(parseur);
}

bool vider_images_decodees(
    ParseurFlvHevc& parseur
)
{
    for (;;)
    {
        const int resultat =
            avcodec_receive_frame(
                parseur.decodeur,
                parseur.image
            );

        if (resultat == 0)
        {
            ++parseur.images_decodees;

            if (!afficher_image(parseur))
            {
                parseur.erreur_decode =
                    !parseur.arret_demande;

                av_frame_unref(parseur.image);
                return false;
            }

            av_frame_unref(parseur.image);
            continue;
        }

        if (
            resultat == AVERROR(EAGAIN) ||
            resultat == AVERROR_EOF
        )
        {
            return true;
        }

        std::cerr
            << "Erreur : décodage HEVC impossible ("
            << resultat
            << ").\n";

        parseur.erreur_decode = true;
        return false;
    }
}

bool envoyer_annexb(
    ParseurFlvHevc& parseur,
    const std::vector<std::uint8_t>& annexb
)
{
    if (annexb.empty())
        return true;

    av_packet_unref(parseur.paquet);

    if (
        av_new_packet(
            parseur.paquet,
            static_cast<int>(annexb.size())
        ) < 0
    )
    {
        parseur.erreur_decode = true;
        return false;
    }

    std::memcpy(
        parseur.paquet->data,
        annexb.data(),
        annexb.size()
    );

    int resultat =
        avcodec_send_packet(
            parseur.decodeur,
            parseur.paquet
        );

    if (resultat == AVERROR(EAGAIN))
    {
        if (!vider_images_decodees(parseur))
            return false;

        resultat =
            avcodec_send_packet(
                parseur.decodeur,
                parseur.paquet
            );
    }

    if (resultat < 0)
    {
        std::cerr
            << "Erreur : paquet HEVC refusé ("
            << resultat
            << ").\n";

        parseur.erreur_decode = true;
        return false;
    }

    return vider_images_decodees(parseur);
}

bool traiter_tag_video(
    ParseurFlvHevc& parseur,
    const std::uint8_t* payload,
    std::size_t taille
)
{
    if (taille < 5)
        return true;

    const int codec_id =
        payload[0] & 0x0f;

    const int packet_type =
        payload[1];

    /*
     * Le NVR annonce AVC/H.264 :
     * codec_id = 7
     *
     * Mais ses NAL sont réellement HEVC/H.265.
     *
     * packet_type = 1 :
     * paquet contenant les NAL vidéo.
     */
    if (
        codec_id != 7 ||
        packet_type != 1
    )
    {
        return true;
    }

    ++parseur.tags_video;

    std::vector<std::uint8_t> annexb;
    annexb.reserve(taille + 16);

    std::size_t pos = 5;

    while (pos + 4 <= taille)
    {
        const std::uint32_t taille_nal =
            lire_u32(payload + pos);

        pos += 4;

        if (
            taille_nal == 0 ||
            pos + taille_nal > taille
        )
        {
            ++parseur.erreurs;
            return false;
        }

        const std::uint8_t* nal =
            payload + pos;

        const unsigned int type_nal =
            (nal[0] >> 1) & 0x3f;

        /*
         * Types HEVC déjà observés sur ce NVR :
         *  1  = image non-IDR
         * 19  = IDR
         * 32  = VPS
         * 33  = SPS
         * 34  = PPS
         * 39  = SEI
         */
        if (
            type_nal == 1 ||
            type_nal == 19 ||
            type_nal == 32 ||
            type_nal == 33 ||
            type_nal == 34 ||
            type_nal == 39
        )
        {
            ++parseur.nals_hevc;
        }

        static const std::uint8_t start_code[4] =
            {0x00, 0x00, 0x00, 0x01};

        annexb.insert(
            annexb.end(),
            start_code,
            start_code + 4
        );

        annexb.insert(
            annexb.end(),
            nal,
            nal + taille_nal
        );

        pos += taille_nal;
    }

    if (pos != taille)
    {
        ++parseur.erreurs;
        return false;
    }

    return envoyer_annexb(
        parseur,
        annexb
    );
}

bool analyser_tampon(
    ParseurFlvHevc& parseur
)
{
    for (;;)
    {
        if (!parseur.entete_lu)
        {
            /*
             * En-tête FLV :
             * 9 octets + PreviousTagSize0 (4 octets)
             */
            if (parseur.tampon.size() < 13)
                return true;

            if (
                parseur.tampon[0] != 'F' ||
                parseur.tampon[1] != 'L' ||
                parseur.tampon[2] != 'V'
            )
            {
                std::cerr
                    << "Erreur : réponse NVR non FLV.\n";

                return false;
            }

            const std::uint32_t taille_entete =
                lire_u32(
                    parseur.tampon.data() + 5
                );

            const std::size_t total_entete =
                static_cast<std::size_t>(
                    taille_entete
                ) + 4;

            if (
                total_entete < 13 ||
                parseur.tampon.size() < total_entete
            )
            {
                return true;
            }

            parseur.tampon.erase(
                parseur.tampon.begin(),
                parseur.tampon.begin() + total_entete
            );

            parseur.entete_lu = true;
        }

        /*
         * Un tag FLV :
         * - en-tête : 11 octets
         * - payload : data_size
         * - PreviousTagSize : 4 octets
         */
        if (parseur.tampon.size() < 11)
            return true;

        const std::uint8_t tag_type =
            parseur.tampon[0];

        const std::uint32_t data_size =
            lire_u24(
                parseur.tampon.data() + 1
            );

        /*
         * Un tag vidéo normal de ce NVR reste très
         * inférieur à cette limite. Refuser immédiatement
         * une taille aberrante évite une croissance
         * incontrôlée du tampon.
         */
        constexpr std::size_t limite_tag_flv =
            8 * 1024 * 1024;

        if (
            static_cast<std::size_t>(data_size) >
            limite_tag_flv
        )
        {
            std::cerr
                << "Erreur : tag FLV anormalement volumineux : "
                << data_size
                << " octets.\n";

            return false;
        }

        const std::size_t total_tag =
            11 +
            static_cast<std::size_t>(data_size) +
            4;

        if (parseur.tampon.size() < total_tag)
            return true;

        const std::uint8_t* payload =
            parseur.tampon.data() + 11;

        if (tag_type == 9)
        {
            const std::uint32_t timestamp =
                lire_u24(
                    parseur.tampon.data() + 4
                ) |
                (
                    static_cast<std::uint32_t>(
                        parseur.tampon[7]
                    ) << 24
                );

            /*
             * Seuls les vrais paquets vidéo NAL
             * servent à cadencer la lecture.
             */
            if (
                data_size >= 5 &&
                (payload[0] & 0x0f) == 7 &&
                payload[1] == 1
            )
            {
                if (
                    !attendre_timestamp_flv(
                        parseur,
                        timestamp
                    )
                )
                {
                    return false;
                }
            }

            if (
                !traiter_tag_video(
                    parseur,
                    payload,
                    data_size
                )
            )
            {
                return false;
            }
        }

        parseur.tampon.erase(
            parseur.tampon.begin(),
            parseur.tampon.begin() + total_tag
        );

        /*
         * Garde-fou :
         * un tag vidéo peut être gros,
         * mais le tampon ne doit jamais croître
         * indéfiniment.
         */
        if (
            parseur.tampon.size() >
            8 * 1024 * 1024
        )
        {
            std::cerr
                << "Erreur : tampon FLV anormalement volumineux.\n";

            return false;
        }
    }
}

std::size_t reception_flv(
    char* donnees,
    std::size_t taille,
    std::size_t nombre,
    void* opaque
)
{
    if (!opaque)
        return 0;

    ParseurFlvHevc* parseur =
        static_cast<ParseurFlvHevc*>(opaque);

    if (!traiter_evenements_sdl(*parseur))
        return 0;

    const std::size_t octets =
        taille * nombre;

    const std::uint8_t* debut =
        reinterpret_cast<const std::uint8_t*>(
            donnees
        );

    parseur->tampon.insert(
        parseur->tampon.end(),
        debut,
        debut + octets
    );

    if (!analyser_tampon(*parseur))
        return 0;

    return octets;
}

int progression_curl(
    void* opaque,
    curl_off_t total,
    curl_off_t recu,
    curl_off_t total_envoi,
    curl_off_t envoye
)
{
    (void)total;
    (void)recu;
    (void)total_envoi;
    (void)envoye;

    if (!opaque)
        return 1;

    ParseurFlvHevc* parseur =
        static_cast<ParseurFlvHevc*>(opaque);

    return
        traiter_evenements_sdl(*parseur)
            ? 0
            : 1;
}

} // namespace

bool lire_archive_hevc_nvr(
    const std::string& url,
    SDL_Window* fenetre,
    SDL_Renderer* rendu
)
{
    (void)fenetre;
    (void)rendu;

    CURL* curl =
        curl_easy_init();

    if (!curl)
        return false;

    ParseurFlvHevc parseur;

    parseur.fenetre = fenetre;
    parseur.rendu = rendu;

    if (
        !parseur.fenetre ||
        !parseur.rendu
    )
    {
        curl_easy_cleanup(curl);
        return false;
    }

    const AVCodec* codec =
        avcodec_find_decoder(
            AV_CODEC_ID_HEVC
        );

    if (!codec)
    {
        std::cerr
            << "Erreur : décodeur HEVC FFmpeg introuvable.\n";

        curl_easy_cleanup(curl);
        return false;
    }

    parseur.decodeur =
        avcodec_alloc_context3(codec);

    parseur.image =
        av_frame_alloc();

    parseur.image_yuv =
        av_frame_alloc();

    parseur.paquet =
        av_packet_alloc();

    if (
        !parseur.decodeur ||
        !parseur.image ||
        !parseur.image_yuv ||
        !parseur.paquet
    )
    {
        av_packet_free(&parseur.paquet);
        av_frame_free(&parseur.image_yuv);
        av_frame_free(&parseur.image);
        avcodec_free_context(&parseur.decodeur);
        curl_easy_cleanup(curl);
        return false;
    }

    if (
        avcodec_open2(
            parseur.decodeur,
            codec,
            nullptr
        ) < 0
    )
    {
        std::cerr
            << "Erreur : ouverture du décodeur HEVC impossible.\n";

        av_packet_free(&parseur.paquet);
        av_frame_free(&parseur.image_yuv);
        av_frame_free(&parseur.image);
        avcodec_free_context(&parseur.decodeur);
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_setopt(
        curl,
        CURLOPT_URL,
        url.c_str()
    );

    curl_easy_setopt(
        curl,
        CURLOPT_CONNECTTIMEOUT,
        5L
    );

    /*
     * Une archive peut durer longtemps :
     * aucune limite arbitraire de durée totale.
     */
    curl_easy_setopt(
        curl,
        CURLOPT_TIMEOUT,
        0L
    );

    /*
     * Mais une connexion réellement bloquée
     * ne doit pas immobiliser GigaPaTChat.
     */
    curl_easy_setopt(
        curl,
        CURLOPT_LOW_SPEED_LIMIT,
        1L
    );

    curl_easy_setopt(
        curl,
        CURLOPT_LOW_SPEED_TIME,
        15L
    );

    curl_easy_setopt(
        curl,
        CURLOPT_NOPROGRESS,
        0L
    );

    curl_easy_setopt(
        curl,
        CURLOPT_XFERINFOFUNCTION,
        progression_curl
    );

    curl_easy_setopt(
        curl,
        CURLOPT_XFERINFODATA,
        &parseur
    );

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        reception_flv
    );

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &parseur
    );

    const CURLcode resultat =
        curl_easy_perform(curl);

    if (
        resultat == CURLE_OK &&
        !parseur.erreur_decode
    )
    {
        const int flush =
            avcodec_send_packet(
                parseur.decodeur,
                nullptr
            );

        if (
            flush >= 0 ||
            flush == AVERROR_EOF
        )
        {
            vider_images_decodees(parseur);
        }
    }

    curl_easy_cleanup(curl);

    std::cout
        << "Lecteur HEVC NVR : "
        << parseur.tags_video
        << " tags vidéo, "
        << parseur.nals_hevc
        << " NAL HEVC, "
        << parseur.images_decodees
        << " image(s) décodée(s), "
        << parseur.erreurs
        << " erreur(s)";

    if (parseur.horloge_initialisee)
    {
        std::cout
            << ", durée FLV observée : "
            << (
                parseur.dernier_timestamp -
                parseur.premier_timestamp
            )
            << " ms";
    }

    std::cout << ".\n";

    const bool transfert_ok =
        resultat == CURLE_OK ||
        (
            parseur.arret_demande &&
            (
                resultat == CURLE_WRITE_ERROR ||
                resultat == CURLE_ABORTED_BY_CALLBACK
            )
        );

    const bool succes =
        transfert_ok &&
        parseur.entete_lu &&
        parseur.nals_hevc > 0 &&
        parseur.images_decodees > 0 &&
        parseur.erreurs == 0 &&
        !parseur.erreur_decode;

    if (parseur.conversion)
        sws_freeContext(parseur.conversion);

    if (parseur.texture)
        SDL_DestroyTexture(parseur.texture);

    av_packet_free(&parseur.paquet);
    av_frame_free(&parseur.image_yuv);
    av_frame_free(&parseur.image);
    avcodec_free_context(&parseur.decodeur);

    return succes;
}
