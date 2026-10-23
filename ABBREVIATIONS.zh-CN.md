# 缩写与开关

[English](ABBREVIATIONS.md) · **简体中文** · [日本語](ABBREVIATIONS.ja.md) · [Español](ABBREVIATIONS.es.md) · [Português](ABBREVIATIONS.pt.md) · [Français](ABBREVIATIONS.fr.md) · [Italiano](ABBREVIATIONS.it.md)

屏幕上那些短词的含义，以及播放器的每一项设置，按其所在位置排列。面板宽 720 px，其中大多数挤在约五分之一宽的按钮里，所以它们是有意做短的。

> 本译文为机器翻译，正典为英文版 [ABBREVIATIONS.md](ABBREVIATIONS.md)。各表“屏幕显示”一列保留源码中的英文字面标签；在中文界面下，除保持英文的词外，屏幕会显示下文「其它语言」表中的译词。

内容取自源码（`browser.c`、`panel.c`、`sleeppage.c`、`ui.c`、`settings.h`、`playlist.h`）。若此处的标签与屏幕不一致，以屏幕为新——请修正本文件。

## 正在播放栏

| 屏幕显示 | 含义 |
|---|---|
| `LIVE` | 电台流。没有可定位的位置，于是定位条变成一分钟的电平历史。 |
| `REC` | 正在录音。定位条以分贝刻度（-60 到 0 dBFS）显示输入电平。 |
| `MUTED` | 正在录音，但输入已连续精确为数字零达 300 ms——静音的耳麦，或没有发送任何信号的音源。 |
| `RG` | 在扬声器图标下方：ReplayGain 正在调整这首曲子。音量滑块上的黄色标记即音量实际所在之处。 |
| `UAC` | 取代扬声器图标：一台 USB 音频类设备占用了输出。 |
| `FILE` | 在格式卡片上，用于没有扩展名的文件。否则卡片显示扩展名（`MP3`、`FLAC`……）。 |
| `45`/`33⅓` | 星形与 `Fx` 之间：唱机转速，播放转速／刻录转速，取 16⅔、33⅓、45、78。点按后每个数字有 + 和 −；点按框外关闭。两数相同为原速，只有边框；不同则为黄色，剩余时间也变黄，并按该转速计算实际剩余时间。持续多久见 AUDIO 标签页的「速度保持」。播放电台流时显示为灰色的 45/45，点按无效：直播流始终以原速播放。 |
| `Fx` | 转速与月亮之间：在整个方形区域上打开 Fx 面板。低音、高音或人声不为 1，回声不为 0，开启低切、高切，底噪、慢抖、快抖未关闭，或宽度不为 1 时显示黄色。点按面板外关闭。 |
| `低音` `高音` | Fx 面板上：低音和高音，各为 ½ 到 2 的增益——½、⅔、1、3/2、2。1 即原样。调高其中一个时，其余部分整体调低以留出余量，因此不会削波。 |
| `人声` | Fx 面板上：人声，即 Audacity 的人声消除——保留多少左右声道共有的成分（主唱通常在此），只作用于 150 Hz 到 8 kHz，因此贝斯和底鼓保留。0 去除（卡拉 OK），½ 后退，1 即原样，3/2 和 2 突出人声，并整体调低其余部分以留出余量。单声道曲目上显示为灰色且不起作用；仍可调整，留给下一首立体声曲目。 |
| `回声` `间隔` | Fx 面板上：Audacity 的回声，逐渐消失的重复。回声是每次重复相对上一次的音量：0 为关闭，然后是 ⅛ ¼ ⅓ ½ ⅔ ¾ ⅞——⅛ 是一次微弱的重复，⅞ 是长长的余音。余音越长，整体调得越低，因此重复不会削波（⅞ 约低 18 dB）。间隔是两次重复之间的时间：60、125、250、500 或 1000 毫秒。 |
| `低切` `高切` | Fx 面板上：Audacity 的高通和低通滤波器。低音、高音是倾斜，这两个是切除——过了截止频率，声音迅速衰减。低切去掉其下方：关、40、80、150 或 300 Hz（隆隆声）。高切去掉其上方：关、12k、8k、4k 或 3k（嘶声、刺耳）。两者合用，150 Hz 与 8k 是 AM 收音机，300 Hz 与 3k 是电话。 |
| `底噪` | Fx 面板上：把磨旧唱片的表面噪声叠加到曲目上——随机的噼啪声，下面一点嘶声。关，然后每秒 1、4、12 或 40 次：1 是干净的压片，40 是阁楼里翻出的 78 转唱片。大多很轻，少数不轻，各自在左右之间不同位置。曲目会稍微调低留出余量，所以噼啪声不会削波。它像唱片一样经过低音、高音和低切高切：配合低切、高切就是 AM 电台里的老唱片。 |
| `宽度` | Fx 面板上：立体声宽度——左右声道不共有的部分保留多少。0 为单声道，½ 变窄，1 保持原样，3/2 和 2 变宽。中央（人声、底鼓、贝斯）不变，单只音箱播放时始终是原曲；变宽会把声像推到音箱之外，并把其余部分稍微调低留出余量。单声道曲目上显示为灰色且不起作用；仍可调整，留给下一首立体声曲目。 |
| `慢抖` `快抖` | Fx 面板上：转得不太匀的唱机。慢抖（wow）每转一圈让音高摆动一次，按转速的 PLAYED：16⅔ 时慢，78 时快；电台直播时按 45。快抖（flutter）每转一圈摆动 18 次，像电机的抖动。数字是音高摆动的幅度（%）：慢抖 关、0.1、0.25、0.5、1；快抖 关、0.05、0.1、0.2、0.5。改动会在几秒内平滑过渡，不会跳变。 |

