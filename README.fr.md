# Defeatist Music Player *pour M5Tab5*

[English](README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [Español](README.es.md) · [Português](README.pt.md) · **Français** · [Italiano](README.it.md)

La plupart des lecteurs multimédias pour le M5Stack Tab5 vous demandent d'abord de *convertir* vos fichiers. Pas celui-ci.

Le but de ce projet est de tirer tout le potentiel de ce matériel sans le modifier. Pas de soudure, pas d'accessoire qu'on ne puisse retirer ensuite. Tous les formats qu'un ESP32-P4 v1.3 peut raisonnablement gérer sans connaître de secrets.

> Ceci est une traduction automatique ; la version de référence est l'anglaise, [README.md](README.md). Pour les étiquettes courtes de l'écran, voir [ABBREVIATIONS.fr.md](ABBREVIATIONS.fr.md).

![](screenshots/IMG_20261001_151110_126.jpg)

## Le M5Stack Tab5 n'est pas un lecteur de musique idéal.

- Vous croyez qu'il a du Bluetooth.
  - Il a du Bluetooth basse consommation (BLE), ce qui veut dire que les appareils plus anciens en Bluetooth classique ne le verront jamais.
  - LE Audio / Auracast demande un autre câblage et d'autres profils. Ça pourrait marcher en reflashant le C6, mais il faut pour cela du [matériel spécial](https://docs.m5stack.com/en/guide/restore_factory/m5tab5_c6_wifi).
- Il a une prise casque.
  - Parfait pour un casque-micro. Ou un câble AUX. Ou, en théorie, pour enregistrer.
  - Mais rien n'écoute les boutons de télécommande du câble. On peut les câbler, paraît-il. Ce qui, là encore, veut dire du matériel spécial (mais pas trop).
- L'écran et le tactile sont pilotés par la même puce. On peut éteindre le rétroéclairage pour économiser l'énergie, mais pas éteindre complètement l'écran.
- Et tout le bazar des contrôleurs d'écran.
  - Première version (2025.5.9) : contrôleur d'écran ILI9881C séparé + contrôleur tactile GT911
  - 2025.10.14 : passage au contrôleur intégré écran-tactile (TDDI) ST7123
  - 2026.4.28 : le circuit est passé du ST7123 au ST7121 (c'est celui que j'ai reçu)

## Les trois cas de figure

1. Je n'ai pas de stockage, mais j'ai du Wi-Fi (ou un adaptateur Ethernet) : vous pouvez écouter la radio sur Internet.
2. J'ai du stockage, mais pas de musique : vous pouvez enregistrer des fichiers FLAC pour les écouter plus tard.
3. J'ai du stockage et de la musique : voici votre lecteur de musique — avec ou sans écouteurs, adaptateurs USB Audio Class (UAC) pris en charge.

## Voici ce que j'ai réussi à faire marcher sous ESP-IDF ~~5.5.5~~ 6.1

- Branchement à chaud de la carte microSD et de la clé USB
  - La microSD est préférable. Elle consomme moins.
  - Il monte quand même l'USB automatiquement s'il est disponible, si l'indicateur USB est vert. S'il ne l'est pas, rallumez l'alimentation USB dans les réglages.
- Prise en charge d'exFAT
  - Des cartes SDHC et SDXC ont été testées (même si la seconde est morte au bout d'une semaine, ce n'est pas la faute du logiciel). Pas les SDUC. Des fichiers audio de la taille d'un Blu-ray passeront-ils ? Aucune idée.
- Bascule automatique du casque au haut-parleur intégré quand on débranche, et inversement
  - L'icône à côté du curseur de volume montre ce qui joue vraiment — un haut-parleur, des écouteurs, ou `UAC` quand un appareil audio USB a la sortie. La toucher coupe et rétablit le son.
- Prise en charge de toutes les variantes du format MP3 (pour autant que je sache). Ce truc a bibliothèque de secours sur bibliothèque de secours. FLAC, Ogg, WAV, les classiques sont là.
- Affichage de la pochette : que l'image soit dans le fichier, ou le cover.jpg / folder.jpg / front.jpg de l'album à côté.
- État de la batterie, en toutes lettres au bout de la ligne d'état : `BATT 73%`, `CHRG 73%`, `CHARGED`, `NO BATT`.
- Le Wi-Fi sur la ligne d'état : vert connecté, jaune en connexion, gris coupé.
- Réglage du volume
- Lecture/pause
- Début de piste / piste précédente (double toucher)
- Piste suivante
- Mise en veille de l'écran
- Glisser pour se déplacer dans la piste. Dans tous les formats de la liste ci-dessus.
- Le bouton d'ordre de lecture passe par ONE / ALL / RPT / EAT / RND / RPT1 — ce que fait chacun, et toutes les autres étiquettes courtes de l'écran, sont dans [ABBREVIATIONS.fr.md](ABBREVIATIONS.fr.md).
- La forme d'onde du volume sur la barre de lecture. Je trouvais ça chouette.
- Prise en charge de l'USB Audio Class — l'adaptateur « mettre un casque Bluetooth sur ma PS5 » marche ici aussi. Port USB-A uniquement.
- La pause coupe l'alimentation de l'ampli.
- Cache en SDRAM. Si des choses bizarres arrivent 20 secondes avant un changement de morceau, ouvrez une issue.
- Prise en charge de ReplayGain. La première fois que vous écoutez un morceau en entier, Defeatist l'écoute avec vous — aux écoutes suivantes, il monte les morceaux plus doux et baisse les plus forts, dans les limites du raisonnable. Raisonnable au sens de [BS.1770](https://www.itu.int/rec/R-REC-BS.1770/en).
  - La forme d'onde de la barre de lecture vient de cette même écoute. Tant qu'un morceau n'a pas été entendu en entier une fois, sa barre reste grise unie.
  - Sauter ou se déplacer pendant cette première écoute l'annule — il réessaiera la prochaine fois.
  - Ces métadonnées sont dans un fichier caché `.songname.rgcache`. On ne peut pas désactiver le calcul ; on peut désactiver l'ajustement du volume dans les réglages.
- ARK 12 couvre une bonne partie d'Unicode, mais pas tout.
- Rouvre le dernier morceau joué au démarrage. Sans lecture automatique.
- Fondu de 3 secondes quand on retire le support.
- Fondu enchaîné réglable.
- Respecte vraiment les données de lecture sans blanc (gapless).
- Radio sur Internet via l'API de <https://www.radio-browser.info>, ou par liste manuelle. On entre là en territoire « reste branché sur secteur ».
  - Flux MP3, AAC et Ogg, et HLS (`.m3u8`) avec de l'AAC ou du MP3 en segments bruts ou MPEG-TS. Pas encore : HLS chiffré ou fMP4, ni les liens `.pls`/`.m3u` servis par une station.
- Titres et noms de station en arabe, liés et mis en page de droite à gauche, contre la marge droite.
- Minuterie de mise en veille (jusqu'à 2 heures, par pas de 15 minutes).
- Extinction après 15 min à 2 h sans lecture, enregistrement ni toucher — distincte de la minuterie. `IDLE` sur la ligne d'état est vert pendant le compte à rebours et jaune quand quelque chose le retient ; le bouton latéral le rallume.
- Protection batterie faible : sous 6,3 V pendant 30 s sur batterie, il termine tout enregistrement et s'éteint de lui-même, avant que la batterie ne soit assez vidée pour s'abîmer. Pas encore testé sur une vraie décharge.
- Réglage de la luminosité.
- Network Time Protocol (NTP), si le Wi-Fi est configuré.
- L'horloge sauvegardée par pile garde l'heure à travers une extinction, donc les enregistrements sont bien nommés sans Wi-Fi ; NTP la corrige quand il peut.
- Configuration du Wi-Fi par portail captif (plusieurs routeurs).
- Ajout manuel de radios Internet depuis le portail web Wi-Fi.
- Favoris étoilés.
- Adaptateurs Ethernet USB : CDC-ECM (Realtek RTL8152 testé ; RTL8153 même chemin, non testé) et un portage de l'[ASIX AX88772](https://github.com/Sudrien/esp_usbh_asix), tous deux confirmés sur la carte.
- Fichiers cue.
- m3u/m3u8 — au moins via MPD.
- Ah oui, la prise en charge de [Music Player Daemon](https://mpd.readthedocs.io/en/stable/user.html) — oui, ça devrait aussi vouloir dire contrôle par Home Assistant.
- Interface web en HTTPS.
- Enregistrement audio, puisque le matériel est là, en FLAC.
  - avec un vumètre pendant l'enregistrement.
- Une ligne d'état sous le volume : alimentation USB, MPD, HTTPS, et les minutes restantes de la minuterie. Vert, c'est allumé.
- Seigneur, j'ai regardé la liste des apps de M5Launcher, corrigeons l'ordre du titre tout de suite.
- Langues de l'interface (English, 简体中文, 日本語, Español, Português, Français, Italiano), à choisir dans l'engrenage des réglages >> onglet BUILD.
- Prise en charge de [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher)
  - retour au launcher
  - import des identifiants Wi-Fi
- Bibliothèques séparées pour https://github.com/Sudrien/lothesome-audio-analyzer-for-m5tab5 ... une erreur ? peut-être
- Quelques filtres
  - réglage des graves
  - réglage des aigus
  - suppression de la voix par la stéréo
  - écho réglable
  - passe-haut
  - passe-bas
  - réglage de la largeur
  - filtre de craquements (vinyle)
  - filtre de scintillement (flutter)
  - filtre de pleurage (wow)
- vitesse de lecture (pour les fichiers)
  - Ce sont des chiffres de platine, vous comprendrez.

## Ce qui pourrait arriver

- Plus de gestion des plantages ; cela dit, vous pouvez toujours le brancher sur `idf.py monitor` et voir ce qui sort.
- Un téléchargeur de podcasts par Wi-Fi ? Envisageable. Il faudrait gérer les chapitres.
  - il y a tant à faire. Tant et tant.

## Ce qui ne peut pas arriver avec le code publié actuel

- Les adaptateurs Bluetooth classique.
- La reprise par fichier.
- Les hubs USB.
  - Peut-il savoir que vous en avez branché un ? Oui.
  - Peut-il utiliser ce qu'on branche dessus ? Probablement pas.
  - Un hub vous sauvera-t-il si votre appareil tire assez de courant pour faire décrocher le Tab5 ? Euh. Définissez « sauver ».
  - Ce sont les vitesses multiples d'appareils que les bibliothèques ne gèrent pas, et je ne peux pas vous dire la vitesse d'un appareil donné.
- Les fichiers avec DRM, c'est non.
- DSD et APE demandent trop de calcul.

## Problèmes possibles

- Les réseaux Wi-Fi enregistrés et le certificat de la télécommande vivent dans la NVS de la carte, espace de noms `defeatist`. Sous un launcher sur carte SD, tous les firmwares partagent cette partition : une autre app peut les lire (la NVS n'est pas chiffrée ici), et une app qui efface la NVS les emporte avec elle. Les réglages reviennent depuis la carte ; les réseaux et le certificat, non.

- Charger sur un USB-C trop faible + batterie en place + écran allumé peut donner ce qui ressemble à un sifflement du haut-parleur, mais n'en est pas un. Prenez un meilleur câble USB, un meilleur hub, un branchement direct sur le chargeur. Vous manquez d'ampères.
- La sélection de fichier est un peu plus lente que je ne voudrais, parce que sélectionner le premier morceau sous votre pouce n'est pas ce que vous voulez.
- Les câbles AUX ne sont pas forcément assez blindés contre tout ce qu'il peut y avoir autour. La règle électromagnétique « éloignez votre téléphone » s'applique.

## Licences

- Ce code est sous MIT, comme les quatre composants feckless-*-for-m5tab5 qu'il tire par git — sauf les polices, ci-dessous.
- minimp3 est sous CC0-1.0 (dédié au domaine public). Aucune obligation d'attribution. Pas dans le dépôt : `cmake/vendored.cmake` le récupère à un commit fixé et vérifie un SHA256, donc la source reste vérifiable.
- esp_audio_codec est livré en **archives précompilées** sous la licence ESPRESSIF MIT. Gratuit, mais la concession est limitée au silicium Espressif. Sans problème ici ; bon à savoir avant de copier ce code ailleurs.
- pngle (kikuchan) et miniz (Rich Geldreich et al.) sont sous MIT, récupérés comme minimp3. La seule condition du MIT est que l'avis de copyright et de permission accompagne « toutes les copies ou parties substantielles », et une image de firmware est une copie : une redistribution binaire porte leurs avis (les en-têtes de `pngle.c` et `miniz.c` récupérés) à côté du `LICENSE` de ce projet.
- stb_image (le décodeur JPEG de dernier recours dans `components/stbjpeg`) est de Sean Barrett, au choix sous MIT ou sous l'Unlicense (domaine public). Ce projet le prend sous l'Unlicense, il n'ajoute donc aucun avis à porter. L'enveloppe autour est sous MIT.
- **`components/usb_host_msc` et `components/usb_host_uac` sont d'Espressif, sous Apache-2.0, pas MIT.** Ce sont des copies intégrées et modifiées des pilotes de classe d'esp-usb ; chacune garde son propre fichier de licence, et les fichiers modifiés gardent les en-têtes SPDX d'Espressif avec une ligne « Modified by » en dessous (Apache-2.0, section 4(b)) ; les manifestes intégrés indiquent `license: "Apache-2.0"`. Une redistribution porte ces fichiers de licence.
- Le fatfs avec exFAT que `cmake/exfat.cmake` intègre à la configuration est le composant d'IDF (Apache-2.0) autour du FatFs de ChaN (licence FatFs, un avis de type BSD à une clause qui doit être conservé).
- **TJpgDec est de ChaN, sous sa propre licence**, et arrive par le composant `espressif/esp_jpeg`. Permissive — libre pour un usage personnel et commercial, redistribution de la source autorisée — mais l'avis de copyright doit être conservé, il voyage donc avec une redistribution comme le fait l'OFL de la police. Utilisé seulement en secours, pour les pochettes que le décodeur JPEG matériel ne peut pas allouer : le décodeur du P4 n'a pas de mise à l'échelle, donc une pochette de 3000 px demande 17 Mo de PSRAM d'un seul bloc et ne les obtient pas.
- **Ark Pixel Font est sous SIL OFL-1.1, et les tables `ark12` de feckless-graphics-handler-for-m5tab5 (`fonts/ark12`) sont donc aussi sous OFL-1.1, pas MIT.** Convertir les PNG de glyphes en tableaux C fait de ces fichiers une Version Modifiée au sens de la section 5 de l'OFL, et la section 5 exige que les Versions Modifiées restent sous l'OFL. Ce n'est pas un problème — l'OFL autorise explicitement le regroupement avec un logiciel sous n'importe quelle licence, et seuls les fichiers de police sont liés — mais `fonts/ark12/LICENSE-OFL` doit accompagner toute redistribution, image de firmware comprise, et les tables ne doivent pas être vendues seules. Ark ne déclare aucun Nom de Police Réservé (Reserved Font Name), le dérivé n'avait donc pas à être renommé ; il s'appelle `ark12` quand même, parce que ce n'est pas la Version Originale.
- **Arabixel Basic est sous CC BY 4.0** (ArabianDev, <https://arabiandev.itch.io/arabixel-basic-font>), tout comme le fichier de police et la table générée de `fonts/arabixel` dans feckless-graphics-handler-for-m5tab5 ; son code de mise en forme est sous MIT. CC BY (<https://creativecommons.org/licenses/by/4.0/>) demande une attribution, un lien vers la licence et une mention des modifications — le README du composant et l'en-tête de la table les portent, et une redistribution, image de firmware comprise, garde `fonts/arabixel/LICENSE.txt` avec elle.
- MurmurHash2 est dans le domaine public.

## Contribuer

Pour commenter les traductions ou les fonctionnalités, ouvrez une issue sur GitHub, à https://github.com/Sudrien/defeatist-music-player-for-m5tab5

Pour contribuer du code, lisez CONTRIBUTING.md et CLAUDE.md pour les règles de mise en forme.

## « Aurora Player — BT Mode »

- ils utilisent Codex, j'utilise Opus, aucun souci de ce côté
- Oui, installer un module ESP32-PICO-D4 donne accès au Bluetooth classique
- J'écris en partant du principe que la plupart des gens ne veulent pas modifier leur matériel de façon permanente
- Je ne peux toujours pas les recommander tant qu'ils
  - n'ont pas un vrai dépôt public du code source (pas forcément git)
  - ne publient pas les informations de licence (les bibliothèques qu'ils utilisent l'exigent)
  - n'ont pas une meilleure URL que https://frolicking-bonbon-c32cfa.netlify.app/
  - 'feedback welcome' NE MÈNE PAS VRAIMENT QUELQUE PART OÙ DONNER SON AVIS
  - n'ont pas des notes de version qui soient vraiment des notes de version

## Une dernière pique

- waveflow for tab5 music player
  - on peut même générer par IA un logo pour le bon appareil
  - vous dites aux gens de télécharger ffmpeg et ENSUITE un script de conversion ????????? Alors qu'ils n'ont peut-être même pas Python au départ ???? shmusica enfin
  - au fait, ça s'appelle du « transcodage »
  - Votre absence de travail m'a assuré qu'il y a des niveaux dans le vibe coding
