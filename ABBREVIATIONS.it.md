# Abbreviazioni e impostazioni

[English](ABBREVIATIONS.md) · [简体中文](ABBREVIATIONS.zh-CN.md) · [日本語](ABBREVIATIONS.ja.md) · [Español](ABBREVIATIONS.es.md) · [Português](ABBREVIATIONS.pt.md) · [Français](ABBREVIATIONS.fr.md) · **Italiano**

Cosa significano le parole brevi dello schermo, e tutte le impostazioni del lettore, ordinate per dove si trovano. Lo schermo è largo 720 px e la maggior parte sta in un pulsante largo un quinto, quindi sono brevi apposta.

> Questa è una traduzione automatica; la versione di riferimento è quella inglese, [ABBREVIATIONS.md](ABBREVIATIONS.md). La colonna "Sullo schermo" mantiene le etichette inglesi letterali del codice sorgente; in un'interfaccia in italiano, tranne le parole tenute in inglese, lo schermo mostra i termini tradotti della tabella "In altre lingue" più sotto.

Preso dal codice sorgente (`browser.c`, `panel.c`, `sleeppage.c`, `ui.c`, `settings.h`, `playlist.h`). Se un'etichetta qui e lo schermo non coincidono, lo schermo è più recente — correggi questo file.

## La barra di riproduzione

