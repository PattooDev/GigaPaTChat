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
