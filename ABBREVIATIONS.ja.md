# 略語とトグル

[English](ABBREVIATIONS.md) · [简体中文](ABBREVIATIONS.zh-CN.md) · **日本語** · [Español](ABBREVIATIONS.es.md) · [Português](ABBREVIATIONS.pt.md) · [Français](ABBREVIATIONS.fr.md) · [Italiano](ABBREVIATIONS.it.md)

画面上の短い語の意味と、プレーヤーが持つすべての設定を、置かれている場所ごとにまとめたもの。パネルは幅 720 px で、その多くは 5 分の 1 ほどの幅のボタンに収まるため、意図的に短くしてある。

> これは機械翻訳です。正典は英語版 [ABBREVIATIONS.md](ABBREVIATIONS.md)。各表の「画面表示」列は、ソース中の英語リテラルのラベルをそのまま載せています。日本語表示では、英語のままにする語を除き、下記「他の言語」表の訳語が画面に出ます。

ソース（`browser.c`、`panel.c`、`sleeppage.c`、`ui.c`、`settings.h`、`playlist.h`）から取った。ここのラベルと画面が食い違う場合、新しいのは画面——このファイルを直すこと。

## 再生中バー

| 画面表示 | 意味 |
|---|---|
| `LIVE` | ラジオストリーム。シークする位置がないので、シークバーは代わりに 1 分間のレベル履歴になる。 |
| `REC` | 録音中。バーは入力レベルをデシベル目盛り（-60〜0 dBFS）で表示する。 |
| `MUTED` | 録音中だが、入力が 300 ms のあいだ厳密にデジタルゼロのまま——ミュートされたヘッドセット、または何も送っていない音源。 |
| `RG` | スピーカーの下：ReplayGain がこの曲を調整中。音量スライダー上の黄色い印が、音量が実質的にある位置。 |
| `UAC` | スピーカーアイコンの代わり：USB オーディオクラス機器が出力を握っている。 |
| `FILE` | フォーマットカード上、拡張子のないファイル向け。それ以外のときはカードに拡張子（`MP3`、`FLAC`…）が出る。 |
| `45`/`33⅓` | 星と月のあいだ：ターンテーブル速度。再生速度／録音速度で、16⅔・33⅓・45・78 から選ぶ。タップすると各数字に + と − が出る。枠の外をタップで閉じる。同じ数字なら通常速度で枠線のみ、違えば黄色。残り時間も黄色になり、その速度での実時間を数える。持続は AUDIO タブの「速度の持続」。ラジオ配信中は 45/45 の灰色で、タップしても何もしない（ライブ配信は常に通常速度）。 |

### ステータス行、音量の下

スイッチではなく表示——ここをタップしても何も起きない。緑がオン、濃いグレーがオフ（WIFI には黄もある）。各項目のスイッチはそれぞれ別の場所にある。

| 画面表示 | 緑になる条件 | スイッチの場所 |
|---|---|---|
| `USB` | USB-A ポートに給電されている | USB タブ、USB 給電 |
| `WIFI` | 接続済みでアドレスを持つ。**黄**：オンだがまだ未接続——起動・スキャン・接続・接続失敗のいずれか | NET タブ、Wi-Fi |
| `MPD` | MPD サーバーが動作中 | NET タブ、MPD サーバー |
| `HTTPS` | ブラウザリモートが動作中 | NET タブ、リモート制御 |
| `SLEEP xxM` | スリープタイマー設定中。切り上げた残り分。未設定時は `SLEEP` のみ | スリープページ |
| `IDLE` | 電源オフが設定され、カウントダウン中：再生・ストリーム・録音・索引のいずれもなし。**黄**：設定済みだが保留——何かが進行中で、待ち時間が繰り返しリセットされる。電源オフが「しない」のときグレー。分表示なし（6020） | スリープページ、電源オフ |

同じ行の右端はバッテリー（バッテリーアイコンはない）：

| 画面表示 | 意味 | 色 |
|---|---|---|
| `BATT 73%` | バッテリー駆動 | 薄いグレー。20% 以下で赤 |
| `CHRG 73%` | 電力が入りバッテリーを充電中 | 緑 |
| `CHARGED` | バッテリー満充電 | 緑 |
| `NO BATT` | バッテリー未装着。USB-C から駆動 | 濃いグレー |
| `BATT` | 残量計が読み取れない | 薄いグレー |

## チューザー（フォルダボタン）

### タブ

| 画面表示 | 意味 |
|---|---|
| `microSD` | microSD カード。 |
| `USB` | USB-A ポートの USB ドライブ。 |
| `RADIO` | インターネットラジオ：`stations.m3u` とお気に入り。 |

### フッターボタン

