# Abreviações e ajustes

[English](ABBREVIATIONS.md) · [简体中文](ABBREVIATIONS.zh-CN.md) · [日本語](ABBREVIATIONS.ja.md) · [Español](ABBREVIATIONS.es.md) · **Português** · [Français](ABBREVIATIONS.fr.md) · [Italiano](ABBREVIATIONS.it.md)

O que significam as palavras curtas da tela, e todos os ajustes do player, organizados pelo lugar onde ficam. A tela tem 720 px de largura e a maioria cabe num botão de um quinto disso, por isso são curtas de propósito.

> Esta é uma tradução automática, em português do Brasil; a versão de referência é a inglesa, [ABBREVIATIONS.md](ABBREVIATIONS.md). A coluna "Na tela" mantém as etiquetas literais em inglês do código-fonte; numa interface em português, exceto as palavras mantidas em inglês, a tela mostra os termos traduzidos da tabela "Em outros idiomas" mais abaixo.

Tirado do código-fonte (`browser.c`, `panel.c`, `sleeppage.c`, `ui.c`, `settings.h`, `playlist.h`). Se uma etiqueta daqui e a tela discordarem, a tela é a mais nova — corrija este arquivo.

## A barra de reprodução

| Na tela | Significa |
|---|---|
| `LIVE` | Um stream de rádio. Não há posição para onde ir, então a barra de progresso vira um minuto de histórico de nível. |
| `REC` | Gravando. A barra mostra o nível da entrada, numa escala de decibéis (-60 a 0 dBFS). |
| `MUTED` | Gravando, mas a entrada está em zero digital exato há 300 ms — um headset no mudo, ou uma fonte que não manda nada. |
| `RG` | Embaixo do alto-falante: o ReplayGain está ajustando esta faixa. A marca amarela no controle de volume é onde o volume fica de fato. |
| `UAC` | No lugar do ícone do alto-falante: um dispositivo USB Audio Class tem a saída. |
| `FILE` | No cartão de formato, para um arquivo sem extensão. Senão o cartão mostra a extensão (`MP3`, `FLAC`…). |
| `45`/`33⅓` | Entre a estrela e `Fx`: velocidade de toca-discos, a de REPRODUÇÃO sobre a de GRAVAÇÃO, entre 16⅔, 33⅓, 45 e 78. Toque para ter + e − em cada número; toque fora da caixa para fechar. Números iguais são a velocidade normal, com contorno; qualquer outra coisa fica amarela, assim como o relógio de tempo restante, que então conta o tempo real que falta nessa velocidade. Quanto tempo dura: "A velocidade dura" na aba AUDIO. Num stream de rádio fica cinza em 45/45 e não faz nada: uma transmissão ao vivo sempre toca na velocidade normal. |
| `Fx` | Entre a velocidade e a lua: abre a folha Fx sobre todo o quadrado. Amarelo quando GRAV, AGUD ou VOZ não está em 1, ECO não está em 0, CGRAV ou CAGUD estão ligados, PÓ, WOW ou FLUT não estão desligados, ou LARG não está em 1. Toque fora da folha para fechar. |
| `GRAV` `AGUD` | Na folha Fx: graves e agudos, cada um um ganho de ½ a 2 -- ½, ⅔, 1, 3/2, 2. 1 é a faixa como era. Subir um baixa todo o resto para dar espaço, então nada satura. |
| `VOZ` | Na folha Fx: a voz, como a Redução vocal do Audacity -- quanto se mantém do que esquerda e direita têm em comum (onde costuma estar a voz principal), entre 150 Hz e 8 kHz para que o baixo e o bumbo fiquem. 0 a remove (karaokê), ½ a recua, 1 é a faixa como era, 3/2 e 2 a trazem à frente e baixam o resto para dar espaço. Numa faixa mono fica em cinza e não faz nada; ainda dá para ajustar, para a próxima faixa estéreo. |
| `ECO` `TEMPO` | Na folha Fx: o Eco do Audacity, repetições que somem. ECO é o volume de cada repetição em relação à anterior: 0 é desligado, depois ⅛ ¼ ⅓ ½ ⅔ ¾ ⅞ -- ⅛ é uma repetição fraca, ⅞ um rastro longo. Quanto mais longo o rastro, mais tudo é abaixado para que as repetições nunca saturem (⅞ fica cerca de 18 dB mais baixo). TEMPO é o intervalo entre repetições: 60, 125, 250, 500 ou 1000 ms. |
| `CGRAV` `CAGUD` | Na folha Fx: os filtros passa-alta e passa-baixa do Audacity. Onde GRAV e AGUD inclinam, estes cortam: passado o ponto de corte o som cai depressa. CGRAV remove o que fica abaixo: não, 40, 80, 150 ou 300 Hz (ronco). CAGUD remove o que fica acima: não, 12k, 8k, 4k ou 3k (chiado, aspereza). Juntos, 150 Hz e 8k é um rádio AM, 300 Hz e 3k um telefone. |
| `PÓ` | Na folha Fx: a superfície de um disco gasto sobre a faixa -- estalos ao acaso e um pouco de chiado por baixo. Desligado, depois 1, 4, 12 ou 40 estalos por segundo: 1 uma prensagem limpa, 40 um 78 rotações achado no sótão. Quase todos são fracos e poucos não, cada um no seu lugar entre as caixas. A faixa baixa um pouco para abrir espaço, então um estalo nunca satura. Passa por GRAV, AGUD e os cortes como o disco passaria: com CGRAV e CAGUD é um disco velho no rádio AM. |
| `LARG` | Na folha Fx: a largura estéreo -- quanto fica do que esquerda e direita não têm em comum. 0 é mono, ½ mais estreito, 1 a faixa como era, 3/2 e 2 mais largo. O centro (voz, bumbo, baixo) nunca se move, e numa só caixa é sempre a faixa; mais largo empurra o que está panorâmico para além das caixas, e baixa um pouco o resto para abrir espaço. Numa faixa mono fica em cinza e não faz nada; ainda dá para ajustar, para a próxima faixa estéreo. |
| `WOW` `FLUT` | Na folha Fx: um toca-discos que não gira bem por igual. WOW ondula a afinação uma vez por volta, na velocidade PLAYED: mais lento a 16⅔, mais rápido a 78; numa rádio, a 45. FLUT (flutter) ondula 18 vezes por volta, o de um motor. Os números são quanto a afinação se desvia, em por cento: WOW desligado, 0.1, 0.25, 0.5, 1; FLUT desligado, 0.05, 0.1, 0.2, 0.5. Uma mudança entra suave em alguns segundos, sem saltos. |

