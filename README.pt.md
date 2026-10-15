# Defeatist Music Player *para M5Tab5*

[English](README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [Español](README.es.md) · **Português** · [Français](README.fr.md) · [Italiano](README.it.md)

A maioria dos players de mídia para o M5Stack Tab5 manda você *converter* seus arquivos primeiro. Este não.

O objetivo deste projeto é tirar o máximo deste hardware sem modificá-lo. Sem solda, sem acessório que não possa ser removido depois. Todo formato que um ESP32-P4 v1.3 consiga tratar razoavelmente sem conhecer segredos.

> Esta é uma tradução automática, em português do Brasil; a versão de referência é a inglesa, [README.md](README.md). Para as etiquetas curtas da tela, veja [ABBREVIATIONS.pt.md](ABBREVIATIONS.pt.md).

![](screenshots/IMG_20261001_151110_126.jpg)

## O M5Stack Tab5 não é um player de música ideal.

- Você acha que ele tem Bluetooth.
  - Tem Bluetooth de baixa energia (BLE), o que significa que aparelhos mais antigos com Bluetooth clássico nunca vão enxergá-lo.
  - LE Audio / Auracast exige outra fiação e outros perfis. Talvez funcione regravando o C6, mas isso exige [equipamento especial](https://docs.m5stack.com/en/guide/restore_factory/m5tab5_c6_wifi).
- Tem uma entrada de headset.
  - Ótimo para um headset. Ou um cabo AUX. Ou, em teoria, para gravar.
  - Mas nada escuta os botões do controle no fio. Dá para ligá-los, dizem. O que, de novo, significa equipamento especial (mas nem tanto).
- A tela e o toque são controlados pelo mesmo chip. Dá para apagar a luz de fundo para economizar energia, mas não dá para desligar a tela por completo.
- E toda a bagunça dos controladores de tela.
  - Lançamento inicial (2025.5.9): controlador de tela ILI9881C separado + controlador de toque GT911
  - 2025.10.14: passou para o controlador integrado tela-toque (TDDI) ST7123
  - 2026.4.28: o CI do controlador mudou de ST7123 para ST7121 (foi o que me mandaram)

## Os três cenários

1. Não tenho armazenamento, mas tenho Wi-Fi (ou um adaptador Ethernet): dá para ouvir rádio pela internet.
2. Tenho armazenamento, mas não tenho música: dá para gravar arquivos FLAC para ouvir depois.
3. Tenho armazenamento e música: este é o seu player de música — com ou sem fones, com suporte a adaptadores USB Audio Class (UAC).

## Isto é o que consegui fazer funcionar no ESP-IDF ~~5.5.5~~ 6.1

- Conexão a quente de cartão microSD e pendrive USB
  - O microSD é o preferido. Gasta menos energia.
  - Mesmo assim ele monta o USB sozinho se estiver disponível, desde que o indicador USB esteja verde. Se não estiver, ligue de novo a alimentação USB nos ajustes.
- Suporte a exFAT
  - Cartões SDHC e SDXC foram testados (mesmo que o segundo tenha morrido depois de uma semana, não foi culpa do software). SDUC não. Arquivos de áudio do tamanho de um Blu-ray vão tocar? Sei lá.
- Troca automática do headset para o alto-falante interno ao desconectar, e vice-versa
  - O ícone ao lado do controle de volume mostra o que está tocando de verdade — um alto-falante, fones, ou `UAC` quando um dispositivo de áudio USB tem a saída. Tocar nele silencia e volta o som.
- Suporte a todas as variantes do formato MP3 (até onde sei). Essa coisa tem biblioteca reserva atrás de biblioteca reserva. FLAC, Ogg, WAV, os padrões estão aqui.
- Exibição da capa: seja a imagem dentro do arquivo, seja o cover.jpg / folder.jpg / front.jpg do álbum ao lado dele.
- Estado da bateria, por extenso no fim da linha de status: `BATT 73%`, `CHRG 73%`, `CHARGED`, `NO BATT`.
- Wi-Fi na linha de status: verde conectado, amarelo conectando, cinza desligado.
- Controle de volume
- Tocar/pausar
- Início da faixa / faixa anterior (toque duplo)
- Próxima faixa
- Desligamento da tela
- Arrastar para avançar ou voltar. Em todos os formatos da lista acima.
- O botão de ordem de reprodução passa por ONE / ALL / RPT / EAT / RND / RPT1 — o que cada um faz, e todas as outras etiquetas curtas da tela, estão em [ABBREVIATIONS.pt.md](ABBREVIATIONS.pt.md).
- Forma de onda do volume na barra de progresso. Achei legal.
- Suporte a USB Audio Class — aquele adaptador de "colocar fone Bluetooth no meu PS5" funciona aqui também. Só na porta USB-A.
- A pausa corta a alimentação do amplificador.
- Cache em SDRAM. Se notar coisas estranhas uns 20 segundos antes de uma troca de música, abra uma issue.
- Suporte a ReplayGain. Na primeira vez que você ouve uma música inteira, o Defeatist ouve junto — assim, nas próximas vezes, ele aumenta as músicas mais baixas e abaixa as mais altas, dentro do razoável. Razoável segundo a [BS.1770](https://www.itu.int/rec/R-REC-BS.1770/en).
  - A forma de onda da barra de progresso vem dessa mesma escuta. Até uma música ser ouvida inteira uma vez, a barra dela fica cinza lisa.
  - Pular ou avançar durante essa primeira escuta a cancela — ele tenta de novo na próxima vez.
  - Esses metadados ficam num `.songname.rgcache` oculto. Não dá para desligar o cálculo; dá para desligar o ajuste de volume nos ajustes.
- A ARK 12 cobre boa parte do Unicode, mas não é perfeita.
- Reabre a última música tocada ao ligar. Sem tocar sozinha.
- Fade de 3 segundos ao remover a mídia.
- Crossfade configurável.
- Respeita de verdade os dados de reprodução sem intervalo (gapless).
- Rádio pela internet pela API de <https://www.radio-browser.info>, ou por lista manual. Aqui é território de "fique na tomada".
  - Streams MP3, AAC e Ogg, e HLS (`.m3u8`) com AAC ou MP3 em segmentos simples ou MPEG-TS. Ainda não: HLS criptografado ou fMP4, nem links `.pls`/`.m3u` que uma rádio sirva.
- Títulos e nomes de rádio em árabe, ligados e dispostos da direita para a esquerda, junto à margem direita.
- Timer para dormir (até 2 horas, em passos de 15 minutos).
- Desligar depois de 15 min a 2 h sem tocar, gravar nem ser tocado — separado do timer. `IDLE` na linha de status fica verde enquanto conta e amarelo enquanto algo o segura; o botão lateral liga de novo.
- Proteção de bateria fraca: abaixo de 6,3 V por 30 s na bateria, ele termina qualquer gravação e se desliga, antes que a bateria fique vazia a ponto de se danificar. Ainda não testado numa descarga real.
- Controle de brilho.
- Network Time Protocol (NTP), se o Wi-Fi estiver configurado.
- O relógio com bateria própria guarda a hora depois de desligar, então as gravações recebem o nome certo sem Wi-Fi; o NTP corrige quando pode.
- Configuração de Wi-Fi por portal cativo (vários roteadores).
- Cadastro manual de rádios pela internet no portal web do Wi-Fi.
- Favoritos com estrela.
- Adaptadores Ethernet USB: CDC-ECM (Realtek RTL8152 testado; RTL8153 mesmo caminho, não testado) e uma adaptação do [ASIX AX88772](https://github.com/Sudrien/esp_usbh_asix), ambos confirmados na placa.
- Arquivos cue.
- m3u/m3u8 — pelo menos via MPD.
- Ah, é, suporte ao [Music Player Daemon](https://mpd.readthedocs.io/en/stable/user.html) — sim, isso deve significar controle pelo Home Assistant também.
- Interface web em HTTPS.
- Gravação de áudio, já que o hardware está ali, em FLAC.
  - com um medidor de nível enquanto grava.
- Uma linha de status abaixo do volume: alimentação USB, MPD, HTTPS e os minutos que faltam no timer. Verde é ligado.
- Meu Deus, olhei a lista de apps do M5Launcher, vamos arrumar a ordem do título agora mesmo.
- Idiomas da interface (English, 简体中文, 日本語, Español, Português, Français, Italiano), escolhidos na engrenagem de ajustes >> aba BUILD.
- Suporte ao [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher)
  - voltar ao launcher
  - importar credenciais de Wi-Fi
- Separei as bibliotecas para https://github.com/Sudrien/lothesome-audio-analyzer-for-m5tab5 ... um erro? talvez
- Alguns filtros
  - ajuste de graves
  - ajuste de agudos
  - cancelamento de voz baseado no estéreo
  - eco configurável
  - passa-alta
  - passa-baixa
  - ajuste de largura
  - filtro de estalos (chiado de vinil)
  - filtro de flutter
  - filtro de wow
- velocidade de reprodução (para arquivos)
  - São números de toca-discos, você vai entender.

## O que pode acontecer

- Mais tratamento de travamentos; ei, você sempre pode ligá-lo no `idf.py monitor` e ver o que aparece.
- Um baixador de podcasts por Wi-Fi? Dá para imaginar. Ia querer suporte a capítulos.
  - tem tanta coisa. Tanta, tanta coisa.

## O que não pode acontecer com o código publicado atual

- Adaptadores de Bluetooth clássico.
- Retomar por arquivo.
- Hubs USB.
  - Ele percebe que você conectou um? Sim.
  - Ele consegue usar o que estiver ligado nele? Provavelmente não.
  - Um hub te salva se o seu aparelho puxa energia suficiente para derrubar a tensão do Tab5? Hã. Defina "salvar".
  - O que as bibliotecas não suportam são várias velocidades de dispositivo, e não sei te dizer a velocidade de um aparelho qualquer.
- Arquivos com DRM, nem pensar.
- DSD e APE exigem processamento demais.

## Problemas possíveis

- As redes Wi-Fi salvas e o certificado do controle remoto ficam na NVS da placa, namespace `defeatist`. Num launcher de cartão SD, todo firmware compartilha essa partição: outro app pode lê-los (a NVS não é criptografada aqui), e um que apague a NVS leva tudo junto. Os ajustes voltam do cartão; as redes e o certificado, não.

- Carregar por um USB-C fraco + bateria colocada + tela ligada pode dar o que parece um chiado no alto-falante, mas não é. Arrume um cabo USB melhor, um hub melhor, uma ligação direta ao carregador. Está faltando ampère.
- A seleção de arquivos é um pouco mais lenta do que eu queria, porque selecionar a primeira música debaixo do seu polegar não é o que você quer.
- Cabos AUX não são necessariamente blindados o bastante contra tudo o que pode estar em volta. Vale a regra eletromagnética de "afaste o celular".

## Licenças

- Este código é MIT, assim como os quatro componentes feckless-*-for-m5tab5 que ele puxa por git — exceto as fontes, abaixo.
- minimp3 é CC0-1.0 (dedicação ao domínio público). Sem obrigação de atribuição. Não está no repositório: `cmake/vendored.cmake` baixa num commit fixo e confere um SHA256, então o código-fonte continua auditável.
- esp_audio_codec vem como **arquivos pré-compilados** sob a licença ESPRESSIF MIT. Grátis, mas a concessão se limita ao silício da Espressif. Tudo certo aqui; bom saber antes de copiar este código para outro lugar.
- pngle (kikuchan) e miniz (Rich Geldreich et al.) são MIT, baixados do mesmo jeito que o minimp3. A única condição do MIT é que o aviso de copyright e de permissão acompanhe "todas as cópias ou partes substanciais", e uma imagem de firmware é uma cópia: uma redistribuição binária leva os avisos deles (os cabeçalhos de `pngle.c` e `miniz.c` baixados) junto com o `LICENSE` deste projeto.
- stb_image (o decodificador JPEG de último recurso em `components/stbjpeg`) é de Sean Barrett, à escolha sob MIT ou a Unlicense (domínio público). Este projeto o usa sob a Unlicense, então não acrescenta aviso a carregar. O invólucro em volta dele é MIT.
- **`components/usb_host_msc` e `components/usb_host_uac` são da Espressif, Apache-2.0, não MIT.** São cópias incluídas e modificadas dos drivers de classe do esp-usb; cada uma mantém seu próprio arquivo de licença, e os arquivos alterados mantêm os cabeçalhos SPDX da Espressif com uma linha "Modified by" embaixo (Apache-2.0, seção 4(b)); os manifestos incluídos dizem `license: "Apache-2.0"`. Uma redistribuição leva esses arquivos de licença.
- O fatfs com exFAT que o `cmake/exfat.cmake` inclui na configuração é o componente do IDF (Apache-2.0) em volta do FatFs de ChaN (licença FatFs, um aviso estilo BSD de uma cláusula que precisa ser mantido).
- **TJpgDec é de ChaN, sob licença própria**, e chega como o componente `espressif/esp_jpeg`. Permissiva — livre para uso pessoal e comercial, redistribuição do código permitida — mas o aviso de copyright precisa ser mantido, então ele viaja com uma redistribuição do mesmo jeito que a OFL da fonte. Usado só como reserva, para capas que o decodificador JPEG por hardware não consegue alocar: o decodificador do P4 não tem redimensionamento, então uma capa de 3000 px quer 17 MB de PSRAM num bloco só e não consegue.
- **A Ark Pixel Font é SIL OFL-1.1, e por isso as tabelas `ark12` em feckless-graphics-handler-for-m5tab5 (`fonts/ark12`) também são OFL-1.1, não MIT.** Converter os PNGs dos glifos em arrays C torna esses arquivos uma Versão Modificada pela seção 5 da OFL, e a seção 5 exige que Versões Modificadas continuem sob a OFL. Isso não é problema — a OFL permite explicitamente empacotar com software sob qualquer licença, e só os arquivos da fonte ficam presos a ela — mas o `fonts/ark12/LICENSE-OFL` precisa acompanhar qualquer redistribuição, imagem de firmware inclusive, e as tabelas não podem ser vendidas sozinhas. A Ark não declara Nome de Fonte Reservado (Reserved Font Name), então o derivado não precisou ser renomeado; chama-se `ark12` mesmo assim, porque não é a Versão Original.
- **Arabixel Basic é CC BY 4.0** (ArabianDev, <https://arabiandev.itch.io/arabixel-basic-font>), assim como o arquivo da fonte e a tabela gerada em `fonts/arabixel` de feckless-graphics-handler-for-m5tab5; o código de modelagem dela é MIT. A CC BY (<https://creativecommons.org/licenses/by/4.0/>) pede crédito, um link para a licença e uma nota das mudanças — o README do componente e o cabeçalho da tabela trazem isso, e uma redistribuição, imagem de firmware inclusive, mantém `fonts/arabixel/LICENSE.txt` junto.
- MurmurHash2 é de domínio público.

## Como contribuir

Se quiser comentar as traduções ou os recursos, abra uma issue no GitHub, em https://github.com/Sudrien/defeatist-music-player-for-m5tab5

Se quiser contribuir com código, leia o CONTRIBUTING.md e o CLAUDE.md para os padrões de formatação.

## "Aurora Player — BT Mode"

- eles usam Codex, eu uso Opus, sem problema aí
- Sim, instalar um módulo ESP32-PICO-D4 dá acesso ao Bluetooth clássico
- Escrevo partindo do princípio de que a maioria das pessoas não quer modificar o hardware de forma permanente
- Ainda não consigo recomendá-los até que
  - tenham um repositório público de código-fonte de verdade (não precisa ser git)
  - publiquem as informações de licença (porque as bibliotecas que usam exigem isso)
  - tenham uma URL melhor que https://frolicking-bonbon-c32cfa.netlify.app/
  - 'feedback welcome' DE FATO LEVE A ALGUM LUGAR PARA DAR FEEDBACK
  - tenham notas de versão que sejam de fato notas de versão

## Uma última alfinetada

- waveflow for tab5 music player
  - dá até para gerar por IA um logo para o aparelho certo
  - vocês mandam o pessoal baixar o ffmpeg e DEPOIS um script de conversão????????? Quando eles podem nem ter Python para começo de conversa???? shmusica, faça-me o favor
  - aliás, isso se chama "transcodificação"
  - A sua falta de trabalho me garantiu que existem camadas no vibe coding