| 画面表示 | 意味 |
|---|---|
| `UP` | 一つ上のフォルダへ。 |
| `RLOD` | RADIO タブの最上層で、`UP` の位置に：カードから局リストを再読み込み。 |
| `FLDR` | このフォルダ全体を再生。 |
| `UP^` | リストを 1 ページ上へ。 |
| `DN` | リストを 1 ページ下へ。 |
| `X` | チューザーを閉じる。 |
| *(順序)* | 再生順——タップで切替。下記参照。 |

### 再生順

順序ボタンをタップすると **ONE → ALL → RPT → EAT → RND → RPT1 →** ONE に戻る。

| 画面表示 | 意味 | MPD 相当 |
|---|---|---|
| `ONE` | この曲を再生して停止。 | `single 1` |
| `ALL` | 次の曲へ。フォルダの終わりで停止。 | （すべてオフ） |
| `RPT` | `ALL`、終わったら先頭からやり直す。 | `repeat 1` |
| `EAT` | `ALL`、曲を離れるたびにリストから取り除く。 | `consume 1` |
| `RND` | ランダム順。全曲を再生し終えるまで繰り返さない。 | `random 1` |
| `RPT1` | 指示があるまでこの曲を繰り返す。 | `repeat 1` + `single 1` |
| `ONE1`、`EAT1` | 1 曲だけの `ONE` または `EAT`。MPD クライアントが `single oneshot` または `consume oneshot` を要求したため。その後は数字なしの名前に戻る。 | `oneshot` |

各順序はちょうど一つの MPD 組み合わせに対応する。逆方向では、MPD の 8 通りの random/repeat/single 組み合わせのうち 3 つはここに正確な順序がない。`mpdmode.h` に対応表と、それぞれが何になるかがある。

## 設定（歯車ボタン）

5 つのタブ：`SD`、`USB`、`BUILD`、`AUDIO`、`NET`。NET は network（ネットワーク）の略。

### SD と USB

| 設定 | 値 | 備考 |
|---|---|---|
| 再索引 | `REINDEX` | そのボリュームのメディア索引を作り直す。当該ボリュームの処理中は `INDEXING`、別のボリュームの処理中は `BUSY`——一度に一つだけ。 |
| USB 給電（USB タブ） | `ON` / `OFF` | USB-A ポートへの給電。そのドライブから曲を再生中は `IN USE` でグレーアウト。何も接続されていないとき、プレーヤーは 1 曲後に自ら切る。 |

### BUILD

何が動いているか、そして画面の言語。

| 設定 | 値 | 備考 |
|---|---|---|
| 言語 | `English` / `简体中文` / `日本語` / `Español` / `Português` / `Français` / `Italiano` | タップで切替。各言語はそれ自身の文字で表記され、この行のラベルとタブ名は決して翻訳されないので、どの言語からでも戻り道が見つかる。カードではなく端末に保存。今のところ一部の画面のみ翻訳済みで、残りは英語のまま。 |

- `app`、`version`、`built`、`idf`、および空き `heap`、`psram`、`uptime`。
- **Source（ソース）**：リポジトリ、github.com/Sudrien/defeatist-music-player-for-m5tab5。
- **Libraries（ライブラリ）**：このビルドに含まれる各コンポーネントと同梱ライブラリ。バージョン（git なら commit の先頭 7 文字）、そして `--` の後にライセンス。`licence not found` は、コンポーネントがライセンスファイルもライセンスフィールドも同梱していないこと。ビルド構成時に `dependencies.lock` と同梱ピンから生成されるので、実際にコンパイルされた内容そのもの。

### AUDIO

| 設定 | 値 | 備考 |
|---|---|---|
| ReplayGain | オン / オフ | 各曲のレベルを互いに合わせる。オフは調整を止めるだけで、測定は止めない。 |
| クロスフェード | オフ、または秒数 | スライダー。 |
| 同一アルバム | オン / オフ | 同じアルバムの曲同士でもクロスフェードする。オフならアルバム内のつなぎには手を付けない。1 フォルダを 1 アルバムと見なす。 |
| 速度の持続 | `曲ごと` / `保持` | ターンテーブル速度をいつまで保つか。`曲ごと`：新しい曲が始まると元に戻る。1 曲リピート（`RPT1`）は新しい曲ではない。`保持`：リセットするまで、電源を切っても保持。`保持` で点灯。 |
| 録音ソース | 下記参照 | 録音ボタンが何を録るか。 |

**録音ソース**——タップで切替。入力が録れる状態にあるとき、その選択肢が点灯する。

| 画面表示 | 録るもの |
|---|---|
| `MONO` | 内蔵マイク 2 本をモノラルに合算。 |
| `STEREO` | 内蔵マイク 2 本、左と右。 |
| `FOCUSED` | 内蔵ペアを画面の外へ向けたビームとして。モノラル。 |
| `HEADSET` | ヘッドホン端子のマイク、モノラル。何かが挿さっていれば点灯——端子はヘッドセットとヘッドホンを区別できない。 |
| `UAC` | USB マイク、その固有のレートとチャンネル数で。 |
| `AUTO` | USB マイクがあれば USB、なければヘッドセット、それもなければ `MONO`。常にモノラル。 |
| `OFF` | 録音なし：トランスポートスイッチは録音位置を失う。 |

