#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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


struct FichierRechercheEsee
{
    std::uint32_t canal = 0;
    std::uint32_t type = 0;
    std::uint32_t qualite = 0;
    std::string debut;
    std::string fin;
};

struct ResultatRechercheEsee
{
    bool websocket = false;
    bool arq = false;
    bool iot = false;
    bool auth = false;
    bool recherche = false;
    int code = 0;
    std::string detail;
    std::vector<FichierRechercheEsee> fichiers;
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


struct ResultatReplayEsee
{
    bool websocket = false;
    bool arq = false;
    bool iot = false;
    bool auth = false;
    bool demarrage = false;
    bool media = false;
    int code = 0;
    std::string detail;
    EnteteNarf premiere_trame;
    std::size_t taille_message_media = 0;
    int offset_narf = -1;
    int offset_marf = -1;
    std::string apercu_hex;
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

ResultatAuthEsee tester_auth3_esee(
    const std::string& adresse_nvr,
    const std::string& utilisateur,
    const std::string& mot_de_passe,
    int port = 10000,
    int timeout_ms = 3000
);



ResultatRechercheEsee tester_recherche_replay_esee(
    const std::string& adresse_nvr,
    const std::string& utilisateur,
    const std::string& mot_de_passe,
    int canal = 0,
    int type = 15,
    int port = 10000,
    int timeout_ms = 3000,
    std::int64_t debut_epoch = 0,
    std::int64_t fin_epoch = 0
);

ResultatRechercheEsee tester_recherche_native_esee(
    const std::string& adresse_nvr,
    const std::string& utilisateur,
    const std::string& mot_de_passe,
    int canal = 0,
    int type = 15,
    int port = 10000,
    int timeout_ms = 3000
);


ResultatReplayEsee tester_replay_start_esee(
    const std::string& adresse_nvr,
    const std::string& utilisateur,
    const std::string& mot_de_passe,
    int canal,
    int type,
    std::int64_t debut_epoch,
    std::int64_t fin_epoch,
    int port = 10000,
    int timeout_ms = 5000
);

EnteteNarf analyser_entete_narf(
    const std::uint8_t* donnees,
    std::size_t taille
);
