# EseeCloud replay — notes expérimentales

Cette branche explore une seconde voie de lecture des archives NVR, en parallèle du chemin existant :

```text
recsearch / gw.cgi -> flv.cgi -> HEVC -> FFmpeg/SDL
```

La voie expérimentale vise le service EseeCloud/Juan natif observé sur le port TCP 10000.

## Références publiques étudiées

- BigGecko01/EseeCloud-Raw-Exporter
  - https://github.com/BigGecko01/EseeCloud-Raw-Exporter
  - notes de protocole : recherche d'archives, replay, encapsulation NARF/MARF et fragmentation ;
  - licence MIT.
- meust3/home-assistant-jooan-nvr
  - https://github.com/meust3/home-assistant-jooan-nvr
  - implémentation indépendante du transport KP2P local et du flux vidéo ;
  - licence MIT.
- dbuezas/icsee-ptz
  - https://github.com/dbuezas/icsee-ptz
  - référence complémentaire pour DVRIP/XM/ICSee ;
  - licence MIT.

Le code de `src/esee_replay.cpp` est une implémentation propre à GigaPaTChat. Les dépôts ci-dessus servent de références d'interopérabilité et de documentation du comportement observé.

## Remerciements et crédits

Un grand merci aux auteurs dont les travaux publics ont permis d'accélérer cette recherche :

- **BigGecko01** — auteur de `EseeCloud-Raw-Exporter`, pour la documentation du replay EseeCloud, de l'encapsulation NARF/MARF et de la fragmentation des trames ;
- **meust3** — auteur de `home-assistant-jooan-nvr`, pour l'implémentation et la documentation du transport local KP2P ;
- **dbuezas** — auteur de `icsee-ptz`, utilisé comme référence complémentaire pour l'écosystème DVRIP/XM/ICSee ;
- **tuyungang** — dépôt `Automatic-Interface-Detection-Tool`, qui rend accessible un en-tête SDK KP2P public contenant notamment les codes d'erreur et les API de recherche/relecture des enregistrements.

Leurs dépôts restent la source de référence pour leurs travaux respectifs. GigaPaTChat ne prétend pas en être l'auteur et conserve les liens et mentions de licence correspondants.

## État actuel

Le module expérimental sait :

1. tester sans identifiants si le port TCP 10000 du NVR est joignable ;
2. reconnaître l'en-tête transport `AB BC CD DE` ;
3. reconnaître un début de trame `NARF` / `MARF` ;
4. extraire la taille déclarée, le timestamp, le codec et les trois champs média principaux.

Il ne lance pas encore de session EseeCloud, n'envoie aucun identifiant et ne modifie pas la voie de lecture NVR actuelle.

## Test sur le matériel

Depuis la branche `experiment/esee-replay` :

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
GIGAPATCHAT_ESEE_PROBE=1 ./build/gigapatchat
```

Le programme demande uniquement l'adresse IP ou le nom du NVR, teste `<NVR>:10000`, affiche `JOIGNABLE` ou `INJOIGNABLE`, puis quitte.

Une fois le port 10000 confirmé, le diagnostic suivant vérifie sans identifiants l'Upgrade WebSocket puis le handshake ARQ/KP2P :

```bash
GIGAPATCHAT_ESEE_HANDSHAKE=1 ./build/gigapatchat
```

Le résultat attendu est `WebSocket : OK` puis, si le NVR utilise bien le transport KP2P identifié, `ARQ/KP2P : OK`.

Après validation de ce handshake, le test suivant ouvre uniquement la couche IOT, toujours sans authentification utilisateur :

```bash
GIGAPATCHAT_ESEE_IOT=1 ./build/gigapatchat
```

Le résultat attendu est `WebSocket : OK`, `ARQ/KP2P : OK` puis `IOT_OPEN : OK`.

Après validation de la couche IOT, l'authentification locale KP2P peut être testée explicitement :

```bash
GIGAPATCHAT_ESEE_AUTH=1 ./build/gigapatchat
```

Le programme demande l'utilisateur local du NVR puis le mot de passe via `getpass()`. Le mot de passe n'est ni affiché ni écrit dans les logs. La requête `API_AUTH_REQ` utilise les champs d'authentification AES-128-ECB observés dans le travail public de **meust3**, qui est crédité ci-dessus et dans le source.

## Suite si le port 10000 répond

Le prochain jalon sera de reproduire proprement la séquence de replay observée :

```text
connexion
-> authentification
-> recherche des enregistrements
-> replay_start
-> réception des messages
-> réassemblage NARF
-> flux H.264/H.265 et G.711
```

Cette voie restera expérimentale tant qu'elle n'aura pas été validée sur le NVR Gigamedia réel.


## Codes d'erreur KP2P confirmés

Le header SDK public `kp2psdk.h` expose notamment :

```text
0    KP2P_ERR_SUCCESS
-20  KP2P_ERR_AUTH_FAILED
-21  KP2P_ERR_GET_NONCE_FAILED
-22  KP2P_ERR_AUTH2_FAILED
-50  KP2P_ERR_REC_SEARCH_FAILED
-51  KP2P_ERR_REC_PLAY_FAILED
```

Sur le NVR Gigamedia testé, `API_AUTH_RSP` a retourné `-20` avec le compte essayé : le transport WebSocket/ARQ/IOT et le format de requête AUTH sont acceptés, mais les identifiants sont refusés.

Le même header confirme aussi l'existence des API :

```text
kp2p_rec_find_file_start(...)
kp2p_rec_find_file_next(...)
kp2p_rec_play_start(...)
kp2p_rec_play_start2(...)
```

Ces éléments serviront à la prochaine étape de rétro-ingénierie du replay natif.
