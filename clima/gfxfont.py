"""Convierte una tipografia TTF/OTF en una fuente GFXfont (Adafruit/Arduino_GFX), 1 bit por pixel.
Uso como modulo: generar(ruta, tam, primero, ultimo, nombre, reemplazos) -> (texto_h, glifos)
reemplazos: {codigo: caracter_a_dibujar} (p.ej. dibujar el simbolo de grados en lugar de ':')."""
from PIL import Image, ImageDraw, ImageFont


def generar(ruta, tam, primero, ultimo, nombre, reemplazos=None, umbral=110):
    reemplazos = reemplazos or {}
    fuente = ImageFont.truetype(ruta, tam)
    asc, desc = fuente.getmetrics()
    bits_total = []
    glifos = []
    for cod in range(primero, ultimo + 1):
        ch = reemplazos.get(cod, chr(cod))
        adv = int(round(fuente.getlength(ch)))
        lienzo = Image.new('L', (tam * 3, tam * 3), 0)
        d = ImageDraw.Draw(lienzo)
        x0, y0 = tam, tam * 2
        d.text((x0, y0), ch, font=fuente, fill=255, anchor='ls')
        bn = lienzo.point(lambda v: 255 if v >= umbral else 0)
        caja = bn.getbbox()
        if not caja:
            glifos.append(dict(cod=cod, off=len(bits_total) // 8 if False else None, w=0, h=0, adv=adv, xo=0, yo=0, px=[]))
            continue
        l, t, r, b = caja
        w, h = r - l, b - t
        px = [[1 if bn.getpixel((l + i, t + j)) else 0 for i in range(w)] for j in range(h)]
        glifos.append(dict(cod=cod, w=w, h=h, adv=adv, xo=l - x0, yo=t - y0, px=px))
    # empaquetar
    bytes_ = []
    for g in glifos:
        g['off'] = len(bytes_)
        if not g['w']:
            continue
        bits = [v for fila in g['px'] for v in fila]
        while len(bits) % 8:
            bits.append(0)
        for i in range(0, len(bits), 8):
            byte = 0
            for k in range(8):
                byte = (byte << 1) | bits[i + k]
            bytes_.append(byte)
    lineas = [f'// Fuente generada desde {ruta.split("/")[-1]} ({tam}px) para Arduino_GFX', '#pragma once', '',
              f'const uint8_t {nombre}Bitmaps[] PROGMEM = {{']
    for i in range(0, len(bytes_), 16):
        lineas.append('  ' + ', '.join(f'0x{b:02X}' for b in bytes_[i:i + 16]) + ',')
    lineas.append('};')
    lineas.append('')
    lineas.append(f'const GFXglyph {nombre}Glyphs[] PROGMEM = {{')
    for g in glifos:
        lineas.append(f'  {{ {g["off"]:5d}, {g["w"]:3d}, {g["h"]:3d}, {g["adv"]:3d}, {g["xo"]:4d}, {g["yo"]:4d} }},  // 0x{g["cod"]:02X}')
    lineas.append('};')
    lineas.append('')
    lineas.append(f'const GFXfont {nombre} PROGMEM = {{(uint8_t *){nombre}Bitmaps, (GFXglyph *){nombre}Glyphs, 0x{primero:02X}, 0x{ultimo:02X}, {asc + desc}}};')
    return '\n'.join(lineas) + '\n', glifos, len(bytes_)
