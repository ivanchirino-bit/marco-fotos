#include <Arduino_GFX_Library.h>
#include "ejemplos.h"   // reemplaza por tus fotos (ver README)

// Pantalla cuadrada ST7789 1.54" 240x240
// Conexion: SCL=18, SDA=23, RES=4, DC=16, CS=5 (si tiene), BLK=GPIO 22, VCC=3.3V, GND=GND
// Si tu pantalla NO tiene pin CS, cambia el 5 por GFX_NOT_DEFINED en la linea del bus

// DC, CS, SCK, MOSI, MISO
Arduino_DataBus *bus = new Arduino_ESP32SPI(16, 5, 18, 23, GFX_NOT_DEFINED);
// bus, RST, rotacion, IPS, ancho, alto, offsets (necesarios en la ST7789 240x240)
Arduino_GFX *gfx = new Arduino_ST7789(bus, 4, 0, true, 240, 240, 0, 0, 0, 80);

// Lista de fotos (estan en ejemplos.h)
const uint16_t *fotos[] = { ejemplo1, ejemplo2 };
const int totalFotos = 2;
const int segundosPorFoto = 5;

// Transicion (fundido) entre fotos
const int pasosFundido = 32;   // mas pasos = fundido mas lento y suave
const int filasPorBloque = 24; // filas que se dibujan de una vez
uint16_t bloque[240 * 24];

// Mezcla dos colores RGB565. t va de 0 (todo a) a 32 (todo b)
uint16_t mezclar(uint16_t a, uint16_t b, uint8_t t) {
  uint16_t r = (((a >> 11) & 0x1F) * (32 - t) + ((b >> 11) & 0x1F) * t) >> 5;
  uint16_t g = (((a >> 5) & 0x3F) * (32 - t) + ((b >> 5) & 0x3F) * t) >> 5;
  uint16_t bl = ((a & 0x1F) * (32 - t) + (b & 0x1F) * t) >> 5;
  return (r << 11) | (g << 5) | bl;
}

void fundido(const uint16_t *desde, const uint16_t *hacia) {
  for (int paso = 1; paso <= pasosFundido; paso++) {
    uint8_t t = paso * 32 / pasosFundido;
    for (int y = 0; y < 240; y += filasPorBloque) {
      int inicio = y * 240;
      for (int k = 0; k < 240 * filasPorBloque; k++) {
        bloque[k] = mezclar(desde[inicio + k], hacia[inicio + k], t);
      }
      gfx->draw16bitRGBBitmap(0, y, bloque, 240, filasPorBloque);
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(22, OUTPUT);
  digitalWrite(22, HIGH);   // enciende la luz de fondo (BLK en GPIO 22)
  if (!gfx->begin()) {
    Serial.println("Error iniciando la pantalla");
  }
  gfx->fillScreen(0x0000);
  gfx->draw16bitRGBBitmap(0, 0, fotos[0], 240, 240);
}

void loop() {
  for (int i = 0; i < totalFotos; i++) {
    delay(segundosPorFoto * 1000);
    int siguiente = (i + 1) % totalFotos;
    fundido(fotos[i], fotos[siguiente]);
  }
}
