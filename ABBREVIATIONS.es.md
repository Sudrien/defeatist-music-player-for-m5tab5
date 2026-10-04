# Abreviaturas y conmutadores

[English](ABBREVIATIONS.md) · [简体中文](ABBREVIATIONS.zh-CN.md) · [日本語](ABBREVIATIONS.ja.md) · **Español**

Qué significan las palabras cortas de la pantalla, y todos los ajustes que tiene el reproductor, por el lugar donde viven. El panel mide 720 px de ancho y la mayoría caben en un botón de una quinta parte de eso, así que son cortos a propósito.

> Esta es una traducción de la comunidad; la versión canónica es la inglesa [ABBREVIATIONS.md](ABBREVIATIONS.md). La columna «En pantalla» conserva las etiquetas literales en inglés del código fuente; en una interfaz en español, salvo las palabras que se mantienen en inglés, la pantalla muestra los términos traducidos de la tabla «En otros idiomas» de más abajo.

Tomado del código fuente (`browser.c`, `panel.c`, `sleeppage.c`, `ui.c`, `settings.h`, `playlist.h`). Si una etiqueta de aquí y la pantalla no coinciden, lo nuevo es la pantalla — corrige este archivo.

## La barra de reproducción

| En pantalla | Significa |
|---|---|
| `LIVE` | Un flujo de radio. No hay posición a la que saltar, así que la barra de búsqueda pasa a ser un minuto de historial de nivel. |
| `REC` | Grabando. La barra muestra el nivel de la entrada, en escala de decibelios (-60 a 0 dBFS). |
| `MUTED` | Grabando, pero la entrada ha sido cero digital exacto durante 300 ms — un headset silenciado, o una fuente que no envía nada. |
| `RG` | Bajo el altavoz: ReplayGain está ajustando esta pista. La marca amarilla del control de volumen es donde el volumen queda en efecto. |
| `UAC` | En lugar del icono de altavoz: un dispositivo USB Audio Class tiene la salida. |
| `FILE` | En la tarjeta de formato, para un archivo sin extensión. Si no, la tarjeta muestra la extensión (`MP3`, `FLAC`…). |

### La línea de estado, bajo el volumen

Informes, no interruptores — nada aquí hace nada al tocarlo. Verde es encendido, gris oscuro apagado (WIFI también tiene amarillo). Cada uno tiene su propio interruptor en otro sitio.

| En pantalla | Verde cuando | Se conmuta desde |
|---|---|---|
| `USB` | El puerto USB-A está alimentado | pestaña USB, alimentación USB |
| `WIFI` | Conectado, con dirección. **Amarillo**: encendido pero aún sin conectar — arrancando, escaneando, uniéndose o fallando | pestaña NET, Wi-Fi |
| `MPD` | El servidor MPD está en marcha | pestaña NET, servidor MPD |
| `HTTPS` | El control remoto por navegador está en marcha | pestaña NET, Control remoto |
| `SLEEP xxM` | El temporizador está puesto; minutos restantes, redondeados hacia arriba. Solo `SLEEP` cuando no lo está | página de reposo |
| `IDLE` | El apagado está puesto y en cuenta atrás: nada reproduciéndose, transmitiendo, grabando ni indexando. **Amarillo**: puesto, pero retenido — algo está ocurriendo, así que la espera se reinicia. Gris cuando el apagado es Nunca. Sin minutos (6020) | página de reposo, Apagado |

En el extremo derecho de la misma línea, la batería (no hay icono de batería):

| En pantalla | Significa | Color |
|---|---|---|
| `BATT 73%` | Funcionando con batería | Gris claro; rojo al 20% o menos |
| `CHRG 73%` | Entra corriente y carga la batería | Verde |
| `CHARGED` | La batería está llena | Verde |
| `NO BATT` | Sin batería puesta; funcionando desde USB-C | Gris oscuro |
| `BATT` | El medidor de batería no da lectura | Gris claro |

## El selector (botón de carpeta)

### Pestañas

