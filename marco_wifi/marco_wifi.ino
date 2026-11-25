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
//  Librerias: GFX Library for Arduino, JPEGDEC.
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
  if (finVuelta && cfgReloj > 0 && !enReloj && horaValida()) {
    if (fundido) { for (int i = 12; i >= 0; i--) { ponerBrillo(cfgBrillo * i / 12); delay(18); } }
    detenerAnim();
    enReloj = true;
    dibujarReloj();
    if (fundido) { for (int i = 0; i <= 12; i++) { ponerBrillo(cfgBrillo * i / 12); delay(18); } }
    else ponerBrillo(cfgBrillo);
    return;
  }
  enReloj = false;

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

  if (millis() >= proximoCambio) {
    mostrarSiguiente();
    unsigned long dur = (unsigned long)cfgSegundos * 1000UL;
    if (enReloj) dur = (unsigned long)cfgReloj * 1000UL;
    else if (animActiva) {   // una animacion se muestra al menos una vuelta completa
      unsigned long vuelta = (unsigned long)animCuadros * animMs;
      if (dur < vuelta) dur = vuelta;
    }
    proximoCambio = millis() + dur;
  }
}
