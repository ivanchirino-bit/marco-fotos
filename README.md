# 🖼️ Marco de fotos ESP32

Marco de fotos digital con un **ESP32** y una pantalla de **240×240** (cuadrada ST7789 de 1.54" o redonda GC9A01 de 1.28").
Muestra fotos, **stickers animados de WhatsApp**, **videos cortos** y la **hora**, y se administra **por WiFi desde el celular**, sin cables ni Arduino IDE.

## Qué hace

- **Carrusel** de fotos con transición de **fundido**.
- **Animaciones:** stickers WebP de WhatsApp, GIF y videos (los primeros 6 segundos), a unos 15 cuadros por segundo.
- **Reloj:** al terminar cada vuelta muestra la hora y la fecha (horario de Chile, por internet) durante 20 segundos (configurable).
- **Página web** para agregar, encuadrar, ordenar y borrar fotos desde el celular o el computador.
- **Brillo** ajustable (pantalla cuadrada).
- Las fotos quedan guardadas en el marco: **funciona sin WiFi** una vez cargadas.

## Contenido del repositorio

| Carpeta / archivo | Qué es |
|---|---|
| `marco_wifi/` | **Programa principal.** El marco guarda las fotos en su memoria y sirve la página por WiFi. |
| `index.html` | La página para preparar fotos. Es la misma que trae el marco; publicada en GitHub Pages sirve para las opciones por cable y microSD. |
| `foto_cuadrada/` | Carrusel simple por cable para la pantalla cuadrada (con 2 imágenes de ejemplo). |
| `foto_atardecer/` | Carrusel simple por cable para la pantalla redonda (con 2 imágenes de ejemplo). |
| `Subir fotos al marco.command` | Programa para Mac: sube por cable el sketch que genera la página, sin abrir Arduino IDE. |

## Materiales

- ESP32 Dev Module (ESP32 clásico, por ejemplo con chip USB CH340).
- Pantalla 240×240: **ST7789 1.54"** (cuadrada) o **GC9A01 1.28"** (redonda).
- Cables dupont.

## Conexiones

Las dos pantallas usan los mismos pines; la cuadrada tiene además **BLK** (luz de fondo).

| Pin pantalla | Función | ESP32 |
|---|---|---|
| GND | Tierra | GND |
| VCC | Alimentación | **3.3V** (nunca 5V) |
| SCL | Reloj SPI (SCK) | GPIO 18 |
| SDA | Datos SPI (MOSI) | GPIO 23 |
| RES | Reinicio | GPIO 4 |
| DC | Dato / comando | GPIO 16 |
| CS | Selección de chip | GPIO 5 |
| BLK *(solo cuadrada)* | Luz de fondo (brillo y fundido) | GPIO 22 |

> El orden de los pines cambia según el fabricante: guíate por lo impreso en la placa de la pantalla.
> La redonda de 7 pines no tiene BLK: su luz queda siempre encendida y el brillo no se puede cambiar.

## Instalación (versión WiFi)

1. Instala **Arduino IDE 2** y el soporte para **ESP32** (Boards Manager → *esp32* de Espressif).
2. Instala las librerías (Library Manager):
   - **GFX Library for Arduino** (moononournation)
   - **JPEGDEC** (Larry Bank)
3. Abre `marco_wifi/marco_wifi.ino` y elige en **Tools**:
   - Board: **ESP32 Dev Module**
   - Partition Scheme: **No OTA (2MB APP/2MB SPIFFS)**
4. Sube el programa con la flecha →.

## Primer uso: conectar el WiFi

1. La pantalla muestra **"Configura WiFi"**.
2. Conecta el celular a la red **MarcoFotos** (clave `marco1234`). Se abre sola la página de configuración; si no, entra a **http://192.168.4.1**.
3. Elige la red de tu casa en la lista, escribe la clave y toca **Guardar y conectar**.
4. El marco se reinicia, se conecta y muestra su dirección.
5. Con el celular de vuelta en tu red, abre **http://marco.local** (o la IP que muestra la pantalla).

