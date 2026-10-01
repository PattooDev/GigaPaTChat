#include "esee_replay.h"

#include <cerrno>
#include <cstring>
#include <sstream>

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