### A linha de status, abaixo do volume

Indicadores, não chaves — nada aqui faz algo ao ser tocado. Verde é ligado, cinza-escuro é desligado (WIFI também tem amarelo). Cada um tem sua própria chave em outro lugar.

| Na tela | Verde quando | Muda em |
|---|---|---|
| `USB` | A porta USB-A está alimentada | aba USB, alimentação USB |
| `WIFI` | Conectado, com endereço. **Amarelo**: ligado mas ainda não conectado — iniciando, buscando, entrando ou falhando | aba NET, Wi-Fi |
| `MPD` | O servidor MPD está rodando | aba NET, Servidor MPD |
| `HTTPS` | O controle remoto pelo navegador está rodando | aba NET, Controle remoto |
| `SLEEP xxM` | O timer está ativo; minutos restantes, arredondados para cima. Só `SLEEP` quando não está | página Repouso |
| `IDLE` | O desligamento está ativo e contando: nada tocando, transmitindo, gravando nem indexando. **Amarelo**: ativo, mas segurado — algo está acontecendo, então a espera recomeça. Cinza quando Desligar está em Nunca. Sem minutos (6020) | página Repouso, Desligar |

Na ponta direita da mesma linha, a bateria (não há ícone de bateria):

| Na tela | Significa | Cor |
|---|---|---|
| `BATT 73%` | Funcionando na bateria | Cinza-claro; vermelho a 20% ou menos |
| `CHRG 73%` | Está entrando energia e carregando a bateria | Verde |
| `CHARGED` | A bateria está cheia | Verde |
| `NO BATT` | Sem bateria colocada; funcionando pelo USB-C | Cinza-escuro |
| `BATT` | O medidor da bateria não dá leitura | Cinza-claro |

