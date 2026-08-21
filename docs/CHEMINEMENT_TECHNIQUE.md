# Cheminement technique de GigaPaTChat

Ce document explique comment GigaPaTChat est passé d’un test de connexion à un
client vidéo natif fonctionnel. Il conserve volontairement les échecs et les
choix intermédiaires : ils sont indispensables pour comprendre le matériel et
éviter de recommencer les mêmes recherches.

## 1. Objectif de départ

Le NVR Gigamedia testé fournit une interface Web ancienne dont l’affichage des
caméras dépend d’un fichier Flash. Les navigateurs modernes ne prennent plus
Flash en charge, et l’objectif a toujours été de créer un vrai client Linux,
pas de remettre Flash en service.

Environnement principal de développement :

- Deepin 25 ;
- C++17 et CMake ;
- NVR à l’adresse `192.168.1.114` ;
- serveur vidéo accessible sur le port TCP 80 ;
- deux caméras utilisées pendant les essais.

## 2. Exploration du NVR

Les premiers contrôles réseau ont montré que le port 80 était ouvert, tandis
que les ports RTSP habituels, notamment 554, étaient fermés. Les essais RTSP
ont donc été abandonnés : le matériel ne proposait pas ce service sur le réseau
testé.

Le serveur HTTP s’identifie comme `JAWS/1.0`. L’examen des pages et scripts de
l’interface d’origine a notamment révélé :

- `view2.html` et `js/view2.js` pour l’affichage ;
- `settings.html` et `js/settings.js` pour la configuration ;
- `/cgi-bin/gw.cgi` pour les échanges XML ;
- `/cgi-bin/snapshot.cgi` pour les images JPEG ;
- `JaViewer.swf` pour l’ancien lecteur vidéo.

Le 21 août 2026, l’endpoint de capture du NVR a encore fourni une image JPEG
valide de 640 × 360. Cela a permis de prouver que le NVR recevait bien la caméra
et d’isoler le problème dans le chemin RTMP du client.

## 3. Compréhension du protocole Flash

Le JavaScript du lecteur d’origine construit une connexion RTMP vers le NVR et
des noms de flux de la forme :

```text
ch0_0.264
ch0_1.264
ch1_0.264
ch1_1.264
```

L’analyse du lecteur a également révélé la séquence d’authentification :

1. ouverture de la connexion RTMP ;
2. réception ou utilisation d’un `nonce` fourni pour la session ;
3. calcul de `MD5(nonce + ":" + mot_de_passe)` ;
4. appel de connexion avec l’utilisateur et le digest calculé ;
5. création du flux RTMP ;
6. commande `play` avec le nom du flux demandé.

Le client historique annonce aussi une valeur de compatibilité
`ja_flash_ver=100`. GigaPaTChat reproduit uniquement les éléments nécessaires
à l’authentification et à la lecture ; il ne dépend pas de Flash.

## 4. Premiers prototypes

Les versions 0.x ont servi à valider séparément :

- la communication HTTP avec libcurl ;
- le calcul MD5 avec OpenSSL ;
- la connexion RTMP avec librtmp ;
- l’envoi de l’authentification challenge/réponse ;
- l’ouverture des noms de flux découverts dans le client Web.

Les premiers essais ont produit plusieurs erreurs utiles :

- `NetStream.Play.Failed` lorsque le nom de flux était incorrect ;
- `NetStream.Play.StreamForbidden` pour un flux non accepté ;
- réinitialisations de connexion pendant certains gros paquets ;
- absence d’informations vidéo lorsque le serveur interrompait le paquet avant
  que FFmpeg puisse reconnaître le flux.

Ces erreurs ont confirmé que le dialogue RTMP et l’authentification étaient
corrects avant même que l’image puisse être affichée.

## 5. Bibliothèque RTMP locale

