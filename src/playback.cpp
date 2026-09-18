#include "playback.h"
#include "playback_hevc.h"

#include <csignal>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <SDL2/SDL.h>

extern volatile std::sig_atomic_t programme_actif;

namespace
{

struct CurlGlobal
{
    CurlGlobal()
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    ~CurlGlobal()
    {
        curl_global_cleanup();
    }
};

void assurer_curl()
{
    static CurlGlobal global;
    (void)global;
}

std::size_t reception_curl(
    char* donnees,
    std::size_t taille,
    std::size_t nombre,
    void* opaque
)
{
    if (!opaque)
        return 0;

    std::string* reponse =
        static_cast<std::string*>(opaque);

    const std::size_t octets =
        taille * nombre;

    reponse->append(donnees, octets);
    return octets;
}

std::string encoder_url(const std::string& texte)
{
    assurer_curl();

    CURL* curl = curl_easy_init();

    if (!curl)
        return "";

    char* encode =
        curl_easy_escape(
            curl,
            texte.c_str(),
            static_cast<int>(texte.size())
        );

    std::string resultat =
        encode ? encode : "";

    if (encode)
        curl_free(encode);

    curl_easy_cleanup(curl);
    return resultat;
}

std::string echapper_xml(const std::string& texte)
{
    std::string resultat;
    resultat.reserve(texte.size());

    for (const char caractere : texte)
    {
        switch (caractere)
        {
        case '&':
            resultat += "&amp;";
            break;
        case '"':
            resultat += "&quot;";
            break;
        case '<':
            resultat += "&lt;";
            break;
        case '>':
            resultat += "&gt;";
            break;
        case '\'':
            resultat += "&apos;";
            break;
        default:
            resultat += caractere;
            break;
        }
    }

    return resultat;
}

bool http_get(
    const std::string& url,
    std::string& reponse
)
{
    assurer_curl();

    CURL* curl = curl_easy_init();

    if (!curl)
        return false;

    reponse.clear();

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

    curl_easy_setopt(
        curl,
        CURLOPT_TIMEOUT,
        15L
    );

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        reception_curl
    );

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &reponse
    );

    const CURLcode resultat =
        curl_easy_perform(curl);

    long code_http = 0;

    if (resultat == CURLE_OK)
    {
        curl_easy_getinfo(
            curl,
            CURLINFO_RESPONSE_CODE,
            &code_http
        );
    }

    if (resultat != CURLE_OK)
    {
        std::cerr
            << "Erreur HTTP NVR : "
            << curl_easy_strerror(resultat)
            << "\n";
    }

    curl_easy_cleanup(curl);

    return
        resultat == CURLE_OK &&
        code_http >= 200 &&
        code_http < 300;
}

std::string attribut_xml(
    const std::string& balise,
    const std::string& nom
)
{
    const std::string marqueur =
        nom + "=\"";

    const std::size_t debut =
        balise.find(marqueur);

    if (debut == std::string::npos)
        return "";

    const std::size_t valeur =
        debut + marqueur.size();

    const std::size_t fin =
        balise.find('"', valeur);

    if (fin == std::string::npos)
        return "";

    return balise.substr(
        valeur,
        fin - valeur
    );
}

std::vector<std::string> separer(
    const std::string& texte,
    char separateur
)
{
    std::vector<std::string> morceaux;
    std::stringstream flux(texte);
    std::string morceau;

    while (std::getline(flux, morceau, separateur))
        morceaux.push_back(morceau);

    return morceaux;
}

std::string nom_type(int type)
{
    switch (type)
    {
    case 1:
        return "Time";
    case 2:
        return "Motion";
    case 4:
        return "Sensor";
    case 8:
        return "Manual";
    default:
        return "Inconnu";
    }
}

std::string formater_epoch(std::int64_t valeur)
{
    const std::time_t temps =
        static_cast<std::time_t>(valeur);

    std::tm locale = {};

    if (!localtime_r(&temps, &locale))
        return std::to_string(valeur);

    char tampon[64] = {};

    if (
        std::strftime(
            tampon,
            sizeof(tampon),
            "%Y-%m-%d %H:%M:%S",
            &locale
        ) == 0
    )
    {
        return std::to_string(valeur);
    }

    return tampon;
}

std::string date_du_jour()
{
    const std::time_t maintenant =
        std::time(nullptr);

    std::tm locale = {};
    localtime_r(&maintenant, &locale);

    char tampon[32] = {};

    std::strftime(
        tampon,
        sizeof(tampon),
        "%Y-%m-%d",
        &locale
    );

    return tampon;
}

