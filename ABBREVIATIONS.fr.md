# Abréviations et réglages

[English](ABBREVIATIONS.md) · [简体中文](ABBREVIATIONS.zh-CN.md) · [日本語](ABBREVIATIONS.ja.md) · [Español](ABBREVIATIONS.es.md) · [Português](ABBREVIATIONS.pt.md) · **Français** · [Italiano](ABBREVIATIONS.it.md)

Ce que veulent dire les mots courts de l'écran, et tous les réglages du lecteur, rangés par l'endroit où ils se trouvent. L'écran fait 720 px de large et la plupart tiennent dans un bouton d'un cinquième de cette largeur, d'où leur brièveté voulue.

> Ceci est une traduction automatique ; la version de référence est l'anglaise, [ABBREVIATIONS.md](ABBREVIATIONS.md). La colonne « À l'écran » garde les étiquettes anglaises littérales du code source ; dans une interface en français, sauf pour les mots gardés en anglais, l'écran affiche les termes traduits du tableau « Dans les autres langues » plus bas.

Tiré du code source (`browser.c`, `panel.c`, `sleeppage.c`, `ui.c`, `settings.h`, `playlist.h`). Si une étiquette d'ici et l'écran ne concordent pas, c'est l'écran le plus récent — corrigez ce fichier.

## La barre de lecture

