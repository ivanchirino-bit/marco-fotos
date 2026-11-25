// =====================================================================
//  Marco de fotos ESP32 - version WiFi
//
//  El ESP32 guarda las fotos en su memoria interna (LittleFS) y sirve
//  la pagina "Marco de fotos ESP32" para subirlas desde el celular o el Mac.
//
//  Primera vez: crea la red WiFi "MarcoFotos" (clave marco1234).
//  Conectate a esa red y abre http://192.168.4.1 para elegir tu WiFi.
//  Despues: abre http://marco.local (o la IP que muestra la pantalla).
//
//  Arduino IDE: placa "ESP32 Dev Module",
//  Partition Scheme "No OTA (2MB APP/2MB SPIFFS)".
//  Librerias: GFX Library for Arduino, JPEGDEC, ArduinoJson.
//
//  Conexion pantalla: SCL=18, SDA=23, RES=4, DC=16, CS=5,
//  BLK=22 (cuadrada), VCC=3.3V, GND=GND
// =====================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <vector>
#include <Arduino_GFX_Library.h>
#include <JPEGDEC.h>        // libreria JPEGDEC (Larry Bank), para las animaciones
#include <time.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>   // libreria ArduinoJson (Benoit Blanchon), para Telegram
#include "FreeSansBold24pt7b.h"
#include "FreeSansBold12pt7b.h"
#include "pagina.h"

const int PIN_BLK = 22;
const char *AP_NOMBRE = "MarcoFotos";
const char *AP_CLAVE = "marco1234";
const char *NOMBRE_RED = "marco";          // http://marco.local
const size_t BYTES_FOTO = 240 * 240 * 2;   // 115200

Arduino_DataBus *bus = new Arduino_ESP32SPI(16, 5, 18, 23, GFX_NOT_DEFINED);
Arduino_GFX *gfx = nullptr;

WebServer server(80);
DNSServer dns;
Preferences prefs;

// Ajustes (se guardan en la memoria del ESP32)
int cfgPantalla = 1;     // 1 = cuadrada ST7789, 2 = redonda GC9A01
int cfgSegundos = 5;
int cfgBrillo = 255;     // 0 a 255
int cfgTransicion = 1;   // 1 = fundido, 0 = directo
int cfgReloj = 20;       // segundos que se muestra la hora al terminar cada vuelta (0 = no mostrar)

// Hora de Chile continental (UTC-4, y UTC-3 en horario de verano)
const char *ZONA_HORARIA = "<-04>4<-03>,M9.1.6/24,M4.1.6/24";
bool enReloj = false;
int ultimoMinutoReloj = -1;

bool modoAP = false;
String ssidGuardado;
// Telegram
String tgToken, tgIds, tgBot;
long tgOffset = 0;
unsigned long tgProximo = 0;
const unsigned long TG_CADA_MS = 15000;
// Clima (Open-Meteo)
int cfgClima = 10;            // segundos de la pantalla de clima despues de la hora (0 = no mostrar)
bool enClima = false;
String climaCiudad, climaLat, climaLon;
bool climaOk = false;
float climaTemp = 0, climaMax = 0, climaMin = 0;
int climaCodigo = 0, climaDia = 1, climaLluvia = 0;
unsigned long climaHora = 0, climaProximo = 0;
const unsigned long CLIMA_CADA_MS = 30UL * 60UL * 1000UL;
std::vector<String> redesEncontradas;   // redes WiFi detectadas al iniciar
std::vector<String> orden;   // ids de las fotos, en orden
int indiceActual = -1;
unsigned long proximoCambio = 0;
bool mostrandoMensaje = false;
uint16_t bloque[240 * 20];

// Animaciones (.anm): "ANM1", cuadros (uint16), ms por cuadro (uint16),
// posiciones (uint32 x cuadros+1) y luego cada cuadro como JPG de 240x240.
// Las fotos tienen id numerico ("12" -> /f/12.bin) y las animaciones
// empiezan con "a" ("a13" -> /f/a13.anm).
JPEGDEC jpeg;
const size_t MAX_JPG = 48 * 1024;
uint8_t *bufJpg = nullptr;
File animArchivo;
uint32_t *animOffs = nullptr;
uint16_t animCuadros = 0, animMs = 80, animCuadro = 0;
uint32_t animBase = 0;
unsigned long animProximo = 0;
bool animActiva = false;

File archivoSubida;
size_t bytesSubidos = 0;
String idSubida, errorSubida;
bool subidaOk = false;
unsigned long reiniciarEn = 0;

// ---------------------------------------------------------------------
// Pantalla y brillo
// ---------------------------------------------------------------------
void ponerBrillo(int v) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_BLK, v);
#else
  ledcWrite(0, v);
#endif
}

void iniciarPantalla() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_BLK, 5000, 8);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_BLK, 0);
#endif
  ponerBrillo(cfgBrillo);
  if (cfgPantalla == 2) {
    gfx = new Arduino_GC9A01(bus, 4, 0, true);
  } else {
    gfx = new Arduino_ST7789(bus, 4, 0, true, 240, 240, 0, 0, 0, 80);
  }
  if (!gfx->begin()) Serial.println("Error iniciando la pantalla");
  gfx->fillScreen(0x0000);
}

// Muestra varias lineas de texto centradas (sirve para la redonda tambien)
void mensaje(const std::vector<String> &lineas) {
  gfx->fillScreen(0x0000);
  gfx->setTextSize(2);
  int alto = 22;
  int y = 120 - (int)lineas.size() * alto / 2;
  for (size_t i = 0; i < lineas.size(); i++) {
    const String &t = lineas[i];
    int x = (240 - (int)t.length() * 12) / 2;
    if (x < 4) x = 4;
    gfx->setTextColor(i == 0 ? 0xFD20 : 0xFFFF);   // primera linea en naranjo
    gfx->setCursor(x, y + i * alto);
    gfx->print(t);
  }
  mostrandoMensaje = true;
}

void mensajeSinFotos() {
  String ip = modoAP ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  mensaje({"Sin fotos", "", "Abre en el", "navegador:", "marco.local", "o " + ip});
}

void mensajeModoAP() {
  mensaje({"Configura WiFi", "", "Conectate a", "la red WiFi", "MarcoFotos", "clave marco1234", "y abre", "192.168.4.1"});
}

bool esAnim(const String &id) { return id.startsWith("a"); }
String rutaDe(const String &id) { return "/f/" + id + (esAnim(id) ? ".anm" : ".bin"); }

int dibujarBloqueJpg(JPEGDRAW *p) {
  gfx->draw16bitRGBBitmap(p->x, p->y, p->pPixels, p->iWidth, p->iHeight);
  return 1;
}

void detenerAnim() {
  if (animArchivo) animArchivo.close();
  if (animOffs) { free(animOffs); animOffs = nullptr; }
  animActiva = false;
}

bool dibujarCuadroAnim() {
  uint32_t ini = animOffs[animCuadro], fin = animOffs[animCuadro + 1];
  uint32_t largo = fin - ini;
  if (largo == 0 || largo > MAX_JPG) return false;
  animArchivo.seek(animBase + ini);
  if (animArchivo.read(bufJpg, largo) != largo) return false;
  if (!jpeg.openRAM(bufJpg, largo, dibujarBloqueJpg)) return false;
  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  jpeg.decode(0, 0, 0);
  jpeg.close();
  return true;
}