## O seletor (botão de pasta)

### Abas

| Na tela | Significa |
|---|---|
| `microSD` | O cartão microSD. |
| `USB` | Um pendrive na porta USB-A. |
| `RADIO` | Rádio pela internet: `stations.m3u` e favoritos. |

### Botões de baixo

| Na tela | Significa |
|---|---|
| `UP` | Sobe uma pasta. |
| `RLOD` | No nível de cima da aba RADIO, no lugar de `UP`: recarrega a lista de rádios do cartão. |
| `FLDR` | Toca esta pasta inteira. |
| `UP^` | Sobe a lista uma página. |
| `DN` | Desce a lista uma página. |
| `X` | Fecha o seletor. |
| *(ordem)* | A ordem de reprodução — toque para alternar. Veja abaixo. |

### Ordem de reprodução

Tocar no botão de ordem passa por **ONE → ALL → RPT → EAT → RND → RPT1 →** e volta a ONE.

| Na tela | Significa | Equivalente MPD |
|---|---|---|
| `ONE` | Toca esta faixa e para. | `single 1` |
| `ALL` | Segue para a próxima faixa; para no fim da pasta. | (tudo desligado) |
| `RPT` | `ALL`, recomeçando do início no fim. | `repeat 1` |
| `EAT` | `ALL`, tirando cada faixa da lista ao sair dela. | `consume 1` |
| `RND` | Ordem aleatória, sem repetir até todas as faixas tocarem. | `random 1` |
| `RPT1` | Esta faixa de novo, até segunda ordem. | `repeat 1` + `single 1` |
| `ONE1`, `EAT1` | `ONE` ou `EAT` para uma música só, porque um cliente MPD pediu `single oneshot` ou `consume oneshot`. Volta ao nome sem número depois. | `oneshot` |

Cada ordem é exatamente uma combinação do MPD. No sentido contrário, três das oito combinações random/repeat/single do MPD não têm ordem exata aqui; o `mpdmode.h` tem a tabela e o que cada uma vira.

## Ajustes (botão de engrenagem)

Cinco abas: `SD`, `USB`, `BUILD`, `AUDIO`, `NET`. NET é abreviação de network (rede).

### SD e USB

| Ajuste | Valores | Observações |
|---|---|---|
| Reindexar | `REINDEX` | Refaz o índice de mídia daquele volume. Mostra `INDEXING` enquanto o deste volume roda, `BUSY` enquanto o de outro roda — um de cada vez. |
| Alimentação USB (aba USB) | `ON` / `OFF` | Energia para a porta USB-A. `IN USE` e em cinza enquanto toca uma faixa do pendrive. O player também a corta sozinho depois de uma faixa quando não há nada ligado. |

### BUILD

O que está rodando, e o idioma da tela.

| Ajuste | Valores | Observações |
|---|---|---|
| Idioma | `English` / `简体中文` / `日本語` / `Español` / `Português` / `Français` / `Italiano` | Toque para alternar. Cada idioma aparece escrito em si mesmo, e a etiqueta da linha e os nomes das abas nunca são traduzidos, então dá para achar o caminho de volta a partir de qualquer um. Guardado no aparelho, não no cartão. Só algumas telas estão traduzidas por enquanto; o resto fica em inglês. |

