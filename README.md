# GigaPaTChat

Client natif open source expérimental pour les enregistreurs vidéo
Gigamedia utilisant le protocole RTMP.

GigaPaTChat est né de l’objectif de remplacer l’ancien lecteur Flash du NVR
par un client moderne fonctionnant sous Deepin Linux.

## État du projet

Version actuelle : **1.1**

Le prototype sait actuellement :

- se connecter au serveur RTMP du NVR ;
- recevoir le challenge d’authentification dynamique ;
- calculer la réponse MD5 utilisée par l’ancien lecteur Flash ;
- authentifier l’utilisateur sans enregistrer son mot de passe ;
- ouvrir les sous-flux `ch0_1.264` et `ch1_1.264` ;
- afficher les deux caméras alternativement ;
- conserver une fenêtre vidéo ouverte grâce à FFplay.

## Dépendances

- CMake 3.16 ou supérieur
- compilateur compatible C++17
- OpenSSL
- libcurl
- librtmp
- FFmpeg / FFplay
- pkg-config

Sous Deepin ou Debian :

```bash
sudo apt install build-essential cmake pkg-config \
libcurl4-openssl-dev libssl-dev librtmp-dev ffmpeg