std::string demander_texte(
    const std::string& libelle,
    const std::string& valeur_defaut
)
{
    std::cout
        << libelle
        << " ["
        << valeur_defaut
        << "] : ";

    std::string valeur;
    std::getline(std::cin, valeur);

    return
        valeur.empty()
            ? valeur_defaut
            : valeur;
}

int demander_entier(
    const std::string& libelle,
    int valeur_defaut
)
{
    const std::string texte =
        demander_texte(
            libelle,
            std::to_string(valeur_defaut)
        );

    try
    {
        return std::stoi(texte);
    }
    catch (...)
    {
        return valeur_defaut;
    }
}

bool rechercher_sessions(
    const std::string& adresse_nvr,
    const std::string& utilisateur,
    const std::string& mot_de_passe,
    int canaux,
    int types,
    const std::string& date,
    const std::string& debut,
    const std::string& fin,
    std::vector<SessionNVR>& sessions,
    int& total
)
{
    std::ostringstream xml;

    xml
        << "<juan ver=\"0\" squ=\"abcdef\" dir=\"0\" enc=\"1\">"
        << "<recsearch usr=\""
        << echapper_xml(utilisateur)
        << "\" pwd=\""
        << echapper_xml(mot_de_passe)
        << "\" channels=\""
        << canaux
        << "\" types=\""
        << types
        << "\" date=\""
        << echapper_xml(date)
        << "\" begin=\""
        << echapper_xml(debut)
        << "\" end=\""
        << echapper_xml(fin)
        << "\" session_index=\"0\" session_count=\"10\" />"
        << "</juan>";

    const std::string xml_encode =
        encoder_url(xml.str());

    if (xml_encode.empty())
    {
        std::cerr
            << "Erreur : encodage de la requête NVR impossible.\n";

        return false;
    }

    const std::string url =
        "http://" +
        adresse_nvr +
        "/cgi-bin/gw.cgi?xml=" +
        xml_encode;

    std::string reponse;

    if (!http_get(url, reponse))
    {
        std::cerr
            << "Erreur : le NVR ne répond pas à la recherche d'archives.\n";

        return false;
    }

    const std::size_t debut_recsearch =
        reponse.find("<recsearch");

    if (debut_recsearch == std::string::npos)
    {
        std::cerr
            << "Erreur : réponse recsearch introuvable.\n";

        return false;
    }

    const std::size_t fin_balise =
        reponse.find('>', debut_recsearch);

    if (fin_balise == std::string::npos)
    {
        std::cerr
            << "Erreur : réponse recsearch invalide.\n";

        return false;
    }

    const std::string balise =
        reponse.substr(
            debut_recsearch,
            fin_balise - debut_recsearch + 1
        );

    const std::string errno_nvr =
        attribut_xml(balise, "errno");

    if (
        !errno_nvr.empty() &&
        errno_nvr != "0"
    )
    {
        std::cerr
            << "Le NVR refuse la recherche (errno="
            << errno_nvr
            << ").\n";

        return false;
    }

    total = 0;

    const std::string total_texte =
        attribut_xml(
            balise,
            "session_total"
        );

    if (!total_texte.empty())
    {
        try
        {
            total =
                std::stoi(total_texte);
        }
        catch (...)
        {
            total = 0;
        }
    }

    sessions.clear();

    std::size_t position = fin_balise;

    while (true)
    {
        const std::size_t ouverture =
            reponse.find("<s>", position);

        if (ouverture == std::string::npos)
            break;

        const std::size_t fermeture =
            reponse.find(
                "</s>",
                ouverture + 3
            );

        if (fermeture == std::string::npos)
            break;

        const std::string contenu =
            reponse.substr(
                ouverture + 3,
                fermeture - ouverture - 3
            );

        const std::vector<std::string> donnees =
            separer(contenu, '|');

        if (donnees.size() >= 6)
        {
            try
            {
                SessionNVR session;

                session.numero = donnees[1];
                session.canal = std::stoi(donnees[2]);
                session.type = std::stoi(donnees[3]);
                session.debut = std::stoll(donnees[4]);
                session.fin = std::stoll(donnees[5]);

                sessions.push_back(session);
            }
            catch (...)
            {
                // Une entrée malformée est ignorée,
                // les autres restent utilisables.
            }
        }

        position =
            fermeture + 4;
    }

    return true;
}

} // namespace

bool rechercher_archives_nvr(
    const std::string& adresse_nvr,
    const std::string& utilisateur,
    const std::string& mot_de_passe,
    int canaux,
    int types,
    const std::string& date,
    const std::string& debut,
    const std::string& fin,
    std::vector<SessionNVR>& sessions,
    int& total
)
{
    return rechercher_sessions(
        adresse_nvr,
        utilisateur,
        mot_de_passe,
        canaux,
        types,
        date,
        debut,
        fin,
        sessions,
        total
    );
}

