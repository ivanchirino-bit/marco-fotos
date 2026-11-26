"""Genera fondos de cielo realistas (240x240) para la pantalla de clima y hora del marco."""
import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter, zoom

N = 240
yy, xx = np.mgrid[0:N, 0:N].astype(np.float32)


def ruido(semilla, escalas=(4, 8, 16, 32, 64), pesos=(1, .5, .25, .13, .07)):
    """Ruido fractal (fbm) suave en [0,1]."""
    rng = np.random.default_rng(semilla)
    total = np.zeros((N, N), np.float32)
    for e, p in zip(escalas, pesos):
        base = rng.random((e + 1, e + 1)).astype(np.float32)
        capa = zoom(base, N / e, order=3)[:N, :N]
        total += p * capa
    total -= total.min()
    return total / total.max()


def degradado(arriba, abajo, curva=1.0):
    t = (yy / (N - 1)) ** curva
    a, b = np.array(arriba, np.float32), np.array(abajo, np.float32)
    return a[None, None, :] * (1 - t[..., None]) + b[None, None, :] * t[..., None]


def mezclar(img, color, alfa):
    c = np.array(color, np.float32)
    return img * (1 - alfa[..., None]) + c[None, None, :] * alfa[..., None]


def resplandor(cx, cy, radio, fuerza):
    d = np.hypot(xx - cx, yy - cy)
    return np.clip(fuerza * np.exp(-(d / radio) ** 2), 0, 1)


def disco(cx, cy, r, suave=1.2):
    d = np.hypot(xx - cx, yy - cy)
    return np.clip((r - d) / suave + .5, 0, 1)


def sol(img, cx=196, cy=34, r=17):
    img = mezclar(img, (255, 244, 214), resplandor(cx, cy, 95, .35))
    img = mezclar(img, (255, 238, 190), resplandor(cx, cy, 45, .55))
    img = mezclar(img, (255, 250, 235), resplandor(cx, cy, 24, .85))
    img = mezclar(img, (255, 253, 245), disco(cx, cy, r))
    return img


def desplazar_abajo(a, k):
    """a[y-k] (lo que hay k pixeles mas arriba), rellenando con el borde."""
    return np.concatenate([np.repeat(a[:1], k, 0), a[:-k]], 0)


def nubes(img, semilla, cobertura, y_max=1.0, luz=(255, 255, 255), sombra=(170, 180, 196), densidad=1.0, suavizar=1.2, despejar_centro=False):
    """Nubes tipo cumulo: textura fractal, iluminadas desde arriba y sombreadas abajo."""
    n = ruido(semilla)
    forma = np.clip((n - (1 - cobertura)) / .20, 0, 1)
    forma = gaussian_filter(forma, suavizar) * densidad
    forma *= np.clip((y_max * N - yy) / 40 + 1, 0, 1)
    if despejar_centro:   # menos nubes detras del texto (centro) para que se lea bien
        centro = np.exp(-(((xx - 120) / 70) ** 2 + ((yy - 110) / 75) ** 2))
        forma *= 1 - .65 * centro
    alto = gaussian_filter(forma, 4)
    arriba = desplazar_abajo(alto, 7)
    # si arriba hay menos nube, este pixel es el "techo" iluminado; si hay mas, queda en sombra
    sombra_k = np.clip(.55 + (arriba - alto) * 2.2 - (alto - .5) * .2, 0, 1)
    detalle = ruido(semilla + 9, escalas=(24, 48, 96), pesos=(1, .5, .3))
    sombra_k = np.clip(sombra_k + (detalle - .5) * .25, 0, 1)
    color = np.array(luz, np.float32)[None, None, :] * (1 - sombra_k[..., None]) + np.array(sombra, np.float32)[None, None, :] * sombra_k[..., None]
    return img * (1 - forma[..., None]) + color * forma[..., None]


def cubierto(semilla, claro, oscuro, contraste=.55):
    """Cielo totalmente cubierto: capas grises suaves con zonas mas claras y mas oscuras."""
    n = ruido(semilla, escalas=(3, 6, 12, 24, 48), pesos=(1, .6, .35, .2, .1))
    n = gaussian_filter(n, 2)
    t = np.clip(.5 + (n - .5) * (1 + contraste) - yy / N * .25, 0, 1)
    a, b = np.array(oscuro, np.float32), np.array(claro, np.float32)
    return a[None, None, :] * (1 - t[..., None]) + b[None, None, :] * t[..., None]