| Sullo schermo | Significa |
|---|---|
| `LIVE` | Uno stream radio. Non c'è una posizione a cui andare, quindi la barra di avanzamento diventa un minuto di storico del livello. |
| `REC` | In registrazione. La barra mostra il livello dell'ingresso, su scala in decibel (da -60 a 0 dBFS). |
| `MUTED` | In registrazione, ma l'ingresso è a zero digitale esatto da 300 ms — cuffie con microfono silenziate, o una sorgente che non manda niente. |
| `RG` | Sotto l'altoparlante: ReplayGain sta regolando questo brano. Il segno giallo sul cursore del volume è dove il volume si trova davvero. |
| `UAC` | Al posto dell'icona dell'altoparlante: un dispositivo USB Audio Class ha l'uscita. |
| `FILE` | Sulla scheda del formato, per un file senza estensione. Altrimenti la scheda mostra l'estensione (`MP3`, `FLAC`…). |
| `45`/`33⅓` | Tra la stella e `Fx`: velocità del giradischi, quella di RIPRODUZIONE su quella di INCISIONE, fra 16⅔, 33⅓, 45 e 78. Toccalo per avere + e − su ogni numero; tocca fuori dal riquadro per chiudere. Numeri uguali sono la velocità normale, contornata; altrimenti è giallo, come l'orologio del tempo rimanente, che allora conta il tempo reale rimasto a quella velocità. Quanto dura: "La velocità dura" nella scheda AUDIO. Su uno stream radio è grigio a 45/45 e non fa niente: una diretta suona sempre a velocità normale. |
| `Fx` | Tra la velocità e la luna: apre il foglio Fx su tutto il quadrato. Giallo quando BASSI, ALTI o VOCE non è a 1, ECO non è a 0, TBASS o TALTI sono attivi, FRUS, WOW o FLUT non sono spenti, o LARG non è a 1. Tocca fuori dal foglio per chiuderlo. |
| `BASSI` `ALTI` | Sul foglio Fx: bassi e alti, ciascuno un guadagno da ½ a 2 -- ½, ⅔, 1, 3/2, 2. 1 è la traccia com'era. Alzarne uno abbassa tutto il resto per fare spazio, quindi niente distorce. |
| `VOCE` | Sul foglio Fx: la voce, come la Riduzione vocale di Audacity -- quanto si tiene di ciò che sinistra e destra hanno in comune (dove di solito sta la voce principale), fra 150 Hz e 8 kHz perché basso e cassa restino. 0 la toglie (karaoke), ½ la arretra, 1 è la traccia com'era, 3/2 e 2 la portano avanti e abbassano il resto per fare spazio. Su una traccia mono è in grigio e non fa nulla; si può comunque regolare, per la prossima traccia stereo. |
| `ECO` `TEMPO` | Sul foglio Fx: l'Eco di Audacity, ripetizioni che si spengono. ECO è il volume di ogni ripetizione rispetto alla precedente: 0 è spento, poi ⅛ ¼ ⅓ ½ ⅔ ¾ ⅞ -- ⅛ è una ripetizione debole, ⅞ una lunga coda. Più lunga la coda, più tutto viene abbassato perché le ripetizioni non distorcano mai (⅞ è circa 18 dB più basso). TEMPO è l'intervallo fra le ripetizioni: 60, 125, 250, 500 o 1000 ms. |
| `TBASS` `TALTI` | Sul foglio Fx: i filtri passa-alto e passa-basso di Audacity. Dove BASSI e ALTI inclinano, questi tagliano: oltre la frequenza di taglio il suono cade in fretta. TBASS toglie ciò che sta sotto: no, 40, 80, 150 o 300 Hz (rombo). TALTI toglie ciò che sta sopra: no, 12k, 8k, 4k o 3k (fruscio, asprezza). Insieme, 150 Hz e 8k è una radio AM, 300 Hz e 3k un telefono. |
| `FRUS` | Sul foglio Fx: la superficie di un disco consumato sopra il brano -- crepitii a caso e un leggero fruscio sotto. Spento, poi 1, 4, 12 o 40 crepitii al secondo: 1 una stampa pulita, 40 un 78 giri trovato in soffitta. Quasi tutti sono deboli e pochi no, ognuno al suo posto tra le casse. Il brano si abbassa un po' per fare spazio, così un crepitio non satura mai. Passa per BASSI, ALTI e i tagli come farebbe il disco: con TBASS e TALTI è un vecchio disco alla radio AM. |
| `LARG` | Sul foglio Fx: la larghezza stereo -- quanto resta di ciò che sinistra e destra non hanno in comune. 0 è mono, ½ più stretto, 1 il brano così com'è, 3/2 e 2 più largo. Il centro (voce, cassa, basso) non si muove mai, e su una sola cassa è sempre il brano; più largo spinge ciò che è panoramicato oltre le casse, e abbassa un po' il resto per fare spazio. Su una traccia mono è in grigio e non fa nulla; si può comunque regolare, per la prossima traccia stereo. |
| `WOW` `FLUT` | Sul foglio Fx: un giradischi che non gira del tutto regolare. WOW fa ondeggiare l'intonazione una volta a giro, alla velocità PLAYED: più lento a 16⅔, più veloce a 78; su una radio, a 45. FLUT (flutter) la fa ondeggiare 18 volte a giro, quello di un motore. I numeri sono quanto si sposta l'intonazione, in percento: WOW spento, 0.1, 0.25, 0.5, 1; FLUT spento, 0.05, 0.1, 0.2, 0.5. Un cambio entra piano in qualche secondo, senza salti. |

### La riga di stato, sotto il volume

Indicatori, non interruttori — qui niente fa qualcosa se toccato. Verde è acceso, grigio scuro spento (WIFI ha anche il giallo). Ognuno ha il suo interruttore altrove.

| Sullo schermo | Verde quando | Si cambia da |
|---|---|---|
| `USB` | La porta USB-A è alimentata | scheda USB, alimentazione USB |
| `WIFI` | Connesso, con un indirizzo. **Giallo**: acceso ma non ancora connesso — in avvio, in ricerca, in collegamento o in errore | scheda NET, Wi-Fi |
| `MPD` | Il server MPD è attivo | scheda NET, Server MPD |
| `HTTPS` | Il telecomando da browser è attivo | scheda NET, Telecomando |
| `SLEEP xxM` | Il timer è impostato; minuti rimasti, arrotondati per eccesso. Solo `SLEEP` quando non lo è | pagina Riposo |
| `IDLE` | Lo spegnimento è impostato e conta: niente in riproduzione, in streaming, in registrazione né in indicizzazione. **Giallo**: impostato ma trattenuto — sta succedendo qualcosa, quindi l'attesa riparte. Grigio quando Spegni è su Mai. Senza minuti (6020) | pagina Riposo, Spegni |