Le projet utilise le dépôt officiel
[`mstorsjo/rtmpdump`](https://github.com/mstorsjo/rtmpdump) comme sous-module.
Cela fixe précisément la version testée et permet de compiler la bibliothèque
statique locale avec GnuTLS :

```bash
git submodule update --init --recursive
make -C rtmpdump-source/librtmp CRYPTO=GNUTLS SHARED=no
```

Le binaire GigaPaTChat est lié à
`rtmpdump-source/librtmp/librtmp.a`. Les fichiers compilés ne sont pas stockés
dans Git ; ils doivent être reconstruits sur la machine cible.

## 6. Décodage FFmpeg et fenêtre SDL2

Une fois les paquets RTMP reçus, FFmpeg analyse et décode la vidéo H.264.
libswscale convertit les images vers un format YUV420P adapté à une texture
SDL2 `IYUV`.

SDL2 fournit ensuite :

- une fenêtre native persistante ;
- un rendu vidéo indépendant du navigateur ;
- la gestion du clavier ;
- le changement de caméra sans fermer l’application ;
- une fermeture propre avec la touche Échap.

Cette architecture a remplacé les premiers essais fondés sur le lancement de
FFplay comme processus externe.

```text
NVR RTMP:80
    → authentification et lecture librtmp
    → analyse et décodage FFmpeg
    → conversion libswscale
    → texture et fenêtre SDL2
```

## 7. Étapes fonctionnelles importantes

### Version 1.4

- première image native décodée ;
- flux des caméras 1 et 2 validés ;
- sélection par les touches `1` et `2` ;
- fenêtre conservée pendant le changement de caméra.

### Version 1.5

- fenêtre SDL2 native stabilisée ;
- contrôles clavier regroupés ;
- ajout d’un chemin d’enregistrement Matroska expérimental ;
- amélioration de la gestion des changements de caméra ;
- conservation du mot de passe uniquement pendant l’exécution ;
- validation visuelle du flux secondaire le 21 août 2026.

## 8. Flux principal et flux secondaire

Le client Web indique la correspondance suivante :

| Suffixe | Rôle annoncé |
|---|---|
| `_0.264` | flux principal |
| `_1.264` | flux secondaire |

Sur le NVR testé, `ch0_0.264` est accepté et renvoie
`NetStream.Play.Start`, mais la connexion est ensuite réinitialisée pendant la
première grosse image. Un essai du 21 août 2026 a échoué pendant un paquet
d’environ 103 546 octets avec l’erreur réseau 104
(`Connection reset by peer`).

À l’inverse, `ch0_1.264` et `ch1_1.264` restent stables. La comparaison directe
entre la fenêtre GigaPaTChat et celle du NVR montre un niveau de détail et un
grain très proches. L’image du NVR est légèrement plus lumineuse et contrastée,
probablement à cause de son traitement d’affichage.

La version 1.5 conserve donc `_1.264`. C’est un choix de fiabilité fondé sur les
essais réels, et non une confusion entre les deux flux.

## 9. Essai d’accès direct à la caméra

Une voie expérimentale a utilisé l’adresse interne `172.20.14.30` et le nom de
flux `360p.264`. Une route vers le réseau `172.20.14.0/24` via le NVR a été
testée. Cet accès direct s’est toutefois montré dépendant de l’état du routage
interne et n’a pas été retenu comme configuration stable.

La version validée revient donc au serveur RTMP du NVR :

```text
rtmp://192.168.1.114:80/
```

L’adresse `172.20.14.30` ne doit pas être considérée comme universelle : elle
appartient uniquement au réseau interne observé pendant le développement.

## 10. Enregistrement vidéo

L’enregistrement a été commencé au format Matroska. Les premiers essais ont
montré des DTS non monotones ou désordonnés. Le bouton `R` et le chemin
d’enregistrement sont présents, mais cette fonction reste expérimentale tant
que la reconstruction des horodatages n’est pas totalement validée.

Cette limite doit rester clairement indiquée : l’affichage est stable, mais
l’enregistrement n’est pas encore une fonction garantie de la version 1.5.

## 11. Diagnostic rapide

Les messages RTMP permettent de localiser rapidement une panne :

- échec avant le handshake : problème d’adresse, de port ou de réseau ;
- échec du login : utilisateur, mot de passe, nonce ou digest incorrect ;
- `StreamForbidden` : nom de flux refusé ;
- `Play.Start` puis coupure : flux reconnu, mais transport interrompu ;
- connexion maintenue sans image : vérifier les données vidéo reçues et leur
  analyse par FFmpeg.

Pour l’appareil de développement, la base connue comme fonctionnelle est :

```text
NVR       : 192.168.1.114:80
Caméra 1  : ch0_1.264
Caméra 2  : ch1_1.264
```

## 12. Sécurité

Le protocole reproduit une authentification ancienne fondée sur MD5. Même si
le mot de passe n’est ni écrit dans la source ni conservé par GigaPaTChat, ce
protocole ne doit pas être exposé directement à Internet.

Recommandations :

- utiliser un réseau local de confiance ;
- protéger le NVR derrière un pare-feu ;
- ne jamais publier de mot de passe ou de digest de test ;
- changer les identifiants utilisés pendant une campagne de diagnostic si
  nécessaire.

## 13. Suite prévue

Les prochaines améliorations peuvent maintenant être réalisées sans perdre la
base stable :

1. déplacer l’adresse, l’utilisateur et les options dans un fichier de
   configuration ;
2. réduire les traces RTMP en utilisation normale et ajouter un mode diagnostic ;
3. fiabiliser les horodatages de l’enregistrement ;
4. étudier la coupure du flux principal sans dégrader le flux stable ;
5. améliorer l’interface et gérer davantage de caméras ;
6. préparer une installation plus simple pour Deepin et Debian.

## 14. Principe de travail

Chaque étape fonctionnelle est sauvegardée avant l’expérience suivante. Les
changements sont testés sur le NVR réel, puis seulement validés dans Git lorsque
la compilation, la connexion et l’image ont été confirmées.

Le projet est développé et testé par **Pattoo**, avec l’accompagnement technique
de **ChatGPT/Codex** pour l’analyse du protocole, le diagnostic, le code et cette
documentation.