bool lire_session_archive_nvr(
    const std::string& adresse_nvr,
    const std::string& utilisateur,
    const std::string& mot_de_passe,
    const SessionNVR& session,
    SDL_Window* fenetre,
    SDL_Renderer* rendu
)
{
    const std::string utilisateur_url =
        encoder_url(utilisateur);

    const std::string mot_de_passe_url =
        encoder_url(mot_de_passe);

    if (
        utilisateur_url.empty() ||
        mot_de_passe_url.empty()
    )
    {
        std::cerr
            << "Erreur : encodage des identifiants NVR impossible.\n";

        return false;
    }

    const std::string url =
        "http://" +
        adresse_nvr +
        "/cgi-bin/flv.cgi?u=" +
        utilisateur_url +
        "&p=" +
        mot_de_passe_url +
        "&mode=time&chn=" +
        std::to_string(session.canal) +
        "&begin=" +
        std::to_string(session.debut) +
        "&end=" +
        std::to_string(session.fin) +
        "&audio=54&mute=false";

    return lire_archive_hevc_nvr(
        url,
        fenetre,
        rendu
    );
}

bool menu_lecture_nvr(
    const std::string& adresse_nvr,
    const std::string& utilisateur,
    const std::string& mot_de_passe,
    SDL_Window* fenetre,
    SDL_Renderer* rendu
)
{
    std::cout
        << "\n=====================================\n"
        << "        Lecture des archives NVR\n"
        << "=====================================\n";

    const std::string date =
        demander_texte(
            "Date (AAAA-MM-JJ)",
            date_du_jour()
        );

    const std::string debut =
        demander_texte(
            "Heure de début",
            "00:00:00"
        );

    const std::string fin =
        demander_texte(
            "Heure de fin",
            "23:59:59"
        );

    std::cout
        << "\nCanal : 0 = caméras 1 et 2, 1 = caméra 1, 2 = caméra 2\n";

    const int canal =
        demander_entier(
            "Canal",
            0
        );

    int canaux = 3;

    if (canal == 1)
        canaux = 1;
    else if (canal == 2)
        canaux = 2;

    std::cout
        << "\nTypes : 1=Time  2=Motion  4=Sensor  8=Manual  15=Tous\n";

    int types =
        demander_entier(
            "Types",
            15
        );

    if (types < 1 || types > 15)
        types = 15;

    std::vector<SessionNVR> sessions;
    int total = 0;

    std::cout
        << "\nRecherche sur le NVR...\n";

    if (
        !rechercher_sessions(
            adresse_nvr,
            utilisateur,
            mot_de_passe,
            canaux,
            types,
            date,
            debut,
            fin,
            sessions,
            total
        )
    )
    {
        return false;
    }

    if (sessions.empty())
    {
        std::cout
            << "Aucun enregistrement trouvé.\n";

        return true;
    }

    std::cout
        << "\nEnregistrements trouvés : "
        << total
        << "\n";

    if (
        total >
        static_cast<int>(sessions.size())
    )
    {
        std::cout
            << "Affichage des "
            << sessions.size()
            << " premiers résultats.\n";
    }

    for (
        std::size_t i = 0;
        i < sessions.size();
        ++i
    )
    {
        const SessionNVR& session =
            sessions[i];

        std::cout
            << std::setw(2)
            << i + 1
            << " | caméra "
            << session.canal + 1
            << " | "
            << std::setw(6)
            << nom_type(session.type)
            << " | "
            << formater_epoch(session.debut)
            << " -> "
            << formater_epoch(session.fin)
            << "\n";
    }

    const int choix =
        demander_entier(
            "Numéro à lire (0 = retour)",
            0
        );

    if (
        choix <= 0 ||
        choix >
            static_cast<int>(
                sessions.size()
            )
    )
    {
        return true;
    }

    const SessionNVR& session =
        sessions[
            static_cast<std::size_t>(
                choix - 1
            )
        ];

    const std::string utilisateur_url =
        encoder_url(utilisateur);

    const std::string mot_de_passe_url =
        encoder_url(mot_de_passe);

    const std::string url =
        "http://" +
        adresse_nvr +
        "/cgi-bin/flv.cgi?u=" +
        utilisateur_url +
        "&p=" +
        mot_de_passe_url +
        "&mode=time&chn=" +
        std::to_string(session.canal) +
        "&begin=" +
        std::to_string(session.debut) +
        "&end=" +
        std::to_string(session.fin) +
        "&audio=54&mute=false";

    return lire_archive_hevc_nvr(
        url,
        fenetre,
        rendu
    );
}