### 状态行，位于音量下方

是上报，不是开关——这里点按任何项都不会有动作。绿色为开，深灰为关（WIFI 还有黄色）。每一项各有其开关在别处。

| 屏幕显示 | 绿色表示 | 开关位于 |
|---|---|---|
| `USB` | USB-A 口已供电 | USB 标签页，USB 供电 |
| `WIFI` | 已连接且有地址。**黄色**：已开启但尚未连上——启动、扫描、连接中，或连接失败 | NET 标签页，Wi-Fi |
| `MPD` | MPD 服务器正在运行 | NET 标签页，MPD 服务器 |
| `HTTPS` | 浏览器远程控制正在运行 | NET 标签页，远程控制 |
| `SLEEP xxM` | 已设睡眠定时；向上取整的剩余分钟。未设置时仅显示 `SLEEP` | 睡眠页 |
| `IDLE` | 已设定关机并正在倒计时：无播放、无串流、无录音、无索引。**黄色**：已设定但被按住——正有事物在进行，于是等待不断重新开始。关机设为“从不”时为灰色。不显示分钟（6020） | 睡眠页，关机 |

同一行右端是电池（没有电池图标）：

| 屏幕显示 | 含义 | 颜色 |
|---|---|---|
| `BATT 73%` | 以电池供电 | 浅灰；20% 及以下为红色 |
| `CHRG 73%` | 有电流输入并在给电池充电 | 绿色 |
| `CHARGED` | 电池已充满 | 绿色 |
| `NO BATT` | 未装电池；以 USB-C 供电 | 深灰 |
| `BATT` | 电量计无读数 | 浅灰 |

## 选择器（文件夹按钮）

### 标签页

| 屏幕显示 | 含义 |
|---|---|
| `microSD` | microSD 卡。 |
| `USB` | 插在 USB-A 口的 U 盘。 |
| `RADIO` | 网络电台：`stations.m3u` 与收藏。 |
| `PODCAST` | 通过 Podcast Index 收听播客。需要你自己的 API 密钥，在遥控器页面上填写。 |

### 底部按钮

| 屏幕显示 | 含义 |
|---|---|
| `UP` | 上一级文件夹。 |
| `RLOD` | 在 RADIO 标签页顶层，位于 `UP` 之处：从卡重新加载电台列表。 |
| `FLDR` | 播放整个文件夹。 |
| `UP^` | 列表上翻一页。 |
| `DN` | 列表下翻一页。 |
| `X` | 关闭选择器。 |
| *(顺序)* | 播放顺序——点按循环。见下。 |

### 播放顺序

点按顺序按钮会 **ONE → ALL → RPT → EAT → RND → RPT1 →** 回到 ONE。

