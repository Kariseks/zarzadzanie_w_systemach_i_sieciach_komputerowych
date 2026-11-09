#!/usr/bin/env python3
"""Przygotowuje obrazy testowe w obrazy/skalowane/ z obrazu NASA Blue Marble.

* Seria 16:9 (E13 i rozmiary HD/4K/8K w innych eksperymentach): środkowy kadr
  19200x10800 oryginału 21600x10800, zmniejszony filtrem Lanczos – tylko w dół.
* Kontrola proporcji (E13): kwadrat 2048x2048 i pasek 16384x256 (te same ok. 4,2 Mpix
  co 2731x1536 z serii 16:9), wycięte z oryginału i zmniejszone.
* Zapis w PNG w skali szarości (bezstratnie, szybko się wczytuje).  Dodatkowo JPG
  w kolorze dla 4K i 8K – do E9 (cały program z wczytaniem JPG i konwersją).
* obrazy/jakosc/ (E14): figury o stałej jasności, mapa prawdziwych krawędzi gt.png
  i wersje z szumem gaussowskim sigma = 0, 5, 10, 20, 30, 40.
* manifest.json: wymiary i SHA-256 każdego pliku (wpisywane też przez edge_bench do meta.json).

Wynik jest deterministyczny dla tej samej wersji Pillow (requirements.txt).
Uruchomienie (z katalogu głównego repo):
    analiza/.venv/bin/python analiza/przygotuj_obrazy.py [--force]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
from pathlib import Path

from PIL import Image

import wspolne as w

ZRODLO = w.REPO / "obrazy" / "nasa_blue_marble_200407_21600x10800.jpg"
CEL = w.REPO / "obrazy" / "skalowane"

# Seria 16:9: od 0,04 do 59 Mpix, mniej więcej równo w skali logarytmicznej
SERIA_16_9 = [
    (256, 144), (426, 240), (640, 360), (960, 540), (1280, 720), (1920, 1080),
    (2560, 1440), (2731, 1536), (3200, 1800), (3840, 2160), (5120, 2880),
    (6400, 3600), (7680, 4320), (10240, 5760),
]
KONTROLA_PROPORCJI = [(2048, 2048), (16384, 256)]   # ~4,2 Mpix jak 2731x1536
JPG_KOLOR = [(3840, 2160), (7680, 4320)]           # do E9


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for blok in iter(lambda: f.read(1 << 20), b""):
            h.update(blok)
    return h.hexdigest()


def kadr(img: Image.Image, szer: int, wys: int) -> Image.Image:
    """Największy środkowy kadr o proporcjach szer:wys."""
    W, H = img.size
    if W * wys > H * szer:            # oryginał szerszy – przycinamy boki
        nw, nh = round(H * szer / wys), H
    else:
        nw, nh = W, round(W * wys / szer)
    x0, y0 = (W - nw) // 2, (H - nh) // 2
    return img.crop((x0, y0, x0 + nw, y0 + nh))


# --- E14: obrazy do oceny jakości wykrywania krawędzi -------------------------------
JAKOSC = w.REPO / "obrazy" / "jakosc"
SZUMY = [0, 5, 10, 20, 30, 40]          # odchylenie standardowe szumu gaussowskiego (poziomy szarości)


def przygotuj_jakosc(force: bool) -> None:
    """Obraz z figurami o stałej jasności (prawdziwe krawędzie znane co do piksela)
    + wersje z szumem gaussowskim.  gt.png: 255 = piksel krawędzi (zmiana jasności
    względem sąsiada z prawej lub z dołu w obrazie bez szumu)."""
    import numpy as np

    pliki = ["czysty.png", "gt.png"] + [f"szum_{s:02d}.png" for s in SZUMY]
    if not force and all((JAKOSC / f).exists() for f in pliki):
        return
    JAKOSC.mkdir(parents=True, exist_ok=True)
    W, H = 768, 512
    rng = np.random.default_rng(14)
    img = np.full((H, W), 110.0)
    yy, xx = np.mgrid[0:H, 0:W]
    for _ in range(14):                                    # prostokąty – krawędzie poziome i pionowe
        x0, y0 = rng.integers(0, W - 60), rng.integers(0, H - 60)
        x1, y1 = x0 + rng.integers(30, 220), y0 + rng.integers(30, 160)
        img[y0:y1, x0:x1] = rng.choice([30, 60, 160, 190, 220])
    for _ in range(10):                                    # koła – krawędzie we wszystkich kierunkach
        cx, cy, r = rng.integers(40, W - 40), rng.integers(40, H - 40), rng.integers(12, 70)
        img[(xx - cx) ** 2 + (yy - cy) ** 2 <= r * r] = rng.choice([20, 70, 150, 200, 240])
    for _ in range(4):                                     # półpłaszczyzny ukośne w pasach
        x0 = rng.integers(0, W - 200)
        a = rng.uniform(0.3, 2.5)
        pas = (xx >= x0) & (xx < x0 + 160) & (yy > a * (xx - x0) + rng.integers(0, H // 2))
        img[pas] = rng.choice([40, 180, 230])
    czysty = img.astype(np.uint8)
    gt = np.zeros_like(czysty, dtype=bool)
    gt[:, :-1] |= czysty[:, :-1] != czysty[:, 1:]
    gt[:-1, :] |= czysty[:-1, :] != czysty[1:, :]
    Image.fromarray(czysty).save(JAKOSC / "czysty.png")
    Image.fromarray((gt * 255).astype(np.uint8)).save(JAKOSC / "gt.png")
    for s in SZUMY:
        szum = np.random.default_rng(1000 + s).normal(0.0, s, size=czysty.shape) if s else 0.0
        Image.fromarray(np.clip(np.rint(czysty + szum), 0, 255).astype(np.uint8)).save(JAKOSC / f"szum_{s:02d}.png")
    w.info(f"obrazy/jakosc: {len(pliki)} plików (krawędzie: {int(gt.sum())} pikseli)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--force", action="store_true", help="generuj od nowa, nawet gdy pliki istnieją")
    args = ap.parse_args()
    przygotuj_jakosc(args.force)

    if not ZRODLO.exists():
        w.info(f"BŁĄD: brak {ZRODLO.relative_to(w.REPO)} – polecenie pobrania jest w obrazy/ZRODLA.md")
        return 1
    CEL.mkdir(parents=True, exist_ok=True)
    manifest_p = CEL / "manifest.json"

    plan = [(s, "png") for s in SERIA_16_9 + KONTROLA_PROPORCJI] + [(s, "jpg") for s in JPG_KOLOR]
    brak = [(s, e) for s, e in plan if args.force or not (CEL / f"{s[0]}x{s[1]}.{e}").exists()]
    if not brak and manifest_p.exists():
        w.info(f"obrazy/skalowane: wszystkie {len(plan)} pliki są aktualne")
        return 0

    start = time.monotonic()
    Image.MAX_IMAGE_PIXELS = None     # oryginał ma 233 Mpix (domyślny limit Pillow to ok. 89 Mpix)
    w.info(f"wczytuję {ZRODLO.name} ...")
    oryginal = Image.open(ZRODLO)
    oryginal.load()
    szary = oryginal.convert("L")
    kadry: dict[tuple[str, tuple[int, int]], Image.Image] = {}

    for (szer, wys), ext in brak:
        proporcje = (16, 9) if (szer, wys) in SERIA_16_9 + JPG_KOLOR else (szer, wys)
        zrodlo = oryginal if ext == "jpg" else szary
        klucz = (ext, proporcje)
        if klucz not in kadry:
            kadry[klucz] = kadr(zrodlo, *proporcje)
        k = kadry[klucz]
        if k.size[0] < szer or k.size[1] < wys:
            w.info(f"BŁĄD: {szer}x{wys} wymagałoby powiększenia z {k.size} – pomijam")
            continue
        out = CEL / f"{szer}x{wys}.{ext}"
        img = k.resize((szer, wys), Image.LANCZOS)
        if ext == "jpg":
            img.save(out, quality=95, subsampling=0)
        else:
            img.save(out, optimize=False)
        w.info(f"  {out.name:<16} {szer * wys / 1e6:7.2f} Mpix")

    manifest = {
        "zrodlo": ZRODLO.name,
        "zrodlo_sha256": sha256(ZRODLO),
        "metoda": "środkowy kadr, Image.resize(LANCZOS), Pillow " + Image.__version__,
        "pliki": {},
    }
    for p in sorted(CEL.glob("*x*.*")):
        if p.suffix not in (".png", ".jpg"):
            continue
        with Image.open(p) as im:
            manifest["pliki"][p.name] = {"width": im.size[0], "height": im.size[1],
                                         "mode": im.mode, "sha256": sha256(p)}
    manifest_p.write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")
    w.info(f"gotowe: {len(manifest['pliki'])} plików w {CEL.relative_to(w.REPO)}, {w.czas_hms(time.monotonic() - start)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