| En pantalla | Significa |
|---|---|
| `microSD` | La tarjeta microSD. |
| `USB` | Una unidad USB en el puerto USB-A. |
| `RADIO` | Radio por Internet: `stations.m3u` y favoritos. |

### Botones del pie

| En pantalla | Significa |
|---|---|
| `UP` | Subir una carpeta. |
| `RLOD` | En el nivel superior de la pestaña RADIO, en lugar de `UP`: recargar la lista de emisoras desde la tarjeta. |
| `FLDR` | Reproducir toda esta carpeta. |
| `UP^` | Subir una página la lista. |
| `DN` | Bajar una página la lista. |
| `X` | Cerrar el selector. |
| *(orden)* | El orden de reproducción — toca para rotar. Ver abajo. |

### Orden de reproducción

Al tocar el botón de orden va **ONE → ALL → RPT → EAT → RND → RPT1 →** de vuelta a ONE.

| En pantalla | Significa | Equivalente MPD |
|---|---|---|
| `ONE` | Reproducir esta pista y parar. | `single 1` |
| `ALL` | Seguir a la siguiente; parar al final de la carpeta. | (todo apagado) |
| `RPT` | `ALL`, empezando de nuevo desde el principio al final. | `repeat 1` |
| `EAT` | `ALL`, quitando cada pista de la lista al dejarla. | `consume 1` |
| `RND` | Orden aleatorio, sin repetir hasta que todas hayan sonado. | `random 1` |
| `RPT1` | Esta pista otra vez, hasta nueva orden. | `repeat 1` + `single 1` |
| `ONE1`, `EAT1` | `ONE` o `EAT` para una sola canción, porque un cliente MPD pidió `single oneshot` o `consume oneshot`. Vuelve al nombre sin número después. | `oneshot` |

Cada orden es exactamente una combinación de MPD. En sentido inverso, tres de las ocho combinaciones random/repeat/single de MPD no tienen orden exacto aquí; `mpdmode.h` tiene la tabla y en qué se convierte cada una.

## Ajustes (botón del engranaje)

Cinco pestañas: `SD`, `USB`, `BUILD`, `AUDIO`, `NET`. NET es abreviatura de network (red).

### SD y USB

| Ajuste | Valores | Notas |
|---|---|---|
| Reindexar | `REINDEX` | Reconstruir el índice de medios de ese volumen. Marca `INDEXING` mientras corre el de este volumen, `BUSY` mientras corre el de otro — de uno en uno. |
| Alimentación USB (pestaña USB) | `ON` / `OFF` | Energía al puerto USB-A. `IN USE` y en gris mientras se reproduce una pista desde la unidad. El reproductor también la corta por sí mismo tras una pista cuando no hay nada conectado. |

### BUILD

Qué está en marcha, y el idioma de la pantalla.

| Ajuste | Valores | Notas |
|---|---|---|
| Idioma | `English` / `简体中文` / `日本語` / `Español` | Toca para rotar. Cada idioma se nombra en sí mismo, y la etiqueta de la fila y los nombres de pestaña nunca se traducen, así que el camino de vuelta se encuentra desde cualquiera. Se guarda en el dispositivo, no en la tarjeta. Por ahora solo hay algunas pantallas traducidas; el resto queda en inglés. |

- `app`, `version`, `built`, `idf`, y `heap`, `psram` y `uptime` libres.
- **Source (código)**: el repositorio, github.com/Sudrien/defeatist-music-player-for-m5tab5.
- **Libraries (bibliotecas)**: cada componente y biblioteca incluida en esta compilación, con su versión (o los primeros siete caracteres del commit si es de git), y su licencia tras `--`. `licence not found` significa que el componente no traía ni archivo ni campo de licencia. Se genera desde `dependencies.lock` y los pines incluidos al configurar la compilación, así que es lo que se compiló.

### AUDIO