- `app`, `version`, `built`, `idf`, e a memória livre `heap`, `psram`, e `uptime`.
- **Source (código-fonte)**: o repositório, github.com/Sudrien/defeatist-music-player-for-m5tab5.
- **Libraries (bibliotecas)**: cada componente e biblioteca incluída nesta compilação, com sua versão (ou os sete primeiros caracteres do commit, se for de git), e sua licença depois de `--`. `licence not found` significa que o componente não trouxe nem arquivo nem campo de licença. Gerado a partir do `dependencies.lock` e das versões incluídas quando a compilação é configurada, então é o que foi compilado.

### AUDIO

| Ajuste | Valores | Observações |
|---|---|---|
| ReplayGain | sim / não | Nivela as faixas entre si. Desligado para o ajuste, não a medição. |
| Crossfade | não, ou segundos | Controle deslizante. |
| Mesmo álbum | sim / não | Crossfade também entre faixas de um mesmo álbum. Desligado deixa as emendas do álbum como estão. Uma pasta conta como um álbum. |
| A velocidade dura | `TRACK` / `BOOT` | Quanto tempo dura uma velocidade de toca-discos. `TRACK`: volta a ficar igual quando começa uma nova faixa; repetir uma (`RPT1`) não é nova faixa. `BOOT`: mantida até ser alterada, mesmo depois de desligar. Aceso em `BOOT`. |
| Gravar de | veja abaixo | O que o botão de gravação grava. |

**Gravar de** — toque para alternar. Uma opção fica acesa quando a entrada dela está ali para gravar.

| Na tela | Grava |
|---|---|
| `MONO` | Os dois microfones internos, somados em mono. |
| `STEREO` | Os dois microfones internos, esquerdo e direito. |
| `FOCUSED` | O par interno como um feixe apontado para fora da tela. Mono. |
| `HEADSET` | O microfone da entrada de fone, em mono. Aceso quando há algo conectado — a entrada não distingue um headset de fones. |
| `UAC` | Um microfone USB, na taxa e no número de canais dele. |
| `AUTO` | USB se houver microfone, senão o headset, senão `MONO`. Sempre mono. |
| `OFF` | Sem gravação: a chave de transporte perde a posição de gravar. |

Todas as gravações são FLAC, em `Recordings/` no cartão SD (no pendrive se não houver cartão).

### NET

| Ajuste | Valores | Observações |
|---|---|---|
| Wi-Fi | sim / não | |
| Controle remoto | sim / não | O controle remoto pelo navegador, via HTTPS. |
| Dispositivos | `ESQUECER` | Os navegadores e os endereços de apps MPD permitidos pela tela do player. `ESQUECER` apaga todos e fecha as páginas do controle remoto; cada um pede de novo. |
| Servidor MPD | sim / não | Para clientes MPD e Home Assistant. |
| Hora da rede | sim / não | NTP, por Wi-Fi ou por um cabo Ethernet USB. |
| Adicionar rede | `START` / `STOP` | Abre um portal cativo para conectar a uma rede Wi-Fi pelo celular. |
| Medir velocidade | `RUN` (`BUSY` enquanto roda) | Com que rapidez dá para puxar o stream de uma rádio sem decodificar nada. |
| Relógio | `RESET` | Esquece o relógio. Só disponível enquanto a hora não foi verificada. |

## Página Repouso (botão da lua)

| Ajuste | Valores |
|---|---|
| Tela | O brilho, em porcentagem. |
| Rotação | `0`, `90`, `180`, `270`. |
| Escurecer tela | Nunca, ou depois de 15 s a 2 min sem toque — para meio brilho. |
| Desligar tela | Nunca, ou 30 s a 5 min. Nunca com esta página aberta. |
| Timer | não, ou 15 min a 2 h em passos de 15 minutos. O som some aos poucos nos últimos 20 segundos. |
| Desligar | Nunca, ou depois de 15 min, 30 min, 1 h ou 2 h sem tocar, gravar nem ser tocado. O aparelho se desliga sozinho; o botão lateral liga de novo. Separado do timer, e nunca com música tocando. |

