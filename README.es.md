# Defeatist Music Player *para M5Tab5*

[English](README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · **Español**

La mayoría de los reproductores multimedia para M5Tab5 te dicen que primero *conviertas* tus archivos. Este no.

El objetivo de este proyecto es exprimir al máximo el potencial de este hardware sin modificarlo. Sin soldaduras, sin accesorios que no se puedan quitar después. Todo formato que un ESP32-P4 v1.3 pueda manejar razonablemente sin conocer trucos secretos.

> Esta es una traducción automática; la versión canónica es la inglesa [README.md](README.md). Para las etiquetas cortas de pantalla, consulta [ABBREVIATIONS.es.md](ABBREVIATIONS.es.md).

![](screenshots/IMG_20261001_151110_126.jpg)

## El M5Stack Tab5 no es un reproductor de música ideal.

- Crees que tiene Bluetooth.
  - Tiene Bluetooth de baja energía (BLE), lo que significa que los dispositivos antiguos con Bluetooth clásico nunca lo verán.
  - LE Audio / Auracast requiere otro cableado y otros perfiles. Podría funcionar si reflasheas el C6, pero eso exige [equipo especial](https://docs.m5stack.com/en/guide/restore_factory/m5tab5_c6_wifi).
- Tiene un puerto de auriculares (headset).
  - Genial para un headset. O para un cable AUX. O, en teoría, para grabar.
  - Pero no hay nada escuchando los botones en línea del cable. Se pueden cablear, dicen. Lo cual, otra vez, implica equipo especial (aunque no demasiado).
- La pantalla y el táctil los controla el mismo chip. Puedes apagar la retroiluminación para ahorrar energía, pero no puedes apagar la pantalla por completo.
- Y todo el lío de los controladores de pantalla.
  - Lanzamiento inicial (2025.5.9): controlador de pantalla ILI9881C independiente + controlador táctil GT911
  - 2025.10.14: se cambió al controlador integrado pantalla-táctil ST7123 (TDDI)
  - 2026.4.28: el IC del controlador pasó de ST7123 a ST7121 (es el que me enviaron)

## Los tres escenarios

1. No tengo almacenamiento, pero sí Wi-Fi (o un adaptador Ethernet): puedes escuchar radio por Internet.
2. Tengo almacenamiento, pero no música: puedes grabar archivos FLAC para reproducirlos después.
3. Tengo almacenamiento y música: este es tu reproductor de música — con auriculares o sin ellos, con adaptadores USB Audio Class (UAC) admitidos.

## Esto es lo que logré hacer funcionar en ESP-IDF ~~5.5.5~~ 6.1

- Conexión en caliente de tarjeta microSD y memoria USB
  - Se prefiere la microSD. Consume menos energía.
  - Aun así montará el USB automáticamente si está disponible, siempre que el indicador USB esté en verde. Si no lo está, vuelve a activar la alimentación USB en ajustes.
- Compatibilidad con exFAT
  - Se han probado tarjetas SDHC y SDXC (aunque esta última murió tras una semana; no es culpa del software). SDUC no se ha probado. ¿Reproducirá archivos de audio del tamaño de un Blu-ray? Ni idea.
- Conmutación automática entre headset y altavoz interno al desconectar y viceversa
  - El icono junto al control de volumen muestra qué salida está sonando realmente — un altavoz, auriculares, o `UAC` cuando un dispositivo de audio USB tiene la salida. Al tocarlo se silencia y se reactiva.
- Compatibilidad con (hasta donde sé) todas las variantes del formato MP3. Esta cosa tiene biblioteca de reserva tras biblioteca de reserva. Flac, ogg, wav, los estándares están aquí.
- Visualización de carátula: ya sea la imagen incrustada en el archivo, o el cover.jpg / folder.jpg / front.jpg del álbum junto a él.
- Estado de la batería, en palabras al final de la línea de estado: `BATT 73%`, `CHRG 73%`, `CHARGED`, `NO BATT`.
- Wi-Fi en la línea de estado: verde conectado, amarillo conectando, gris apagado.
- Control de volumen
- Reproducir/pausar
- Inicio de la pista / pista anterior (doble toque)
- Pista siguiente
- Apagado de pantalla
- Arrastrar para buscar posición. En todos los formatos de la lista anterior.
- El botón de orden de reproducción recorre ONE / ALL / RPT / EAT / RND / RPT1 — qué hace cada uno, y cualquier otra etiqueta corta de la pantalla, está en [ABBREVIATIONS.es.md](ABBREVIATIONS.es.md).
- Forma de onda del volumen sobre la barra de búsqueda. Me pareció genial.
- Compatibilidad con USB Audio Class (UAC) — ese adaptador de "añadir auriculares Bluetooth a mi PS5" también funciona aquí. Solo por el puerto USB-A.
- La pausa corta la alimentación al amplificador.
- Caché en SDRAM. Si notas rarezas unos 20 segundos antes de un cambio de canción, abre un issue.
- Compatibilidad con ReplayGain. La primera vez que escuchas una canción entera, Defeatist la escucha contigo — así, en reproducciones posteriores, sube las canciones más bajas y baja las más altas, dentro de lo razonable. Razonable según [BS.1770](https://www.itu.int/rec/R-REC-BS.1770/en).
  - La forma de onda de la barra de búsqueda viene de esa misma escucha. Hasta que una canción se ha oído entera una vez, su barra es gris lisa.
  - Saltar o buscar durante esa primera escucha la cancela — lo volverá a intentar la próxima vez.
  - Estos metadatos están en un `.songname.rgcache` oculto. No puedes desactivar el cálculo; sí puedes desactivar los ajustes de volumen en ajustes.
- ARK 12 cubre una buena parte de Unicode, pero no es perfecta.
- Reabrir la última canción reproducida al arrancar. No es reproducción automática.
- Fundido de 3 segundos al retirar el medio.
- Crossfade configurable.
- De verdad hace caso a los datos de reproducción sin huecos (gapless).
- Radio por Internet vía la API de <https://www.radio-browser.info>, o por lista manual. Esto es territorio de "atado a la corriente".
  - Flujos MP3, AAC y Ogg, y HLS (`.m3u8`) con AAC o MP3 en segmentos en claro o MPEG-TS. Todavía no: HLS cifrado o fMP4, ni los enlaces `.pls`/`.m3u` que sirve una emisora.
- Títulos y nombres de emisora en árabe, unidos y dispuestos de derecha a izquierda, contra el margen derecho.
- Temporizador de apagado (hasta 2 horas, en pasos de 15 minutos).
- Apagado por inactividad: tras 15 min a 2 h sin reproducir, grabar ni tocar — independiente del temporizador. `IDLE` en la línea de estado está en verde mientras cuenta atrás, y en amarillo mientras algo lo retiene; el botón lateral vuelve a encenderlo.
- Protección de batería baja: por debajo de 6,3 V durante 30 s con la batería, termina cualquier grabación y se apaga solo, antes de dejar el pack tan vacío como para dañarlo. Aún no probado con una descarga real.
- Control de brillo.
- Protocolo de Tiempo de Red (NTP) (si el Wi-Fi está configurado).
- El reloj con respaldo de batería mantiene la hora tras un apagado, así las grabaciones se nombran bien sin Wi-Fi; NTP la corrige cuando puede.
- Configuración de Wi-Fi por portal cautivo (varios routers).
- Alta manual de emisoras de radio por Internet desde el portal web de Wi-Fi.
- Favoritos (Starred Favorites).
- Adaptadores Ethernet por USB: CDC-ECM (Realtek RTL8152 probado; RTL8153 misma ruta, sin probar) y la adaptación de [ASIX AX88772](https://github.com/Sudrien/esp_usbh_asix), ambos confirmados en la placa.
- Hojas Cue.
- m3u/m3u8 — al menos a través de MPD.
- Ah, sí, compatibilidad con [Music Player Daemon](https://mpd.readthedocs.io/en/stable/user.html) — sí, esto debería significar también control desde Home Assistant.
- Interfaz web por HTTPS.
- Grabación de audio, ya que el hardware está ahí mismo, en FLAC.
  - con un medidor de nivel mientras graba.
- una línea de estado bajo el volumen: alimentación USB, MPD, HTTPS y los minutos que quedan del temporizador. Verde es encendido.
- Ay, Dios, miré la lista de apps de M5Launcher; arreglemos ya el orden del título.
- Idiomas de la interfaz (English, 简体中文, 日本語, Español), se eligen en el engranaje de Ajustes >> pestaña BUILD.
- Compatibilidad con [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher)
  - salir de vuelta al launcher
  - importación de credenciales Wi-Fi

## Lo que podría pasar

- Más manejo de fallos y catástrofes; oye, siempre puedes conectar `idf.py monitor` y ver qué sale.
- ¿Un descargador de pódcast por Wi-Fi? Concebible. Querría soporte de capítulos.
  - hay tantísimo. Tantísimo.

## Lo que no puede pasar con el código publicado actual

- Soporte de adaptadores Bluetooth clásico.
- Reanudación por archivo.
- Hubs USB.
  - ¿Puede saber que has enchufado uno? Sí.
  - ¿Puede usar lo que enchufes en ellos? Probablemente no.
  - ¿Te salvará uno si tu dispositivo consume lo suficiente como para provocar una caída de tensión en el Tab5? Eh. Define "salvar".
  - Las bibliotecas no admiten varias velocidades de dispositivo, y no puedo decirte la velocidad de un dispositivo cualquiera.
- Los archivos con DRM no son viables.
- DSD y APE requieren demasiado procesamiento.

## Problemas potenciales

- Las redes Wi-Fi guardadas y el certificado del control remoto viven en la NVS de la placa, espacio de nombres `defeatist`. Bajo un launcher de tarjeta SD, cada firmware de la placa comparte esa partición: otra app puede leerlos (aquí la NVS no está cifrada), y una que borre la NVS se los lleva con ella. Los ajustes vuelven desde la tarjeta; las redes y el certificado no.

- Cargar desde un USB-C con poca potencia + batería puesta + pantalla encendida puede producir lo que parece un zumbido del altavoz, pero no lo es. Consigue un mejor cable USB, un mejor hub, una conexión directa al cargador. Vas escaso de amperios.
- La selección de archivos es algo más lenta de lo que me gustaría, porque seleccionar la primera canción bajo tu pulgar no es lo que quieres.
- Los cables AUX no están necesariamente lo bastante apantallados contra todo lo que puedas tener alrededor. Aplica el "aleja más el móvil" del electromagnetismo.

## Licencias

- Este código es MIT.
- minimp3 es CC0/dominio público. Sin obligación de atribución; aun así se incluye en el árbol (vendored) para que la fuente sea auditable.
- esp_audio_codec se distribuye como **archivos precompilados** bajo la licencia ESPRESSIF MIT. Gratis, pero la concesión se limita al silicio de Espressif. Aquí está bien; conviene saberlo antes de que este código se copie a otra parte.
- pngle y miniz son MIT.
- **TJpgDec es de ChaN, bajo su propia licencia**, y llega como el componente `espressif/esp_jpeg`. Permisiva — gratis para uso personal y comercial, se permite redistribuir la fuente — pero hay que conservar el aviso de copyright, así que viaja con una redistribución igual que lo hace la OFL de la fuente. Se usa solo como reserva, para las carátulas que el decodificador JPEG por hardware no puede reservar en memoria: el decodificador del P4 no tiene escalador, así que una carátula de 3000 px quiere 17 MB de PSRAM en un solo bloque y no los consigue.
- **La fuente Ark Pixel es SIL OFL-1.1, y por tanto `components/ark12` también es OFL-1.1, no MIT.** Convertir los PNG de glifos en arrays de C hace que esos archivos sean una Versión Modificada según la sección 5 de la OFL, y la sección 5 exige que las Versiones Modificadas sigan bajo la OFL. Esto no es un problema — la OFL permite explícitamente empaquetar con software bajo cualquier licencia, y solo los archivos de fuente quedan sujetos — pero `components/ark12/LICENSE-OFL` debe acompañar cualquier redistribución, incluida una imagen de firmware, y las tablas no deben venderse por separado. Ark no declara Nombre de Fuente Reservado (Reserved Font Name), así que la derivada no tuvo que renombrarse; se llama `ark12` de todos modos, porque no es la Versión Original.
- **Arabixel Basic es CC BY 4.0** (ArabianDev, <https://arabiandev.itch.io/arabixel-basic-font>), y también lo son el archivo de fuente y la tabla generada de `components/arabixel`; su código de shaping es MIT. CC BY pide crédito, un enlace a la licencia y una nota de los cambios — el README del componente y la cabecera de la tabla los llevan, y una redistribución, imagen de firmware incluida, conserva `components/arabixel/LICENSE.txt`.
- MurmurHash2 es de dominio público.

## Un último insulto

- waveflow for tab5 music player
  - hasta puedes generar con IA un logo para el dispositivo correcto
  - ¿le dices a la gente que descargue ffmpeg y LUEGO un script de conversión????????? ¿cuando puede que ni siquiera tengan Python para empezar???? shmusica, por Dios
  - por cierto, eso se llama "transcodificar"
  - Tu falta de trabajo me aseguró que hay capas en el vibe coding
