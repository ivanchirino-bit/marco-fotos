#!/bin/bash
# ---------------------------------------------------------------
#  Marco de fotos ESP32 - subir por cable desde el Mac
#
#  1. Descarga "marco_cable.zip" desde la pagina del marco.
#  2. Conecta el ESP32 por USB.
#  3. Haz doble clic en este archivo.
#
#  Toma el marco_cable mas reciente de la carpeta Descargas, lo compila
#  con la herramienta arduino-cli que trae Arduino IDE y lo sube al ESP32.
# ---------------------------------------------------------------

FQBN="esp32:esp32:esp32:PartitionScheme=huge_app"
DESCARGAS="$HOME/Downloads"

verde()    { printf "\033[32m%s\033[0m\n" "$1"; }
rojo()     { printf "\033[31m%s\033[0m\n" "$1"; }
amarillo() { printf "\033[33m%s\033[0m\n" "$1"; }
paso()     { printf "\n\033[1m%s\033[0m\n" "$1"; }

terminar() {
  echo
  read -n 1 -s -r -p "Presiona cualquier tecla para cerrar esta ventana..."
  echo
  [ -n "$TEMP" ] && rm -rf "$TEMP"
  exit "$1"
}

clear
echo "==============================================="
echo "   🖼️  Marco de fotos ESP32 - subir por cable"
echo "==============================================="

# ---------- 1. Buscar arduino-cli ----------
paso "1/4  Buscando la herramienta de Arduino..."
CLI=""
for c in \
  "/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli" \
  "$HOME/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"; do
  if [ -x "$c" ]; then CLI="$c"; break; fi
done
[ -z "$CLI" ] && CLI="$(command -v arduino-cli 2>/dev/null)"
if [ -z "$CLI" ]; then
  rojo "No encontre arduino-cli."
  echo "Revisa que Arduino IDE este instalado en la carpeta Aplicaciones."
  terminar 1
fi
CONF=()
if [ -f "$HOME/.arduinoIDE/arduino-cli.yaml" ]; then
  CONF=(--config-file "$HOME/.arduinoIDE/arduino-cli.yaml")
fi
verde "OK"

# ---------- 2. Buscar el marco_cable mas reciente en Descargas ----------
# Sirve tanto el .zip como la carpeta ya descomprimida (Safari la descomprime solo).
paso "2/4  Buscando marco_cable en Descargas..."

# Fecha en que el archivo llego a Descargas (macOS la guarda aparte).
# No se usa la fecha de modificacion porque al descomprimir un zip
# los archivos conservan la hora guardada dentro del zip, que puede venir corrida.
fecha_llegada() {
  local s
  s=$(mdls -raw -name kMDItemDateAdded "$1" 2>/dev/null)
  if [ -n "$s" ] && [ "$s" != "(null)" ]; then
    date -j -f "%Y-%m-%d %H:%M:%S %z" "$s" +%s 2>/dev/null && return
  fi
  stat -f %m "$1"
}

ORIGEN=""
FECHA=0
shopt -s nullglob
for z in "$DESCARGAS"/marco_cable*.zip; do
  f=$(fecha_llegada "$z")
  if [ "$f" -gt "$FECHA" ]; then FECHA=$f; ORIGEN="$z"; fi
done
for d in "$DESCARGAS"/marco_cable*/; do
  d="${d%/}"
  ino="$d/marco_cable.ino"
  [ -f "$ino" ] || ino="$d/marco_cable/marco_cable.ino"
  if [ -f "$ino" ]; then
    f=$(fecha_llegada "$d")
    if [ "$f" -gt "$FECHA" ]; then FECHA=$f; ORIGEN="$(dirname "$ino")"; fi
  fi
done
shopt -u nullglob

if [ -z "$ORIGEN" ]; then
  rojo "No encontre ningun marco_cable en Descargas."
  echo "Descargalo desde la pagina: pestaña 'Por cable' -> 'Descargar sketch (.zip)'."
  terminar 1
fi
echo "Usando: ${ORIGEN#$DESCARGAS/}  (descargado el $(date -r "$FECHA" '+%d/%m/%Y a las %H:%M'))"

TEMP=$(mktemp -d /tmp/marco_cable.XXXXXX)
SKETCH="$TEMP/marco_cable"
if [[ "$ORIGEN" == *.zip ]]; then
  if ! unzip -q "$ORIGEN" -d "$TEMP"; then
    rojo "No se pudo descomprimir el archivo."
    terminar 1
  fi
  if [ ! -f "$SKETCH/marco_cable.ino" ]; then
    encontrado=$(find "$TEMP" -name marco_cable.ino -print -quit)
    [ -n "$encontrado" ] && SKETCH="$(dirname "$encontrado")"
  fi
else
  cp -R "$ORIGEN" "$SKETCH"
fi
if [ ! -f "$SKETCH/marco_cable.ino" ]; then
  rojo "El archivo no trae marco_cable.ino. Vuelve a descargarlo desde la pagina."
  terminar 1
fi
FOTOS=$(ls "$SKETCH"/foto*.h 2>/dev/null | wc -l | tr -d ' ')
verde "OK: $FOTOS fotos"

# ---------- 3. Buscar el ESP32 conectado ----------
paso "3/4  Buscando el ESP32 conectado por USB..."
shopt -s nullglob
PUERTOS=(/dev/cu.usbserial-* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART* /dev/cu.usbmodem*)
shopt -u nullglob
if [ ${#PUERTOS[@]} -eq 0 ]; then
  rojo "No encontre ningun ESP32 conectado."
  echo "Conecta el ESP32 con un cable USB de datos y vuelve a abrir este archivo."
  terminar 1
elif [ ${#PUERTOS[@]} -eq 1 ]; then
  PUERTO="${PUERTOS[0]}"
else
  echo "Hay varios dispositivos conectados. Elige el ESP32:"
  select p in "${PUERTOS[@]}"; do
    if [ -n "$p" ]; then PUERTO="$p"; break; fi
  done
fi
verde "OK: $PUERTO"

# ---------- 4. Compilar y subir ----------
paso "4/4  Compilando y subiendo (tarda 1 a 2 minutos)..."
echo "No desconectes el ESP32 hasta que termine."
echo "Compilando... (este paso no muestra avance, ten paciencia)"
echo
if "$CLI" "${CONF[@]}" compile --fqbn "$FQBN" --upload -p "$PUERTO" "$SKETCH"; then
  echo
  verde "==============================================="
  verde "  ✅ Listo: $FOTOS fotos subidas al marco"
  verde "==============================================="
  terminar 0
else
  echo
  rojo "==============================================="
  rojo "  ❌ No se pudo subir"
  rojo "==============================================="
  echo "Posibles causas:"
  echo " - El Monitor Serie de Arduino IDE esta abierto y ocupa el puerto: cierralo."
  echo " - Se quedo en 'Connecting...': manten apretado el boton BOOT del ESP32 y vuelve a intentar."
  echo " - Demasiadas fotos para la memoria: usa 22 o menos."
  terminar 1
fi
