#include "esee_replay.h"

#include <cerrno>
#include <cstring>
#include <sstream>
#include <array>
#include <random>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

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
    if (payload.size() > 125)
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
        2 +
        masque.size() +
        payload.size()
    );

    trame.push_back(0x82);
    trame.push_back(
        static_cast<std::uint8_t>(
            0x80 |
            payload.size()
        )
    );

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
        resultat.detail =
            detail_tcp;
        return resultat;
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
        resultat.detail =
            "échec envoi Upgrade WebSocket";
        close(fd);
        return resultat;
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
        resultat.detail =
            "aucune réponse HTTP/WebSocket";
        close(fd);
        return resultat;
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
        resultat.detail =
            "Upgrade WebSocket refusé : " +
            statut;

        close(fd);
        return resultat;
    }

    resultat.websocket = true;

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

    constexpr std::uint32_t sid =
        1234;

    std::vector<std::uint8_t>
        ouverture(
            std::begin(arq_open),
            std::end(arq_open)
        );

    ouverture.push_back(
        static_cast<std::uint8_t>(
            sid & 0xFF
        )
    );

    ouverture.push_back(
        static_cast<std::uint8_t>(
            (sid >> 8) & 0xFF
        )
    );

    ouverture.push_back(
        static_cast<std::uint8_t>(
            (sid >> 16) & 0xFF
        )
    );

    ouverture.push_back(
        static_cast<std::uint8_t>(
            (sid >> 24) & 0xFF
        )
    );

    if (
        !envoyer_ws_binaire(
            fd,
            ouverture,
            timeout_ms
        )
    )
    {
        resultat.detail =
            "WebSocket OK, échec envoi ARQ_OPEN";
        close(fd);
        return resultat;
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
        resultat.detail =
            "WebSocket OK, aucune réponse ARQ";
        close(fd);
        return resultat;
    }

    if (
        reponse_arq.size() ==
            sizeof(arq_reponse) &&
        std::memcmp(
            reponse_arq.data(),
            arq_reponse,
            sizeof(arq_reponse)
        ) == 0
    )
    {
        resultat.arq = true;
        resultat.detail =
            "WebSocket 101 + ARQ_OPEN reconnu";
    }
    else
    {
        std::ostringstream detail;

        detail
            << "WebSocket OK, réponse ARQ inattendue ("
            << reponse_arq.size()
            << " octets)";

        resultat.detail =
            detail.str();
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