> Solo funciona con redes de **2.4 GHz**. Si cambias de casa o de red, el marco no encuentra su WiFi guardado y vuelve a crear la red **MarcoFotos** para configurarlo de nuevo.
> La clave `marco1234` de la red de configuración se puede cambiar en `AP_CLAVE` dentro de `marco_wifi.ino`.

## Uso diario

En **http://marco.local**:

1. **Agregar fotos, stickers o videos** desde la galería o el computador.
2. **Encuadrar:** arrastra para mover y usa el zoom. La vista previa se ve redonda o cuadrada según la pantalla.
3. **Enviar fotos nuevas al marco.** Aparecen al instante.
4. En **Fotos guardadas en el marco** puedes reordenar (▲▼ + *Guardar nuevo orden*) o borrar (🗑).
5. En **Pantalla y ajustes** cambia segundos por foto, brillo, transición y el tiempo del reloj, y presiona **Guardar ajustes en el marco**.

Caben unas **16 fotos** (cada una ocupa ~115 KB; un sticker animado ocupa más o menos lo mismo que una foto).

**Fuera de casa:** el marco sigue mostrando sus fotos sin WiFi. Para cambiarlas, conecta el celular a **MarcoFotos** y abre **http://192.168.4.1/editor**.

## Otras formas de cargar fotos

### Por cable (sin WiFi)

1. En la página, pestaña **Por cable** → **Descargar sketch (.zip)** (hasta 22 fotos).
2. Con el ESP32 conectado por USB:
   - **En Mac:** doble clic en `Subir fotos al marco.command` (usa el `marco_cable` más reciente de Descargas y lo sube solo).
     Si lo descargaste de GitHub, dale permiso una vez en Terminal: `chmod +x "Subir fotos al marco.command"`; la primera vez ábrelo con clic derecho → Abrir.
   - **En Arduino IDE:** abre `marco_cable/marco_cable.ino`, Partition Scheme **Huge APP (3MB No OTA/1MB SPIFFS)** y sube.

### Carruseles simples (`foto_cuadrada` / `foto_atardecer`)

Traen 2 imágenes de ejemplo en `ejemplos.h`. Para usar tus fotos, genera los archivos con la pestaña **Por cable** de la página, o reemplaza los arreglos de `ejemplos.h` (240×240, RGB565). Partition Scheme **Huge APP**.

## Detalles técnicos

- **Formato de foto:** 240×240 RGB565 crudo (115.200 bytes), guardado en LittleFS.
- **Formato de animación (`.anm`):** `"ANM1"` + cantidad de cuadros (uint16) + ms por cuadro (uint16) + posiciones (uint32 × cuadros+1) + cada cuadro como JPG 240×240. El ESP32 los decodifica con JPEGDEC.
- **Stickers WebP:** la página los separa en cuadros con su propio lector, porque `ImageDecoder` del navegador no está disponible en páginas http (como la del marco).
- **Hora:** NTP (`pool.ntp.org`) con zona horaria de Chile continental, incluido el horario de verano.
- **API del marco:** `GET /api/info`, `GET /api/foto?id=`, `POST /api/fotos` (multipart), `POST /api/borrar?id=`, `POST /api/orden`, `POST /api/config`, `POST /api/wifi`.

## Créditos

- Pantalla: [GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX).
- JPG: [JPEGDEC](https://github.com/bitbank2/JPEGDEC).
- Fuentes FreeSansBold de [Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library) (licencia BSD).
- Conversión de fotos HEIC en el navegador: [heic2any](https://github.com/alexcorvi/heic2any).

## Próximos pasos

- [ ] Versión portátil: batería LiPo + módulo cargador con salida 5V, interruptor de apagado y botón para dormir.
- [ ] Carcasa impresa en 3D.
- [ ] Lector microSD para guardar cientos de fotos.