All'estremità destra della stessa riga, la batteria (non c'è un'icona della batteria):

| Sullo schermo | Significa | Colore |
|---|---|---|
| `BATT 73%` | Funziona a batteria | Grigio chiaro; rosso al 20% o meno |
| `CHRG 73%` | Arriva corrente e carica la batteria | Verde |
| `CHARGED` | La batteria è carica | Verde |
| `NO BATT` | Nessuna batteria inserita; funziona dall'USB-C | Grigio scuro |
| `BATT` | L'indicatore della batteria non dà lettura | Grigio chiaro |

## Il selettore (pulsante cartella)

### Schede

| Sullo schermo | Significa |
|---|---|
| `microSD` | La scheda microSD. |
| `USB` | Una chiavetta nella porta USB-A. |
| `RADIO` | Radio via internet: `stations.m3u` e preferiti. |

### Pulsanti in basso

| Sullo schermo | Significa |
|---|---|
| `UP` | Sale di una cartella. |
| `RLOD` | Al livello più alto della scheda RADIO, al posto di `UP`: ricarica l'elenco delle radio dalla scheda. |
| `FLDR` | Riproduce tutta questa cartella. |
| `UP^` | Scorre l'elenco su di una pagina. |
| `DN` | Scorre l'elenco giù di una pagina. |
| `X` | Chiude il selettore. |
| *(ordine)* | L'ordine di riproduzione — tocca per cambiarlo. Vedi sotto. |

### Ordine di riproduzione

Toccare il pulsante dell'ordine passa per **ONE → ALL → RPT → EAT → RND → RPT1 →** e torna a ONE.

| Sullo schermo | Significa | Equivalente MPD |
|---|---|---|
| `ONE` | Riproduce questo brano, poi si ferma. | `single 1` |
| `ALL` | Passa al brano successivo; si ferma alla fine della cartella. | (tutto spento) |
| `RPT` | `ALL`, ricominciando dall'inizio alla fine. | `repeat 1` |
| `EAT` | `ALL`, togliendo ogni brano dall'elenco quando lo lascia. | `consume 1` |
| `RND` | Ordine casuale, senza ripetizioni finché tutti i brani non hanno suonato. | `random 1` |
| `RPT1` | Ancora questo brano, fino a nuovo ordine. | `repeat 1` + `single 1` |
| `ONE1`, `EAT1` | `ONE` o `EAT` per un solo brano, perché un client MPD ha chiesto `single oneshot` o `consume oneshot`. Dopo torna al nome senza numero. | `oneshot` |

Ogni ordine è esattamente una combinazione di MPD. Nell'altro senso, tre delle otto combinazioni random/repeat/single di MPD non hanno un ordine esatto qui; `mpdmode.h` ha la tabella e cosa diventa ciascuna.

## Impostazioni (pulsante ingranaggio)

Cinque schede: `SD`, `USB`, `BUILD`, `AUDIO`, `NET`. NET sta per network (rete).

### SD e USB

| Impostazione | Valori | Note |
|---|---|---|
| Reindicizza | `REINDEX` | Ricostruisce l'indice dei media di quel volume. Mostra `INDEXING` mentre gira quello di questo volume, `BUSY` mentre gira quello di un altro — uno alla volta. |
| Alimentazione USB (scheda USB) | `ON` / `OFF` | Corrente alla porta USB-A. `IN USE` e in grigio mentre suona un brano dalla chiavetta. Il lettore la toglie anche da solo dopo un brano quando non c'è niente collegato. |

### BUILD

Cosa è in esecuzione, e la lingua dello schermo.