| Ajuste | Valores | Notas |
|---|---|---|
| ReplayGain | sí / no | Nivela las pistas entre sí. Apagarlo detiene el ajuste, no la medición. |
| Crossfade | no, o segundos | Deslizador. |
| Mismo álbum | sí / no | Hacer crossfade también entre pistas de un mismo álbum. Apagado deja en paz las uniones del álbum. Una carpeta cuenta como un álbum. |
| Grabar desde | ver abajo | Qué graba el botón de grabación. |

**Grabar desde** — toca para rotar. Una opción se enciende cuando su entrada está presente para grabar.

| En pantalla | Graba |
|---|---|
| `MONO` | Los dos micrófonos internos, sumados a mono. |
| `STEREO` | Los dos micrófonos internos, izquierdo y derecho. |
| `FOCUSED` | El par interno como un haz apuntando fuera de la pantalla. Mono. |
| `HEADSET` | El micrófono del conector de auriculares, mono. Encendido cuando hay algo enchufado — el conector no distingue un headset de unos auriculares. |
| `UAC` | Un micrófono USB, a su propia frecuencia y número de canales. |
| `AUTO` | USB si hay micrófono, si no el headset, si no `MONO`. Siempre mono. |
| `OFF` | Sin grabación: el conmutador de transporte pierde su posición de grabar. |

Todas las grabaciones son FLAC, en `Recordings/` de la tarjeta SD (la unidad USB si no hay tarjeta).

### NET

| Ajuste | Valores | Notas |
|---|---|---|
| Wi-Fi | sí / no | |
| Control remoto | sí / no | El remoto por navegador, sobre HTTPS. |
| Servidor MPD | sí / no | Para clientes MPD y Home Assistant. |
| Hora de red | sí / no | NTP. En gris mientras el Wi-Fi está apagado; el ajuste se conserva. |
| Añadir una red | `START` / `STOP` | Abre un portal cautivo para unirse a una red Wi-Fi desde un móvil. |
| Benchmark | `RUN` (`BUSY` mientras corre) | Con qué rapidez se puede extraer el flujo de una emisora sin decodificar nada. |
| Reloj | `RESET` | Olvidar el reloj. Solo disponible mientras la hora no se ha verificado. |

## Página de reposo (botón de la luna)

| Ajuste | Valores |
|---|---|
| Pantalla | Brillo, un porcentaje. |
| Rotación | `0`, `90`, `180`, `270`. |
| Atenuar pantalla | Nunca, o tras 15 s a 2 min sin tocar — a medio brillo. |
| Apagar pantalla | Nunca, o 30 s a 5 min. Nunca mientras esta página está abierta. |
| Temporizador de reposo | no, o 15 min a 2 h en pasos de 15 minutos. El sonido se desvanece en los últimos 20 segundos. |
| Apagado | Nunca, o tras 15 min, 30 min, 1 h o 2 h sin reproducir, grabar ni tocar. El dispositivo se apaga solo; el botón lateral lo enciende. Independiente del temporizador, y nunca mientras suena la música. |

## En otros idiomas (6019)

Las etiquetas de arriba en mandarín, japonés y español, elegidas en la pestaña BUILD. Cada una traduce la palabra completa que la etiqueta representa, reacortada donde un botón es más estrecho que la palabra: los botones del selector miden 112 px (8 caracteres latinos o 4 CJK), las pastillas de ajustes 124 px (5 o 3). Las palabras del orden de reproducción siguen el vocabulario MPD de Cantata donde lo tiene — 重复/随机/单曲 y 播完删除 (de 播放后删除), リピート/ランダム, Repetir — y ON/OFF siguen el 开/关 que la pestaña Audio usa desde 6013.

En todos los idiomas se mantienen en inglés, mediante `same()`: los nombres de pestaña (SD, USB, BUILD, AUDIO, NET, RADIO), USB, WIFI, MPD, HTTPS, UAC, RG, y las unidades.