| 屏幕显示 | 含义 | MPD 等价 |
|---|---|---|
| `ONE` | 播放这一曲，然后停止。 | `single 1` |
| `ALL` | 继续下一曲；到文件夹末尾停止。 | （全部关闭） |
| `RPT` | `ALL`，到末尾后从头再来。 | `repeat 1` |
| `EAT` | `ALL`，每离开一曲就把它从列表中移除。 | `consume 1` |
| `RND` | 随机顺序，在每一曲都放过之前不重复。 | `random 1` |
| `RPT1` | 反复这一曲，直到另有指示。 | `repeat 1` + `single 1` |
| `ONE1`、`EAT1` | 只对一首生效的 `ONE` 或 `EAT`，因为某个 MPD 客户端请求了 `single oneshot` 或 `consume oneshot`。之后恢复到不带数字的名称。 | `oneshot` |

每种顺序恰好对应一个 MPD 组合。反过来看，MPD 的八种 random/repeat/single 组合中有三种在此没有精确对应的顺序；`mpdmode.h` 里有对照表以及各自会变成什么。

## 设置（齿轮按钮）

五个标签页：`SD`、`USB`、`BUILD`、`AUDIO`、`NET`。NET 是 network（网络）的简写。

### SD 与 USB

| 设置 | 取值 | 说明 |
|---|---|---|
| 重新索引 | `REINDEX` | 重建该存储卷的媒体索引。本卷进行时显示 `INDEXING`，另一卷进行时显示 `BUSY`——一次只跑一个。 |
| USB 供电（USB 标签页） | `ON` / `OFF` | 给 USB-A 口供电。从该盘播放曲目时显示 `IN USE` 并置灰。无设备连接时，播完一曲后播放器也会自行切断。 |

### BUILD

正在运行的是什么，以及屏幕语言。

| 设置 | 取值 | 说明 |
|---|---|---|
| 语言 | `English` / `简体中文` / `日本語` / `Español` / `Português` / `Français` / `Italiano` | 点按循环。每种语言以其自身文字命名，而该行的标签与标签页名称从不翻译，因此从任一语言都能找回。保存在设备中，不在卡上。目前仅部分界面已翻译，其余保持英文。 |

- `app`、`version`、`built`、`idf`，以及空闲的 `heap`、`psram` 和 `uptime`。
- **Source（源码）**：仓库，github.com/Sudrien/defeatist-music-player-for-m5tab5。
- **Libraries（库）**：本次构建中的每个组件和就地引入的库，附其版本（git 的则取其提交哈希前七位），`--` 之后是其许可。`licence not found` 表示该组件既无许可文件也无许可字段。该列表在构建配置时由 `dependencies.lock` 与就地引入的固定版本生成，因此就是实际编译的内容。

### AUDIO

| 设置 | 取值 | 说明 |
|---|---|---|
| ReplayGain | 开 / 关 | 让各曲电平彼此看齐。关闭只停止调整，不停止测量。 |
| 交叉淡化（Crossfade） | 关，或秒数 | 滑块。 |
| 同专辑 | 开 / 关 | 同一专辑的曲目之间也交叉淡化。关闭则不动专辑内的衔接。一个文件夹算作一张专辑。 |
| 速度保持 | `每曲` / `保持` | 唱机转速保留多久。`每曲`：新曲目开始时恢复原速；单曲重复（`RPT1`）不算新曲目。`保持`：在更改前一直保留，关机后也保留。`保持` 时点亮。 |
| 录音来源 | 见下 | 录音按钮录的是什么。 |

**录音来源**——点按循环。某项的输入存在可录时，该项点亮。

| 屏幕显示 | 录制 |
|---|---|
| `MONO` | 两只内置麦克风，合成单声道。 |
| `STEREO` | 两只内置麦克风，左与右。 |
| `FOCUSED` | 内置麦克风对，作为指向屏幕外的波束。单声道。 |
| `HEADSET` | 耳机插孔里的麦克风，单声道。只要插入任何东西就点亮——插孔分不清耳麦和耳机。 |
| `UAC` | 一只 USB 麦克风，按其自身的采样率与声道数。 |
| `AUTO` | 有 USB 麦就用 USB，否则用耳麦，再否则用 `MONO`。始终单声道。 |
| `OFF` | 不录音：传送带开关失去其录音档位。 |

所有录音均为 FLAC，存于 SD 卡的 `Recordings/`（无卡时存 U 盘）。

### NET