| Impostazione | Valori | Note |
|---|---|---|
| Lingua | `English` / `简体中文` / `日本語` / `Español` / `Português` / `Français` / `Italiano` | Tocca per cambiare. Ogni lingua è scritta nella propria lingua, e l'etichetta della riga e i nomi delle schede non vengono mai tradotti, quindi la strada del ritorno si trova da qualunque lingua. Salvata nel dispositivo, non sulla scheda. Per ora sono tradotte solo alcune schermate; il resto resta in inglese. |

- `app`, `version`, `built`, `idf`, e la memoria libera `heap`, `psram`, e `uptime`.
- **Source (codice sorgente)**: il repository, github.com/Sudrien/defeatist-music-player-for-m5tab5.
- **Libraries (librerie)**: ogni componente e libreria inclusa in questa build, con la sua versione (o i primi sette caratteri del commit, se viene da git), e la sua licenza dopo `--`. `licence not found` significa che il componente non aveva né un file né un campo di licenza. Generato da `dependencies.lock` e dalle versioni incluse quando la build viene configurata, quindi è ciò che è stato compilato.

### AUDIO

| Impostazione | Valori | Note |
|---|---|---|
| ReplayGain | sì / no | Livella i brani fra loro. Spento ferma la regolazione, non la misura. |
| Dissolvenza | no, o secondi | Cursore. |
| Stesso album | sì / no | Dissolvenza anche fra brani dello stesso album. Spento lascia stare i passaggi dell'album. Una cartella vale un album. |
| La velocità dura | `TRACK` / `BOOT` | Quanto dura una velocità del giradischi. `TRACK`: torna uguale quando inizia un nuovo brano; ripeti uno (`RPT1`) non è un nuovo brano. `BOOT`: resta finché non la cambi, anche dopo lo spegnimento. Acceso su `BOOT`. |
| Registra da | vedi sotto | Cosa registra il pulsante di registrazione. |

**Registra da** — tocca per cambiare. Una scelta è accesa quando il suo ingresso c'è per registrare.

| Sullo schermo | Registra |
|---|---|
| `MONO` | I due microfoni interni, sommati in mono. |
| `STEREO` | I due microfoni interni, sinistro e destro. |
| `FOCUSED` | La coppia interna come un fascio diretto fuori dallo schermo. Mono. |
| `HEADSET` | Il microfono nella presa cuffie, in mono. Acceso quando c'è qualcosa collegato — la presa non distingue cuffie con microfono da cuffie semplici. |
| `UAC` | Un microfono USB, alla sua frequenza e col suo numero di canali. |
| `AUTO` | USB se c'è un microfono, altrimenti la presa cuffie, altrimenti `MONO`. Sempre mono. |
| `OFF` | Nessuna registrazione: l'interruttore di trasporto perde la posizione di registrazione. |