def estrellas(img, semilla=7, cantidad=90, y_max=210):
    rng = np.random.default_rng(semilla)
    for _ in range(cantidad):
        x, y = rng.integers(0, N), rng.integers(0, y_max)
        b = rng.random() ** 3
        img = mezclar(img, (255, 255, 255), resplandor(x, y, .55 + 1.1 * b, .30 + .70 * b))
    return img


def luna(img, cx=194, cy=38, r=16, semilla=4):
    img = mezclar(img, (200, 210, 235), resplandor(cx, cy, 70, .18))
    img = mezclar(img, (220, 228, 245), resplandor(cx, cy, 30, .30))
    textura = ruido(semilla, escalas=(8, 16, 32), pesos=(1, .6, .4))
    crateres = gaussian_filter(textura, 1.0)
    d = np.hypot(xx - cx, yy - cy)
    limbo = np.clip(1 - (d / r) ** 4 * .25, 0, 1)          # borde un poco mas oscuro
    tono = (205 + 45 * (crateres - .5) * 1.4) * limbo
    color = np.stack([tono, tono * 1.0, tono * .96], -1)
    a = disco(cx, cy, r)[..., None]
    return img * (1 - a) + color * a


def lluvia(img, semilla, cantidad=150, fuerza=.26):
    rng = np.random.default_rng(semilla)
    capa = np.zeros((N, N), np.float32)
    for _ in range(cantidad):
        x0, y0 = rng.uniform(-20, N), rng.uniform(-10, N)
        largo = rng.uniform(10, 20)
        for t in np.linspace(0, 1, 18):
            x, y = int(x0 - 4 * t * largo / 12), int(y0 + t * largo)
            if 0 <= x < N and 0 <= y < N:
                capa[y, x] = max(capa[y, x], rng.uniform(.5, 1))
    capa = gaussian_filter(capa, .45) * 1.6
    return mezclar(img, (200, 215, 235), np.clip(capa * fuerza, 0, 1))


def nieve(img, semilla, cantidad=140):
    rng = np.random.default_rng(semilla)
    for _ in range(cantidad):
        x, y = rng.uniform(0, N), rng.uniform(0, N)
        img = mezclar(img, (255, 255, 255), resplandor(x, y, rng.uniform(.8, 1.8), .9))
    return img


def neblina(img, semilla):
    n = ruido(semilla, escalas=(2, 4, 8, 16), pesos=(1, .6, .3, .15))
    bandas = gaussian_filter(n, (2, 18))
    alfa = np.clip(.35 + bandas * .5, 0, .85) * np.clip(yy / N + .3, 0, 1)
    return mezclar(img, (214, 220, 228), alfa)


def generar_todos():
    c = {}
    dia = degradado((22, 88, 186), (120, 176, 232), .9)
    c['despejado_dia'] = sol(dia)
    c['parcial_dia'] = nubes(sol(dia), 11, .55, sombra=(150, 166, 190), despejar_centro=True)
    c['nublado_dia'] = nubes(cubierto(21, (176, 186, 200), (110, 122, 140)), 22, .45, y_max=.7, luz=(214, 220, 230), sombra=(140, 150, 166), densidad=.7)
    lluv = cubierto(31, (112, 120, 134), (52, 60, 74))
    c['lluvia_dia'] = lluvia(nubes(lluv, 33, .5, y_max=.6, luz=(130, 138, 152), sombra=(64, 72, 86), densidad=.6), 32)
    c['tormenta'] = lluvia(cubierto(41, (78, 82, 98), (26, 28, 40), .8), 42, 190, .30)
    c['nieve'] = nieve(cubierto(51, (214, 220, 230), (150, 160, 178)), 52)
    c['neblina'] = neblina(degradado((150, 160, 172), (196, 202, 210)), 61)
    noche = degradado((4, 8, 26), (30, 42, 84), 1.1)
    c['despejado_noche'] = luna(estrellas(noche))
    c['parcial_noche'] = nubes(luna(estrellas(noche)), 71, .45, y_max=.8, luz=(78, 90, 120), sombra=(22, 28, 48), densidad=.9)
    c['nublado_noche'] = nubes(cubierto(81, (52, 58, 76), (14, 18, 30)), 83, .4, y_max=.6, luz=(70, 76, 94), sombra=(24, 28, 40), densidad=.6)
    c['lluvia_noche'] = lluvia(cubierto(82, (46, 52, 68), (12, 15, 26)), 84, 150, .22)
    return {k: Image.fromarray(np.clip(v, 0, 255).astype(np.uint8)) for k, v in c.items()}


if __name__ == '__main__':
    import os
    os.makedirs('/home/claude/clima/cielos', exist_ok=True)
    for k, im in generar_todos().items():
        im.save(f'/home/claude/clima/cielos/{k}.png')
    print('ok')