| 设置 | 取值 | 说明 |
|---|---|---|
| Wi-Fi | 开 / 关 | |
| 远程控制 | 开 / 关 | 基于 HTTPS 的浏览器远程。 |
| 已允许的设备 | `清除` | 在播放器屏幕上允许的浏览器和 MPD 应用地址。`清除` 会全部删除并关闭远程页面；每个都需重新请求。 |
| MPD 服务器 | 开 / 关 | 供 MPD 客户端与 Home Assistant 使用。 |
| 网络时间 | 开 / 关 | NTP，通过 Wi-Fi 或 USB 以太网线均可。 |
| 添加网络 | `START` / `STOP` | 打开强制门户，用手机加入一个 Wi-Fi 网络。 |
| 基准测试 | `RUN`（运行时为 `BUSY`） | 在不解码的情况下，一路电台流能被拉取得多快。 |
| 时钟 | `RESET` | 遗忘时钟。仅在时间尚未被核实时可用。 |

## 睡眠页（月亮按钮）

| 设置 | 取值 |
|---|---|
| 屏幕 | 亮度，百分比。 |
| 旋转 | `0`、`90`、`180`、`270`。 |
| 屏幕变暗 | 从不，或在无触摸 15 秒到 2 分钟后——降到半亮。 |
| 关闭屏幕 | 从不，或 30 秒到 5 分钟。本页打开时从不。 |
| 睡眠定时 | 关，或 15 分钟到 2 小时，15 分钟一档。最后 20 秒声音淡出。 |
| 关机 | 从不，或在无播放、无录音、无触摸经过 15 分钟、30 分钟、1 小时或 2 小时后。设备自行关机；侧键将其开机。与睡眠定时相互独立，且音乐播放时从不。 |

## 其它语言（6019）

以上标签的普通话、日语、西班牙语、葡萄牙语（巴西）、法语与意大利语版本，在 BUILD 标签页选择。每一项都翻译标签所代表的完整词语，并在按钮比词更窄时重新缩短：选择器按钮为 112 px（8 个拉丁字符或 4 个中日韩字符），设置胶囊为 124 px（5 个或 3 个）。播放顺序词在 Cantata 有对应 MPD 词汇处沿用之——重复/随机/单曲 与 播完删除（取自 播放后删除）、リピート/ランダム、Repetir——而 ON/OFF 沿用自 6013 起 Audio 标签页一直使用的 开/关。

在每种语言中都由 `same()` 保持英文：标签页名称（SD、USB、BUILD、AUDIO、NET、RADIO）、USB、WIFI、MPD、HTTPS、UAC、RG，以及各单位。

