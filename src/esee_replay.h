#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct ResultatSondeEsee
{
    bool joignable = false;
    int port = 10000;
    std::string detail;
};

struct ResultatHandshakeEsee
{
    bool websocket = false;
    bool arq = false;
    std::string detail;
};

struct ResultatSessionIotEsee
{
    bool websocket = false;
    bool arq = false;
    bool iot = false;
    std::string detail;
};

struct ResultatAuthEsee
{
    bool websocket = false;
    bool arq = false;
    bool iot = false;
    bool auth = false;
    int code = 0;
    std::string detail;
};

struct EnteteNarf
{
    bool valide = false;
    std::uint32_t taille_payload = 0;
    std::uint64_t timestamp_ms = 0;
    std::string codec;
    std::uint32_t champ_120 = 0;
    std::uint32_t champ_124 = 0;
    std::uint32_t champ_128 = 0;
};

ResultatSondeEsee sonder_service_esee(
    const std::string& adresse_nvr,
    int port = 10000,
    int timeout_ms = 2000
);

ResultatHandshakeEsee tester_handshake_esee(
    const std::string& adresse_nvr,
    int port = 10000,
    int timeout_ms = 3000
);

ResultatSessionIotEsee tester_session_iot_esee(
    const std::string& adresse_nvr,
    int port = 10000,
    int timeout_ms = 3000
);

ResultatAuthEsee tester_auth_esee(
    const std::string& adresse_nvr,
    const std::string& utilisateur,
    const std::string& mot_de_passe,
    int port = 10000,
    int timeout_ms = 3000
);

EnteteNarf analyser_entete_narf(
    const std::uint8_t* donnees,
    std::size_t taille
);
