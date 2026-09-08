# Lecture des archives NVR — état du développement

État au **8 septembre 2026**.

> La version stable publiée de GigaPaTChat reste **1.5.1**. La lecture des archives NVR est encore en développement et ne constitue pas une nouvelle version stable.

## Objectif

Ajouter à GigaPaTChat la recherche et la lecture des enregistrements présents sur le NVR, en reproduisant le comportement de l’ancienne interface Web/Flash sans dépendre du navigateur ni du greffon propriétaire.

## Architecture observée

Le direct et les archives suivent deux chemins distincts :

```text
Direct :
NVR RTMP -> librtmp -> FFmpeg -> SDL2

Archives :
HTTP gw.cgi / recsearch -> HTTP flv.cgi -> flux FLV -> décodage HEVC -> SDL2
```

Les adaptations RTMP utilisées pour le direct ne participent donc pas directement à la lecture des archives.

## Protocole officiel du NVR

L’analyse de l’interface Web d’origine a permis d’identifier le chemin suivant :

1. le login Web utilise une requête XML `rpermission` envoyée à `/cgi-bin/gw.cgi` ;
2. l’interface conserve ensuite `dvr_usr` et `dvr_pwd` dans des cookies ;
3. la recherche d’archives utilise une requête XML `recsearch` envoyée à `/cgi-bin/gw.cgi?xml=...` ;
4. la première page demande `session_count="10"` et le lecteur Web gère la pagination avec `session_index` ;
5. une session est ensuite lue avec `/cgi-bin/flv.cgi` en utilisant le canal et les horodatages de début et de fin.

Les types d’enregistrement observés sont :

- `1` : Time ;
- `2` : Motion ;
- `4` : Sensor ;
- `8` : Manual ;
- `15` : tous les types combinés.

## Tests validés sur le matériel réel

### Permissions

Une requête `rpermission` avec les identifiants valides du NVR a répondu :

```text
errno = 0
config base = 1
playback base = 1
```

Le compte testé possède donc bien l’autorisation de lire les archives.

### Recherche sans session préalable

Une requête `recsearch` directe, sans login `rpermission` préalable et sans conservation de cookies HTTP, a également répondu avec `errno = 0`.

Cela montre que, sur le NVR testé, la recherche d’archives n’exige pas une session Web préalable : les identifiants portés par `recsearch` suffisent.

Lors d’un test sur une journée contenant des enregistrements, le NVR a annoncé :

```text
session_total = 50
```

et a renvoyé les 10 premières sessions, conformément à `session_count="10"`.

Un autre test sur une journée sans enregistrement a répondu `errno = 0` avec `session_total = 0`, ce qui confirme que l’absence de résultat n’est pas une erreur d’authentification.

## État du prototype GigaPaTChat

Le développement en cours comprend déjà :

- une interface SDL de recherche des archives ;
- la génération de la requête `recsearch` ;
- le décodage de la réponse XML et des sessions retournées ;
- la construction de l’URL `flv.cgi` ;
- un lecteur expérimental du flux FLV des archives ;
- un décodage HEVC expérimental à partir des données vidéo du NVR.

Le NVR annonce les paquets vidéo avec un identifiant FLV associé à AVC/H.264, alors que les données observées dans les archives sont traitées comme HEVC/H.265 dans le prototype. Ce comportement reste spécifique au matériel testé et doit encore être fiabilisé.

## Points encore ouverts

- expliquer définitivement les anciens retours `errno=4` observés pendant les essais ;
- vérifier que le format de date et d’heure généré par le C++ est accepté exactement comme celui de l’interface Web ;
- ajouter la pagination lorsque `session_total > 10` ;
- rendre la date, les heures, les canaux et les types sélectionnables dans l’interface ;
- fiabiliser la lecture FLV/HEVC et les horodatages ;
- améliorer la gestion de l’arrêt, des erreurs réseau et de la fermeture de fenêtre ;
- éviter toute exposition inutile des identifiants dans les journaux ou la documentation.

## Sécurité

Aucun mot de passe réel, identifiant Cloud P2P, QR code ni adresse privée du matériel n’est publié ici.

Le protocole HTTP du NVR est ancien et transporte des informations sensibles d’une manière qui ne répond pas aux standards de sécurité modernes. Ces travaux sont destinés à un usage sur un réseau local de confiance.

## Prochaine vérification

La prochaine étape prévue est de reproduire une requête `recsearch` avec exactement le format actuellement produit par le code C++ (notamment la date avec zéros et `00:00:00`) afin d’éliminer les dernières différences de format avant de modifier le prototype.
