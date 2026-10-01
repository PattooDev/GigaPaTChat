/*
 * EseeCloud/KP2P interoperability work for GigaPaTChat.
 *
 * Public protocol references that helped this implementation:
 * - BigGecko01/EseeCloud-Raw-Exporter
 *   https://github.com/BigGecko01/EseeCloud-Raw-Exporter
 * - meust3/home-assistant-jooan-nvr
 *   https://github.com/meust3/home-assistant-jooan-nvr
 * - tuyungang/Automatic-Interface-Detection-Tool
 *   https://github.com/tuyungang/Automatic-Interface-Detection-Tool
 * - harsh-chalo/trv-log-all-configs (public CameraSDK connector.js reference)
 *   https://github.com/harsh-chalo/trv-log-all-configs
 *
 * Thanks to these authors/maintainers for making protocol research and
 * SDK reference material publicly available.
 * See docs/ESEECLOUD-REPLAY-EXPERIMENT.md for detailed credits and licenses.
 */

#include "esee_replay.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <array>
#include <iterator>
#include <random>
#include <vector>
#include <utility>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/evp.h>

namespace
{

std::uint32_t lire_u32_le(
    const std::uint8_t* p
)
{
    return
        static_cast<std::uint32_t>(p[0]) |
        (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) |
        (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t lire_u64_le(
    const std::uint8_t* p
)
{
    return
        static_cast<std::uint64_t>(lire_u32_le(p)) |
        (static_cast<std::uint64_t>(lire_u32_le(p + 4)) << 32);
}

bool correspond(
    const std::uint8_t* p,
    const char* texte,
    std::size_t longueur
)
{
    return std::memcmp(p, texte, longueur) == 0;
}

std::string lire_chaine_ascii(
    const std::uint8_t* p,
    std::size_t longueur
)
{
    std::string resultat;

    for (std::size_t i = 0; i < longueur; ++i)
    {
        if (p[i] == 0)
            break;

        const unsigned char c = p[i];

        if (c < 32 || c > 126)
            break;

        resultat.push_back(
            static_cast<char>(c)
        );
    }

    return resultat;
}


bool attendre_fd(
    int fd,
    short evenements,
    int timeout_ms
)
{
    pollfd attente = {};
    attente.fd = fd;
    attente.events = evenements;

    return poll(
        &attente,
        1,
        timeout_ms
    ) > 0;
}

int ouvrir_socket_tcp(
    const std::string& hote,
    int port,
    int timeout_ms,
    std::string& detail
)
{
    addrinfo criteres = {};
    criteres.ai_family = AF_UNSPEC;
    criteres.ai_socktype = SOCK_STREAM;
    criteres.ai_protocol = IPPROTO_TCP;

    addrinfo* adresses = nullptr;

    const std::string service =
        std::to_string(port);

    const int resolution =
        getaddrinfo(
            hote.c_str(),
            service.c_str(),
            &criteres,
            &adresses
        );

    if (resolution != 0)
    {
        detail =
            std::string("résolution impossible : ") +
            gai_strerror(resolution);
        return -1;
    }

    int resultat_fd = -1;
    detail = "connexion impossible";

    for (
        addrinfo* courant = adresses;
        courant;
        courant = courant->ai_next
    )
    {
        const int fd =
            socket(
                courant->ai_family,
                courant->ai_socktype,
                courant->ai_protocol
            );

        if (fd < 0)
            continue;

        const int drapeaux =
            fcntl(fd, F_GETFL, 0);

        if (
            drapeaux < 0 ||
            fcntl(
                fd,
                F_SETFL,
                drapeaux | O_NONBLOCK
            ) < 0
        )
        {
            close(fd);
            continue;
        }

        const int connexion =
            connect(
                fd,
                courant->ai_addr,
                courant->ai_addrlen
            );

        bool connecte =
            connexion == 0;

        if (
            !connecte &&
            errno == EINPROGRESS &&
            attendre_fd(
                fd,
                POLLOUT,
                timeout_ms
            )
        )
        {
            int erreur_socket = 0;
            socklen_t taille_erreur =
                sizeof(erreur_socket);

            if (
                getsockopt(
                    fd,
                    SOL_SOCKET,
                    SO_ERROR,
                    &erreur_socket,
                    &taille_erreur
                ) == 0 &&
                erreur_socket == 0
            )
            {
                connecte = true;
            }
            else if (erreur_socket != 0)
            {
                detail =
                    std::strerror(erreur_socket);
            }
        }

        if (connecte)
        {
            fcntl(
                fd,
                F_SETFL,
                drapeaux
            );

            resultat_fd = fd;
            detail = "connexion TCP établie";
            break;
        }

        close(fd);
    }

    freeaddrinfo(adresses);
    return resultat_fd;
}

bool envoyer_tout(
    int fd,
    const std::uint8_t* donnees,
    std::size_t taille,
    int timeout_ms
)
{
    std::size_t envoyes = 0;

    while (envoyes < taille)
    {
        if (
            !attendre_fd(
                fd,
                POLLOUT,
                timeout_ms
            )
        )
        {
            return false;
        }

        const ssize_t n =
            send(
                fd,
                donnees + envoyes,
                taille - envoyes,
                MSG_NOSIGNAL
            );

        if (n <= 0)
            return false;

        envoyes +=
            static_cast<std::size_t>(n);
    }

    return true;
}

bool recevoir_exact(
    int fd,
    std::uint8_t* donnees,
    std::size_t taille,
    int timeout_ms
)
{
    std::size_t recus = 0;

    while (recus < taille)
    {
        if (
            !attendre_fd(
                fd,
                POLLIN,
                timeout_ms
            )
        )
        {
            return false;
        }

        const ssize_t n =
            recv(
                fd,
                donnees + recus,
                taille - recus,
                0
            );

        if (n <= 0)
            return false;

        recus +=
            static_cast<std::size_t>(n);
    }

    return true;
}

bool lire_entete_http(
    int fd,
    std::string& entete,
    int timeout_ms
)
{
    entete.clear();

    while (
        entete.size() < 16384 &&
        entete.find("\r\n\r\n") ==
            std::string::npos
    )
    {
        std::uint8_t octet = 0;

        if (
            !recevoir_exact(
                fd,
                &octet,
                1,
                timeout_ms
            )
        )
        {
            return false;
        }

        entete.push_back(
            static_cast<char>(octet)
        );
    }

    return
        entete.find("\r\n\r\n") !=
        std::string::npos;
}

bool envoyer_ws_binaire(
    int fd,
    const std::vector<std::uint8_t>& payload,
    int timeout_ms
)
{
    if (payload.size() > 65535)
        return false;

    std::array<std::uint8_t, 4> masque = {};

    std::random_device aleatoire;

    for (auto& octet : masque)
    {
        octet =
            static_cast<std::uint8_t>(
                aleatoire()
            );
    }

    std::vector<std::uint8_t> trame;
    trame.reserve(
        4 +
        masque.size() +
        payload.size()
    );

    trame.push_back(0x82);

    if (payload.size() <= 125)
    {
        trame.push_back(
            static_cast<std::uint8_t>(
                0x80 |
                payload.size()
            )
        );
    }
    else
    {
        trame.push_back(
            static_cast<std::uint8_t>(
                0x80 | 126
            )
        );

        const std::uint16_t longueur =
            static_cast<std::uint16_t>(
                payload.size()
            );

        trame.push_back(
            static_cast<std::uint8_t>(
                (longueur >> 8) & 0xFF
            )
        );

        trame.push_back(
            static_cast<std::uint8_t>(
                longueur & 0xFF
            )
        );
    }

    trame.insert(
        trame.end(),
        masque.begin(),
        masque.end()
    );

    for (
        std::size_t i = 0;
        i < payload.size();
        ++i
    )
    {
        trame.push_back(
            payload[i] ^
            masque[i % masque.size()]
        );
    }

    return envoyer_tout(
        fd,
        trame.data(),
        trame.size(),
        timeout_ms
    );
}

bool recevoir_ws_binaire(
    int fd,
    std::vector<std::uint8_t>& payload,
    int timeout_ms
)
{
    std::uint8_t entete[2] = {};

    if (
        !recevoir_exact(
            fd,
            entete,
            sizeof(entete),
            timeout_ms
        )
    )
    {
        return false;
    }

    const bool fin =
        (entete[0] & 0x80) != 0;

    const std::uint8_t opcode =
        entete[0] & 0x0F;

    const bool masque =
        (entete[1] & 0x80) != 0;

    std::uint64_t longueur =
        entete[1] & 0x7F;

    if (longueur == 126)
    {
        std::uint8_t etendue[2] = {};

        if (
            !recevoir_exact(
                fd,
                etendue,
                sizeof(etendue),
                timeout_ms
            )
        )
        {
            return false;
        }

        longueur =
            (
                static_cast<std::uint64_t>(
                    etendue[0]
                ) << 8
            ) |
            etendue[1];
    }
    else if (longueur == 127)
    {
        return false;
    }

    if (
        !fin ||
        opcode != 0x02 ||
        longueur > 1024 * 1024
    )
    {
        return false;
    }

    std::array<std::uint8_t, 4>
        cle_masque = {};

    if (
        masque &&
        !recevoir_exact(
            fd,
            cle_masque.data(),
            cle_masque.size(),
            timeout_ms
        )
    )
    {
        return false;
    }

    payload.resize(
        static_cast<std::size_t>(
            longueur
        )
    );

    if (
        longueur > 0 &&
        !recevoir_exact(
            fd,
            payload.data(),
            payload.size(),
            timeout_ms
        )
    )
    {
        return false;
    }

    if (masque)
    {
        for (
            std::size_t i = 0;
            i < payload.size();
            ++i
        )
        {
            payload[i] ^=
                cle_masque[
                    i %
                    cle_masque.size()
                ];
        }
    }

    return true;
}


void ajouter_u32_le(
    std::vector<std::uint8_t>& sortie,
    std::uint32_t valeur
)
{
    sortie.push_back(
        static_cast<std::uint8_t>(
            valeur & 0xFF
        )
    );
    sortie.push_back(
        static_cast<std::uint8_t>(
            (valeur >> 8) & 0xFF
        )
    );
    sortie.push_back(
        static_cast<std::uint8_t>(
            (valeur >> 16) & 0xFF
        )
    );
    sortie.push_back(
        static_cast<std::uint8_t>(
            (valeur >> 24) & 0xFF
        )
    );
}

std::vector<std::uint8_t> fabriquer_paquet_iot(
    std::uint32_t commande,
    std::uint32_t sid,
    const std::vector<std::uint8_t>& payload,
    std::int32_t erreur = 0
)
{
    std::vector<std::uint8_t> paquet(
        32,
        0
    );

    paquet[0] = 0xAB;
    paquet[1] = 0xBC;
    paquet[2] = 0xCD;
    paquet[3] = 0xDE;

    paquet[4] =
        static_cast<std::uint8_t>(
            commande & 0xFF
        );
    paquet[5] =
        static_cast<std::uint8_t>(
            (commande >> 8) & 0xFF
        );
    paquet[6] =
        static_cast<std::uint8_t>(
            (commande >> 16) & 0xFF
        );
    paquet[7] =
        static_cast<std::uint8_t>(
            (commande >> 24) & 0xFF
        );

    paquet[11] = 0x01;

    paquet[16] =
        static_cast<std::uint8_t>(
            sid & 0xFF
        );
    paquet[17] =
        static_cast<std::uint8_t>(
            (sid >> 8) & 0xFF
        );
    paquet[18] =
        static_cast<std::uint8_t>(
            (sid >> 16) & 0xFF
        );
    paquet[19] =
        static_cast<std::uint8_t>(
            (sid >> 24) & 0xFF
        );

    const std::uint32_t erreur_u =
        static_cast<std::uint32_t>(
            erreur
        );

    paquet[24] =
        static_cast<std::uint8_t>(
            erreur_u & 0xFF
        );
    paquet[25] =
        static_cast<std::uint8_t>(
            (erreur_u >> 8) & 0xFF
        );
    paquet[26] =
        static_cast<std::uint8_t>(
            (erreur_u >> 16) & 0xFF
        );
    paquet[27] =
        static_cast<std::uint8_t>(
            (erreur_u >> 24) & 0xFF
        );

    const std::uint32_t longueur =
        static_cast<std::uint32_t>(
            payload.size()
        );

    paquet[28] =
        static_cast<std::uint8_t>(
            longueur & 0xFF
        );
    paquet[29] =
        static_cast<std::uint8_t>(
            (longueur >> 8) & 0xFF
        );
    paquet[30] =
        static_cast<std::uint8_t>(
            (longueur >> 16) & 0xFF
        );
    paquet[31] =
        static_cast<std::uint8_t>(
            (longueur >> 24) & 0xFF
        );

    paquet.insert(
        paquet.end(),
        payload.begin(),
        payload.end()
    );

    return paquet;
}

bool envoyer_iot(
    int fd,
    std::uint32_t commande,
    std::uint32_t sid,
    const std::vector<std::uint8_t>& payload,
    int timeout_ms
)
{
    static constexpr std::uint8_t
        arq_data[4] =
    {
        0xCE, 0xFA, 0xEF, 0xFE
    };

    const std::vector<std::uint8_t> paquet =
        fabriquer_paquet_iot(
            commande,
            sid,
            payload
        );

    std::vector<std::uint8_t> annonce(
        std::begin(arq_data),
        std::end(arq_data)
    );

    ajouter_u32_le(
        annonce,
        static_cast<std::uint32_t>(
            paquet.size()
        )
    );

    return
        envoyer_ws_binaire(
            fd,
            annonce,
            timeout_ms
        ) &&
        envoyer_ws_binaire(
            fd,
            paquet,
            timeout_ms
        );
}

bool recevoir_iot(
    int fd,
    std::uint32_t& commande,
    std::int32_t& erreur,
    std::vector<std::uint8_t>& payload,
    int timeout_ms
)
{
    const auto debut =
        std::chrono::steady_clock::now();

    for (;;)
    {
        const auto ecoule =
            std::chrono::duration_cast<
                std::chrono::milliseconds
            >(
                std::chrono::steady_clock::now() -
                debut
            ).count();

        const int restant =
            timeout_ms -
            static_cast<int>(
                ecoule
            );

        if (restant <= 0)
            return false;

        std::vector<std::uint8_t> message;

        if (
            !recevoir_ws_binaire(
                fd,
                message,
                restant
            )
        )
        {
            return false;
        }

        if (
            message.size() >= 4 &&
            message[0] == 0xCE &&
            message[1] == 0xFA &&
            message[2] == 0xEF &&
            message[3] == 0xFE
        )
        {
            continue;
        }

        if (
            message.size() < 32 ||
            message[0] != 0xAB ||
            message[1] != 0xBC ||
            message[2] != 0xCD ||
            message[3] != 0xDE
        )
        {
            continue;
        }

        commande =
            lire_u32_le(
                message.data() + 4
            );

        erreur =
            static_cast<std::int32_t>(
                lire_u32_le(
                    message.data() + 24
                )
            );

        const std::uint32_t longueur =
            lire_u32_le(
                message.data() + 28
            );

        if (
            longueur >
            message.size() - 32
        )
        {
            return false;
        }

        payload.assign(
            message.begin() + 32,
            message.begin() + 32 + longueur
        );

        return true;
    }
}

int ouvrir_websocket_arq(
    const std::string& adresse_nvr,
    int port,
    int timeout_ms,
    bool& websocket_ok,
    bool& arq_ok,
    std::uint32_t& sid,
    std::string& detail
)
{
    websocket_ok = false;
    arq_ok = false;
    sid = 1234;

    std::string detail_tcp;

    const int fd =
        ouvrir_socket_tcp(
            adresse_nvr,
            port,
            timeout_ms,
            detail_tcp
        );

    if (fd < 0)
    {
        detail = detail_tcp;
        return -1;
    }

    std::ostringstream requete;

    requete
        << "GET / HTTP/1.1\r\n"
        << "Host: "
        << adresse_nvr
        << ":"
        << port
        << "\r\n"
        << "Upgrade: websocket\r\n"
        << "Connection: Upgrade\r\n"
        << "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        << "Sec-WebSocket-Version: 13\r\n"
        << "\r\n";

    const std::string texte =
        requete.str();

    if (
        !envoyer_tout(
            fd,
            reinterpret_cast<
                const std::uint8_t*
            >(
                texte.data()
            ),
            texte.size(),
            timeout_ms
        )
    )
    {
        detail =
            "échec envoi Upgrade WebSocket";
        close(fd);
        return -1;
    }

    std::string reponse_http;

    if (
        !lire_entete_http(
            fd,
            reponse_http,
            timeout_ms
        )
    )
    {
        detail =
            "aucune réponse HTTP/WebSocket";
        close(fd);
        return -1;
    }

    const std::size_t fin_ligne =
        reponse_http.find("\r\n");

    const std::string statut =
        reponse_http.substr(
            0,
            fin_ligne
        );

    if (
        statut.find(" 101 ") ==
            std::string::npos
    )
    {
        detail =
            "Upgrade WebSocket refusé : " +
            statut;

        close(fd);
        return -1;
    }

    websocket_ok = true;

    static constexpr std::uint8_t
        arq_open[16] =
    {
        0xD9, 0xFF, 0xCC, 0x02,
        0x8C, 0x38, 0xEE, 0xD2,
        0xD1, 0x99, 0xAC, 0x60,
        0x26, 0x94, 0x7F, 0xAE
    };

    static constexpr std::uint8_t
        arq_reponse[16] =
    {
        0x96, 0xD5, 0x39, 0x0D,
        0x12, 0xFC, 0xBE, 0x8F,
        0x47, 0x90, 0xD9, 0x32,
        0xCC, 0xD8, 0x49, 0xF3
    };

    std::vector<std::uint8_t>
        ouverture(
            std::begin(arq_open),
            std::end(arq_open)
        );

    ajouter_u32_le(
        ouverture,
        sid
    );

    if (
        !envoyer_ws_binaire(
            fd,
            ouverture,
            timeout_ms
        )
    )
    {
        detail =
            "WebSocket OK, échec envoi ARQ_OPEN";
        close(fd);
        return -1;
    }

    std::vector<std::uint8_t>
        reponse_arq;

    if (
        !recevoir_ws_binaire(
            fd,
            reponse_arq,
            timeout_ms
        )
    )
    {
        detail =
            "WebSocket OK, aucune réponse ARQ";
        close(fd);
        return -1;
    }

    if (
        reponse_arq.size() !=
            sizeof(arq_reponse) ||
        std::memcmp(
            reponse_arq.data(),
            arq_reponse,
            sizeof(arq_reponse)
        ) != 0
    )
    {
        std::ostringstream erreur_detail;

        erreur_detail
            << "WebSocket OK, réponse ARQ inattendue ("
            << reponse_arq.size()
            << " octets)";

        detail =
            erreur_detail.str();

        close(fd);
        return -1;
    }

    arq_ok = true;
    detail =
        "WebSocket 101 + ARQ_OPEN reconnu";

    return fd;
}


bool ouvrir_iot_sur_fd(
    int fd,
    std::uint32_t sid,
    int timeout_ms,
    std::string& detail
)
{
    std::vector<std::uint8_t> payload_ouverture;

    ajouter_u32_le(payload_ouverture, sid);
    ajouter_u32_le(payload_ouverture, 0);

    if (!envoyer_iot(fd, 20, sid, payload_ouverture, timeout_ms))
    {
        detail = "ARQ OK, échec envoi IOT_OPEN_REQ";
        return false;
    }

    std::uint32_t commande = 0;
    std::int32_t erreur = 0;
    std::vector<std::uint8_t> payload;

    if (!recevoir_iot(fd, commande, erreur, payload, timeout_ms))
    {
        detail = "ARQ OK, aucune réponse IOT";
        return false;
    }

    if (commande != 21 || erreur != 0)
    {
        std::ostringstream sortie;
        sortie
            << "réponse IOT inattendue : cmd="
            << commande
            << " erreur="
            << erreur;
        detail = sortie.str();
        return false;
    }

    detail = "IOT_OPEN confirmé";
    return true;
}

bool chiffrer_champ_auth(
    const std::string& texte,
    std::vector<std::uint8_t>& sortie
)
{
    if (texte.size() >= 32)
        return false;

    static constexpr unsigned char cle[16] =
    {
        '~', '!', 'J', 'U',
        'A', 'N', '*', '&',
        'V', 'i', 's', 'i',
        'o', 'n', '-', '='
    };

    std::array<unsigned char, 32> entree = {};
    std::memcpy(
        entree.data(),
        texte.data(),
        texte.size()
    );

    EVP_CIPHER_CTX* contexte =
        EVP_CIPHER_CTX_new();

    if (!contexte)
        return false;

    std::array<unsigned char, 48> tampon = {};
    int ecrits = 0;
    int final = 0;

    const bool succes =
        EVP_EncryptInit_ex(
            contexte,
            EVP_aes_128_ecb(),
            nullptr,
            cle,
            nullptr
        ) == 1 &&
        EVP_CIPHER_CTX_set_padding(
            contexte,
            0
        ) == 1 &&
        EVP_EncryptUpdate(
            contexte,
            tampon.data(),
            &ecrits,
            entree.data(),
            static_cast<int>(entree.size())
        ) == 1 &&
        EVP_EncryptFinal_ex(
            contexte,
            tampon.data() + ecrits,
            &final
        ) == 1;

    EVP_CIPHER_CTX_free(contexte);

    if (!succes || ecrits + final != 32)
        return false;

    sortie.assign(
        tampon.begin(),
        tampon.begin() + 32
    );

    return true;
}

std::vector<std::uint8_t> fabriquer_paquet_api(
    std::uint32_t ticket,
    std::uint32_t commande,
    const std::vector<std::uint8_t>& payload,
    std::int32_t resultat = 0
)
{
    std::vector<std::uint8_t> paquet;
    paquet.reserve(24 + payload.size());

    ajouter_u32_le(paquet, 0x4B503250);
    ajouter_u32_le(paquet, 1);
    ajouter_u32_le(paquet, ticket);
    ajouter_u32_le(paquet, commande);
    ajouter_u32_le(
        paquet,
        static_cast<std::uint32_t>(
            resultat
        )
    );
    ajouter_u32_le(
        paquet,
        static_cast<std::uint32_t>(
            payload.size()
        )
    );

    paquet.insert(
        paquet.end(),
        payload.begin(),
        payload.end()
    );

    return paquet;
}

bool envoyer_api(
    int fd,
    std::uint32_t sid,
    std::uint32_t ticket,
    std::uint32_t commande,
    const std::vector<std::uint8_t>& payload,
    int timeout_ms
)
{
    return envoyer_iot(
        fd,
        19,
        sid,
        fabriquer_paquet_api(
            ticket,
            commande,
            payload
        ),
        timeout_ms
    );
}

bool recevoir_api(
    int fd,
    std::uint32_t& commande_api,
    std::int32_t& resultat_api,
    std::vector<std::uint8_t>& payload_api,
    int timeout_ms
)
{
    const auto debut =
        std::chrono::steady_clock::now();

    for (;;)
    {
        const auto ecoule =
            std::chrono::duration_cast<
                std::chrono::milliseconds
            >(
                std::chrono::steady_clock::now() -
                debut
            ).count();

        const int restant =
            timeout_ms -
            static_cast<int>(ecoule);

        if (restant <= 0)
            return false;

        std::uint32_t commande_iot = 0;
        std::int32_t erreur_iot = 0;
        std::vector<std::uint8_t> payload_iot;

        if (!recevoir_iot(
                fd,
                commande_iot,
                erreur_iot,
                payload_iot,
                restant
            ))
        {
            return false;
        }

        if (
            erreur_iot != 0 ||
            (
                commande_iot != 19 &&
                commande_iot != 43
            ) ||
            payload_iot.size() < 24
        )
        {
            continue;
        }

        if (
            lire_u32_le(
                payload_iot.data()
            ) != 0x4B503250
        )
        {
            continue;
        }

        commande_api =
            lire_u32_le(
                payload_iot.data() + 12
            );

        resultat_api =
            static_cast<std::int32_t>(
                lire_u32_le(
                    payload_iot.data() + 16
                )
            );

        const std::uint32_t longueur =
            lire_u32_le(
                payload_iot.data() + 20
            );

        if (
            longueur >
            payload_iot.size() - 24
        )
        {
            return false;
        }

        payload_api.assign(
            payload_iot.begin() + 24,
            payload_iot.begin() + 24 + longueur
        );

        return true;
    }
}

} // namespace

ResultatSondeEsee sonder_service_esee(
    const std::string& adresse_nvr,
    int port,
    int timeout_ms
)
{
    ResultatSondeEsee resultat;
    resultat.port = port;

    if (adresse_nvr.empty())
    {
        resultat.detail =
            "adresse NVR vide";
        return resultat;
    }

    if (port <= 0 || port > 65535)
    {
        resultat.detail =
            "port invalide";
        return resultat;
    }

    if (timeout_ms <= 0)
        timeout_ms = 2000;

    addrinfo criteres = {};
    criteres.ai_family = AF_UNSPEC;
    criteres.ai_socktype = SOCK_STREAM;
    criteres.ai_protocol = IPPROTO_TCP;

    addrinfo* adresses = nullptr;

    const std::string service =
        std::to_string(port);

    const int resolution =
        getaddrinfo(
            adresse_nvr.c_str(),
            service.c_str(),
            &criteres,
            &adresses
        );

    if (resolution != 0)
    {
        resultat.detail =
            std::string("résolution impossible : ") +
            gai_strerror(resolution);

        return resultat;
    }

    std::string derniere_erreur =
        "aucune adresse joignable";

    for (
        addrinfo* courant = adresses;
        courant;
        courant = courant->ai_next
    )
    {
        const int fd =
            socket(
                courant->ai_family,
                courant->ai_socktype,
                courant->ai_protocol
            );

        if (fd < 0)
        {
            derniere_erreur =
                std::strerror(errno);
            continue;
        }

        const int drapeaux =
            fcntl(fd, F_GETFL, 0);

        if (
            drapeaux < 0 ||
            fcntl(
                fd,
                F_SETFL,
                drapeaux | O_NONBLOCK
            ) < 0
        )
        {
            derniere_erreur =
                std::strerror(errno);

            close(fd);
            continue;
        }

        const int connexion =
            connect(
                fd,
                courant->ai_addr,
                courant->ai_addrlen
            );

        if (connexion == 0)
        {
            resultat.joignable = true;
            resultat.detail =
                "connexion TCP immédiate";

            close(fd);
            break;
        }

        if (errno != EINPROGRESS)
        {
            derniere_erreur =
                std::strerror(errno);

            close(fd);
            continue;
        }

        pollfd attente = {};
        attente.fd = fd;
        attente.events = POLLOUT;

        const int pret =
            poll(
                &attente,
                1,
                timeout_ms
            );

        if (pret > 0)
        {
            int erreur_socket = 0;
            socklen_t taille_erreur =
                sizeof(erreur_socket);

            if (
                getsockopt(
                    fd,
                    SOL_SOCKET,
                    SO_ERROR,
                    &erreur_socket,
                    &taille_erreur
                ) == 0 &&
                erreur_socket == 0
            )
            {
                resultat.joignable = true;
                resultat.detail =
                    "connexion TCP établie";

                close(fd);
                break;
            }

            if (erreur_socket != 0)
            {
                derniere_erreur =
                    std::strerror(erreur_socket);
            }
        }
        else if (pret == 0)
        {
            derniere_erreur =
                "délai de connexion dépassé";
        }
        else
        {
            derniere_erreur =
                std::strerror(errno);
        }

        close(fd);
    }

    freeaddrinfo(adresses);

    if (!resultat.joignable)
    {
        resultat.detail =
            derniere_erreur;
    }

    return resultat;
}


ResultatHandshakeEsee tester_handshake_esee(
    const std::string& adresse_nvr,
    int port,
    int timeout_ms
)
{
    ResultatHandshakeEsee resultat;

    if (
        adresse_nvr.empty() ||
        port <= 0 ||
        port > 65535
    )
    {
        resultat.detail =
            "paramètres invalides";
        return resultat;
    }

    if (timeout_ms <= 0)
        timeout_ms = 3000;

    std::uint32_t sid = 0;

    const int fd =
        ouvrir_websocket_arq(
            adresse_nvr,
            port,
            timeout_ms,
            resultat.websocket,
            resultat.arq,
            sid,
            resultat.detail
        );

    if (fd >= 0)
        close(fd);

    return resultat;
}

ResultatSessionIotEsee tester_session_iot_esee(
    const std::string& adresse_nvr,
    int port,
    int timeout_ms
)
{
    ResultatSessionIotEsee resultat;

    if (
        adresse_nvr.empty() ||
        port <= 0 ||
        port > 65535
    )
    {
        resultat.detail =
            "paramètres invalides";
        return resultat;
    }

    if (timeout_ms <= 0)
        timeout_ms = 3000;

    std::uint32_t sid = 0;

    const int fd =
        ouvrir_websocket_arq(
            adresse_nvr,
            port,
            timeout_ms,
            resultat.websocket,
            resultat.arq,
            sid,
            resultat.detail
        );

    if (fd < 0)
        return resultat;

    std::vector<std::uint8_t>
        payload_ouverture;

    ajouter_u32_le(
        payload_ouverture,
        sid
    );

    ajouter_u32_le(
        payload_ouverture,
        0
    );

    if (
        !envoyer_iot(
            fd,
            20,
            sid,
            payload_ouverture,
            timeout_ms
        )
    )
    {
        resultat.detail =
            "ARQ OK, échec envoi IOT_OPEN_REQ";
        close(fd);
        return resultat;
    }

    std::uint32_t commande = 0;
    std::int32_t erreur = 0;
    std::vector<std::uint8_t> payload;

    if (
        !recevoir_iot(
            fd,
            commande,
            erreur,
            payload,
            timeout_ms
        )
    )
    {
        resultat.detail =
            "ARQ OK, aucune réponse IOT";
        close(fd);
        return resultat;
    }

    if (
        commande == 21 &&
        erreur == 0
    )
    {
        resultat.iot = true;
        resultat.detail =
            "WebSocket + ARQ + IOT_OPEN confirmés";
    }
    else
    {
        std::ostringstream detail;

        detail
            << "réponse IOT inattendue : cmd="
            << commande
            << " erreur="
            << erreur;

        resultat.detail =
            detail.str();
    }

    close(fd);
    return resultat;
}


ResultatAuthEsee tester_auth_esee(
    const std::string& adresse_nvr,
    const std::string& utilisateur,
    const std::string& mot_de_passe,
    int port,
    int timeout_ms
)
{
    ResultatAuthEsee resultat;
    resultat.code = -1;

    if (
        adresse_nvr.empty() ||
        utilisateur.empty() ||
        port <= 0 ||
        port > 65535
    )
    {
        resultat.detail =
            "paramètres invalides";
        return resultat;
    }

    if (
        utilisateur.size() >= 32 ||
        mot_de_passe.size() >= 32
    )
    {
        resultat.detail =
            "identifiants trop longs pour KP2P";
        return resultat;
    }

    if (timeout_ms <= 0)
        timeout_ms = 3000;

    std::uint32_t sid = 0;

    const int fd =
        ouvrir_websocket_arq(
            adresse_nvr,
            port,
            timeout_ms,
            resultat.websocket,
            resultat.arq,
            sid,
            resultat.detail
        );

    if (fd < 0)
        return resultat;

    if (
        !ouvrir_iot_sur_fd(
            fd,
            sid,
            timeout_ms,
            resultat.detail
        )
    )
    {
        close(fd);
        return resultat;
    }

    resultat.iot = true;

    std::vector<std::uint8_t>
        utilisateur_chiffre;
    std::vector<std::uint8_t>
        mot_de_passe_chiffre;

    if (
        !chiffrer_champ_auth(
            utilisateur,
            utilisateur_chiffre
        ) ||
        !chiffrer_champ_auth(
            mot_de_passe,
            mot_de_passe_chiffre
        )
    )
    {
        resultat.detail =
            "échec du chiffrement des identifiants";
        close(fd);
        return resultat;
    }

    std::vector<std::uint8_t> auth_payload;
    auth_payload.reserve(64);

    auth_payload.insert(
        auth_payload.end(),
        utilisateur_chiffre.begin(),
        utilisateur_chiffre.end()
    );

    auth_payload.insert(
        auth_payload.end(),
        mot_de_passe_chiffre.begin(),
        mot_de_passe_chiffre.end()
    );

    if (
        !envoyer_api(
            fd,
            sid,
            1,
            10,
            auth_payload,
            timeout_ms
        )
    )
    {
        resultat.detail =
            "IOT OK, échec envoi API_AUTH_REQ";
        close(fd);
        return resultat;
    }

    std::uint32_t commande_api = 0;
    std::int32_t resultat_api = -1;
    std::vector<std::uint8_t> reponse_payload;

    if (
        !recevoir_api(
            fd,
            commande_api,
            resultat_api,
            reponse_payload,
            timeout_ms
        )
    )
    {
        resultat.detail =
            "IOT OK, aucune réponse API_AUTH_RSP";
        close(fd);
        return resultat;
    }

    resultat.code =
        resultat_api;

    if (
        commande_api == 11 &&
        resultat_api == 0
    )
    {
        resultat.auth = true;
        resultat.detail =
            "WebSocket + ARQ + IOT + authentification KP2P confirmés";
    }
    else
    {
        std::ostringstream detail;

        detail
            << "authentification refusée ou réponse inattendue : cmd="
            << commande_api
            << " code="
            << resultat_api;

        if (resultat_api == -20)
        {
            detail
                << " (KP2P_ERR_AUTH_FAILED)";
        }
        else if (resultat_api == -21)
        {
            detail
                << " (KP2P_ERR_GET_NONCE_FAILED)";
        }
        else if (resultat_api == -22)
        {
            detail
                << " (KP2P_ERR_AUTH2_FAILED)";
        }

        resultat.detail =
            detail.str();
    }

    close(fd);
    return resultat;
}


ResultatAuthEsee tester_auth3_esee(
    const std::string& adresse_nvr,
    const std::string& utilisateur,
    const std::string& mot_de_passe,
    int port,
    int timeout_ms
)
{
    ResultatAuthEsee resultat;
    resultat.code = -1;

    if (
        adresse_nvr.empty() ||
        utilisateur.empty() ||
        port <= 0 ||
        port > 65535
    )
    {
        resultat.detail =
            "paramètres invalides";
        return resultat;
    }

    if (
        utilisateur.size() >= 1024 ||
        mot_de_passe.size() >= 1024
    )
    {
        resultat.detail =
            "identifiants trop longs pour AUTH3";
        return resultat;
    }

    if (timeout_ms <= 0)
        timeout_ms = 3000;

    std::uint32_t sid = 0;

    const int fd =
        ouvrir_websocket_arq(
            adresse_nvr,
            port,
            timeout_ms,
            resultat.websocket,
            resultat.arq,
            sid,
            resultat.detail
        );

    if (fd < 0)
        return resultat;

    if (
        !ouvrir_iot_sur_fd(
            fd,
            sid,
            timeout_ms,
            resultat.detail
        )
    )
    {
        close(fd);
        return resultat;
    }

    resultat.iot = true;

    std::vector<std::uint8_t> auth_payload(
        2048,
        0
    );

    std::memcpy(
        auth_payload.data(),
        utilisateur.data(),
        utilisateur.size()
    );

    std::memcpy(
        auth_payload.data() + 1024,
        mot_de_passe.data(),
        mot_de_passe.size()
    );

    if (
        !envoyer_api(
            fd,
            sid,
            1,
            140,
            auth_payload,
            timeout_ms
        )
    )
    {
        resultat.detail =
            "IOT OK, échec envoi API_AUTH3_REQ";
        close(fd);
        return resultat;
    }

    std::uint32_t commande_api = 0;
    std::int32_t resultat_api = -1;
    std::vector<std::uint8_t> reponse_payload;

    if (
        !recevoir_api(
            fd,
            commande_api,
            resultat_api,
            reponse_payload,
            timeout_ms
        )
    )
    {
        resultat.detail =
            "IOT OK, aucune réponse API_AUTH3_RSP";
        close(fd);
        return resultat;
    }

    resultat.code =
        resultat_api;

    if (
        commande_api == 141 &&
        resultat_api == 0
    )
    {
        resultat.auth = true;
        resultat.detail =
            "WebSocket + ARQ + IOT + AUTH3 confirmés";
    }
    else
    {
        std::ostringstream detail;

        detail
            << "AUTH3 refusée ou réponse inattendue : cmd="
            << commande_api
            << " code="
            << resultat_api;

        resultat.detail =
            detail.str();
    }

    close(fd);
    return resultat;
}



ResultatRechercheEsee tester_recherche_replay_esee(
    const std::string& adresse_nvr,
    const std::string& utilisateur,
    const std::string& mot_de_passe,
    int canal,
    int type,
    int port,
    int timeout_ms
)
{
    ResultatRechercheEsee resultat;
    resultat.code = -1;

    if (
        adresse_nvr.empty() ||
        utilisateur.empty() ||
        canal < 0 ||
        type < 0 ||
        port <= 0 ||
        port > 65535
    )
    {
        resultat.detail = "paramètres invalides";
        return resultat;
    }

    if (
        utilisateur.size() >= 32 ||
        mot_de_passe.size() >= 32
    )
    {
        resultat.detail =
            "identifiants trop longs pour KP2P";
        return resultat;
    }

    if (timeout_ms <= 0)
        timeout_ms = 3000;

    std::uint32_t sid = 0;

    const int fd =
        ouvrir_websocket_arq(
            adresse_nvr,
            port,
            timeout_ms,
            resultat.websocket,
            resultat.arq,
            sid,
            resultat.detail
        );

    if (fd < 0)
        return resultat;

    if (
        !ouvrir_iot_sur_fd(
            fd,
            sid,
            timeout_ms,
            resultat.detail
        )
    )
    {
        close(fd);
        return resultat;
    }

    resultat.iot = true;

    std::vector<std::uint8_t> utilisateur_chiffre;
    std::vector<std::uint8_t> mot_de_passe_chiffre;

    if (
        !chiffrer_champ_auth(
            utilisateur,
            utilisateur_chiffre
        ) ||
        !chiffrer_champ_auth(
            mot_de_passe,
            mot_de_passe_chiffre
        )
    )
    {
        resultat.detail =
            "échec du chiffrement des identifiants";
        close(fd);
        return resultat;
    }

    std::vector<std::uint8_t> auth_payload;
    auth_payload.reserve(64);
    auth_payload.insert(
        auth_payload.end(),
        utilisateur_chiffre.begin(),
        utilisateur_chiffre.end()
    );
    auth_payload.insert(
        auth_payload.end(),
        mot_de_passe_chiffre.begin(),
        mot_de_passe_chiffre.end()
    );

    if (
        !envoyer_api(
            fd,
            sid,
            1,
            10,
            auth_payload,
            timeout_ms
        )
    )
    {
        resultat.detail =
            "échec envoi API_AUTH_REQ";
        close(fd);
        return resultat;
    }

    std::uint32_t commande_api = 0;
    std::int32_t resultat_api = -1;
    std::vector<std::uint8_t> payload_api;

    if (
        !recevoir_api(
            fd,
            commande_api,
            resultat_api,
            payload_api,
            timeout_ms
        ) ||
        commande_api != 11 ||
        resultat_api != 0
    )
    {
        resultat.code = resultat_api;
        resultat.detail =
            "authentification KP2P refusée avant recherche REPLAY";
        close(fd);
        return resultat;
    }

    resultat.auth = true;

    const std::time_t maintenant =
        std::time(nullptr);

    std::tm debut_tm = {};
    localtime_r(
        &maintenant,
        &debut_tm
    );

    debut_tm.tm_hour = 0;
    debut_tm.tm_min = 0;
    debut_tm.tm_sec = 0;
    debut_tm.tm_isdst = -1;

    std::tm fin_tm = debut_tm;
    fin_tm.tm_hour = 23;
    fin_tm.tm_min = 59;
    fin_tm.tm_sec = 59;

    const std::time_t debut_t =
        std::mktime(&debut_tm);

    const std::time_t fin_t =
        std::mktime(&fin_tm);

    if (
        debut_t == static_cast<std::time_t>(-1) ||
        fin_t == static_cast<std::time_t>(-1)
    )
    {
        resultat.detail =
            "impossible de calculer l'intervalle de recherche";
        close(fd);
        return resultat;
    }

    const std::vector<int> types_recherche =
        type == 15
            ? std::vector<int>{1, 2, 4, 8}
            : std::vector<int>{type};

    auto formater_epoch = [](
        std::uint32_t valeur
    )
    {
        const std::time_t t =
            static_cast<std::time_t>(
                valeur
            );

        std::tm locale = {};
        char tampon[64] = {};

        if (
            !localtime_r(
                &t,
                &locale
            ) ||
            std::strftime(
                tampon,
                sizeof(tampon),
                "%Y-%m-%d %H:%M:%S",
                &locale
            ) == 0
        )
        {
            return
                std::to_string(
                    valeur
                );
        }

        return std::string(tampon);
    };

    std::uint32_t ticket = 2;
    std::uint32_t total_annonce = 0;
    std::ostringstream detail_types;
    bool premier_type = true;

    for (const int type_courant : types_recherche)
    {
        std::vector<std::uint8_t> recherche(
            52,
            0
        );

        auto ecrire_u32 = [&recherche](
            std::size_t offset,
            std::uint32_t valeur
        )
        {
            recherche[offset + 0] =
                static_cast<std::uint8_t>(
                    valeur & 0xFF
                );
            recherche[offset + 1] =
                static_cast<std::uint8_t>(
                    (valeur >> 8) & 0xFF
                );
            recherche[offset + 2] =
                static_cast<std::uint8_t>(
                    (valeur >> 16) & 0xFF
                );
            recherche[offset + 3] =
                static_cast<std::uint8_t>(
                    (valeur >> 24) & 0xFF
                );
        };

        ecrire_u32(0, 1);
        ecrire_u32(4, 0);

        if (canal > 0 && canal < 128)
        {
            const std::size_t octet =
                8 +
                static_cast<std::size_t>(
                    canal / 8
                );

            if (octet < 24)
            {
                recherche[octet] |=
                    static_cast<std::uint8_t>(
                        1U << (canal % 8)
                    );
            }
        }

        ecrire_u32(
            24,
            static_cast<std::uint32_t>(
                type_courant
            )
        );
        ecrire_u32(28, 0);
        ecrire_u32(
            32,
            static_cast<std::uint32_t>(
                debut_t
            )
        );
        ecrire_u32(
            36,
            static_cast<std::uint32_t>(
                fin_t
            )
        );
        ecrire_u32(40, 0);
        ecrire_u32(44, 0);
        ecrire_u32(48, 10);

        if (
            !envoyer_api(
                fd,
                sid,
                ticket++,
                40,
                recherche,
                timeout_ms
            )
        )
        {
            resultat.detail =
                "AUTH OK, échec envoi REPLAY SEARCH";
            close(fd);
            return resultat;
        }

        payload_api.clear();

        if (
            !recevoir_api(
                fd,
                commande_api,
                resultat_api,
                payload_api,
                timeout_ms
            )
        )
        {
            resultat.detail =
                "AUTH OK, aucune réponse REPLAY SEARCH";
            close(fd);
            return resultat;
        }

        resultat.code = resultat_api;

        if (
            commande_api != 41 ||
            resultat_api != 0
        )
        {
            std::ostringstream detail;
            detail
                << "REPLAY SEARCH refusé ou inattendu : cmd="
                << commande_api
                << " code="
                << resultat_api
                << " type="
                << type_courant;
            resultat.detail = detail.str();
            close(fd);
            return resultat;
        }

        if (payload_api.size() < 52)
        {
            resultat.detail =
                "REPLAY SEARCH reçu avec payload trop court";
            close(fd);
            return resultat;
        }

        const std::uint32_t replay_cmd =
            lire_u32_le(
                payload_api.data() + 0
            );

        const std::uint32_t file_count =
            lire_u32_le(
                payload_api.data() + 44
            );

        const std::uint32_t file_total =
            lire_u32_le(
                payload_api.data() + 48
            );

        if (replay_cmd != 1)
        {
            std::ostringstream detail;
            detail
                << "REPLAY_RSP inattendu : sous-commande="
                << replay_cmd;
            resultat.detail = detail.str();
            close(fd);
            return resultat;
        }

        total_annonce +=
            file_total;

        if (!premier_type)
            detail_types << ", ";

        detail_types
            << "type "
            << type_courant
            << "="
            << file_total;

        premier_type = false;

        const std::size_t disponibles =
            (payload_api.size() - 52) / 20;

        const std::size_t places_restantes =
            resultat.fichiers.size() < 5
                ? 5 - resultat.fichiers.size()
                : 0;

        const std::size_t a_lire =
            std::min<std::size_t>(
                {
                    static_cast<std::size_t>(
                        file_count
                    ),
                    disponibles,
                    places_restantes
                }
            );

        for (
            std::size_t i = 0;
            i < a_lire;
            ++i
        )
        {
            const std::uint8_t* p =
                payload_api.data() +
                52 +
                i * 20;

            FichierRechercheEsee fichier;
            fichier.canal =
                lire_u32_le(p + 0);
            fichier.type =
                lire_u32_le(p + 4);
            fichier.debut =
                formater_epoch(
                    lire_u32_le(p + 8)
                );
            fichier.fin =
                formater_epoch(
                    lire_u32_le(p + 12)
                );
            fichier.qualite =
                lire_u32_le(p + 16);

            resultat.fichiers.push_back(
                std::move(fichier)
            );
        }
    }

    resultat.recherche = true;
    resultat.code = 0;

    std::ostringstream detail;
    detail
        << "REPLAY SEARCH confirmé : "
        << total_annonce
        << " au total ("
        << detail_types.str()
        << ")";

    resultat.detail = detail.str();

    close(fd);
    return resultat;
}

ResultatRechercheEsee tester_recherche_native_esee(
    const std::string& adresse_nvr,
    const std::string& utilisateur,
    const std::string& mot_de_passe,
    int canal,
    int type,
    int port,
    int timeout_ms
)
{
    ResultatRechercheEsee resultat;
    resultat.code = -1;

    if (
        adresse_nvr.empty() ||
        utilisateur.empty() ||
        canal < 0 ||
        type < 0 ||
        port <= 0 ||
        port > 65535
    )
    {
        resultat.detail = "paramètres invalides";
        return resultat;
    }

    if (
        utilisateur.size() >= 32 ||
        mot_de_passe.size() >= 32
    )
    {
        resultat.detail =
            "identifiants trop longs pour KP2P";
        return resultat;
    }

    if (timeout_ms <= 0)
        timeout_ms = 3000;

    std::uint32_t sid = 0;

    const int fd =
        ouvrir_websocket_arq(
            adresse_nvr,
            port,
            timeout_ms,
            resultat.websocket,
            resultat.arq,
            sid,
            resultat.detail
        );

    if (fd < 0)
        return resultat;

    if (
        !ouvrir_iot_sur_fd(
            fd,
            sid,
            timeout_ms,
            resultat.detail
        )
    )
    {
        close(fd);
        return resultat;
    }

    resultat.iot = true;

    std::vector<std::uint8_t> utilisateur_chiffre;
    std::vector<std::uint8_t> mot_de_passe_chiffre;

    if (
        !chiffrer_champ_auth(
            utilisateur,
            utilisateur_chiffre
        ) ||
        !chiffrer_champ_auth(
            mot_de_passe,
            mot_de_passe_chiffre
        )
    )
    {
        resultat.detail =
            "échec du chiffrement des identifiants";
        close(fd);
        return resultat;
    }

    std::vector<std::uint8_t> auth_payload;
    auth_payload.reserve(64);
    auth_payload.insert(
        auth_payload.end(),
        utilisateur_chiffre.begin(),
        utilisateur_chiffre.end()
    );
    auth_payload.insert(
        auth_payload.end(),
        mot_de_passe_chiffre.begin(),
        mot_de_passe_chiffre.end()
    );

    if (
        !envoyer_api(
            fd,
            sid,
            1,
            10,
            auth_payload,
            timeout_ms
        )
    )
    {
        resultat.detail =
            "échec envoi API_AUTH_REQ";
        close(fd);
        return resultat;
    }

    std::uint32_t commande_api = 0;
    std::int32_t resultat_api = -1;
    std::vector<std::uint8_t> payload_api;

    if (
        !recevoir_api(
            fd,
            commande_api,
            resultat_api,
            payload_api,
            timeout_ms
        ) ||
        commande_api != 11 ||
        resultat_api != 0
    )
    {
        resultat.code = resultat_api;
        resultat.detail =
            "authentification KP2P refusée avant recherche";
        close(fd);
        return resultat;
    }

    resultat.auth = true;
    resultat.code = 0;

    const std::time_t maintenant =
        std::time(nullptr);

    std::tm locale = {};

    if (!localtime_r(&maintenant, &locale))
    {
        resultat.detail =
            "impossible de déterminer la date locale";
        close(fd);
        return resultat;
    }

    std::vector<std::uint8_t> find_start;
    find_start.reserve(56);

    ajouter_u32_le(
        find_start,
        static_cast<std::uint32_t>(canal)
    );
    ajouter_u32_le(
        find_start,
        static_cast<std::uint32_t>(type)
    );

    auto ajouter_date = [&find_start](
        std::uint32_t annee,
        std::uint32_t mois,
        std::uint32_t jour,
        std::uint32_t heure,
        std::uint32_t minute,
        std::uint32_t seconde
    )
    {
        ajouter_u32_le(find_start, annee);
        ajouter_u32_le(find_start, mois);
        ajouter_u32_le(find_start, jour);
        ajouter_u32_le(find_start, heure);
        ajouter_u32_le(find_start, minute);
        ajouter_u32_le(find_start, seconde);
    };

    const std::uint32_t annee =
        static_cast<std::uint32_t>(
            locale.tm_year + 1900
        );
    const std::uint32_t mois =
        static_cast<std::uint32_t>(
            locale.tm_mon + 1
        );
    const std::uint32_t jour =
        static_cast<std::uint32_t>(
            locale.tm_mday
        );

    ajouter_date(
        annee,
        mois,
        jour,
        0,
        0,
        0
    );

    ajouter_date(
        annee,
        mois,
        jour,
        23,
        59,
        59
    );

    if (
        !envoyer_api(
            fd,
            sid,
            2,
            90,
            find_start,
            timeout_ms
        )
    )
    {
        resultat.detail =
            "AUTH OK, échec envoi FIND_START";
        close(fd);
        return resultat;
    }

    payload_api.clear();

    if (
        !recevoir_api(
            fd,
            commande_api,
            resultat_api,
            payload_api,
            timeout_ms
        )
    )
    {
        resultat.detail =
            "AUTH OK, aucune réponse FIND_START";
        close(fd);
        return resultat;
    }

    resultat.code = resultat_api;

    if (
        commande_api != 91 ||
        resultat_api != 0
    )
    {
        std::ostringstream detail;
        detail
            << "FIND_START refusé ou inattendu : cmd="
            << commande_api
            << " code="
            << resultat_api;
        resultat.detail = detail.str();
        close(fd);
        return resultat;
    }

    resultat.recherche = true;

    auto formater_date = [](
        const std::uint8_t* p
    )
    {
        char tampon[64] = {};

        std::snprintf(
            tampon,
            sizeof(tampon),
            "%04u-%02u-%02u %02u:%02u:%02u",
            lire_u32_le(p + 0),
            lire_u32_le(p + 4),
            lire_u32_le(p + 8),
            lire_u32_le(p + 12),
            lire_u32_le(p + 16),
            lire_u32_le(p + 20)
        );

        return std::string(tampon);
    };

    std::uint32_t ticket = 3;
    bool anomalie_next = false;

    for (int i = 0; i < 5; ++i)
    {
        std::vector<std::uint8_t> next_payload;
        ajouter_u32_le(next_payload, 0);

        if (
            !envoyer_api(
                fd,
                sid,
                ticket++,
                100,
                next_payload,
                timeout_ms
            )
        )
        {
            resultat.detail =
                "FIND_START OK, échec envoi FIND_NEXT";
            anomalie_next = true;
            break;
        }

        payload_api.clear();

        if (
            !recevoir_api(
                fd,
                commande_api,
                resultat_api,
                payload_api,
                timeout_ms
            )
        )
        {
            resultat.detail =
                "FIND_START OK, aucune réponse FIND_NEXT";
            anomalie_next = true;
            break;
        }

        if (
            commande_api != 101 ||
            resultat_api != 0
        )
        {
            resultat.code = resultat_api;

            std::ostringstream detail;
            detail
                << "FIND_NEXT terminé ou refusé : cmd="
                << commande_api
                << " code="
                << resultat_api;

            resultat.detail = detail.str();
            anomalie_next = true;
            break;
        }

        if (payload_api.size() < 60)
        {
            resultat.detail =
                "FIND_NEXT reçu avec payload trop court";
            anomalie_next = true;
            break;
        }

        FichierRechercheEsee fichier;
        fichier.canal =
            lire_u32_le(
                payload_api.data() + 0
            );
        fichier.type =
            lire_u32_le(
                payload_api.data() + 4
            );
        fichier.taille =
            lire_u32_le(
                payload_api.data() + 8
            );
        fichier.debut =
            formater_date(
                payload_api.data() + 12
            );
        fichier.fin =
            formater_date(
                payload_api.data() + 36
            );

        resultat.fichiers.push_back(
            std::move(fichier)
        );
    }

    std::vector<std::uint8_t> stop_payload;
    ajouter_u32_le(stop_payload, 0);

    (void)envoyer_api(
        fd,
        sid,
        ticket,
        110,
        stop_payload,
        timeout_ms
    );

    if (!anomalie_next)
    {
        resultat.code = 0;

        std::ostringstream detail;
        detail
            << "recherche native KP2P confirmée, "
            << resultat.fichiers.size()
            << " enregistrement(s) lu(s)";

        resultat.detail = detail.str();
    }

    close(fd);
    return resultat;
}

EnteteNarf analyser_entete_narf(
    const std::uint8_t* donnees,
    std::size_t taille
)
{
    EnteteNarf resultat;

    if (
        !donnees ||
        taille < 136
    )
    {
        return resultat;
    }

    static constexpr std::uint8_t magic_transport[4] =
    {
        0xAB, 0xBC, 0xCD, 0xDE
    };

    if (
        std::memcmp(
            donnees,
            magic_transport,
            sizeof(magic_transport)
        ) != 0
    )
    {
        return resultat;
    }

    if (
        !correspond(
            donnees + 32,
            "NARF",
            4
        ) ||
        !correspond(
            donnees + 80,
            "MARF",
            4
        )
    )
    {
        return resultat;
    }

    resultat.valide = true;

    resultat.taille_payload =
        lire_u32_le(
            donnees + 84
        );

    resultat.timestamp_ms =
        lire_u64_le(
            donnees + 88
        );

    resultat.codec =
        lire_chaine_ascii(
            donnees + 112,
            8
        );

    resultat.champ_120 =
        lire_u32_le(
            donnees + 120
        );

    resultat.champ_124 =
        lire_u32_le(
            donnees + 124
        );

    resultat.champ_128 =
        lire_u32_le(
            donnees + 128
        );

    return resultat;
}