録音はすべて FLAC で、SD カードの `Recordings/`（カードがなければ USB ドライブ）に保存される。

### NET

| 設定 | 値 | 備考 |
|---|---|---|
| Wi-Fi | オン / オフ | |
| リモート制御 | オン / オフ | HTTPS 経由のブラウザリモート。 |
| MPD サーバー | オン / オフ | MPD クライアントと Home Assistant 向け。 |
| ネットワーク時刻 | オン / オフ | NTP。Wi-Fi がオフの間はグレーアウト。設定は保持される。 |
| ネットワークを追加 | `START` / `STOP` | キャプティブポータルを開き、スマホから Wi-Fi に参加する。 |
| ベンチマーク | `RUN`（実行中は `BUSY`） | 何もデコードせずに局のストリームをどれだけ速く引けるか。 |
| 時計 | `RESET` | 時計を忘れる。時刻がまだ検証されていない間のみ利用可。 |

## スリープページ（月ボタン）

| 設定 | 値 |
|---|---|
| 画面 | 明るさ、パーセント。 |
| 回転 | `0`、`90`、`180`、`270`。 |
| 画面を暗く | しない、または無タッチ 15 秒〜2 分後——半分の明るさへ。 |
| 画面オフ | しない、または 30 秒〜5 分。このページを開いている間はしない。 |
| スリープタイマー | オフ、または 15 分〜2 時間、15 分刻み。最後の 20 秒で音がフェードアウト。 |
| 電源オフ | しない、または再生・録音・操作のいずれもなく 15 分・30 分・1 時間・2 時間後。端末は自ら電源を切る。サイドボタンで入れる。スリープタイマーとは別物で、音楽の再生中は決して行わない。 |

## 他の言語（6019）

上記ラベルの北京語・日本語・スペイン語・ポルトガル語（ブラジル）・フランス語・イタリア語版。BUILD タブで選ぶ。各項目はラベルが表す語全体を訳し、ボタンが語より狭いところで再度短縮する：チューザーボタンは 112 px（ラテン 8 字または CJK 4 字）、設定ピルは 124 px（5 字または 3 字）。再生順の語は、Cantata が MPD 語彙を持つところではそれに倣う——重复/随机/单曲 と 播完删除（播放后删除 から）、リピート/ランダム、Repetir——そして ON/OFF は 6013 以来 Audio タブが使ってきた 开/关 に倣う。

どの言語でも `same()` によって英語のまま保たれる：タブ名（SD、USB、BUILD、AUDIO、NET、RADIO）、USB、WIFI、MPD、HTTPS、UAC、RG、そして各単位。

