#include <Arduino_GFX_Library.h>
#include "ejemplos.h"   // reemplaza por tus fotos (ver README)

// Pantalla redonda GC9A01 1.28" 240x240
// Conexion: SCL=18, SDA=23, CS=5, DC=16, RES=4, VCC=3.3V, GND=GND

// DC, CS, SCK, MOSI, MISO
Arduino_DataBus *bus = new Arduino_ESP32SPI(16, 5, 18, 23, GFX_NOT_DEFINED);
// bus, RST, rotacion, IPS
Arduino_GFX *gfx = new Arduino_GC9A01(bus, 4, 0, true);

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