bool iniciarAnim(const String &id) {
  detenerAnim();
  if (!bufJpg) bufJpg = (uint8_t *)malloc(MAX_JPG);
  if (!bufJpg) { Serial.println("[marco] Sin memoria para animaciones"); return false; }
  animArchivo = LittleFS.open(rutaDe(id), "r");
  if (!animArchivo) return false;
  uint8_t cab[8];
  if (animArchivo.read(cab, 8) != 8 || memcmp(cab, "ANM1", 4) != 0) { detenerAnim(); return false; }
  animCuadros = cab[4] | (cab[5] << 8);
  animMs = cab[6] | (cab[7] << 8);
  if (animCuadros == 0 || animCuadros > 600) { detenerAnim(); return false; }
  if (animMs < 30) animMs = 30;
  animOffs = (uint32_t *)malloc((animCuadros + 1) * 4);
  if (!animOffs) { detenerAnim(); return false; }
  animArchivo.read((uint8_t *)animOffs, (animCuadros + 1) * 4);
  animBase = 8 + (animCuadros + 1) * 4;
  animCuadro = 0;
  animActiva = true;
  dibujarCuadroAnim();
  animProximo = millis() + animMs;
  return true;
}

void avanzarAnim() {
  if (!animActiva || millis() < animProximo) return;
  animCuadro = (animCuadro + 1) % animCuadros;
  dibujarCuadroAnim();
  animProximo += animMs;
  if (millis() > animProximo + 500) animProximo = millis() + animMs;   // si se atraso, no acumular
}

bool dibujarFoto(const String &id) {
  detenerAnim();
  if (esAnim(id)) {
    bool ok = iniciarAnim(id);
    mostrandoMensaje = false;
    return ok;
  }
  File f = LittleFS.open(rutaDe(id), "r");
  if (!f) return false;
  if (f.size() != BYTES_FOTO) { f.close(); return false; }
  for (int y = 0; y < 240; y += 20) {
    f.read((uint8_t *)bloque, sizeof(bloque));
    gfx->draw16bitRGBBitmap(0, y, bloque, 240, 20);
  }
  f.close();
  mostrandoMensaje = false;
  return true;
}

// ---------------------------------------------------------------------
// Reloj (se muestra al terminar cada vuelta de fotos)
// ---------------------------------------------------------------------
bool horaValida() { return time(nullptr) > 1700000000; }

void textoCentrado(const String &t, int y, const GFXfont *fuente, uint16_t color) {
  gfx->setFont(fuente);
  gfx->setTextSize(1);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(t, 0, y, &x1, &y1, &w, &h);
  gfx->setTextColor(color);
  gfx->setCursor((240 - (int)w) / 2 - x1, y);
  gfx->print(t);
}

void dibujarReloj() {
  static const char *DIAS[] = {"Domingo", "Lunes", "Martes", "Miercoles", "Jueves", "Viernes", "Sabado"};
  static const char *MESES[] = {"enero", "febrero", "marzo", "abril", "mayo", "junio", "julio",
                                "agosto", "septiembre", "octubre", "noviembre", "diciembre"};
  time_t ahora = time(nullptr);
  struct tm t;
  localtime_r(&ahora, &t);
  char hora[6];
  snprintf(hora, sizeof(hora), "%02d:%02d", t.tm_hour, t.tm_min);
  gfx->fillScreen(0x0000);
  textoCentrado(DIAS[t.tm_wday], 82, &FreeSansBold12pt7b, 0xFD20);                     // naranjo
  textoCentrado(hora, 140, &FreeSansBold24pt7b, 0xFFFF);                               // blanco
  textoCentrado(String(t.tm_mday) + " de " + MESES[t.tm_mon], 180, &FreeSansBold12pt7b, 0xBDF7);  // gris
  gfx->setFont((const GFXfont *)nullptr);   // vuelve a la letra normal para los mensajes
  ultimoMinutoReloj = t.tm_min;
}

void actualizarReloj() {
  if (!enReloj) return;
  time_t ahora = time(nullptr);
  struct tm t;
  localtime_r(&ahora, &t);
  if (t.tm_min != ultimoMinutoReloj) dibujarReloj();
}

// Cambia a la foto siguiente. En la pantalla cuadrada hace un fundido a negro
// usando la luz de fondo, asi no necesita guardar dos fotos en memoria.
void mostrarSiguiente() {
  if (orden.empty()) {
    if (!mostrandoMensaje) { if (modoAP) mensajeModoAP(); else mensajeSinFotos(); }
    return;
  }
  bool fundido = cfgTransicion == 1 && cfgPantalla == 1 && !mostrandoMensaje;

  // Al terminar la vuelta (despues de la ultima foto) se muestra la hora
  bool finVuelta = indiceActual >= 0 && indiceActual == (int)orden.size() - 1;
  // y despues el clima (pantalla aparte)
  if (finVuelta && !enClima) {
    int especial = 0;
    if (!enReloj && cfgReloj > 0 && horaValida()) especial = 1;
    else if (cfgClima > 0 && climaListo()) especial = 2;
    if (especial) {
      if (fundido) { for (int i = 12; i >= 0; i--) { ponerBrillo(cfgBrillo * i / 12); delay(18); } }
      detenerAnim();
      enReloj = especial == 1;
      enClima = especial == 2;
      if (enReloj) dibujarReloj(); else dibujarClima();
      if (fundido) { for (int i = 0; i <= 12; i++) { ponerBrillo(cfgBrillo * i / 12); delay(18); } }
      else ponerBrillo(cfgBrillo);
      return;
    }
  }
  enReloj = false;
  enClima = false;

  indiceActual = (indiceActual + 1) % orden.size();
  if (fundido) {
    for (int i = 12; i >= 0; i--) { ponerBrillo(cfgBrillo * i / 12); delay(18); }
  }
  dibujarFoto(orden[indiceActual]);
  if (fundido) {
    for (int i = 0; i <= 12; i++) { ponerBrillo(cfgBrillo * i / 12); delay(18); }
  } else {
    ponerBrillo(cfgBrillo);
  }
}

// ---------------------------------------------------------------------
// Ajustes y orden de fotos
// ---------------------------------------------------------------------
void cargarAjustes() {
  prefs.begin("marco", false);
  cfgPantalla = prefs.getInt("pantalla", 1);
  cfgSegundos = prefs.getInt("segundos", 5);
  cfgBrillo = prefs.getInt("brillo", 255);
  cfgTransicion = prefs.getInt("transicion", 1);
  cfgReloj = prefs.getInt("reloj", 20);
  ssidGuardado = prefs.getString("ssid", "");
  tgToken = prefs.getString("tgToken", "");
  tgBot = prefs.getString("tgBot", "");
  tgIds = prefs.getString("tgIds", "");
  cfgClima = prefs.getInt("clima", 10);
  climaCiudad = prefs.getString("climaCiudad", "");
  climaLat = prefs.getString("climaLat", "");
  climaLon = prefs.getString("climaLon", "");
}

void guardarOrden() {
  String s;
  for (size_t i = 0; i < orden.size(); i++) {
    if (i) s += ",";
    s += orden[i];
  }
  prefs.putString("orden", s);
}

void cargarOrden() {
  orden.clear();
  String s = prefs.getString("orden", "");
  int inicio = 0;
  while (inicio < (int)s.length()) {
    int coma = s.indexOf(',', inicio);
    if (coma < 0) coma = s.length();
    String id = s.substring(inicio, coma);
    id.trim();
    if (id.length() && LittleFS.exists(rutaDe(id))) orden.push_back(id);
    inicio = coma + 1;
  }
}

String jsonTexto(const String &t) {
  String r = "\"";
  for (size_t i = 0; i < t.length(); i++) {
    char c = t[i];
    if (c == '"' || c == '\\') { r += '\\'; r += c; }
    else if ((uint8_t)c >= 0x20) r += c;
  }
  return r + "\"";
}

// ---------------------------------------------------------------------
// Servidor web
// ---------------------------------------------------------------------
void enviarPagina() {
  server.send_P(200, "text/html; charset=utf-8", PAGINA);
}