| 場所 | English | 简体中文 | 日本語 | Español | Português | Français | Italiano |
|---|---|---|---|---|---|---|---|
| ステータス行 | `SLEEP` | 睡眠 | 睡眠 | SUEÑO | SONO | VEILLE | SONNO |
| ステータス行 | `SLEEP %dM` | 睡眠%d分 | 睡眠%d分 | SUEÑO %dM | SONO %dM | VEILLE %dM | SONNO %dM |
| ステータス行 | `IDLE` | 空闲 | 待機 | INAC | OCIO | INACT | INATT |
| ステータス行 | `BATT` | 电池 | 電池 | BAT | BAT | BATT | BATT |
| ステータス行 | `BATT %d%%` | 电池 %d%% | 電池 %d%% | BAT %d%% | BAT %d%% | BATT %d%% | BATT %d%% |
| ステータス行 | `CHRG %d%%` | 充电 %d%% | 充電 %d%% | CARG %d%% | CARR %d%% | CHRG %d%% | CARIC %d%% |
| ステータス行 | `CHARGED` | 已充满 | 満充電 | CARGADA | CARREGADA | CHARGÉE | CARICA |
| ステータス行 | `NO BATT` | 无电池 | 電池なし | SIN BAT | SEM BAT | SANS BATT | NO BATT |
| 再生中 | `LIVE` | 直播 | ライブ | VIVO | AO VIVO | DIRECT | LIVE |
| 再生中 | `REC` | 录音 | 録音 | GRAB | GRAV | ENR | REG |
| 再生中 | `MUTED` | 静音 | 無音 | MUDO | MUDO | MUET | MUTO |
| チューザー下部 | `UP` | 上级 | 上へ | SUBIR | SUBIR | MONTER | SU |
| チューザー下部 | `RLOD` | 重新加载 | 再読込 | RECARGAR | RECARR. | RECHARG. | RICARICA |
| チューザー下部 | `FLDR` | 播放目录 | フォルダ | CARPETA | PASTA | DOSSIER | CARTELLA |
| チューザー下部 | `UP^` | 上页 | 前頁 | RE PÁG | PG CIMA | PG HAUT | PAG SU |
| チューザー下部 | `DN` | 下页 | 次頁 | AV PÁG | PG BAIXO | PG BAS | PAG GIÙ |
| 再生順 | `ONE` | 单曲 | 単曲 | ÚNICA | UMA | UNE | UNO |
| 再生順 | `ALL` | 顺序 | 全曲 | TODAS | TODAS | TOUT | TUTTI |
| 再生順 | `RPT` | 重复 | リピート | REPETIR | REPETIR | RÉPÉTER | RIPETI |
| 再生順 | `EAT` | 播完删除 | 消費 | BORRAR | COMER | MANGER | MANGIA |
| 再生順 | `RND` | 随机 | ランダム | AZAR | ALEAT | ALÉA | CASUALE |
| 再生順 | `RPT1` | 单曲重复 | 1曲反復 | REPITE 1 | REPETE 1 | RÉPÉTER1 | RIPETI1 |
| 再生順 | `ONE1` | 单曲1 | 単曲1 | ÚNICA1 | UMA1 | UNE1 | UNO1 |
| 再生順 | `EAT1` | 删除1 | 消費1 | BORRAR1 | COMER1 | MANGER1 | MANGIA1 |
| 設定 | `ON` | 开 | オン | SÍ | SIM | OUI | SÌ |
| 設定 | `OFF` | 关 | オフ | NO | NÃO | NON | NO |
| 設定 | `IN USE` | 使用中 | 使用中 | EN USO | EM USO | UTILISÉ | IN USO |
| 設定 | `REINDEX` | 重新索引 | 再索引 | REINDEXAR | REINDEXAR | RÉINDEXER | REINDICIZZA |
| 設定 | `INDEXING` | 索引中 | 索引中 | INDEXANDO | INDEXANDO | INDEXATION | INDICIZZA |
| 設定 | `BUSY` | 进行中 | 作業中 | OCUP. | OCUP. | OCCUPÉ | OCCUP. |
| 設定 | `START` | 开始 | 開始 | ABRIR | ABRIR | OUVRIR | APRI |
| 設定 | `STOP` | 停止 | 停止 | PARAR | PARAR | ARRÊT | STOP |
| 設定 | `RUN` | 运行 | 実行 | MEDIR | MEDIR | LANCER | MISURA |
| 設定 | `RESET` | 重置 | 戻す | VOLVER | VOLTAR | RÉTABLIR | AZZERA |
| 設定 | `CLOSE` | 关闭 | 閉じる | CERRAR | FECHAR | FERMER | CHIUDI |
| 設定 | `IMPORT` | 导入 | 取込 | IMPORTAR | IMPORTAR | IMPORTER | IMPORTA |
| 設定 | `DONE` | 完成 | 完了 | HECHO | OK | OK | FATTO |
| 設定 | `RETRY` | 重试 | 再試行 | REPETIR | REPETIR | RÉESSAYER | RIPROVA |
| 設定 | `TRACK` | 每曲 | 曲ごと | PISTA | FAIXA | PISTE | BRANO |
| 設定 | `BOOT` | 保持 | 保持 | SIEMPRE | SEMPRE | TOUJOURS | SEMPRE |
| 録音ソース | `MONO` | 单声道 | モノラル | MONO | MONO | MONO | MONO |
| 録音ソース | `STEREO` | 立体声 | ステレオ | ESTÉREO | ESTÉREO | STÉRÉO | STEREO |
| 録音ソース | `FOCUSED` | 定向 | フォーカス | ENFOCADO | FOCADO | DIRIGÉ | DIRETTO |
| 録音ソース | `HEADSET` | 耳麦 | ヘッドセット | AURICULAR | HEADSET | CASQUE | CUFFIA |
| 録音ソース | `AUTO` | 自动 | 自動 | AUTO | AUTO | AUTO | AUTO |

## ビルドスイッチ

画面上にはない——`idf.py -D<名前>=1 build`。各スイッチは 0 に戻すまで `CMakeCache.txt` に残るので、リリースはクリーンなツリーからビルドすること。

| スイッチ | 働き |
|---|---|
| `HEAPCHECK` | 破損の直前に最後に動いたサブシステムを指すヒープ整合性チェックポイント。遅い。 |
| `WAVEFORM` | 全ファイルの波形スキャン。 |
| `O3CHECK` | `main` 全体を -O3 でビルドし、警告を拾う。チェック用であり、リリース設定ではない。 |
| `DECBENCH` | minimp3 のフレームあたりデコードコストを CPU サイクルで記録。 |
