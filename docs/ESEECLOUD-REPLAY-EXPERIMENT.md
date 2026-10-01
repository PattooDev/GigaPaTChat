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


## Variante d'authentification AUTH3

Le header public `include/proto.h` du dépôt `tuyungang/Automatic-Interface-Detection-Tool` documente aussi :

```text
APP_PROTO_CMD_NONCE_REQ  = 120
APP_PROTO_CMD_NONCE_RSP  = 121
APP_PROTO_CMD_AUTH2_REQ  = 130
APP_PROTO_CMD_AUTH2_RSP  = 131
APP_PROTO_CMD_AUTH3_REQ  = 140
APP_PROTO_CMD_AUTH3_RSP  = 141
```

La structure `auth3_req_data_t` contient deux champs de 1024 octets pour le nom d'utilisateur et le mot de passe. GigaPaTChat expose donc un diagnostic séparé qui utilise uniquement les identifiants fournis par l'utilisateur, sans essai automatique de comptes :

```bash
GIGAPATCHAT_ESEE_AUTH3=1 ./build/gigapatchat
```

AUTH1 et AUTH3 restent deux chemins expérimentaux distincts afin de pouvoir comparer précisément le comportement du firmware.


## Vérification indépendante des identifiants via HTTP

Pour distinguer un refus propre à KP2P d'identifiants réellement invalides, GigaPaTChat peut tester la même paire utilisateur/mot de passe sur l'API HTTP `recsearch` déjà utilisée pour les archives :

```bash
GIGAPATCHAT_HTTP_AUTH_TEST=1 ./build/gigapatchat
```

Le mot de passe reste saisi avec `getpass()`. Un résultat `RECSEARCH : OK` signifie que l'API HTTP du NVR accepte les identifiants, même si aucun enregistrement n'existe pour la journée.


## Authentification KP2P confirmée sur le NVR Gigamedia

Validation matérielle effectuée sur le NVR de test :

```text
WebSocket : OK
ARQ/KP2P  : OK
IOT_OPEN  : OK
AUTH       : OK
Code       : 0
```

La chaîne locale complète est donc confirmée sur le matériel :

```text
TCP -> WebSocket -> ARQ/KP2P -> IOT_OPEN -> API_AUTH_REQ/API_AUTH_RSP
```

Le compte local actuellement valide sur le NVR est `admin` avec mot de passe vide. Le compte secondaire précédemment utilisé n'est plus présent dans la configuration du NVR. Ce constat explique les précédents retours `KP2P_ERR_AUTH_FAILED (-20)`.

La prochaine étape expérimentale est la recherche native des enregistrements, puis l'ouverture du replay KP2P.


## Recherche native des enregistrements

Le SDK KP2P public documente deux familles de recherche.

### Ancienne famille FIND 90/100/110

```text
FIND_START_REQ/RSP  90/91
FIND_NEXT_REQ/RSP   100/101
FIND_STOP_REQ/RSP   110/111
```

Cette voie a été testée sur le NVR Gigamedia. Le transport et l'authentification sont corrects, et le NVR reconnaît la commande, mais répond :

```text
cmd=91 code=-1
```

Cette famille n'est donc pas retenue pour la suite du replay natif.

### Famille REPLAY SEARCH 40/41

Le fichier public `CameraSDK/connector.js` du dépôt `harsh-chalo/trv-log-all-configs` montre que `find_file_start_2(...)` utilise :

```text
APP_PROTO_CMD_REPLAY_REQ = 40
APP_PROTO_CMD_REPLAY_RSP = 41
APP_PROTO_PARAM_REPLAY_CMD_SEARCH = 1
```

Le même code donne la disposition exacte du payload de 52 octets :
- sous-commande REPLAY ;
- type d'ouverture ;
- masque de canaux ;
- type d'enregistrement ;
- timestamps Unix de début et de fin ;
- index de page ;
- nombre d'enregistrements demandés.

Chaque enregistrement renvoyé occupe ensuite 20 octets : canal, type, début, fin et qualité.

GigaPaTChat conserve la même commande de diagnostic :

```bash
GIGAPATCHAT_ESEE_FIND=1 ./build/gigapatchat
```

Le test s'authentifie avec AUTH1 et interroge la caméra 1 (canal 0) pour la journée courante. Comme l'interface EseeCloud publique ne propose que les types 1, 2, 4 et 8, la valeur interne 15 (« tous ») est développée en quatre requêtes REPLAY SEARCH séparées : 1=continu, 2=mouvement, 4=alarme, 8=manuel. GigaPaTChat affiche au maximum cinq enregistrements et indique le total annoncé pour chaque type.

Références :
- `tuyungang/Automatic-Interface-Detection-Tool` pour les constantes et structures KP2P ;
- `harsh-chalo/trv-log-all-configs/CameraSDK/connector.js` comme référence publique du comportement de `find_file_start_2` et `replay_start`. Ce dépôt est crédité comme source publique de référence, sans supposer qu'il est l'auteur original du SDK minifié.


