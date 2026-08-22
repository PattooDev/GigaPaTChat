# GigaPaTChat

Client natif open source expérimental pour certains enregistreurs vidéo
Gigamedia utilisant RTMP.

GigaPaTChat est né d’un objectif simple : remplacer l’ancien lecteur Flash du
NVR par une application native moderne fonctionnant sous Deepin Linux, sans
navigateur et sans greffon propriétaire obsolète.

## État du projet

Version actuelle validée : **1.5.1** — 21 août 2026.

Cette version sait :

- se connecter au serveur RTMP du NVR sur le port 80 ;
- recevoir le challenge d’authentification dynamique du NVR ;
- calculer la réponse MD5 attendue par l’ancien lecteur Flash ;
- demander le mot de passe au lancement sans l’enregistrer dans le code ;
- ouvrir les flux stables `ch0_1.264` et `ch1_1.264` ;
- décoder la vidéo H.264 avec FFmpeg ;
- afficher la vidéo dans une fenêtre native SDL2 persistante ;
- passer d’une caméra à l’autre avec les touches du clavier.

L’enregistrement est présent dans le prototype, mais reste **expérimental** :
la gestion des horodatages doit encore être fiabilisée.

## Validation mobile EseeCloud — 22 août 2026

En complément du client Linux, le NVR de développement a été ajouté avec succès
à l’application Android **EseeCloud** au moyen du QR code / identifiant
**Cloud P2P** présent sur l’étiquette de l’appareil.

Les essais ont confirmé :

- l’ajout du NVR dans EseeCloud ;
- l’affichage correct des flux des caméras 1 et 2 ;
- l’affichage `NO VIDEO` sur les canaux non utilisés ;
- le fonctionnement sur le réseau Wi-Fi local ;
- le fonctionnement hors Wi-Fi, en données mobiles 5G ;
- qu’après un premier affichage « hors ligne » en 5G, une fermeture complète
  puis une relance d’EseeCloud permet au NVR de revenir en ligne et d’afficher
  les flux.

Cette validation confirme que l’écosystème **NVR / EseeCloud / Cloud P2P** du
matériel testé fonctionne encore sur Android. Elle est indépendante du chemin
RTMP local utilisé par GigaPaTChat, mais elle fournit une référence utile pour
comparer le comportement du NVR et préparer de futurs travaux sur l’accès
mobile.

Aucun QR code, identifiant P2P ni identifiant privé du matériel n’est publié
dans ce dépôt.

## Matériel et compatibilité

Le développement a été réalisé avec un NVR Gigamedia dont l’interface HTTP
répond comme `JAWS/1.0` et dont le lecteur d’origine repose sur Flash.

La compatibilité avec d’autres modèles ou versions de micrologiciel n’est pas
garantie. Le protocole utilisé par ces appareils n’est pas documenté
officiellement ; il a été compris par observation du client Web d’origine et
par essais successifs.

## Dépendances

- CMake 3.16 ou supérieur ;
- compilateur compatible C++17 ;
- libcurl et OpenSSL ;
- GnuTLS, Nettle, GMP et zlib ;
- SDL2 ;
- bibliothèques de développement FFmpeg ;
- source `rtmpdump`, incluse comme sous-module Git.

Sous Deepin ou Debian :

```bash
sudo apt install build-essential cmake pkg-config git \
libcurl4-openssl-dev libssl-dev libgnutls28-dev \
nettle-dev libgmp-dev zlib1g-dev libsdl2-dev \
libavcodec-dev libavformat-dev libavutil-dev libswscale-dev
```

## Clonage et compilation

Clonez le projet avec son sous-module RTMP :

```bash
git clone --recurse-submodules https://github.com/PattooDev/GigaPaTChat.git
cd GigaPaTChat
```

Si le dépôt a déjà été cloné sans les sous-modules :

```bash
git submodule update --init --recursive
```

Compilez d’abord la bibliothèque RTMP locale, puis GigaPaTChat :

```bash
make -C rtmpdump-source/librtmp CRYPTO=GNUTLS SHARED=no
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

## Configuration

L’adresse du NVR, le nom d’utilisateur et le mot de passe sont demandés au
lancement. Ils ne sont pas enregistrés dans le dépôt :

```text
Adresse IP ou nom du NVR : <IP_DU_NVR>
Nom d'utilisateur du NVR : <UTILISATEUR_DU_NVR>
Mot de passe du NVR :
```

`<IP_DU_NVR>` est un emplacement à remplacer par l’adresse privée réelle du
NVR, sans `http://` ni `rtmp://`. Le port RTMP reste fixé à 80 dans cette
version. Le nom des flux est construit sous la forme
`ch<numero>_1.264`.

## Lancement

```bash
./build/gigapatchat
```

Commandes disponibles :

| Touche | Action |
|---|---|
| `1` | afficher la caméra 1 |
| `2` | afficher la caméra 2 |
| `R` | démarrer ou arrêter l’enregistrement expérimental |
| `Échap` | quitter GigaPaTChat |

## Choix du flux vidéo

Le NVR annonce deux familles de flux :

- `_0.264` : flux principal ;
- `_1.264` : flux secondaire.

Sur l’appareil testé, le flux principal démarre puis le serveur coupe la
connexion pendant la première grosse image. Le flux secondaire reste stable.
La comparaison visuelle du 21 août 2026 montre une image GigaPaTChat très
proche de celle du logiciel NVR ; cette dernière est seulement un peu plus
lumineuse et contrastée.

La version 1.5 utilise donc volontairement `_1.264` comme base fiable.

## Limites connues

- port RTMP encore fixé à 80 dans la source ;
- deux caméras seulement dans l’interface actuelle ;
- journal RTMP encore très détaillé ;
- enregistrement vidéo à stabiliser ;
- flux principal `_0.264` instable avec le NVR testé ;
- compatibilité limitée aux appareils employant le même protocole.

## Sécurité

L’authentification MD5 reproduit le fonctionnement du matériel ancien ; elle
ne constitue pas une protection moderne. Utilisez GigaPaTChat uniquement sur
un réseau local de confiance et n’exposez pas directement le NVR à Internet.

Le fonctionnement Cloud P2P observé avec EseeCloud ne modifie pas cette
recommandation : il ne faut pas ouvrir manuellement le port RTMP du NVR vers
Internet pour reproduire cet accès.

Aucun mot de passe réel, QR code ou identifiant P2P ne doit être ajouté au dépôt
Git.

## Documentation technique

Le détail complet du cheminement, des essais et des décisions est disponible
dans [docs/CHEMINEMENT_TECHNIQUE.md](docs/CHEMINEMENT_TECHNIQUE.md).

## Auteurs et méthode

GigaPaTChat est développé et testé sur le matériel réel par **Pattoo**. Le
diagnostic du protocole, la conception progressive et la documentation ont été
menés en collaboration avec **ChatGPT/Codex**.