| Dónde | English | 简体中文 | 日本語 | Español |
|---|---|---|---|---|
| Línea de estado | `SLEEP` | 睡眠 | 睡眠 | SUEÑO |
| Línea de estado | `SLEEP %dM` | 睡眠%d分 | 睡眠%d分 | SUEÑO %dM |
| Línea de estado | `IDLE` | 空闲 | 待機 | INAC |
| Línea de estado | `BATT` | 电池 | 電池 | BAT |
| Línea de estado | `BATT %d%%` | 电池 %d%% | 電池 %d%% | BAT %d%% |
| Línea de estado | `CHRG %d%%` | 充电 %d%% | 充電 %d%% | CARG %d%% |
| Línea de estado | `CHARGED` | 已充满 | 満充電 | CARGADA |
| Línea de estado | `NO BATT` | 无电池 | 電池なし | SIN BAT |
| Reproduciendo | `LIVE` | 直播 | ライブ | VIVO |
| Reproduciendo | `REC` | 录音 | 録音 | GRAB |
| Reproduciendo | `MUTED` | 静音 | 無音 | MUDO |
| Pie del selector | `UP` | 上级 | 上へ | SUBIR |
| Pie del selector | `RLOD` | 重新加载 | 再読込 | RECARGAR |
| Pie del selector | `FLDR` | 播放目录 | フォルダ | CARPETA |
| Pie del selector | `UP^` | 上页 | 前頁 | RE PÁG |
| Pie del selector | `DN` | 下页 | 次頁 | AV PÁG |
| Orden | `ONE` | 单曲 | 単曲 | ÚNICA |
| Orden | `ALL` | 顺序 | 全曲 | TODAS |
| Orden | `RPT` | 重复 | リピート | REPETIR |
| Orden | `EAT` | 播完删除 | 消費 | BORRAR |
| Orden | `RND` | 随机 | ランダム | AZAR |
| Orden | `RPT1` | 单曲重复 | 1曲反復 | REPITE 1 |
| Orden | `ONE1` | 单曲1 | 単曲1 | ÚNICA1 |
| Orden | `EAT1` | 删除1 | 消費1 | BORRAR1 |
| Ajustes | `ON` | 开 | オン | SÍ |
| Ajustes | `OFF` | 关 | オフ | NO |
| Ajustes | `IN USE` | 使用中 | 使用中 | EN USO |
| Ajustes | `REINDEX` | 重新索引 | 再索引 | REINDEXAR |
| Ajustes | `INDEXING` | 索引中 | 索引中 | INDEXANDO |
| Ajustes | `BUSY` | 进行中 | 作業中 | OCUP. |
| Ajustes | `START` | 开始 | 開始 | ABRIR |
| Ajustes | `STOP` | 停止 | 停止 | PARAR |
| Ajustes | `RUN` | 运行 | 実行 | MEDIR |
| Ajustes | `RESET` | 重置 | 戻す | VOLVER |
| Ajustes | `CLOSE` | 关闭 | 閉じる | CERRAR |
| Grabar desde | `MONO` | 单声道 | モノラル | MONO |
| Grabar desde | `STEREO` | 立体声 | ステレオ | ESTÉREO |
| Grabar desde | `FOCUSED` | 定向 | フォーカス | ENFOCADO |
| Grabar desde | `HEADSET` | 耳麦 | ヘッドセット | AURICULAR |
| Grabar desde | `AUTO` | 自动 | 自動 | AUTO |

## Conmutadores de compilación

No están en la pantalla — `idf.py -D<NOMBRE>=1 build`. Cada uno permanece en `CMakeCache.txt` hasta ponerlo de nuevo a 0, así que compila una versión desde un árbol limpio.

| Conmutador | Hace |
|---|---|
| `HEAPCHECK` | Puntos de control de integridad del heap que nombran el último subsistema antes de una corrupción. Lento. |
| `WAVEFORM` | El escaneo de forma de onda de archivo completo. |
| `O3CHECK` | Todo `main` a -O3, para cazar avisos. Una comprobación, no un ajuste de versión. |
| `DECBENCH` | Registra el coste de decodificación de minimp3 en ciclos de CPU por fotograma. |