void apiInfo() {
  String j = "{\"marco\":true";
  j += ",\"pantalla\":" + String(cfgPantalla);
  j += ",\"segundos\":" + String(cfgSegundos);
  j += ",\"brillo\":" + String(cfgBrillo);
  j += ",\"transicion\":" + String(cfgTransicion);
  j += ",\"reloj\":" + String(cfgReloj);
  j += ",\"horaValida\":" + String(horaValida() ? "true" : "false");
  j += ",\"total\":" + String((unsigned long)LittleFS.totalBytes());
  j += ",\"usado\":" + String((unsigned long)LittleFS.usedBytes());
  j += ",\"modoAP\":" + String(modoAP ? "true" : "false");
  j += ",\"ssid\":" + jsonTexto(ssidGuardado);
  j += ",\"ip\":" + jsonTexto(modoAP ? WiFi.softAPIP().toString() : WiFi.localIP().toString());
  j += ",\"telegram\":{\"activo\":" + String(tgToken.length() ? "true" : "false") + ",\"bot\":" + jsonTexto(tgBot) + ",\"ids\":" + jsonTexto(tgIds) + "}";
  j += ",\"clima\":{\"ciudad\":" + jsonTexto(climaCiudad) + ",\"segundos\":" + String(cfgClima) +
       ",\"ok\":" + String(climaOk ? "true" : "false") + ",\"temp\":" + String(climaTemp, 1) +
       ",\"desc\":" + jsonTexto(climaDescripcion()) + "}";
  j += ",\"fotos\":[";
  for (size_t i = 0; i < orden.size(); i++) {
    if (i) j += ",";
    j += jsonTexto(orden[i]);
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void apiFoto() {
  String id = server.arg("id");
  File f = LittleFS.open(rutaDe(id), "r");
  if (!f) { server.send(404, "text/plain", "No existe"); return; }
  server.streamFile(f, "application/octet-stream");
  f.close();
}

void manejarSubida() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    subidaOk = false;
    errorSubida = "";
    bytesSubidos = 0;
    bool anim = up.filename.endsWith(".anm");
    size_t libre = LittleFS.totalBytes() - LittleFS.usedBytes();
    if (libre < (anim ? 65536 : BYTES_FOTO + 16384)) { errorSubida = "La memoria del marco esta llena"; return; }
    int sig = prefs.getInt("sig", 1);
    prefs.putInt("sig", sig + 1);
    idSubida = (anim ? "a" : "") + String(sig);
    archivoSubida = LittleFS.open(rutaDe(idSubida), "w");
    if (!archivoSubida) errorSubida = "No se pudo crear el archivo";
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (archivoSubida) {
      size_t w = archivoSubida.write(up.buf, up.currentSize);
      bytesSubidos += w;
      if (w != up.currentSize) errorSubida = "La memoria del marco esta llena";
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (archivoSubida) {
      archivoSubida.flush();
      archivoSubida.close();
      size_t t = bytesSubidos;   // size() del archivo abierto puede dar 0 antes de cerrarlo
      Serial.printf("[marco] Foto %s recibida: %u bytes\n", idSubida.c_str(), (unsigned)t);
      bool valido = false;
      if (errorSubida.length()) {
        valido = false;
      } else if (esAnim(idSubida)) {
        File v = LittleFS.open(rutaDe(idSubida), "r");
        uint8_t cab[4] = {0};
        if (v) { v.read(cab, 4); v.close(); }
        valido = t > 16 && memcmp(cab, "ANM1", 4) == 0;
        if (!valido) errorSubida = "Archivo de animacion invalido";
      } else {
        valido = t == BYTES_FOTO;
        if (!valido) errorSubida = "Tamano incorrecto (" + String((unsigned long)t) + " bytes)";
      }
      if (valido) {
        orden.push_back(idSubida);
        guardarOrden();
        subidaOk = true;
      } else {
        LittleFS.remove(rutaDe(idSubida));
      }
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (archivoSubida) {
      archivoSubida.close();
      LittleFS.remove(rutaDe(idSubida));
    }
    errorSubida = "Subida cancelada";
  }
}

void finSubida() {
  if (subidaOk) {
    server.send(200, "application/json", "{\"ok\":true,\"id\":" + jsonTexto(idSubida) + "}");
    if (mostrandoMensaje) proximoCambio = 0;   // muestra la primera foto de inmediato
  } else {
    server.send(500, "text/plain", errorSubida.length() ? errorSubida : "Error al subir");
  }
}

void apiBorrar() {
  String id = server.arg("id");
  for (size_t i = 0; i < orden.size(); i++) {
    if (orden[i] == id) {
      orden.erase(orden.begin() + i);
      if ((int)i <= indiceActual) indiceActual--;
      break;
    }
  }
  detenerAnim();   // por si se estaba mostrando
  LittleFS.remove(rutaDe(id));
  guardarOrden();
  proximoCambio = 0;
  if (orden.empty()) { indiceActual = -1; mostrandoMensaje = false; proximoCambio = 0; }
  server.send(200, "application/json", "{\"ok\":true}");
}

void apiOrden() {
  String s = server.arg("plain");
  std::vector<String> nuevo;
  int inicio = 0;
  while (inicio < (int)s.length()) {
    int coma = s.indexOf(',', inicio);
    if (coma < 0) coma = s.length();
    String id = s.substring(inicio, coma);
    id.trim();
    if (id.length() && LittleFS.exists(rutaDe(id))) nuevo.push_back(id);
    inicio = coma + 1;
  }
  // no perder fotos que no vinieron en la lista
  for (auto &id : orden) {
    bool esta = false;
    for (auto &n : nuevo) if (n == id) { esta = true; break; }
    if (!esta) nuevo.push_back(id);
  }
  orden = nuevo;
  guardarOrden();
  indiceActual = -1;
  proximoCambio = 0;
  server.send(200, "application/json", "{\"ok\":true}");
}

void apiConfig() {
  int pantallaAntes = cfgPantalla;
  if (server.hasArg("pantalla")) cfgPantalla = constrain(server.arg("pantalla").toInt(), 1, 2);
  if (server.hasArg("segundos")) cfgSegundos = constrain(server.arg("segundos").toInt(), 1, 3600);
  if (server.hasArg("brillo")) cfgBrillo = constrain(server.arg("brillo").toInt(), 0, 255);
  if (server.hasArg("transicion")) cfgTransicion = server.arg("transicion").toInt() ? 1 : 0;
  if (server.hasArg("reloj")) cfgReloj = constrain(server.arg("reloj").toInt(), 0, 600);
  prefs.putInt("reloj", cfgReloj);
  if (server.hasArg("clima")) cfgClima = constrain(server.arg("clima").toInt(), 0, 600);
  prefs.putInt("clima", cfgClima);
  prefs.putInt("pantalla", cfgPantalla);
  prefs.putInt("segundos", cfgSegundos);
  prefs.putInt("brillo", cfgBrillo);
  prefs.putInt("transicion", cfgTransicion);
  ponerBrillo(cfgBrillo);
  server.send(200, "application/json", "{\"ok\":true}");
  if (cfgPantalla != pantallaAntes) reiniciarEn = millis() + 1500;   // otra pantalla: reiniciar
}

void apiWifi() {
  String ssid = server.arg("ssid");
  ssid.trim();
  if (!ssid.length()) { server.send(400, "text/plain", "Falta el nombre de la red"); return; }
  Serial.println("[marco] Recibida config WiFi (editor) para: " + ssid);
  prefs.putString("ssid", ssid);
  prefs.putString("clave", server.arg("clave"));
  server.send(200, "application/json", "{\"ok\":true}");
  mensaje({"Guardado", "", "Conectando a", ssid.substring(0, 18), "", "Reiniciando..."});
  reiniciarEn = millis() + 1500;
}

String htmlEscapar(const String &t) {
  String r;
  for (size_t i = 0; i < t.length(); i++) {
    char c = t[i];
    if (c == '&') r += "&amp;";
    else if (c == '<') r += "&lt;";
    else if (c == '>') r += "&gt;";
    else if (c == '"') r += "&quot;";
    else r += c;
  }
  return r;
}

const char ESTILO[] PROGMEM = "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<style>body{font-family:-apple-system,Roboto,sans-serif;background:#f4f1ec;color:#1f2328;margin:0;padding:20px}"
  ".c{max-width:420px;margin:auto;background:#fff;border-radius:14px;padding:20px;border:1px solid #e5e1da}"
  "h1{font-size:1.3rem;margin:0 0 6px}p{color:#555}label{display:block;margin:14px 0 4px;font-size:.95rem}"
  "select,input{font-size:1rem;width:100%;padding:10px;border-radius:9px;border:1px solid #ccc;box-sizing:border-box}"
  "button{margin-top:18px;width:100%;padding:13px;font-size:1rem;border:0;border-radius:10px;background:#d9622b;color:#fff}"
  "a{color:#d9622b}.n{font-size:.85rem;color:#777;margin-top:14px}</style>";

// Pagina sencilla (sin JavaScript) para elegir el WiFi: funciona en la
// ventanita de "portal cautivo" que abre el celular al conectarse a MarcoFotos.
void paginaConfigWifi() {
  String h = "<!DOCTYPE html><html lang='es'><head><title>Configurar marco</title>";
  h += FPSTR(ESTILO);
  h += "</head><body><div class='c'><h1>&#128444;&#65039; Configurar WiFi del marco</h1>";
  h += "<p>Elige la red WiFi de tu casa y escribe su clave.</p>";
  h += "<form method='POST' action='/guardar-wifi'>";
  h += "<label for='ssid'>Red WiFi</label><select name='ssid' id='ssid'>";
  for (auto &r : redesEncontradas) {
    h += "<option value=\"" + htmlEscapar(r) + "\"";
    if (r == ssidGuardado) h += " selected";
    h += ">" + htmlEscapar(r) + "</option>";
  }
  h += "<option value=''>Otra (escribirla abajo)</option></select>";
  h += "<label for='otra'>Si no aparece, escribe el nombre:</label><input name='otra' id='otra' autocomplete='off' autocapitalize='none'>";
  h += "<label for='clave'>Clave del WiFi</label><input name='clave' id='clave' type='password' autocomplete='off'>";
  h += "<button type='submit'>Guardar y conectar</button></form>";
  h += "<p class='n'>Solo redes de 2.4 GHz (no las que dicen 5G).<br>";
  h += "Editor de fotos: <a href='/editor'>abrir editor completo</a></p></div></body></html>";
  server.send(200, "text/html; charset=utf-8", h);
}

void guardarWifiFormulario() {
  String ssid = server.arg("otra");
  ssid.trim();
  if (!ssid.length()) ssid = server.arg("ssid");
  ssid.trim();
  Serial.println("[marco] Recibida config WiFi para: " + ssid);
  String h = "<!DOCTYPE html><html lang='es'><head><title>Marco</title>";
  h += FPSTR(ESTILO);
  h += "</head><body><div class='c'>";
  if (!ssid.length()) {
    h += "<h1>Falta la red</h1><p>Elige una red o escribe su nombre.</p><p><a href='/'>Volver</a></p></div></body></html>";
    server.send(400, "text/html; charset=utf-8", h);
    return;
  }
  prefs.putString("ssid", ssid);
  prefs.putString("clave", server.arg("clave"));
  h += "<h1>&#9989; Guardado</h1><p>El marco se reinicia y se conecta a <b>" + htmlEscapar(ssid) + "</b>.</p>";
  h += "<p>1. Vuelve a conectar tu celular a <b>" + htmlEscapar(ssid) + "</b>.<br>";
  h += "2. Mira la pantalla del marco: mostrar&aacute; su direcci&oacute;n.<br>";
  h += "3. Abre <b>http://marco.local</b> o esa direcci&oacute;n.</p>";
  h += "<p class='n'>Si no logra conectarse, vuelve a aparecer la red MarcoFotos para intentarlo de nuevo.</p></div></body></html>";
  server.send(200, "text/html; charset=utf-8", h);
  mensaje({"Guardado", "", "Conectando a", ssid.substring(0, 18), "", "Reiniciando..."});
  reiniciarEn = millis() + 2500;
}

void inicio() {
  if (modoAP) paginaConfigWifi();
  else enviarPagina();
}

// ---------------------------------------------------------------------
// Telegram: recibir fotos desde cualquier lugar
// El marco consulta el bot cada 15 segundos (no hace falta abrir puertos
// en el router). El token y los IDs autorizados se guardan solo en el marco.
// ---------------------------------------------------------------------
WiFiClientSecure tgCliente;

String urlCodificar(const String &t) {
  String r;
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < t.length(); i++) {
    uint8_t c = t[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += (char)c;
    else { r += '%'; r += hex[c >> 4]; r += hex[c & 15]; }
  }
  return r;
}

// Llama a la API de Telegram y deja la respuesta (filtrada) en doc
bool tgLlamar(const String &metodo, const String &params, JsonDocument &doc, JsonDocument *filtro, const String &token) {
  HTTPClient http;
  tgCliente.setInsecure();   // sin validar certificado (ver README)
  String url = "https://api.telegram.org/bot" + token + "/" + metodo;
  if (params.length()) url += "?" + params;
  if (!http.begin(tgCliente, url)) { Serial.println("[telegram] " + metodo + ": no pude iniciar la conexion"); return false; }
  http.setConnectTimeout(4000);
  http.setTimeout(6000);
  int codigo = http.GET();
  if (codigo != 200) {
    Serial.printf("[telegram] %s: HTTP %d (%s) heap libre %u\n", metodo.c_str(), codigo,
                  codigo < 0 ? http.errorToString(codigo).c_str() : http.getString().substring(0, 120).c_str(), ESP.getFreeHeap());
    http.end(); return false;
  }
  String cuerpo = http.getString();
  http.end();
  DeserializationError e = filtro ? deserializeJson(doc, cuerpo, DeserializationOption::Filter(*filtro))
                                  : deserializeJson(doc, cuerpo);
  if (e) Serial.printf("[telegram] %s: JSON invalido (%s), %u bytes\n", metodo.c_str(), e.c_str(), cuerpo.length());
  return !e && doc["ok"].as<bool>();
}

void tgEnviar(const String &chat, const String &texto) {
  JsonDocument doc, filtro;
  filtro["ok"] = true;
  tgLlamar("sendMessage", "chat_id=" + chat + "&text=" + urlCodificar(texto), doc, &filtro, tgToken);
}

bool tgAutorizado(const String &chat) {
  String lista = "," + tgIds + ",";
  lista.replace(" ", "");
  return tgIds.length() && lista.indexOf("," + chat + ",") >= 0;
}

// --- Decodificar el JPG recibido y guardarlo recortado a 240x240 ---
File tgArchivo;
int tgRecorteX = 0, tgRecorteY = 0;

// La foto se escribe en orden, fila por fila (sin saltar dentro del archivo:
// en LittleFS eso duplica bloques y puede llenar la memoria).
// "bloque" guarda la franja de filas que JPEGDEC esta entregando.
int tgFranjaY = -1, tgFranjaAlto = 0, tgFilasEscritas = 0;
bool tgErrorEscritura = false;
void tgEscribirFila(const uint8_t *datos) {
  tgFilasEscritas++;   // siempre avanza, aunque falle la escritura (evita quedar pegado)
  if (tgErrorEscritura) return;
  if (tgArchivo.write(datos, 480) != 480) tgErrorEscritura = true;
}
void tgVaciarFranja() {
  static const uint8_t negro[480] = {0};
  for (int r = 0; r < tgFranjaAlto; r++) {
    int dy = tgFranjaY + r - tgRecorteY;
    if (dy < 0 || dy >= 240) continue;
    while (tgFilasEscritas < dy) tgEscribirFila(negro);
    if (tgFilasEscritas == dy) tgEscribirFila((uint8_t *)(bloque + r * 240));
  }
  tgFranjaY = -1;
  tgFranjaAlto = 0;
}
int tgGuardarBloque(JPEGDRAW *p) {
  if (p->y != tgFranjaY) {
    if (tgFranjaY >= 0) tgVaciarFranja();
    tgFranjaY = p->y;
    memset(bloque, 0, sizeof(bloque));
  }
  if (p->iHeight > tgFranjaAlto) tgFranjaAlto = min(p->iHeight, 20);
  for (int fila = 0; fila < p->iHeight && fila < 20; fila++) {
    int x0 = p->x - tgRecorteX, desde = 0, ancho = p->iWidth;
    if (x0 < 0) { desde = -x0; ancho -= desde; x0 = 0; }
    if (x0 + ancho > 240) ancho = 240 - x0;
    if (ancho <= 0) continue;
    memcpy(bloque + fila * 240 + x0, p->pPixels + fila * p->iWidth + desde, ancho * 2);
  }
  return tgErrorEscritura ? 0 : 1;
}
// Descarga la foto de Telegram, la recorta al centro y la agrega al marco
// Lectura del JPG desde LittleFS (asi no hace falta tenerlo entero en RAM)
File tgJpg;
void *tgJpgAbrir(const char *nombre, int32_t *largo) {
  tgJpg = LittleFS.open(nombre, "r");
  if (!tgJpg) return nullptr;
  *largo = tgJpg.size();
  return &tgJpg;
}
void tgJpgCerrar(void *h) { if (h) ((File *)h)->close(); }
int32_t tgJpgLeer(JPEGFILE *f, uint8_t *buf, int32_t n) { return ((File *)f->fHandle)->read(buf, n); }
int32_t tgJpgMover(JPEGFILE *f, int32_t pos) { return ((File *)f->fHandle)->seek(pos) ? pos : -1; }

const char *TG_TEMPORAL = "/tg.jpg";

String tgAgregarFoto(const String &fileId, int escala) {
  size_t libre = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (libre < BYTES_FOTO + 16384) return "La memoria del marco esta llena. Borra algunas fotos desde la pagina del marco.";

  JsonDocument doc, filtro;
  filtro["ok"] = true;
  filtro["result"]["file_path"] = true;
  if (!tgLlamar("getFile", "file_id=" + urlCodificar(fileId), doc, &filtro, tgToken)) return "No pude obtener la foto de Telegram.";
  String ruta = doc["result"]["file_path"].as<String>();

  HTTPClient http;
  tgCliente.setInsecure();
  if (!http.begin(tgCliente, "https://api.telegram.org/file/bot" + tgToken + "/" + ruta)) return "No pude descargar la foto.";
  http.setTimeout(15000);
  if (http.GET() != 200) { http.end(); return "No pude descargar la foto."; }
  int largo = http.getSize();
  if (largo > 400000) { http.end(); return "La foto es demasiado grande."; }
  if (largo > 0 && libre < BYTES_FOTO + (size_t)largo + 16384) { http.end(); return "La memoria del marco esta llena. Borra algunas fotos desde la pagina del marco."; }
  // Se guarda primero en un archivo temporal: mientras la conexion segura esta abierta queda poca RAM
  File tmp = LittleFS.open(TG_TEMPORAL, "w");
  if (!tmp) { http.end(); return "No pude guardar la foto."; }
  WiFiClient *flujo = http.getStreamPtr();
  uint8_t trozo[1024];
  int leidos = 0;
  bool sinEspacio = false;
  unsigned long limite = millis() + 20000;
  while ((largo < 0 || leidos < largo) && millis() < limite && (http.connected() || flujo->available())) {
    int n = flujo->read(trozo, sizeof(trozo));
    if (n > 0) {
      if (tmp.write(trozo, n) != (size_t)n) { sinEspacio = true; break; }
      leidos += n; limite = millis() + 20000;
    }
    else delay(5);
  }
  tmp.close();
  http.end();
  Serial.printf("[telegram] foto descargada: %d bytes\n", leidos);
  if (sinEspacio) { LittleFS.remove(TG_TEMPORAL); return "La memoria del marco esta llena. Borra algunas fotos desde la pagina del marco."; }
  if (leidos <= 0 || (largo > 0 && leidos != largo)) { LittleFS.remove(TG_TEMPORAL); return "La descarga se corto."; }
  if (LittleFS.totalBytes() - LittleFS.usedBytes() < BYTES_FOTO + 16384) {
    LittleFS.remove(TG_TEMPORAL);
    return "La memoria del marco esta llena. Borra algunas fotos desde la pagina del marco.";
  }

  if (!jpeg.open(TG_TEMPORAL, tgJpgAbrir, tgJpgCerrar, tgJpgLeer, tgJpgMover, tgGuardarBloque)) {
    LittleFS.remove(TG_TEMPORAL); return "No pude leer la foto (formato JPG).";
  }
  int w = jpeg.getWidth() / escala, h = jpeg.getHeight() / escala;
  tgRecorteX = (w - 240) / 2;
  tgRecorteY = (h - 240) / 2;

  int sig = prefs.getInt("sig", 1);
  prefs.putInt("sig", sig + 1);
  String id = String(sig);
  tgArchivo = LittleFS.open(rutaDe(id), "w");
  tgFranjaY = -1; tgFranjaAlto = 0; tgFilasEscritas = 0; tgErrorEscritura = false;
  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  int opciones = escala == 2 ? JPEG_SCALE_HALF : escala == 4 ? JPEG_SCALE_QUARTER : escala == 8 ? JPEG_SCALE_EIGHTH : 0;
  int ok = jpeg.decode(0, 0, opciones);
  jpeg.close();
  if (tgFranjaY >= 0) tgVaciarFranja();
  { static const uint8_t negro[480] = {0}; while (!tgErrorEscritura && tgFilasEscritas < 240) tgEscribirFila(negro); }
  tgArchivo.close();
  LittleFS.remove(TG_TEMPORAL);
  if (tgErrorEscritura) {
    LittleFS.remove(rutaDe(id));
    Serial.println("[telegram] memoria llena al guardar la foto");
    return "La memoria del marco esta llena. Borra algunas fotos desde la pagina del marco.";
  }
  if (!ok) { LittleFS.remove(rutaDe(id)); return "No pude decodificar la foto."; }

  orden.push_back(id);
  guardarOrden();
  // mostrarla de inmediato
  detenerAnim();
  enReloj = false;
  enClima = false;
  indiceActual = (int)orden.size() - 2;
  proximoCambio = 0;
  Serial.println("[marco] Foto de Telegram agregada: " + id);
  int quedan = (LittleFS.totalBytes() - LittleFS.usedBytes()) / (BYTES_FOTO + 512);
  return "Foto agregada al marco (" + String((int)orden.size()) + " en total, espacio para " + String(quedan) + " mas).";
}

// ---------------------------------------------------------------------
// Clima (Open-Meteo: gratis y sin registro). Se muestra en una pantalla
// aparte despues de la hora, al terminar cada vuelta de fotos.
// ---------------------------------------------------------------------
bool obtenerJson(const String &url, JsonDocument &doc, JsonDocument &filtro) {
  WiFiClientSecure cliente;
  cliente.setInsecure();   // sin validar certificado (ver README)
  HTTPClient http;
  if (!http.begin(cliente, url)) return false;
  http.setConnectTimeout(4000);
  http.setTimeout(6000);
  int codigo = http.GET();
  if (codigo != 200) {
    Serial.printf("[clima] HTTP %d\n", codigo);
    http.end();
    return false;
  }
  String cuerpo = http.getString();
  http.end();
  DeserializationError e = deserializeJson(doc, cuerpo, DeserializationOption::Filter(filtro));
  if (e) Serial.printf("[clima] JSON invalido (%s)\n", e.c_str());
  return !e;
}

// Las letras de la pantalla no tienen tildes: "Concepción" -> "Concepcion"
String sinTildes(const String &t) {
  static const char *de = "\xA1\xA9\xAD\xB3\xBA\xB1\x81\x89\x8D\x93\x9A\x91\xBC\xA3\xA7\xA0\xA8\xAC\xB2\xB9";
  static const char *a = "aeiounAEIOUNuacaeiou";
  String r;
  for (size_t i = 0; i < t.length(); i++) {
    uint8_t c = t[i];
    if (c < 0x80) { r += (char)c; continue; }
    if (c == 0xC3 && i + 1 < t.length()) {
      const char *p = strchr(de, t[i + 1]);
      if (p) r += a[p - de];
      i++;
      continue;
    }
    while (i + 1 < t.length() && ((uint8_t)t[i + 1] & 0xC0) == 0x80) i++;   // otro simbolo: se omite
  }
  return r;
}

bool climaListo() {
  return climaOk && climaLat.length() && millis() - climaHora < 3UL * 3600UL * 1000UL;
}

String climaDescripcion() {
  int c = climaCodigo;
  if (!climaOk) return "";
  if (c == 0) return "Despejado";
  if (c == 1) return "Poco nublado";
  if (c == 2) return "Algo nublado";
  if (c == 3) return "Nublado";
  if (c == 45 || c == 48) return "Neblina";
  if (c >= 51 && c <= 57) return "Llovizna";
  if (c >= 61 && c <= 67) return "Lluvia";
  if (c >= 71 && c <= 77) return "Nieve";
  if (c >= 80 && c <= 82) return "Chubascos";
  if (c == 85 || c == 86) return "Nieve";
  if (c >= 95) return "Tormenta";
  return "Clima";
}

bool actualizarClima() {
  if (!climaLat.length()) return false;
  JsonDocument doc, filtro;
  filtro["current"]["temperature_2m"] = true;
  filtro["current"]["weather_code"] = true;
  filtro["current"]["is_day"] = true;
  filtro["daily"]["temperature_2m_max"] = true;
  filtro["daily"]["temperature_2m_min"] = true;
  filtro["daily"]["precipitation_probability_max"] = true;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + climaLat + "&longitude=" + climaLon +
               "&current=temperature_2m,weather_code,is_day"
               "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max"
               "&timezone=auto&forecast_days=1";
  if (!obtenerJson(url, doc, filtro) || doc["current"].isNull()) return false;
  climaTemp = doc["current"]["temperature_2m"] | 0.0f;
  climaCodigo = doc["current"]["weather_code"] | 0;
  climaDia = doc["current"]["is_day"] | 1;
  climaMax = doc["daily"]["temperature_2m_max"][0] | climaTemp;
  climaMin = doc["daily"]["temperature_2m_min"][0] | climaTemp;
  climaLluvia = doc["daily"]["precipitation_probability_max"][0] | 0;
  climaOk = true;
  climaHora = millis();
  Serial.printf("[clima] %s: %.1f C, codigo %d\n", climaCiudad.c_str(), climaTemp, climaCodigo);
  return true;
}

void revisarClima() {
  if (modoAP || !climaLat.length() || millis() < climaProximo || WiFi.status() != WL_CONNECTED) return;
  if (enClima) return;                                           // no actualizar mientras se muestra
  if (animActiva && millis() < climaProximo + 60000) return;     // no cortar una animacion
  climaProximo = millis() + (actualizarClima() ? CLIMA_CADA_MS : 5UL * 60UL * 1000UL);
}

// --- Dibujos del clima (hechos con figuras, sin imagenes) ---
const uint16_t C_AMARILLO = 0xFE60, C_NUBE = 0xCE79, C_NUBE_OSCURA = 0x9CF3, C_AZUL = 0x4D9F,
               C_GRIS = 0xBDF7, C_NARANJO = 0xFD20, C_CELESTE = 0x867F;

void dibujarSol(int cx, int cy, int r) {
  for (int i = 0; i < 8; i++) {
    float ang = i * PI / 4;
    int x1 = cx + cos(ang) * (r + 5), y1 = cy + sin(ang) * (r + 5);
    int x2 = cx + cos(ang) * (r + 12), y2 = cy + sin(ang) * (r + 12);
    gfx->drawLine(x1, y1, x2, y2, C_AMARILLO);
    gfx->drawLine(x1 + 1, y1, x2 + 1, y2, C_AMARILLO);
  }
  gfx->fillCircle(cx, cy, r, C_AMARILLO);
}

void dibujarLuna(int cx, int cy, int r) {
  gfx->fillCircle(cx, cy, r, 0xFFF0);
  gfx->fillCircle(cx + r / 2, cy - r / 3, r * 85 / 100, 0x0000);
}

void dibujarNube(int cx, int cy, uint16_t color) {
  gfx->fillCircle(cx - 14, cy + 4, 12, color);
  gfx->fillCircle(cx + 2, cy - 6, 16, color);
  gfx->fillCircle(cx + 18, cy + 4, 12, color);
  gfx->fillRoundRect(cx - 26, cy + 2, 56, 15, 7, color);
}

void dibujarIconoClima(int cx, int cy) {
  int c = climaCodigo;
  bool lluvia = (c >= 51 && c <= 67) || (c >= 80 && c <= 82);
  bool nieve = (c >= 71 && c <= 77) || c == 85 || c == 86;
  bool tormenta = c >= 95;
  if (c == 0) {
    if (climaDia) dibujarSol(cx, cy, 18); else dibujarLuna(cx, cy, 20);
    return;
  }
  if (c == 1 || c == 2) {
    if (climaDia) dibujarSol(cx - 14, cy - 10, 13); else dibujarLuna(cx - 14, cy - 10, 14);
    dibujarNube(cx + 6, cy + 4, C_NUBE);
    return;
  }
  if (c == 45 || c == 48) {
    for (int i = 0; i < 4; i++) gfx->fillRoundRect(cx - 30 + (i % 2) * 8, cy - 18 + i * 11, 52, 5, 2, C_GRIS);
    return;
  }
  dibujarNube(cx, cy - 8, (lluvia || tormenta) ? C_NUBE_OSCURA : C_NUBE);
  if (lluvia) {
    for (int i = 0; i < 4; i++) {
      int x = cx - 16 + i * 12;
      gfx->drawLine(x, cy + 14, x - 4, cy + 24, C_AZUL);
      gfx->drawLine(x + 1, cy + 14, x - 3, cy + 24, C_AZUL);
    }
  }
  if (nieve) {
    for (int i = 0; i < 4; i++) gfx->fillCircle(cx - 16 + i * 12, cy + 17 + (i % 2) * 5, 3, 0xFFFF);
  }
  if (tormenta) {
    gfx->fillTriangle(cx + 4, cy + 8, cx - 8, cy + 22, cx + 1, cy + 22, C_AMARILLO);
    gfx->fillTriangle(cx - 2, cy + 20, cx + 8, cy + 20, cx - 5, cy + 34, C_AMARILLO);
  }
}

int anchoTexto(const String &t, const GFXfont *fuente) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx->setFont(fuente);
  gfx->getTextBounds(t, 0, 100, &x1, &y1, &w, &h);
  return w;
}

void dibujarClima() {
  gfx->fillScreen(0x0000);

  // Ciudad (sin region ni pais, recortada si no cabe)
  String ciudad = sinTildes(climaCiudad);
  int coma = ciudad.indexOf(',');
  if (coma > 0) ciudad = ciudad.substring(0, coma);
  // si no cabe, se quitan palabras del final ("Santiago de Chile" -> "Santiago")
  while (anchoTexto(ciudad, &FreeSansBold12pt7b) > 190 && ciudad.lastIndexOf(' ') > 0) {
    ciudad = ciudad.substring(0, ciudad.lastIndexOf(' '));
    String ult = ciudad.substring(ciudad.lastIndexOf(' ') + 1);
    ult.toLowerCase();
    if ((ult == "de" || ult == "del" || ult == "la" || ult == "el" || ult == "los" || ult == "las") && ciudad.lastIndexOf(' ') > 0)
      ciudad = ciudad.substring(0, ciudad.lastIndexOf(' '));
  }
  while (ciudad.length() > 3 && anchoTexto(ciudad, &FreeSansBold12pt7b) > 190) ciudad.remove(ciudad.length() - 1);
  textoCentrado(ciudad, 52, &FreeSansBold12pt7b, C_GRIS);

  dibujarIconoClima(120, 92);

  // Temperatura actual con el simbolo de grados (la letra no lo trae)
  String t = String((int)lroundf(climaTemp));
  gfx->setFont(&FreeSansBold24pt7b);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(t, 0, 160, &x1, &y1, &w, &h);
  int x = (240 - ((int)w + 14)) / 2 - x1;
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(x, 160);
  gfx->print(t);
  int gx = x + x1 + w + 9;
  gfx->drawCircle(gx, 160 - 29, 5, 0xFFFF);
  gfx->drawCircle(gx, 160 - 29, 4, 0xFFFF);

  // Descripcion (o probabilidad de lluvia si esta nublado y puede llover)
  String desc = climaDescripcion();
  if (climaCodigo < 51 && climaLluvia >= 40) desc = "Lluvia " + String(climaLluvia) + "%";
  textoCentrado(desc, 190, &FreeSansBold12pt7b, C_NARANJO);

  // Minima (celeste) / maxima (naranjo) del dia
  String sMin = String((int)lroundf(climaMin)), sSep = " / ", sMax = String((int)lroundf(climaMax));
  int total = anchoTexto(sMin, &FreeSansBold12pt7b) + anchoTexto(sSep, &FreeSansBold12pt7b) + anchoTexto(sMax, &FreeSansBold12pt7b);
  gfx->setFont(&FreeSansBold12pt7b);
  gfx->setCursor((240 - total) / 2, 216);
  gfx->setTextColor(C_CELESTE); gfx->print(sMin);
  gfx->setTextColor(C_GRIS); gfx->print(sSep);
  gfx->setTextColor(C_NARANJO); gfx->print(sMax);

  gfx->setFont((const GFXfont *)nullptr);   // vuelve a la letra normal para los mensajes
}

// POST /api/clima  ciudad=...  (vacio o quitar=1 para desactivar)
void apiClima() {
  String ciudad = server.arg("ciudad");
  ciudad.trim();
  if (server.hasArg("quitar") || !ciudad.length()) {
    climaCiudad = ""; climaLat = ""; climaLon = ""; climaOk = false;
    prefs.remove("climaCiudad"); prefs.remove("climaLat"); prefs.remove("climaLon");
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  // "Ciudad, Pais" permite elegir el pais; si no se indica, se prefiere Chile
  String nombre = ciudad, pais = "";
  int coma = ciudad.indexOf(',');
  if (coma >= 0) { nombre = ciudad.substring(0, coma); pais = ciudad.substring(coma + 1); nombre.trim(); pais.trim(); }
  pais = sinTildes(pais);
  pais.toLowerCase();

  JsonDocument doc, filtro;
  JsonObject f = filtro["results"][0].to<JsonObject>();
  f["name"] = true; f["latitude"] = true; f["longitude"] = true;
  f["country"] = true; f["country_code"] = true; f["admin1"] = true;
  if (!obtenerJson("https://geocoding-api.open-meteo.com/v1/search?count=10&language=es&format=json&name=" + urlCodificar(nombre), doc, filtro)) {
    server.send(400, "text/plain", "No pude consultar el servicio del clima. Revisa que el marco tenga internet.");
    return;
  }
  JsonArray res = doc["results"].as<JsonArray>();
  if (res.isNull() || res.size() == 0) {
    server.send(400, "text/plain", "No encontre esa ciudad. Prueba escribiendola de otra forma.");
    return;
  }
  JsonObject elegido = res[0];
  for (JsonObject c : res) {
    String cc = c["country_code"] | "";
    String cn = sinTildes(c["country"] | "");
    cc.toLowerCase(); cn.toLowerCase();
    bool coincide = pais.length() ? (cc == pais || cn.indexOf(pais) >= 0) : cc == "cl";
    if (coincide) { elegido = c; break; }
  }
  String n = elegido["name"] | "";
  String region = elegido["admin1"] | "";
  String paisNombre = elegido["country"] | "";
  climaCiudad = n;
  if (region.length() && region != n) climaCiudad += ", " + region;
  if (paisNombre.length()) climaCiudad += ", " + paisNombre;
  climaLat = String(elegido["latitude"].as<float>(), 4);
  climaLon = String(elegido["longitude"].as<float>(), 4);
  prefs.putString("climaCiudad", climaCiudad);
  prefs.putString("climaLat", climaLat);
  prefs.putString("climaLon", climaLon);
  climaOk = false;
  bool ok = actualizarClima();
  climaProximo = millis() + (ok ? CLIMA_CADA_MS : 5UL * 60UL * 1000UL);
  server.send(200, "application/json", "{\"ok\":true,\"ciudad\":" + jsonTexto(climaCiudad) +
              ",\"clima\":" + String(ok ? "true" : "false") + ",\"temp\":" + String(climaTemp, 1) +
              ",\"desc\":" + jsonTexto(climaDescripcion()) + "}");
}

void revisarTelegram() {
  if (modoAP || !tgToken.length() || millis() < tgProximo || WiFi.status() != WL_CONNECTED) return;
  // Durante una animacion se revisa menos seguido, para no cortarla
  if (animActiva && millis() < tgProximo + 45000) return;
  tgProximo = millis() + TG_CADA_MS;

  JsonDocument filtro;
  filtro["ok"] = true;
  filtro["result"][0]["update_id"] = true;
  JsonObject m = filtro["result"][0]["message"].to<JsonObject>();
  m["chat"]["id"] = true;
  m["from"]["first_name"] = true;
  m["text"] = true;
  m["photo"][0]["file_id"] = true;
  m["photo"][0]["width"] = true;
  m["photo"][0]["height"] = true;
  m["document"]["mime_type"] = true;
  JsonDocument doc;
  if (!tgLlamar("getUpdates", "offset=" + String(tgOffset) + "&limit=3&timeout=0&allowed_updates=%5B%22message%22%5D", doc, &filtro, tgToken)) {
    tgProximo = millis() + 60000;   // si Telegram no responde, esperar 1 minuto para no trabar el marco
    return;
  }

  if (doc["result"].size()) Serial.printf("[telegram] %u mensaje(s) nuevo(s)\n", (unsigned)doc["result"].size());
  for (JsonObject u : doc["result"].as<JsonArray>()) {
    tgOffset = u["update_id"].as<long>() + 1;
    JsonObject msg = u["message"];
    if (msg.isNull()) continue;
    char chatBuf[24];
    snprintf(chatBuf, sizeof(chatBuf), "%lld", msg["chat"]["id"].as<long long>());
    String chat = chatBuf;
    String nombre = msg["from"]["first_name"] | "";

    if (!tgAutorizado(chat)) {
      tgEnviar(chat, "Hola " + nombre + "! Tu ID de Telegram es " + chat +
                     ". Para enviar fotos al marco, agrega este numero en la pagina del marco (Recibir fotos por Telegram).");
      continue;
    }
    JsonArray fotos = msg["photo"].as<JsonArray>();
    if (!fotos.isNull() && fotos.size()) {
      // Elige el tamano y la escala que dejen el lado menor lo mas cerca posible de 240 px
      String mejorId;
      int mejorEscala = 1, mejorLado = 100000;
      for (JsonObject f : fotos) {
        int w = f["width"], h = f["height"];
        int lado = min(w, h);
        for (int esc = 1; esc <= 8; esc *= 2) {
          int l = lado / esc;
          if (l >= 216 && l < mejorLado && (long)w * h <= 1600L * 1600L) { mejorLado = l; mejorId = f["file_id"].as<String>(); mejorEscala = esc; }
        }
      }
      if (!mejorId.length()) { JsonObject f = fotos[fotos.size() - 1]; mejorId = f["file_id"].as<String>(); mejorEscala = 1; }
      tgEnviar(chat, "Recibida, la estoy poniendo en el marco...");
      tgEnviar(chat, tgAgregarFoto(mejorId, mejorEscala));
    } else if (!msg["document"].isNull()) {
      tgEnviar(chat, "Envia la imagen como foto (no como archivo) para que el marco la pueda leer.");
    } else {
      String texto = msg["text"] | "";
      if (texto == "/estado") {
        int quedan = (LittleFS.totalBytes() - LittleFS.usedBytes()) / (BYTES_FOTO + 512);
        tgEnviar(chat, "El marco tiene " + String((int)orden.size()) + " fotos y espacio para " + String(quedan) + " mas.");
      } else {
        tgEnviar(chat, "Hola " + nombre + "! Enviame una foto y aparecera en el marco en unos segundos. Comando: /estado");
      }
    }
  }
}

void apiTelegram() {
  String token = server.arg("token");
  token.trim();
  String ids = server.arg("ids");
  ids.replace(" ", "");
  if (token.length()) {
    JsonDocument doc, filtro;
    filtro["ok"] = true;
    filtro["result"]["username"] = true;
    if (!tgLlamar("getMe", "", doc, &filtro, token)) { server.send(400, "text/plain", "El token no es valido o el marco no tiene internet"); return; }
    tgToken = token;
    tgBot = doc["result"]["username"].as<String>();
    prefs.putString("tgToken", tgToken);
    prefs.putString("tgBot", tgBot);
    tgOffset = 0;
  }
  if (server.hasArg("quitar")) {
    tgToken = ""; tgBot = "";
    prefs.remove("tgToken"); prefs.remove("tgBot");
  }
  tgIds = ids;
  prefs.putString("tgIds", tgIds);
  tgProximo = 0;
  server.send(200, "application/json", "{\"ok\":true,\"bot\":" + jsonTexto(tgBot) + "}");
}

void noEncontrado() {
  if (modoAP) {   // portal cautivo: cualquier direccion lleva a la pagina
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "No encontrado");
  }
}

// ---------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------
void conectarWifi() {
  String clave = prefs.getString("clave", "");
  if (ssidGuardado.length()) {
    mensaje({"Conectando a", ssidGuardado.substring(0, 18), "", "espera..."});
    Serial.println("[marco] Conectando a " + ssidGuardado);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(NOMBRE_RED);
    WiFi.begin(ssidGuardado.c_str(), clave.c_str());
    unsigned long inicio = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - inicio < 20000) delay(250);
    Serial.printf("[marco] Estado WiFi: %d\n", (int)WiFi.status());
  }
  if (WiFi.status() == WL_CONNECTED) {
    modoAP = false;
    configTzTime(ZONA_HORARIA, "pool.ntp.org", "time.google.com");   // hora por internet
    MDNS.begin(NOMBRE_RED);
    MDNS.addService("http", "tcp", 80);
    mensaje({"Conectado!", "", "Abre en el", "navegador:", "marco.local", "o " + WiFi.localIP().toString()});
    Serial.println("Conectado. IP: " + WiFi.localIP().toString());
    delay(5000);
  } else {
    modoAP = true;
    // Buscar redes cercanas para mostrarlas en la pagina de configuracion
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    mensaje({"Buscando", "redes WiFi..."});
    int n = WiFi.scanNetworks();
    redesEncontradas.clear();
    for (int i = 0; i < n; i++) {
      String r = WiFi.SSID(i);
      if (!r.length()) continue;
      bool repetida = false;
      for (auto &x : redesEncontradas) if (x == r) { repetida = true; break; }
      if (!repetida) redesEncontradas.push_back(r);
    }
    WiFi.scanDelete();
    Serial.printf("[marco] Redes encontradas: %d\n", (int)redesEncontradas.size());
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_NOMBRE, AP_CLAVE);
    dns.start(53, "*", WiFi.softAPIP());
    if (ssidGuardado.length()) {
      mensaje({"No se pudo", "conectar a", ssidGuardado.substring(0, 18), "", "Abriendo red", "MarcoFotos..."});
      delay(4000);
    }
    mensajeModoAP();
    Serial.println("Modo AP: red MarcoFotos, http://192.168.4.1");
  }
}

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("[marco] Iniciando...");
  cargarAjustes();
  Serial.printf("[marco] Ajustes: pantalla=%d segundos=%d brillo=%d ssid='%s'\n", cfgPantalla, cfgSegundos, cfgBrillo, ssidGuardado.c_str());
  iniciarPantalla();
  Serial.println("[marco] Pantalla lista");
  mensaje({"Marco de fotos", "", "Preparando", "memoria..."});
  Serial.println("[marco] Montando memoria (la primera vez se formatea, puede tardar)...");
  if (!LittleFS.begin(true)) {
    Serial.println("[marco] ERROR: no se pudo montar LittleFS");
    mensaje({"Error", "", "No hay espacio", "para fotos.", "Revisa el", "Partition Scheme"});
    while (true) delay(1000);
  }
  LittleFS.mkdir("/f");
  cargarOrden();
  Serial.printf("[marco] Memoria: %u de %u bytes usados, %u fotos\n", (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes(), (unsigned)orden.size());

  conectarWifi();

  server.on("/", HTTP_GET, inicio);
  server.on("/editor", HTTP_GET, enviarPagina);
  server.on("/guardar-wifi", HTTP_POST, guardarWifiFormulario);
  server.on("/api/info", HTTP_GET, apiInfo);
  server.on("/api/foto", HTTP_GET, apiFoto);
  server.on("/api/fotos", HTTP_POST, finSubida, manejarSubida);
  server.on("/api/borrar", HTTP_POST, apiBorrar);
  server.on("/api/orden", HTTP_POST, apiOrden);
  server.on("/api/config", HTTP_POST, apiConfig);
  server.on("/api/wifi", HTTP_POST, apiWifi);
  server.on("/api/telegram", HTTP_POST, apiTelegram);
  server.on("/api/clima", HTTP_POST, apiClima);
  server.onNotFound(noEncontrado);
  server.begin();
  Serial.println("[marco] Servidor web listo");

  if (!modoAP) {
    if (orden.empty()) mensajeSinFotos();
    proximoCambio = 0;
  }
}

void loop() {
  if (modoAP) dns.processNextRequest();
  server.handleClient();

  if (reiniciarEn && millis() > reiniciarEn) ESP.restart();

  // En modo AP sin fotos se deja el mensaje de configuracion en pantalla
  if (modoAP && orden.empty()) return;

  avanzarAnim();
  actualizarReloj();
  revisarTelegram();
  revisarClima();

  if (millis() >= proximoCambio) {
    mostrarSiguiente();
    unsigned long dur = (unsigned long)cfgSegundos * 1000UL;
    if (enReloj) dur = (unsigned long)cfgReloj * 1000UL;
    else if (enClima) dur = (unsigned long)cfgClima * 1000UL;
    else if (animActiva) {   // una animacion se muestra al menos una vuelta completa
      unsigned long vuelta = (unsigned long)animCuadros * animMs;
      if (dur < vuelta) dur = vuelta;
    }
    proximoCambio = millis() + dur;
  }
}