Tutte le registrazioni sono FLAC, in `Recordings/` sulla scheda SD (sulla chiavetta USB se non c'è la scheda).

### NET

| Impostazione | Valori | Note |
|---|---|---|
| Wi-Fi | sì / no | |
| Telecomando | sì / no | Il telecomando da browser, in HTTPS. |
| Server MPD | sì / no | Per i client MPD e Home Assistant. |
| Ora di rete | sì / no | NTP. In grigio col Wi-Fi spento; l'impostazione resta. |
| Aggiungi rete | `START` / `STOP` | Apre un portale captive per collegarsi a una rete Wi-Fi dal telefono. |
| Misura velocità | `RUN` (`BUSY` mentre gira) | Quanto velocemente si può scaricare lo stream di una radio senza decodificare niente. |
| Orologio | `RESET` | Dimentica l'orologio. Disponibile solo finché l'ora non è stata verificata. |

## Pagina Riposo (pulsante luna)

| Impostazione | Valori |
|---|---|
| Schermo | La luminosità, in percentuale. |
| Rotazione | `0`, `90`, `180`, `270`. |
| Attenua schermo | Mai, o dopo 15 s a 2 min senza tocchi — a metà luminosità. |
| Spegni schermo | Mai, o 30 s a 5 min. Mai con questa pagina aperta. |
| Timer | no, o da 15 min a 2 h a passi di 15 minuti. Il suono sfuma negli ultimi 20 secondi. |
| Spegni | Mai, o dopo 15 min, 30 min, 1 h o 2 h senza riprodurre, registrare né toccare. Il dispositivo si spegne da solo; il tasto laterale lo riaccende. Separato dal timer, e mai con la musica in corso. |

## In altre lingue (6019)

Le etichette sopra in mandarino, giapponese, spagnolo, portoghese (Brasile), francese e italiano, da scegliere nella scheda BUILD. Ognuna traduce la parola intera che l'etichetta abbrevia, riaccorciata dove un pulsante è più stretto della parola: i pulsanti del selettore sono di 112 px (8 caratteri latini o 4 CJK), le pillole delle impostazioni di 124 px (5 o 3). Le parole dell'ordine di riproduzione seguono il vocabolario MPD di Cantata dove ne ha uno — 重复/随机/单曲 e 播完删除 (da 播放后删除), リピート/ランダム, Repetir — e ON/OFF seguono il 开/关 che la scheda Audio usa dalla 6013.

Tenuti in inglese in ogni lingua, con `same()`: i nomi delle schede (SD, USB, BUILD, AUDIO, NET, RADIO), USB, WIFI, MPD, HTTPS, UAC, RG, e le unità.

| Dove | English | 简体中文 | 日本語 | Español | Português | Français | Italiano |
|---|---|---|---|---|---|---|---|
| Riga di stato | `SLEEP` | 睡眠 | 睡眠 | SUEÑO | SONO | VEILLE | SONNO |
| Riga di stato | `SLEEP %dM` | 睡眠%d分 | 睡眠%d分 | SUEÑO %dM | SONO %dM | VEILLE %dM | SONNO %dM |
| Riga di stato | `IDLE` | 空闲 | 待機 | INAC | OCIO | INACT | INATT |
| Riga di stato | `BATT` | 电池 | 電池 | BAT | BAT | BATT | BATT |
| Riga di stato | `BATT %d%%` | 电池 %d%% | 電池 %d%% | BAT %d%% | BAT %d%% | BATT %d%% | BATT %d%% |
| Riga di stato | `CHRG %d%%` | 充电 %d%% | 充電 %d%% | CARG %d%% | CARR %d%% | CHRG %d%% | CARIC %d%% |
| Riga di stato | `CHARGED` | 已充满 | 満充電 | CARGADA | CARREGADA | CHARGÉE | CARICA |
| Riga di stato | `NO BATT` | 无电池 | 電池なし | SIN BAT | SEM BAT | SANS BATT | NO BATT |
| In riproduzione | `LIVE` | 直播 | ライブ | VIVO | AO VIVO | DIRECT | LIVE |
| In riproduzione | `REC` | 录音 | 録音 | GRAB | GRAV | ENR | REG |
| In riproduzione | `MUTED` | 静音 | 無音 | MUDO | MUDO | MUET | MUTO |
| Fondo del selettore | `UP` | 上级 | 上へ | SUBIR | SUBIR | MONTER | SU |
| Fondo del selettore | `RLOD` | 重新加载 | 再読込 | RECARGAR | RECARR. | RECHARG. | RICARICA |
| Fondo del selettore | `FLDR` | 播放目录 | フォルダ | CARPETA | PASTA | DOSSIER | CARTELLA |
| Fondo del selettore | `UP^` | 上页 | 前頁 | RE PÁG | PG CIMA | PG HAUT | PAG SU |
| Fondo del selettore | `DN` | 下页 | 次頁 | AV PÁG | PG BAIXO | PG BAS | PAG GIÙ |
| Ordine | `ONE` | 单曲 | 単曲 | ÚNICA | UMA | UNE | UNO |
| Ordine | `ALL` | 顺序 | 全曲 | TODAS | TODAS | TOUT | TUTTI |
| Ordine | `RPT` | 重复 | リピート | REPETIR | REPETIR | RÉPÉTER | RIPETI |
| Ordine | `EAT` | 播完删除 | 消費 | BORRAR | COMER | MANGER | MANGIA |
| Ordine | `RND` | 随机 | ランダム | AZAR | ALEAT | ALÉA | CASUALE |
| Ordine | `RPT1` | 单曲重复 | 1曲反復 | REPITE 1 | REPETE 1 | RÉPÉTER1 | RIPETI1 |
| Ordine | `ONE1` | 单曲1 | 単曲1 | ÚNICA1 | UMA1 | UNE1 | UNO1 |
| Ordine | `EAT1` | 删除1 | 消費1 | BORRAR1 | COMER1 | MANGER1 | MANGIA1 |
| Impostazioni | `ON` | 开 | オン | SÍ | SIM | OUI | SÌ |
| Impostazioni | `OFF` | 关 | オフ | NO | NÃO | NON | NO |
| Impostazioni | `IN USE` | 使用中 | 使用中 | EN USO | EM USO | UTILISÉ | IN USO |
| Impostazioni | `REINDEX` | 重新索引 | 再索引 | REINDEXAR | REINDEXAR | RÉINDEXER | REINDICIZZA |
| Impostazioni | `INDEXING` | 索引中 | 索引中 | INDEXANDO | INDEXANDO | INDEXATION | INDICIZZA |
| Impostazioni | `BUSY` | 进行中 | 作業中 | OCUP. | OCUP. | OCCUPÉ | OCCUP. |
| Impostazioni | `START` | 开始 | 開始 | ABRIR | ABRIR | OUVRIR | APRI |
| Impostazioni | `STOP` | 停止 | 停止 | PARAR | PARAR | ARRÊT | STOP |
| Impostazioni | `RUN` | 运行 | 実行 | MEDIR | MEDIR | LANCER | MISURA |
| Impostazioni | `RESET` | 重置 | 戻す | VOLVER | VOLTAR | RÉTABLIR | AZZERA |
| Impostazioni | `CLOSE` | 关闭 | 閉じる | CERRAR | FECHAR | FERMER | CHIUDI |
| Impostazioni | `IMPORT` | 导入 | 取込 | IMPORTAR | IMPORTAR | IMPORTER | IMPORTA |
| Impostazioni | `DONE` | 完成 | 完了 | HECHO | OK | OK | FATTO |
| Impostazioni | `RETRY` | 重试 | 再試行 | REPETIR | REPETIR | RÉESSAYER | RIPROVA |
| Impostazioni | `TRACK` | 每曲 | 曲ごと | PISTA | FAIXA | PISTE | BRANO |
| Impostazioni | `BOOT` | 保持 | 保持 | SIEMPRE | SEMPRE | TOUJOURS | SEMPRE |
| Registra da | `MONO` | 单声道 | モノラル | MONO | MONO | MONO | MONO |
| Registra da | `STEREO` | 立体声 | ステレオ | ESTÉREO | ESTÉREO | STÉRÉO | STEREO |
| Registra da | `FOCUSED` | 定向 | フォーカス | ENFOCADO | FOCADO | DIRIGÉ | DIRETTO |
| Registra da | `HEADSET` | 耳麦 | ヘッドセット | AURICULAR | HEADSET | CASQUE | CUFFIA |
| Registra da | `AUTO` | 自动 | 自動 | AUTO | AUTO | AUTO | AUTO |

## Opzioni di compilazione

Non sullo schermo — `idf.py -D<NOME>=1 build`. Ognuna resta in `CMakeCache.txt` finché non viene rimessa a 0, quindi compila una release da un albero pulito.

| Opzione | Fa |
|---|---|
| `HEAPCHECK` | Controlli di integrità dell'heap che nominano l'ultimo sottosistema prima di una corruzione. Lento. |
| `WAVEFORM` | La scansione della forma d'onda dell'intero file. |
| `O3CHECK` | Tutto `main` a -O3, per scovare avvisi. Un controllo, non un'impostazione da release. |
| `DECBENCH` | Registra nel log il costo di decodifica di minimp3 in cicli di CPU per frame. |