## Em outros idiomas (6019)

As etiquetas acima em mandarim, japonês, espanhol, português (Brasil), francês e italiano, escolhidos na aba BUILD. Cada uma traduz a palavra inteira que a etiqueta abrevia, encurtada de novo onde um botão é mais estreito que a palavra: os botões do seletor têm 112 px (8 caracteres latinos ou 4 CJK), as pílulas de ajuste 124 px (5 ou 3). As palavras da ordem de reprodução seguem o vocabulário MPD do Cantata onde ele tem um — 重复/随机/单曲 e 播完删除 (de 播放后删除), リピート/ランダム, Repetir — e ON/OFF seguem o 开/关 que a aba Audio usa desde a 6013.

Mantidos em inglês em todos os idiomas, por `same()`: os nomes das abas (SD, USB, BUILD, AUDIO, NET, RADIO), USB, WIFI, MPD, HTTPS, UAC, RG, e as unidades.

| Onde | English | 简体中文 | 日本語 | Español | Português | Français | Italiano |
|---|---|---|---|---|---|---|---|
| Linha de status | `SLEEP` | 睡眠 | 睡眠 | SUEÑO | SONO | VEILLE | SONNO |
| Linha de status | `SLEEP %dM` | 睡眠%d分 | 睡眠%d分 | SUEÑO %dM | SONO %dM | VEILLE %dM | SONNO %dM |
| Linha de status | `IDLE` | 空闲 | 待機 | INAC | OCIO | INACT | INATT |
| Linha de status | `BATT` | 电池 | 電池 | BAT | BAT | BATT | BATT |
| Linha de status | `BATT %d%%` | 电池 %d%% | 電池 %d%% | BAT %d%% | BAT %d%% | BATT %d%% | BATT %d%% |
| Linha de status | `CHRG %d%%` | 充电 %d%% | 充電 %d%% | CARG %d%% | CARR %d%% | CHRG %d%% | CARIC %d%% |
| Linha de status | `CHARGED` | 已充满 | 満充電 | CARGADA | CARREGADA | CHARGÉE | CARICA |
| Linha de status | `NO BATT` | 无电池 | 電池なし | SIN BAT | SEM BAT | SANS BATT | NO BATT |
| Tocando | `LIVE` | 直播 | ライブ | VIVO | AO VIVO | DIRECT | LIVE |
| Tocando | `REC` | 录音 | 録音 | GRAB | GRAV | ENR | REG |
| Tocando | `MUTED` | 静音 | 無音 | MUDO | MUDO | MUET | MUTO |
| Rodapé do seletor | `UP` | 上级 | 上へ | SUBIR | SUBIR | MONTER | SU |
| Rodapé do seletor | `RLOD` | 重新加载 | 再読込 | RECARGAR | RECARR. | RECHARG. | RICARICA |
| Rodapé do seletor | `FLDR` | 播放目录 | フォルダ | CARPETA | PASTA | DOSSIER | CARTELLA |
| Rodapé do seletor | `UP^` | 上页 | 前頁 | RE PÁG | PG CIMA | PG HAUT | PAG SU |
| Rodapé do seletor | `DN` | 下页 | 次頁 | AV PÁG | PG BAIXO | PG BAS | PAG GIÙ |
| Ordem de reprodução | `ONE` | 单曲 | 単曲 | ÚNICA | UMA | UNE | UNO |
| Ordem de reprodução | `ALL` | 顺序 | 全曲 | TODAS | TODAS | TOUT | TUTTI |
| Ordem de reprodução | `RPT` | 重复 | リピート | REPETIR | REPETIR | RÉPÉTER | RIPETI |
| Ordem de reprodução | `EAT` | 播完删除 | 消費 | BORRAR | COMER | MANGER | MANGIA |
| Ordem de reprodução | `RND` | 随机 | ランダム | AZAR | ALEAT | ALÉA | CASUALE |
| Ordem de reprodução | `RPT1` | 单曲重复 | 1曲反復 | REPITE 1 | REPETE 1 | RÉPÉTER1 | RIPETI1 |
| Ordem de reprodução | `ONE1` | 单曲1 | 単曲1 | ÚNICA1 | UMA1 | UNE1 | UNO1 |
| Ordem de reprodução | `EAT1` | 删除1 | 消費1 | BORRAR1 | COMER1 | MANGER1 | MANGIA1 |
| Ajustes | `ON` | 开 | オン | SÍ | SIM | OUI | SÌ |
| Ajustes | `OFF` | 关 | オフ | NO | NÃO | NON | NO |
| Ajustes | `IN USE` | 使用中 | 使用中 | EN USO | EM USO | UTILISÉ | IN USO |
| Ajustes | `REINDEX` | 重新索引 | 再索引 | REINDEXAR | REINDEXAR | RÉINDEXER | REINDICIZZA |
| Ajustes | `INDEXING` | 索引中 | 索引中 | INDEXANDO | INDEXANDO | INDEXATION | INDICIZZA |
| Ajustes | `BUSY` | 进行中 | 作業中 | OCUP. | OCUP. | OCCUPÉ | OCCUP. |
| Ajustes | `START` | 开始 | 開始 | ABRIR | ABRIR | OUVRIR | APRI |
| Ajustes | `STOP` | 停止 | 停止 | PARAR | PARAR | ARRÊT | STOP |
| Ajustes | `RUN` | 运行 | 実行 | MEDIR | MEDIR | LANCER | MISURA |
| Ajustes | `RESET` | 重置 | 戻す | VOLVER | VOLTAR | RÉTABLIR | AZZERA |
| Ajustes | `CLOSE` | 关闭 | 閉じる | CERRAR | FECHAR | FERMER | CHIUDI |
| Ajustes | `IMPORT` | 导入 | 取込 | IMPORTAR | IMPORTAR | IMPORTER | IMPORTA |
| Ajustes | `DONE` | 完成 | 完了 | HECHO | OK | OK | FATTO |
| Ajustes | `RETRY` | 重试 | 再試行 | REPETIR | REPETIR | RÉESSAYER | RIPROVA |
| Ajustes | `TRACK` | 每曲 | 曲ごと | PISTA | FAIXA | PISTE | BRANO |
| Ajustes | `BOOT` | 保持 | 保持 | SIEMPRE | SEMPRE | TOUJOURS | SEMPRE |
| Gravar de | `MONO` | 单声道 | モノラル | MONO | MONO | MONO | MONO |
| Gravar de | `STEREO` | 立体声 | ステレオ | ESTÉREO | ESTÉREO | STÉRÉO | STEREO |
| Gravar de | `FOCUSED` | 定向 | フォーカス | ENFOCADO | FOCADO | DIRIGÉ | DIRETTO |
| Gravar de | `HEADSET` | 耳麦 | ヘッドセット | AURICULAR | HEADSET | CASQUE | CUFFIA |
| Gravar de | `AUTO` | 自动 | 自動 | AUTO | AUTO | AUTO | AUTO |

## Opções de compilação

Não aparecem na tela — `idf.py -D<NOME>=1 build`. Cada uma fica no `CMakeCache.txt` até ser posta de volta em 0, então compile uma versão de lançamento a partir de uma árvore limpa.

| Opção | Faz |
|---|---|
| `HEAPCHECK` | Pontos de verificação de integridade do heap que nomeiam o último subsistema antes de uma corrupção. Lento. |
| `WAVEFORM` | A varredura de forma de onda do arquivo inteiro. |
| `O3CHECK` | Todo o `main` em -O3, para pegar avisos. Uma verificação, não um ajuste de lançamento. |
| `DECBENCH` | Registra o custo de decodificação do minimp3 em ciclos de CPU por quadro. |