| 位置 | English | 简体中文 | 日本語 | Español | Português | Français | Italiano |
|---|---|---|---|---|---|---|---|
| 状态行 | `SLEEP` | 睡眠 | 睡眠 | SUEÑO | SONO | VEILLE | SONNO |
| 状态行 | `SLEEP %dM` | 睡眠%d分 | 睡眠%d分 | SUEÑO %dM | SONO %dM | VEILLE %dM | SONNO %dM |
| 状态行 | `IDLE` | 空闲 | 待機 | INAC | OCIO | INACT | INATT |
| 状态行 | `BATT` | 电池 | 電池 | BAT | BAT | BATT | BATT |
| 状态行 | `BATT %d%%` | 电池 %d%% | 電池 %d%% | BAT %d%% | BAT %d%% | BATT %d%% | BATT %d%% |
| 状态行 | `CHRG %d%%` | 充电 %d%% | 充電 %d%% | CARG %d%% | CARR %d%% | CHRG %d%% | CARIC %d%% |
| 状态行 | `CHARGED` | 已充满 | 満充電 | CARGADA | CARREGADA | CHARGÉE | CARICA |
| 状态行 | `NO BATT` | 无电池 | 電池なし | SIN BAT | SEM BAT | SANS BATT | NO BATT |
| 正在播放 | `LIVE` | 直播 | ライブ | VIVO | AO VIVO | DIRECT | LIVE |
| 正在播放 | `REC` | 录音 | 録音 | GRAB | GRAV | ENR | REG |
| 正在播放 | `MUTED` | 静音 | 無音 | MUDO | MUDO | MUET | MUTO |
| 选择器底栏 | `UP` | 上级 | 上へ | SUBIR | SUBIR | MONTER | SU |
| 选择器底栏 | `RLOD` | 重新加载 | 再読込 | RECARGAR | RECARR. | RECHARG. | RICARICA |
| 选择器底栏 | `FLDR` | 播放目录 | フォルダ | CARPETA | PASTA | DOSSIER | CARTELLA |
| 选择器底栏 | `UP^` | 上页 | 前頁 | RE PÁG | PG CIMA | PG HAUT | PAG SU |
| 选择器底栏 | `DN` | 下页 | 次頁 | AV PÁG | PG BAIXO | PG BAS | PAG GIÙ |
| 播放顺序 | `ONE` | 单曲 | 単曲 | ÚNICA | UMA | UNE | UNO |
| 播放顺序 | `ALL` | 顺序 | 全曲 | TODAS | TODAS | TOUT | TUTTI |
| 播放顺序 | `RPT` | 重复 | リピート | REPETIR | REPETIR | RÉPÉTER | RIPETI |
| 播放顺序 | `EAT` | 播完删除 | 消費 | BORRAR | COMER | MANGER | MANGIA |
| 播放顺序 | `RND` | 随机 | ランダム | AZAR | ALEAT | ALÉA | CASUALE |
| 播放顺序 | `RPT1` | 单曲重复 | 1曲反復 | REPITE 1 | REPETE 1 | RÉPÉTER1 | RIPETI1 |
| 播放顺序 | `ONE1` | 单曲1 | 単曲1 | ÚNICA1 | UMA1 | UNE1 | UNO1 |
| 播放顺序 | `EAT1` | 删除1 | 消費1 | BORRAR1 | COMER1 | MANGER1 | MANGIA1 |
| 设置 | `ON` | 开 | オン | SÍ | SIM | OUI | SÌ |
| 设置 | `OFF` | 关 | オフ | NO | NÃO | NON | NO |
| 设置 | `IN USE` | 使用中 | 使用中 | EN USO | EM USO | UTILISÉ | IN USO |
| 设置 | `REINDEX` | 重新索引 | 再索引 | REINDEXAR | REINDEXAR | RÉINDEXER | REINDICIZZA |
| 设置 | `INDEXING` | 索引中 | 索引中 | INDEXANDO | INDEXANDO | INDEXATION | INDICIZZA |
| 设置 | `BUSY` | 进行中 | 作業中 | OCUP. | OCUP. | OCCUPÉ | OCCUP. |
| 设置 | `START` | 开始 | 開始 | ABRIR | ABRIR | OUVRIR | APRI |
| 设置 | `STOP` | 停止 | 停止 | PARAR | PARAR | ARRÊT | STOP |
| 设置 | `RUN` | 运行 | 実行 | MEDIR | MEDIR | LANCER | MISURA |
| 设置 | `RESET` | 重置 | 戻す | VOLVER | VOLTAR | RÉTABLIR | AZZERA |
| 设置 | `CLOSE` | 关闭 | 閉じる | CERRAR | FECHAR | FERMER | CHIUDI |
| 设置 | `IMPORT` | 导入 | 取込 | IMPORTAR | IMPORTAR | IMPORTER | IMPORTA |
| 设置 | `DONE` | 完成 | 完了 | HECHO | OK | OK | FATTO |
| 设置 | `RETRY` | 重试 | 再試行 | REPETIR | REPETIR | RÉESSAYER | RIPROVA |
| 设置 | `TRACK` | 每曲 | 曲ごと | PISTA | FAIXA | PISTE | BRANO |
| 设置 | `BOOT` | 保持 | 保持 | SIEMPRE | SEMPRE | TOUJOURS | SEMPRE |
| 录音来源 | `MONO` | 单声道 | モノラル | MONO | MONO | MONO | MONO |
| 录音来源 | `STEREO` | 立体声 | ステレオ | ESTÉREO | ESTÉREO | STÉRÉO | STEREO |
| 录音来源 | `FOCUSED` | 定向 | フォーカス | ENFOCADO | FOCADO | DIRIGÉ | DIRETTO |
| 录音来源 | `HEADSET` | 耳麦 | ヘッドセット | AURICULAR | HEADSET | CASQUE | CUFFIA |
| 录音来源 | `AUTO` | 自动 | 自動 | AUTO | AUTO | AUTO | AUTO |

## 构建开关

不在屏幕上——`idf.py -D<名称>=1 build`。每一项都会留在 `CMakeCache.txt` 中直到设回 0，因此发布版请从干净的树构建。

| 开关 | 作用 |
|---|---|
| `HEAPCHECK` | 堆完整性检查点，在发生损坏前点出最后运行的子系统。较慢。 |
| `WAVEFORM` | 全文件波形扫描。 |
| `O3CHECK` | 以 -O3 编译整个 `main`，用于抓警告。是检查手段，不是发布设置。 |
| `DECBENCH` | 记录 minimp3 每帧解码耗费的 CPU 周期。 |