| À l'écran | Signifie |
|---|---|
| `LIVE` | Un flux radio. Il n'y a pas de position où aller, donc la barre de lecture devient une minute d'historique de niveau. |
| `REC` | Enregistrement. La barre montre le niveau de l'entrée, sur une échelle en décibels (-60 à 0 dBFS). |
| `MUTED` | Enregistrement, mais l'entrée est au zéro numérique exact depuis 300 ms — un casque-micro coupé, ou une source qui n'envoie rien. |
| `RG` | Sous le haut-parleur : ReplayGain ajuste cette piste. La marque jaune sur le curseur de volume indique le volume effectif. |
| `UAC` | À la place de l'icône du haut-parleur : un appareil USB Audio Class a la sortie. |
| `FILE` | Sur la vignette de format, pour un fichier sans extension. Sinon la vignette montre l'extension (`MP3`, `FLAC`…). |
| `45`/`33⅓` | Entre l'étoile et `Fx` : vitesse de tourne-disque, celle de LECTURE sur celle de GRAVURE, parmi 16⅔, 33⅓, 45 et 78. Touchez pour avoir + et − sur chaque nombre ; touchez hors du cadre pour fermer. Deux nombres égaux, c'est la vitesse normale, entourée ; sinon c'est jaune, comme l'horloge du temps restant, qui compte alors le temps réel restant à cette vitesse. Combien de temps ça dure : « La vitesse dure » dans l'onglet AUDIO. Sur un flux radio, c'est gris à 45/45 et sans effet : un direct joue toujours à vitesse normale. |
| `Fx` | Entre la vitesse et la lune : ouvre la feuille Fx sur tout le carré. Jaune quand GRAV, AIGU ou VOIX n'est pas à 1. Touchez hors de la feuille pour la fermer. |
| `GRAV` `AIGU` | Sur la feuille Fx : les graves et les aigus, chacun un gain de ½ à 2 -- ½, ⅔, 1, 3/2, 2. 1, c'est la piste telle quelle. En monter un baisse tout le reste pour faire de la place, donc rien ne sature. |
| `VOIX` | Sur la feuille Fx : la voix, comme la Réduction vocale d'Audacity -- ce qu'on garde de ce que gauche et droite ont en commun (là où se trouve d'ordinaire la voix principale), entre 150 Hz et 8 kHz pour que la basse et la grosse caisse restent. 0 l'enlève (karaoké), ½ la recule, 1, c'est la piste telle quelle, 3/2 et 2 l'avancent et baissent le reste pour faire de la place. Sur une piste mono, cela amincit cette plage de tout. |

### La ligne d'état, sous le volume

Des indicateurs, pas des interrupteurs — rien ici ne réagit au toucher. Vert, c'est allumé ; gris foncé, éteint (WIFI a aussi du jaune). Chacun a son propre interrupteur ailleurs.

| À l'écran | Vert quand | Se règle depuis |
|---|---|---|
| `USB` | Le port USB-A est alimenté | onglet USB, alimentation USB |
| `WIFI` | Connecté, avec une adresse. **Jaune** : activé mais pas encore connecté — démarrage, recherche, connexion ou échec | onglet NET, Wi-Fi |
| `MPD` | Le serveur MPD tourne | onglet NET, serveur MPD |
| `HTTPS` | La télécommande par navigateur tourne | onglet NET, Télécommande |
| `SLEEP xxM` | La minuterie est réglée ; minutes restantes, arrondies au-dessus. Juste `SLEEP` quand elle ne l'est pas | page Veille |
| `IDLE` | L'extinction est réglée et décompte : rien ne joue, ne diffuse, n'enregistre ni n'indexe. **Jaune** : réglée mais retenue — il se passe quelque chose, donc l'attente redémarre. Gris quand l'extinction est sur Jamais. Pas de minutes (6020) | page Veille, Éteindre |

Au bout droit de la même ligne, la batterie (il n'y a pas d'icône de batterie) :

| À l'écran | Signifie | Couleur |
|---|---|---|
| `BATT 73%` | Fonctionne sur batterie | Gris clair ; rouge à 20 % et moins |
| `CHRG 73%` | Le courant arrive et charge la batterie | Vert |
| `CHARGED` | La batterie est pleine | Vert |
| `NO BATT` | Pas de batterie en place ; fonctionne sur l'USB-C | Gris foncé |
| `BATT` | La jauge de batterie ne donne pas de lecture | Gris clair |

## Le sélecteur (bouton dossier)

### Onglets

| À l'écran | Signifie |
|---|---|
| `microSD` | La carte microSD. |
| `USB` | Une clé USB dans le port USB-A. |
| `RADIO` | La radio sur Internet : `stations.m3u` et les favoris. |

### Boutons du bas

| À l'écran | Signifie |
|---|---|
| `UP` | Remonter d'un dossier. |
| `RLOD` | Au premier niveau de l'onglet RADIO, à la place de `UP` : recharger la liste des stations depuis la carte. |
| `FLDR` | Lire tout ce dossier. |
| `UP^` | Remonter la liste d'une page. |
| `DN` | Descendre la liste d'une page. |
| `X` | Fermer le sélecteur. |
| *(ordre)* | L'ordre de lecture — touchez pour le faire tourner. Voir ci-dessous. |

### Ordre de lecture

Toucher le bouton d'ordre passe par **ONE → ALL → RPT → EAT → RND → RPT1 →** et revient à ONE.

| À l'écran | Signifie | Équivalent MPD |
|---|---|---|
| `ONE` | Lire cette piste, puis s'arrêter. | `single 1` |
| `ALL` | Passer à la piste suivante ; s'arrêter à la fin du dossier. | (tout désactivé) |
| `RPT` | `ALL`, en recommençant depuis le début à la fin. | `repeat 1` |
| `EAT` | `ALL`, en retirant chaque piste de la liste quand on la quitte. | `consume 1` |
| `RND` | Ordre aléatoire, sans répétition tant que toutes les pistes n'ont pas joué. | `random 1` |
| `RPT1` | Cette piste encore, jusqu'à nouvel ordre. | `repeat 1` + `single 1` |
| `ONE1`, `EAT1` | `ONE` ou `EAT` pour un seul morceau, parce qu'un client MPD a demandé `single oneshot` ou `consume oneshot`. Revient ensuite au nom sans chiffre. | `oneshot` |

Chaque ordre correspond à exactement une combinaison MPD. Dans l'autre sens, trois des huit combinaisons random/repeat/single de MPD n'ont pas d'ordre exact ici ; `mpdmode.h` donne la table et ce que chacune devient.

## Réglages (bouton engrenage)

Cinq onglets : `SD`, `USB`, `BUILD`, `AUDIO`, `NET`. NET est l'abréviation de network (réseau).

### SD et USB

| Réglage | Valeurs | Remarques |
|---|---|---|
| Réindexer | `REINDEX` | Reconstruit l'index des médias de ce volume. Affiche `INDEXING` pendant celui de ce volume, `BUSY` pendant celui d'un autre — un à la fois. |
| Alimentation USB (onglet USB) | `ON` / `OFF` | Le courant du port USB-A. `IN USE` et grisé pendant la lecture d'une piste depuis la clé. Le lecteur le coupe aussi de lui-même après une piste quand rien n'est branché. |

### BUILD

Ce qui tourne, et la langue de l'écran.

| Réglage | Valeurs | Remarques |
|---|---|---|
| Langue | `English` / `简体中文` / `日本語` / `Español` / `Português` / `Français` / `Italiano` | Touchez pour faire défiler. Chaque langue est écrite dans sa propre langue, et l'étiquette de la ligne et les noms d'onglets ne sont jamais traduits, donc on retrouve le chemin du retour depuis n'importe laquelle. Gardée dans l'appareil, pas sur la carte. Seuls certains écrans sont traduits pour l'instant ; le reste reste en anglais. |

- `app`, `version`, `built`, `idf`, et la mémoire libre `heap`, `psram`, et `uptime`.
- **Source (code source)** : le dépôt, github.com/Sudrien/defeatist-music-player-for-m5tab5.
- **Libraries (bibliothèques)** : chaque composant et bibliothèque intégrée de cette compilation, avec sa version (ou les sept premiers caractères de son commit pour un composant git), et sa licence après `--`. `licence not found` signifie que le composant n'avait ni fichier ni champ de licence. Généré depuis `dependencies.lock` et les versions intégrées au moment de configurer la compilation, c'est donc ce qui a été compilé.

### AUDIO

| Réglage | Valeurs | Remarques |
|---|---|---|
| ReplayGain | oui / non | Met les pistes au même niveau. Désactivé arrête l'ajustement, pas la mesure. |
| Fondu enchaîné | non, ou des secondes | Curseur. |
| Même album | oui / non | Fondu enchaîné aussi entre les pistes d'un même album. Désactivé laisse les enchaînements de l'album intacts. Un dossier compte comme un album. |
| La vitesse dure | `TRACK` / `BOOT` | Combien de temps dure une vitesse de tourne-disque. `TRACK` : revient à l'égalité quand une nouvelle piste commence ; répéter une (`RPT1`) n'est pas une nouvelle piste. `BOOT` : gardée jusqu'au prochain changement, même après extinction. Allumé sur `BOOT`. |
| Enreg. depuis | voir ci-dessous | Ce qu'enregistre le bouton d'enregistrement. |

**Enreg. depuis** — touchez pour faire défiler. Un choix est allumé quand son entrée est là pour enregistrer.

| À l'écran | Enregistre |
|---|---|
| `MONO` | Les deux micros intégrés, mélangés en mono. |
| `STEREO` | Les deux micros intégrés, gauche et droite. |
| `FOCUSED` | La paire intégrée en faisceau dirigé hors de l'écran. Mono. |
| `HEADSET` | Le micro de la prise casque, en mono. Allumé quand quelque chose est branché — la prise ne distingue pas un casque-micro d'écouteurs. |
| `UAC` | Un micro USB, à sa propre fréquence et son propre nombre de canaux. |
| `AUTO` | L'USB s'il y a un micro, sinon le casque, sinon `MONO`. Toujours en mono. |
| `OFF` | Pas d'enregistrement : l'interrupteur de transport perd sa position d'enregistrement. |

Tous les enregistrements sont en FLAC, dans `Recordings/` sur la carte SD (sur la clé USB s'il n'y a pas de carte).

### NET

| Réglage | Valeurs | Remarques |
|---|---|---|
| Wi-Fi | oui / non | |
| Télécommande | oui / non | La télécommande par navigateur, en HTTPS. |
| Serveur MPD | oui / non | Pour les clients MPD et Home Assistant. |
| Heure réseau | oui / non | NTP. Grisé quand le Wi-Fi est coupé ; le réglage est conservé. |
| Ajouter un réseau | `START` / `STOP` | Ouvre un portail captif pour connecter un réseau Wi-Fi depuis un téléphone. |
| Test de débit | `RUN` (`BUSY` pendant le test) | À quelle vitesse on peut tirer le flux d'une station sans rien décoder. |
| Horloge | `RESET` | Oublier l'heure. Disponible seulement tant que l'heure n'a pas été vérifiée. |

## Page Veille (bouton lune)

| Réglage | Valeurs |
|---|---|
| Écran | La luminosité, en pourcentage. |
| Rotation | `0`, `90`, `180`, `270`. |
| Assombrir l'écran | Jamais, ou après 15 s à 2 min sans toucher — à mi-luminosité. |
| Écran éteint | Jamais, ou 30 s à 5 min. Jamais quand cette page est ouverte. |
| Minuterie | non, ou 15 min à 2 h par pas de 15 minutes. Le son baisse en fondu pendant les 20 dernières secondes. |
| Éteindre | Jamais, ou après 15 min, 30 min, 1 h ou 2 h sans lecture, enregistrement ni toucher. L'appareil s'éteint de lui-même ; le bouton latéral le rallume. Distinct de la minuterie, et jamais pendant la musique. |

## Dans les autres langues (6019)

Les étiquettes ci-dessus en mandarin, japonais, espagnol, portugais (Brésil), français et italien, au choix dans l'onglet BUILD. Chacune traduit le mot entier que l'étiquette abrège, raccourci à nouveau là où un bouton est plus étroit que le mot : les boutons du sélecteur font 112 px (8 caractères latins ou 4 CJC), les pastilles de réglage 124 px (5 ou 3). Les mots de l'ordre de lecture suivent le vocabulaire MPD de Cantata là où il en a un — 重复/随机/单曲 et 播完删除 (de 播放后删除), リピート/ランダム, Repetir — et ON/OFF suivent le 开/关 qu'utilise l'onglet Audio depuis 6013.

Gardés en anglais dans toutes les langues, par `same()` : les noms d'onglets (SD, USB, BUILD, AUDIO, NET, RADIO), USB, WIFI, MPD, HTTPS, UAC, RG, et les unités.

| Où | English | 简体中文 | 日本語 | Español | Português | Français | Italiano |
|---|---|---|---|---|---|---|---|
| Ligne d'état | `SLEEP` | 睡眠 | 睡眠 | SUEÑO | SONO | VEILLE | SONNO |
| Ligne d'état | `SLEEP %dM` | 睡眠%d分 | 睡眠%d分 | SUEÑO %dM | SONO %dM | VEILLE %dM | SONNO %dM |
| Ligne d'état | `IDLE` | 空闲 | 待機 | INAC | OCIO | INACT | INATT |
| Ligne d'état | `BATT` | 电池 | 電池 | BAT | BAT | BATT | BATT |
| Ligne d'état | `BATT %d%%` | 电池 %d%% | 電池 %d%% | BAT %d%% | BAT %d%% | BATT %d%% | BATT %d%% |
| Ligne d'état | `CHRG %d%%` | 充电 %d%% | 充電 %d%% | CARG %d%% | CARR %d%% | CHRG %d%% | CARIC %d%% |
| Ligne d'état | `CHARGED` | 已充满 | 満充電 | CARGADA | CARREGADA | CHARGÉE | CARICA |
| Ligne d'état | `NO BATT` | 无电池 | 電池なし | SIN BAT | SEM BAT | SANS BATT | NO BATT |
| Lecture en cours | `LIVE` | 直播 | ライブ | VIVO | AO VIVO | DIRECT | LIVE |
| Lecture en cours | `REC` | 录音 | 録音 | GRAB | GRAV | ENR | REG |
| Lecture en cours | `MUTED` | 静音 | 無音 | MUDO | MUDO | MUET | MUTO |
| Bas du sélecteur | `UP` | 上级 | 上へ | SUBIR | SUBIR | MONTER | SU |
| Bas du sélecteur | `RLOD` | 重新加载 | 再読込 | RECARGAR | RECARR. | RECHARG. | RICARICA |
| Bas du sélecteur | `FLDR` | 播放目录 | フォルダ | CARPETA | PASTA | DOSSIER | CARTELLA |
| Bas du sélecteur | `UP^` | 上页 | 前頁 | RE PÁG | PG CIMA | PG HAUT | PAG SU |
| Bas du sélecteur | `DN` | 下页 | 次頁 | AV PÁG | PG BAIXO | PG BAS | PAG GIÙ |
| Ordre de lecture | `ONE` | 单曲 | 単曲 | ÚNICA | UMA | UNE | UNO |
| Ordre de lecture | `ALL` | 顺序 | 全曲 | TODAS | TODAS | TOUT | TUTTI |
| Ordre de lecture | `RPT` | 重复 | リピート | REPETIR | REPETIR | RÉPÉTER | RIPETI |
| Ordre de lecture | `EAT` | 播完删除 | 消費 | BORRAR | COMER | MANGER | MANGIA |
| Ordre de lecture | `RND` | 随机 | ランダム | AZAR | ALEAT | ALÉA | CASUALE |
| Ordre de lecture | `RPT1` | 单曲重复 | 1曲反復 | REPITE 1 | REPETE 1 | RÉPÉTER1 | RIPETI1 |
| Ordre de lecture | `ONE1` | 单曲1 | 単曲1 | ÚNICA1 | UMA1 | UNE1 | UNO1 |
| Ordre de lecture | `EAT1` | 删除1 | 消費1 | BORRAR1 | COMER1 | MANGER1 | MANGIA1 |
| Réglages | `ON` | 开 | オン | SÍ | SIM | OUI | SÌ |
| Réglages | `OFF` | 关 | オフ | NO | NÃO | NON | NO |
| Réglages | `IN USE` | 使用中 | 使用中 | EN USO | EM USO | UTILISÉ | IN USO |
| Réglages | `REINDEX` | 重新索引 | 再索引 | REINDEXAR | REINDEXAR | RÉINDEXER | REINDICIZZA |
| Réglages | `INDEXING` | 索引中 | 索引中 | INDEXANDO | INDEXANDO | INDEXATION | INDICIZZA |
| Réglages | `BUSY` | 进行中 | 作業中 | OCUP. | OCUP. | OCCUPÉ | OCCUP. |
| Réglages | `START` | 开始 | 開始 | ABRIR | ABRIR | OUVRIR | APRI |
| Réglages | `STOP` | 停止 | 停止 | PARAR | PARAR | ARRÊT | STOP |
| Réglages | `RUN` | 运行 | 実行 | MEDIR | MEDIR | LANCER | MISURA |
| Réglages | `RESET` | 重置 | 戻す | VOLVER | VOLTAR | RÉTABLIR | AZZERA |
| Réglages | `CLOSE` | 关闭 | 閉じる | CERRAR | FECHAR | FERMER | CHIUDI |
| Réglages | `IMPORT` | 导入 | 取込 | IMPORTAR | IMPORTAR | IMPORTER | IMPORTA |
| Réglages | `DONE` | 完成 | 完了 | HECHO | OK | OK | FATTO |
| Réglages | `RETRY` | 重试 | 再試行 | REPETIR | REPETIR | RÉESSAYER | RIPROVA |
| Réglages | `TRACK` | 每曲 | 曲ごと | PISTA | FAIXA | PISTE | BRANO |
| Réglages | `BOOT` | 保持 | 保持 | SIEMPRE | SEMPRE | TOUJOURS | SEMPRE |
| Enreg. depuis | `MONO` | 单声道 | モノラル | MONO | MONO | MONO | MONO |
| Enreg. depuis | `STEREO` | 立体声 | ステレオ | ESTÉREO | ESTÉREO | STÉRÉO | STEREO |
| Enreg. depuis | `FOCUSED` | 定向 | フォーカス | ENFOCADO | FOCADO | DIRIGÉ | DIRETTO |
| Enreg. depuis | `HEADSET` | 耳麦 | ヘッドセット | AURICULAR | HEADSET | CASQUE | CUFFIA |
| Enreg. depuis | `AUTO` | 自动 | 自動 | AUTO | AUTO | AUTO | AUTO |

## Options de compilation

Pas à l'écran — `idf.py -D<NOM>=1 build`. Chacune reste dans `CMakeCache.txt` jusqu'à être remise à 0, donc compilez une version de diffusion depuis un arbre propre.

| Option | Effet |
|---|---|
| `HEAPCHECK` | Points de contrôle d'intégrité du tas qui nomment le dernier sous-système avant une corruption. Lent. |
| `WAVEFORM` | L'analyse de forme d'onde du fichier entier. |
| `O3CHECK` | Tout `main` en -O3, pour attraper des avertissements. Une vérification, pas un réglage de diffusion. |
| `DECBENCH` | Journalise le coût de décodage de minimp3 en cycles CPU par trame. |