### Validation intermédiaire sur le NVR Gigamedia

Le premier essai REPLAY SEARCH a été accepté par le NVR :

```text
WebSocket : OK
ARQ/KP2P  : OK
IOT_OPEN  : OK
AUTH       : OK
SEARCH     : OK
Code       : 0
```

Avec le type 15 envoyé directement, le NVR a répondu `0 reçu(s), 0 au total`. La comparaison avec l'interface du client EseeCloud public a montré que ce client n'envoie pas 15 : il propose uniquement 1, 2, 4 et 8. Le diagnostic a donc été corrigé pour tester ces quatre types séparément.

Le dernier champ de chaque entrée de 20 octets est également documenté comme `quality`, et non comme une taille de fichier.


### Comparaison directe HTTP -> KP2P

Pour éliminer les ambiguïtés de canal, type et plage temporelle, le diagnostic `GIGAPATCHAT_ESEE_FIND=1` récupère maintenant d'abord une archive réelle via l'API HTTP `recsearch`. Il réutilise ensuite exactement le canal, le type, l'heure de début et l'heure de fin de cette archive comme critères de la requête `REPLAY SEARCH`.

Cette comparaison permet de distinguer :
- un problème de choix de critères ;
- un problème d'encodage natif KP2P ;
- un comportement différent entre les index HTTP et KP2P du même NVR.


## REPLAY SEARCH confirmé sur une archive réelle

Validation matérielle croisée HTTP -> KP2P sur le NVR Gigamedia :

```text
[HTTP] caméra 2 | type 8 | début=1790859600 | fin=1790860380

[EseeCloud] WebSocket : OK
[EseeCloud] ARQ/KP2P   : OK
[EseeCloud] IOT_OPEN   : OK
[EseeCloud] AUTH       : OK
[EseeCloud] SEARCH     : OK
[EseeCloud] Code       : 0
[EseeCloud] Détail     : REPLAY SEARCH confirmé : 2 au total (type 8=2)
```

Cette validation confirme que la voie native `APP_PROTO_CMD_REPLAY_REQ/RSP 40/41` avec la sous-commande `SEARCH=1` retrouve effectivement les archives du NVR lorsque canal, type et intervalle correspondent à un enregistrement réel.

Le prochain jalon est `REPLAY START` (sous-commande 3) sur ce même enregistrement, puis la réception d'une première trame de replay.


## Diagnostic REPLAY START

Le diagnostic suivant choisit d'abord une archive réelle via `recsearch`, puis lance un replay KP2P natif sur exactement le même canal, type et intervalle :

```bash
GIGAPATCHAT_ESEE_REPLAY=1 ./build/gigapatchat
```

Séquence expérimentale :

```text
HTTP recsearch -> archive réelle
TCP -> WebSocket -> ARQ -> IOT_OPEN -> AUTH
APP_PROTO_CMD_REPLAY_REQ (40), sous-commande START=3
APP_PROTO_CMD_REPLAY_RSP (41)
première trame média
REPLAY STOP=2
```

Le test s'arrête dès la première trame média détectée. Il n'écrit aucun fichier et n'effectue encore aucun décodage FFmpeg.

Le lecteur WebSocket accepte désormais les longueurs étendues 64 bits (indicateur 127), nécessaires pour les gros fragments H.265 pouvant dépasser 65 535 octets. Les références publiques `BigGecko01/EseeCloud-Raw-Exporter` et `harsh-chalo/trv-log-all-configs/CameraSDK/connector.js` documentent respectivement le transport NARF/MARF et la construction du `REPLAY START`.


### Ajustement du diagnostic REPLAY START

Un premier essai matériel a confirmé WebSocket, ARQ/KP2P, IOT_OPEN et AUTH, mais n'a produit ni confirmation REPLAY ni trame média dans le diagnostic initial.

L'analyse du `CameraSDK/connector.js` public montre deux points importants :

1. le client lance toujours `find_file_start_2(...)` avant `replay_start(...)`, sur la même connexion ;
2. la réponse `APP_PROTO_CMD_REPLAY_RSP` n'est pas utilisée comme confirmation obligatoire du démarrage : les trames média constituent la preuve opérationnelle du replay.

Le diagnostic a donc été corrigé pour reproduire la séquence réelle :

```text
AUTH
-> REPLAY SEARCH (40/41, sous-commande 1)
-> réponse de recherche non vide
-> pause 300 ms
-> REPLAY START (40, sous-commande 3)
-> première trame NARF ou MARF = replay confirmé
-> REPLAY STOP (sous-commande 2)
```

Le lecteur WebSocket gère également désormais les PING/PONG, la fragmentation et les longueurs étendues afin de se rapprocher du comportement du WebSocket navigateur utilisé par le SDK JavaScript.
