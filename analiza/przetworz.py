#!/usr/bin/env python3
"""(2) Przetwarzanie wyników: analiza/wyniki/*/ -> rysunki, tabele i makra do sprawozdania.

Niczego nie mierzy, więc można go uruchamiać dowolnie często.
  * rysunki PDF        -> sprawko/zrzuty/e<NR>_<opis>.pdf  (stałe nazwy)
  * tabele booktabs    -> sprawko/tabele/e<NR>_<opis>.tex   (\\input w sprawozdaniu)
  * liczby do tekstu   -> sprawko/wyniki.tex: \\wynik{klucz}, np. \\wynik{e1/V6/4k/przysp}
  * zbiorcze mediany   -> analiza/wyniki/podsumowanie.csv
Brakujący eksperyment -> ostrzeżenie, reszta generuje się dalej.

Statystyka: mediana z powtórzeń jako wartość, min–maks jako słupki błędu.
S(N) = T(1)/T(N), E(N) = S(N)/N; udział części sekwencyjnej s dopasowany do
prawa Amdahla metodą najmniejszych kwadratów.

Przykłady (z katalogu głównego repo):
  analiza/.venv/bin/python analiza/przetworz.py
  analiza/.venv/bin/python analiza/przetworz.py --wyniki /tmp/zssk_quick --zrzuty /tmp/zrzuty_test
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
import traceback
from dataclasses import dataclass, field
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402
from matplotlib import font_manager, ticker  # noqa: E402

import wspolne as w  # noqa: E402

CM = 1 / 2.54
SZER = 16.0 * CM          # \textwidth przy marginesach 2,5 cm
SZER_POL = 7.8 * CM
WYS = 6.2 * CM

ID_COLS = list(w.KLUCZ_GRUPY)
STR_COLS = ["experiment", "variant", "kernel", "image", "mask", "norm", "scaling", "schedule",
            "mech", "reduce", "affinity", "gpu_prec", "gpu_wg"]


# =============================================================================
#  Styl i formatowanie po polsku
# =============================================================================

def ustaw_styl(slajdy: bool = False) -> None:
    lm = Path("/usr/share/texmf/fonts/opentype/public/lm")
    for f in ("lmroman10-regular.otf", "lmroman10-bold.otf", "lmroman10-italic.otf", "lmroman10-bolditalic.otf"):
        if (lm / f).exists():
            font_manager.fontManager.addfont(str(lm / f))
    rodzina = "Latin Modern Roman" if (lm / "lmroman10-regular.otf").exists() else "DejaVu Serif"
    plt.rcParams.update({
        "font.family": "serif",
        "font.serif": [rodzina, "DejaVu Serif"],
        "mathtext.fontset": "cm",
        "font.size": 9,
        "axes.titlesize": 9,
        "axes.labelsize": 9,
        "legend.fontsize": 7.5,
        "xtick.labelsize": 8,
        "ytick.labelsize": 8,
        "axes.grid": True,
        "grid.alpha": 0.3,
        "grid.linewidth": 0.5,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "lines.linewidth": 1.2,
        "lines.markersize": 4,
        "legend.frameon": False,
        "savefig.bbox": "tight",
        "savefig.pad_inches": 0.03,
        "pdf.fonttype": 3,       # Type 3: bezpieczne osadzanie czcionek OTF w PDF-ie dla pdflatex
        "axes.unicode_minus": True,
        "axes.formatter.use_mathtext": False,
    })
    if slajdy:   # slajd 16:9 ~ 14 cm szerokości: większa czcionka, grubsze linie
        plt.rcParams.update({"font.size": 10, "axes.labelsize": 10, "axes.titlesize": 10, "legend.fontsize": 8.5,
                             "xtick.labelsize": 9, "ytick.labelsize": 9, "lines.linewidth": 1.6, "lines.markersize": 5})


# Paleta czytelna w druku czarno-białym: kolor + marker + styl linii
KOLORY = ["#1b4f72", "#c0392b", "#27ae60", "#8e44ad", "#d68910", "#17a589", "#5d6d7e", "#a04000", "#2e86c1", "#7d3c98"]
MARKERY = ["o", "s", "^", "D", "v", "P", "X", "*", "<", ">"]
LINIE = ["-", "--", "-.", ":", (0, (5, 1)), (0, (3, 1, 1, 1)), "-", "--", "-.", ":"]


def legenda_pod(fig, ax=None, ncol: int = 4, uchwyty=None) -> None:
    """Wspólna legenda pod rysunkiem (nie zasłania danych)."""
    if uchwyty is None:
        h, l = (ax or fig.axes[0]).get_legend_handles_labels()
    else:
        h, l = uchwyty
    for a in fig.axes:
        if a.get_legend():
            a.get_legend().remove()
    fig.legend(h, l, loc="upper center", bbox_to_anchor=(0.5, 0.0), ncol=ncol, fontsize=7)


def styl(i: int) -> dict:
    return dict(color=KOLORY[i % len(KOLORY)], marker=MARKERY[i % len(MARKERY)], linestyle=LINIE[i % len(LINIE)])


def liczba(v: float, miejsca: int | None = None) -> str:
    """Liczba po polsku: przecinek dziesiętny, automatyczna precyzja."""
    if v is None or (isinstance(v, float) and (math.isnan(v) or math.isinf(v))):
        return "–" if not (isinstance(v, float) and math.isinf(v)) else "∞"
    if miejsca is None:
        a = abs(v)
        miejsca = 0 if a >= 100 else 1 if a >= 10 else 2 if a >= 1 else 3
    s = f"{v:.{miejsca}f}"
    return s.replace(".", ",")


def liczba_tex(v: float, miejsca: int | None = None) -> str:
    s = liczba(v, miejsca)
    if s == "∞":
        return r"$\infty$"
    cz, _, ul = s.partition(",")
    znak = "-" if cz.startswith("-") else ""
    cz = cz.lstrip("-")
    if len(cz) > 4:                                   # 12\,345 – odstęp tysięcy
        grupy = []
        while cz:
            grupy.insert(0, cz[-3:])
            cz = cz[:-3]
        cz = r"\,".join(grupy)
    return znak + cz + ("," + ul if ul else "")


def os_przecinek(ax, os_x: bool = True, os_y: bool = True) -> None:
    def fmt(v, _pos):
        if v == 0:
            return "0"
        a = abs(v)
        if a >= 1 and abs(v - round(v)) < 1e-9:
            return f"{int(round(v))}"
        return liczba(v, 0 if a >= 100 else 1 if a >= 1 else max(1, int(-math.floor(math.log10(a)))))
    f = ticker.FuncFormatter(fmt)
    if os_x:
        ax.xaxis.set_major_formatter(f)
        ax.xaxis.set_minor_formatter(ticker.NullFormatter())
    if os_y:
        ax.yaxis.set_major_formatter(f)
        ax.yaxis.set_minor_formatter(ticker.NullFormatter())


# =============================================================================
#  Wczytywanie i agregacja
# =============================================================================

@dataclass
class Eksperyment:
    nazwa: str
    res: pd.DataFrame
    cor: pd.DataFrame
    meta: dict


def wczytaj(katalog: Path, nazwa_pliku: str) -> Eksperyment | None:
    k = katalog / nazwa_pliku
    if not (k / "results.csv").exists():
        return None
    meta = json.loads((k / "meta.json").read_text(encoding="utf-8")) if (k / "meta.json").exists() else {}
    res = pd.read_csv(k / "results.csv", dtype={c: str for c in STR_COLS})
    cor = pd.read_csv(k / "correctness.csv", dtype={c: str for c in STR_COLS}) if (k / "correctness.csv").exists() else pd.DataFrame()
    for df in (res, cor):
        for c in STR_COLS:
            if c in df.columns:
                df[c] = df[c].fillna("")
        if "threads" in df.columns:
            df["threads"] = df["threads"].fillna(1).astype(int)
        if "chunk" in df.columns:
            df["chunk"] = df["chunk"].fillna(0).astype(int)
    # Po przerwaniu: grupy z mniejszą liczbą powtórzeń niż zaplanowano – pomijamy
    powt = meta.get("spec", {}).get("repeats")
    if powt and not meta.get("complete", True) and len(res):
        liczn = res.groupby(ID_COLS)["rep"].transform("count")
        res = res[liczn >= powt]
    if len(res):
        res["mpix"] = res["width"] * res["height"] / 1e6
    return Eksperyment(nazwa_pliku, res, cor, meta)


def agreguj(df: pd.DataFrame, kol: str = "wall_ms", klucze: list[str] | None = None) -> pd.DataFrame:
    klucze = klucze or ID_COLS
    g = df.groupby(klucze + ["width", "height"], dropna=False)[kol]
    out = g.agg(mediana="median", min="min", max="max", n="count").reset_index()
    out["mpix"] = out["width"] * out["height"] / 1e6
    return out


def amdahl(N: np.ndarray, s: float) -> np.ndarray:
    return 1.0 / (s + (1.0 - s) / N)


def dopasuj_amdahla(N: np.ndarray, S: np.ndarray) -> float:
    """s minimalizujące sum (S - Amdahl)^2 – przeszukanie siatki [0, 1]."""
    N, S = np.asarray(N, float), np.asarray(S, float)
    siatka = np.linspace(0.0, 1.0, 20001)
    bledy = [(np.sum((S - amdahl(N, s)) ** 2), s) for s in siatka]
    return min(bledy)[1]


ZNANE = {"1920x1080": ("hd", "HD 1920×1080"), "3840x2160": ("4k", "4K 3840×2160"),
         "7680x4320": ("8k", "8K 7680×4320")}


def klucz_obrazu(src: str) -> str:
    if src.startswith("synthetic:"):
        wh = src.split(":")[1]
        return "syn-" + wh
    stem = Path(src).stem
    if stem in ZNANE:
        return ZNANE[stem][0] + ("-jpg" if src.endswith(".jpg") else "")
    return stem.replace("_", "-")


def etykieta_obrazu(src: str, w_: int | None = None, h_: int | None = None) -> str:
    if src.startswith("synthetic:"):
        return "syntetyczny " + src.split(":")[1].replace("x", "×")
    stem = Path(src).stem
    if stem in ZNANE:
        return ZNANE[stem][1]
    nazwy = {"kodim08": "Kodak 08", "kodim23": "Kodak 23", "lena_gray": "Lena", "lena_color": "Lena"}
    if stem in nazwy:
        return f"{nazwy[stem]} {w_}×{h_}" if w_ else nazwy[stem]
    return stem.replace("x", "×")


def opis_wariantu(r) -> str:
    v = r["variant"]
    if v in ("T", "OMP") and r.get("kernel"):
        return f"{v}/{r['kernel']}"
    if v.startswith("GPU") and r.get("gpu_prec"):
        return f"{v} ({r['gpu_prec']})"
    return v


# =============================================================================
#  Wyjście: rysunki, tabele, makra
# =============================================================================

@dataclass
class Wyjscie:
    zrzuty: Path
    tabele: Path
    makra: dict[str, str] = field(default_factory=dict)
    rysunki: list[str] = field(default_factory=list)
    pliki_tabel: list[str] = field(default_factory=list)
    uwagi: list[str] = field(default_factory=list)

    def zapisz_rys(self, fig, nazwa: str, zrodlo: str) -> None:
        self.zrzuty.mkdir(parents=True, exist_ok=True)
        p = self.zrzuty / f"{nazwa}.pdf"
        fig.savefig(p)
        plt.close(fig)
        self.rysunki.append(p.name)
        w.info(f"  rysunek {p.name}  ({zrodlo})")

    def zapisz_tab(self, nazwa: str, tex: str) -> None:
        self.tabele.mkdir(parents=True, exist_ok=True)
        p = self.tabele / f"{nazwa}.tex"
        p.write_text("% Wygenerowane przez analiza/przetworz.py – nie edytować ręcznie\n" + tex, encoding="utf-8")
        self.pliki_tabel.append(p.name)
        w.info(f"  tabela  {p.name}")

    def m(self, klucz: str, wartosc, miejsca: int | None = None) -> None:
        klucz = klucz.replace("_", "-")
        if not re.fullmatch(r"[A-Za-z0-9/\-.]+", klucz):
            raise ValueError(f"zły klucz makra: {klucz}")
        if isinstance(wartosc, (int, float, np.floating, np.integer)):
            wartosc = liczba_tex(float(wartosc), miejsca)
        self.makra[klucz] = str(wartosc)


def tabela(naglowek: list[str], wiersze: list[list[str]], wyrownanie: str | None = None) -> str:
    wyr = wyrownanie or ("l" + "r" * (len(naglowek) - 1))
    lin = [r"\begin{tabular}{" + wyr + "}", r"\toprule", " & ".join(naglowek) + r" \\", r"\midrule"]
    lin += [" & ".join(r) + r" \\" for r in wiersze]
    lin += [r"\bottomrule", r"\end{tabular}", ""]
    return "\n".join(lin)


def tex_escape(s: str) -> str:
    return (s.replace("\\", r"\textbackslash{}").replace("_", r"\_").replace("%", r"\%")
             .replace("&", r"\&").replace("#", r"\#").replace("×", r"$\times$"))


# =============================================================================
#  Eksperymenty (5 wersji W1–W5; zasady: .claude/skills/prezentacja-wynikow)
# =============================================================================

WERSJE = ["W1", "W2", "W3", "W4", "W5"]
OPIS_W = {"W1": "W1 sekwencyjna", "W2": "W2 algorytm i pamięć", "W3": "W3 SIMD (AVX2)",
          "W4": "W4 wątki CPU", "W5": "W5 GPU"}
# Stałe kolory wersji (Okabe–Ito) + drugie kodowanie – skill prezentacja-wynikow, p. 3
STYL_W = {
    "W1": dict(color="#595959", marker="o", hatch=""),
    "W2": dict(color="#0072B2", marker="s", hatch="//"),
    "W3": dict(color="#009E73", marker="^", hatch="\\\\"),
    "W4": dict(color="#D55E00", marker="D", hatch=".."),
    "W5": dict(color="#CC79A7", marker="v", hatch="xx"),
}
# Kolejne serie (harmonogramy, maski, sposoby redukcji) – reszta palety Okabe–Ito
INNE = ["#E69F00", "#56B4E9", "#000000", "#0072B2", "#D55E00", "#009E73", "#CC79A7"]
INNE_M = ["o", "s", "^", "D", "v", "P", "X"]
INNE_L = ["-", "--", "-.", ":", (0, (5, 1)), (0, (3, 1, 1, 1)), "-"]


def styl_inny(i: int) -> dict:
    return dict(color=INNE[i % len(INNE)], marker=INNE_M[i % len(INNE_M)], linestyle=INNE_L[i % len(INNE_L)])


def linia_w(v: str) -> dict:
    s = STYL_W[v]
    return dict(color=s["color"], marker=s["marker"], linestyle="-", label=OPIS_W[v])


def nazwa_maski(mk: str) -> str:
    return {"sobel3": "Sobel 3×3", "sobel5": "Sobel 5×5", "sobel7": "Sobel 7×7", "prewitt": "Prewitt 3×3",
            "scharr": "Scharr 3×3", "roberts": "Roberts 2×2"}.get(mk, mk)


def obraz_wybierz(a: pd.DataFrame, wh: str) -> str | None:
    c = [o for o in dict.fromkeys(a["image"]) if wh in o]
    return c[0] if c else None


def slupki_wersji(ax, d: pd.DataFrame, baza: float, log: bool | None = None) -> None:
    """Słupki poziome W1–W5 (W1 na górze), etykieta „×N względem W1” na końcu słupka."""
    d = d.set_index("variant").reindex([v for v in WERSJE if v in set(d["variant"])])
    y = np.arange(len(d))[::-1]
    for yi, (v, x) in zip(y, d.iterrows()):
        st = STYL_W[v]
        ax.barh(yi, x["mediana"], 0.62, color=st["color"], hatch=st["hatch"], edgecolor="white", linewidth=0.4)
        ax.errorbar(x["mediana"], yi, xerr=[[x["mediana"] - x["min"]], [x["max"] - x["mediana"]]],
                    color="#222", capsize=2, linewidth=0.7)
        ax.text(x["max"] * (1.08 if log else 1.0) + (0 if log else 0.01 * d["max"].max()), yi,
                f" {liczba(x['mediana'])} ms  (×{liczba(baza / x['mediana'])})", va="center", fontsize=plt.rcParams["legend.fontsize"])
    ax.set_yticks(y, [OPIS_W[v] for v in d.index])
    rozp = d["mediana"].max() / d["mediana"].min()
    if log if log is not None else rozp > 20:
        ax.set_xscale("log")
        ax.set_xlim(d["min"].min() / 1.6, d["max"].max() * 12)
    else:
        ax.set_xlim(0, d["max"].max() * 1.55)
    ax.grid(axis="y", visible=False)
    os_przecinek(ax, os_y=False)


def e1(ex: Eksperyment, out: Wyjscie) -> None:
    """Porównanie W1–W5, sam algorytm. Pytanie: ile daje każdy rodzaj przyspieszenia?"""
    a = agreguj(ex.res)
    for mk in sorted(set(a["mask"])):
        obr = [o for o in (obraz_wybierz(a, "1920x1080"), obraz_wybierz(a, "3840x2160"), obraz_wybierz(a, "7680x4320")) if o]
        fig, osie = plt.subplots(1, len(obr), figsize=(SZER, WYS * 0.95), squeeze=False, sharey=True)
        for ax, o in zip(osie[0], obr):
            d = a[(a["mask"] == mk) & (a["image"] == o)]
            baza = d[d["variant"] == "W1"]["mediana"].iloc[0]
            slupki_wersji(ax, d, baza)
            ax.set_title(etykieta_obrazu(o), fontsize=plt.rcParams["axes.labelsize"])
            ax.set_xlabel("czas [ms]")
        fig.tight_layout()
        out.zapisz_rys(fig, f"e1_wersje_{mk}", "E1")

    # Ta sama wersja, dwie maski: ranking się zmienia
    o4 = obraz_wybierz(a, "3840x2160")
    if o4 and len(set(a["mask"])) > 1:
        fig, osie = plt.subplots(1, 2, figsize=(SZER, WYS * 0.95), squeeze=False, sharey=True)
        for ax, mk in zip(osie[0], ["sobel3", "sobel7"]):
            d = a[(a["mask"] == mk) & (a["image"] == o4)]
            slupki_wersji(ax, d, d[d["variant"] == "W1"]["mediana"].iloc[0], log=True)
            ax.set_title(f"{nazwa_maski(mk)}, obraz 4K", fontsize=plt.rcParams["axes.labelsize"])
            ax.set_xlabel("czas [ms] (skala log.)")
        fig.tight_layout()
        out.zapisz_rys(fig, "e1_wersje_maski", "E1")

    # Tabela + makra
    wiersze = []
    obr = [o for o in dict.fromkeys(a.sort_values("mpix")["image"])]
    for v in WERSJE:
        r = [OPIS_W[v]]
        for mk in ["sobel3", "sobel7"]:
            for o in [obraz_wybierz(a, "1920x1080"), o4, obraz_wybierz(a, "7680x4320")]:
                d = a[(a["variant"] == v) & (a["mask"] == mk) & (a["image"] == o)]
                b = a[(a["variant"] == "W1") & (a["mask"] == mk) & (a["image"] == o)]
                if mk == "sobel7" and o != o4:
                    continue
                r.append(f"{liczba_tex(d['mediana'].iloc[0])} ({liczba_tex(b['mediana'].iloc[0] / d['mediana'].iloc[0])}$\\times$)" if len(d) else "–")
        wiersze.append(r)
    out.zapisz_tab("e1_wersje", tabela(["Wersja", "HD, 3$\\times$3", "4K, 3$\\times$3", "8K, 3$\\times$3", "4K, 7$\\times$7"], wiersze))
    for o in obr:
        for mk in set(a["mask"]):
            d = a[(a["image"] == o) & (a["mask"] == mk)].set_index("variant")
            if "W1" not in d.index:
                continue
            for v in d.index:
                out.m(f"e1/{v}/{klucz_obrazu(o)}/{mk}/czas", d.loc[v, "mediana"])
                out.m(f"e1/{v}/{klucz_obrazu(o)}/{mk}/przysp", d.loc["W1", "mediana"] / d.loc[v, "mediana"])
    etapy = ex.res.groupby(["variant", "image", "mask"])[["conv_ms", "normalize_ms", "upload_ms", "kernel_ms", "download_ms"]].median()
    for (v, o, mk), x in etapy.iterrows():
        for kol, nazwa in [("conv_ms", "splot"), ("normalize_ms", "skalowanie"), ("upload_ms", "przesyl-do"),
                           ("kernel_ms", "kernel"), ("download_ms", "przesyl-z")]:
            if not math.isnan(x[kol]):
                out.m(f"e1/{v}/{klucz_obrazu(o)}/{mk}/{nazwa}", x[kol])
    # „pozostałe” = czas algorytmu poza splotem i skalowaniem (przydział buforów, synchronizacja) – CPU
    pelne = ex.res.groupby(["variant", "image", "mask"])[["wall_ms", "conv_ms", "normalize_ms"]].median()
    for (v, o, mk), x in pelne.iterrows():
        if v == "W5" or math.isnan(x["conv_ms"]) or math.isnan(x["normalize_ms"]):
            continue
        reszta = max(x["wall_ms"] - x["conv_ms"] - x["normalize_ms"], 0.0)
        out.m(f"e1/{v}/{klucz_obrazu(o)}/{mk}/pozostale", reszta)
        out.m(f"e1/{v}/{klucz_obrazu(o)}/{mk}/udzial-pozostale", 100 * reszta / x["wall_ms"], 0)


def e1b(ex: Eksperyment, out: Wyjscie) -> None:
    """Cały program (wczytanie + przetwarzanie + zapis) dla W1–W5."""
    r = ex.res
    a = r.groupby(["variant", "image", "mask", "width", "height"])[["wall_ms", "process_ms", "load_ms", "save_ms"]].median().reset_index()
    for mk in sorted(set(a["mask"])):
        obr = list(dict.fromkeys(a[a["mask"] == mk].sort_values("width")["image"]))
        obr = [o for o in obr if "x" in Path(o).stem] or obr
        fig, osie = plt.subplots(1, len(obr), figsize=(SZER, WYS * 0.95), squeeze=False, sharey=True)
        for ax, o in zip(osie[0], obr):
            d = a[(a["mask"] == mk) & (a["image"] == o)].set_index("variant").reindex(WERSJE).dropna(how="all")
            y = np.arange(len(d))[::-1]
            lewo = np.zeros(len(d))
            for i, (kol, nazwa) in enumerate([("load_ms", "wczytanie pliku"), ("process_ms", "przetwarzanie"), ("save_ms", "zapis JPG")]):
                v = d[kol].to_numpy()
                ax.barh(y, v, 0.62, left=lewo, color=["#BDBDBD", "#4D4D4D", "#8C8C8C"][i], hatch=["", "", "//"][i],
                        edgecolor="white", linewidth=0.4, label=nazwa)
                lewo += v
            b = d.loc["W1", "wall_ms"]
            for yi, (v_, x) in zip(y, d.iterrows()):
                ax.text(x["wall_ms"] * 1.01, yi, f" {liczba(x['wall_ms'])} ms (×{liczba(b / x['wall_ms'])})", va="center",
                        fontsize=plt.rcParams["legend.fontsize"])
            ax.set_yticks(y, [OPIS_W[v] for v in d.index])
            for t, v in zip(ax.get_yticklabels(), d.index):
                t.set_color(STYL_W[v]["color"])
            ax.set_xlim(0, d["wall_ms"].max() * 1.45)
            ax.set_xlabel("czas [ms]")
            ax.set_title(etykieta_obrazu(o) + (" (JPG)" if o.endswith(".jpg") else ""), fontsize=plt.rcParams["axes.labelsize"])
            ax.grid(axis="y", visible=False)
            os_przecinek(ax, os_y=False)
        fig.tight_layout()
        legenda_pod(fig, osie[0][0], ncol=3)
        out.zapisz_rys(fig, f"e1b_calosc_{mk}", "E1b")

    wiersze = []
    o4 = obraz_wybierz(a, "3840x2160")
    for v in WERSJE:
        d = a[(a["variant"] == v) & (a["image"] == o4) & (a["mask"] == "sobel3")]
        b = a[(a["variant"] == "W1") & (a["image"] == o4) & (a["mask"] == "sobel3")]
        if d.empty:
            continue
        x, bb = d.iloc[0], b.iloc[0]
        wiersze.append([OPIS_W[v], liczba_tex(x["load_ms"]), liczba_tex(x["process_ms"]), liczba_tex(x["save_ms"]),
                        liczba_tex(x["wall_ms"]), liczba_tex(bb["process_ms"] / x["process_ms"]), liczba_tex(bb["wall_ms"] / x["wall_ms"])])
    _rozklad_wersji(ex, out)
    out.zapisz_tab("e1b_calosc", tabela(["Wersja", "Wczyt. [ms]", "Algor. [ms]", "Zapis [ms]", "Razem [ms]",
                                         "$S$ algor.", "$S$ całości"], wiersze))
    for _, x in a.iterrows():
        k = f"e1b/{x['variant']}/{klucz_obrazu(x['image'])}/{x['mask']}"
        b = a[(a["variant"] == "W1") & (a["image"] == x["image"]) & (a["mask"] == x["mask"])].iloc[0]
        for kol, n in [("load_ms", "wczytanie"), ("process_ms", "przetwarzanie"), ("save_ms", "zapis"), ("wall_ms", "razem")]:
            out.m(f"{k}/{n}", x[kol])
        out.m(f"{k}/przysp-calosc", b["wall_ms"] / x["wall_ms"])
        out.m(f"{k}/przysp-algorytm", b["process_ms"] / x["process_ms"])
        out.m(f"{k}/udzial-io", (x["load_ms"] + x["save_ms"]) / x["wall_ms"] * 100, 1)


def _rozklad_wersji(ex: Eksperyment, out: Wyjscie) -> None:
    """Dla każdej wersji: (a) cały program = wczytanie / algorytm / zapis, (b) sam algorytm = jego etapy."""
    med = ex.res.groupby(["variant", "image", "mask", "width"])[
        ["wall_ms", "load_ms", "process_ms", "save_ms", "conv_ms", "normalize_ms", "upload_ms", "kernel_ms", "download_ms"]
    ].median().reset_index()
    jasne = {"W1": "#BDBDBD", "W2": "#9CC8E6", "W3": "#8FD3BE", "W4": "#F0B48C", "W5": "#E8C6DC"}
    fs = plt.rcParams["legend.fontsize"]

    ciemne = {s_["color"] for s_ in STYL_W.values()} | {"#4D4D4D", "#8C8C8C"}

    def slupek(ax, yi, czesci, razem):
        """Słupek 100 %: udziały etapów; na końcu czas bezwzględny (skala obrazów różni się 1000×)."""
        lewo = 0.0
        for wart, kolor, hatch, nazwa in czesci:
            if wart is None or (isinstance(wart, float) and math.isnan(wart)) or wart <= 0:
                continue
            proc = wart / razem * 100
            ax.barh(yi, proc, 0.6, left=lewo, color=kolor, hatch=hatch, edgecolor="white", linewidth=0.4, label=nazwa)
            if proc >= 8:
                ax.text(lewo + proc / 2, yi, f"{liczba(proc, 0)}%", ha="center", va="center", fontsize=fs * 0.9,
                        color="white" if kolor in ciemne else "black")
            lewo += proc
        ax.text(101, yi, f" {liczba(razem)} ms", va="center", fontsize=fs)

    for mk in sorted(set(med["mask"])):
        for v in [x for x in WERSJE if x in set(med["variant"])]:
            d = med[(med["variant"] == v) & (med["mask"] == mk)].sort_values("width")
            if d.empty:
                continue
            kol = STYL_W[v]["color"]
            fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(SZER, WYS * 0.85))
            y = np.arange(len(d))[::-1]
            etyk = [etykieta_obrazu(o) + (" (JPG)" if o.endswith(".jpg") else "") for o in d["image"]]
            for yi, (_, x) in zip(y, d.iterrows()):
                inne = x["wall_ms"] - x["load_ms"] - x["process_ms"] - x["save_ms"]
                slupek(ax1, yi, [(x["load_ms"], "#BDBDBD", "", "wczytanie pliku"),
                                 (x["process_ms"], kol, STYL_W[v]["hatch"], "algorytm"),
                                 (x["save_ms"], "#8C8C8C", "//", "zapis wyniku (JPG)"),
                                 (max(inne, 0.0), "#EEEEEE", "", "pozostałe (kopie obrazu, pomiar)")], x["wall_ms"])
                if v == "W5":
                    reszta = x["process_ms"] - x["upload_ms"] - x["kernel_ms"] - x["download_ms"]
                    czesci = [(x["upload_ms"], jasne[v], "//", "przesył do GPU"), (x["kernel_ms"], kol, "", "obliczenia na GPU"),
                              (x["download_ms"], jasne[v], "\\\\", "przesył z GPU"),
                              (max(reszta, 0.0), "#DDDDDD", "..", "pozostałe (sterowanie z CPU)")]
                else:
                    reszta = x["process_ms"] - x["conv_ms"] - (0 if math.isnan(x["normalize_ms"]) else x["normalize_ms"])
                    czesci = [(x["conv_ms"], kol, "", "splot i moduł gradientu"),
                              (x["normalize_ms"], jasne[v], "//", "skalowanie do 0–255"),
                              (max(reszta, 0.0), "#DDDDDD", "..", "pozostałe (przydział buforów, synchronizacja)")]
                slupek(ax2, yi, czesci, x["process_ms"])
            for ax, tyt in [(ax1, "cały program"), (ax2, "sam algorytm")]:
                ax.set_yticks(y, etyk)
                ax.set_xlim(0, 125)
                ax.set_xticks([0, 25, 50, 75, 100])
                ax.set_xlabel("udział w czasie [%]")
                ax.set_title(f"{OPIS_W[v]}: {tyt}", fontsize=plt.rcParams["axes.labelsize"])
                ax.grid(axis="y", visible=False)
                os_przecinek(ax, os_y=False)
            ax2.set_yticklabels([])
            fig.tight_layout()
            h1, l1 = ax1.get_legend_handles_labels()
            h2, l2 = ax2.get_legend_handles_labels()
            uniq = dict(zip(l1 + l2, h1 + h2))
            legenda_pod(fig, uchwyty=(list(uniq.values()), list(uniq.keys())), ncol=3)
            out.zapisz_rys(fig, f"e1b_rozklad_{v}_{mk}", "E1b")
            for _, x in d.iterrows():
                k = f"e1b/{v}/{klucz_obrazu(x['image'])}/{mk}"
                out.m(f"{k}/udzial-algorytmu", x["process_ms"] / x["wall_ms"] * 100, 1)
                out.m(f"{k}/udzial-wczytania", x["load_ms"] / x["wall_ms"] * 100, 1)
                out.m(f"{k}/udzial-zapisu", x["save_ms"] / x["wall_ms"] * 100, 1)


def e2(ex: Eksperyment, out: Wyjscie) -> None:
    """Maska: czas k = 3/5/7 dla każdej wersji (złożoność k² vs 2k)."""
    a = agreguj(ex.res)
    sob = a[a["mask"].isin(["sobel3", "sobel5", "sobel7"])].copy()
    sob["k"] = sob["mask"].str[-1].astype(int)
    fig, ax = plt.subplots(figsize=(SZER * 0.62, WYS))
    for v in [x for x in WERSJE if x in set(sob["variant"])]:
        d = sob[sob["variant"] == v].sort_values("k")
        ax.errorbar(d["k"], d["mediana"], yerr=[d["mediana"] - d["min"], d["max"] - d["mediana"]], capsize=2, **linia_w(v))
    ax.set_yscale("log")
    ax.set_xticks([3, 5, 7], ["3×3", "5×5", "7×7"])
    ax.set_xlabel("rozmiar maski Sobela")
    ax.set_ylabel("czas [ms] (skala log.)")
    os_przecinek(ax, os_x=False)
    ax.legend(loc="center left", bbox_to_anchor=(1.01, 0.5))
    out.zapisz_rys(fig, "e2_maski", "E2")

    maski = [m for m in ["sobel3", "sobel5", "sobel7", "prewitt", "scharr", "roberts"] if m in set(a["mask"])]
    wiersze = []
    for mk in maski:
        d = a[a["mask"] == mk].set_index("variant")
        r = [tex_escape(nazwa_maski(mk))]
        for v in WERSJE:
            if v in d.index:
                r.append(liczba_tex(d.loc[v, "mediana"]))
                out.m(f"e2/{v}/{mk}/czas", d.loc[v, "mediana"])
                out.m(f"e2/{v}/{mk}/przysp", d.loc["W1", "mediana"] / d.loc[v, "mediana"])
            else:
                r.append("–")
        wiersze.append(r)
    out.zapisz_tab("e2_maski", tabela(["Maska"] + [f"{v} [ms]" for v in WERSJE], wiersze))
    for v in WERSJE:
        d = sob[sob["variant"] == v].set_index("k")["mediana"]
        if 3 in d.index and 7 in d.index:
            out.m(f"e2/{v}/wzrost-3-7", d[7] / d[3])


def _linie_topologii(ax, maks: int = 48) -> None:
    for n, opis in ((6, "6 = 1 CCD"), (12, "12 = rdzenie fizyczne"), (24, "24 = wątki SMT")):
        if n <= maks:
            ax.axvline(n, color="#999", linewidth=0.6, linestyle=":")
            ax.text(n + 0.4, 0.03, opis, rotation=90, fontsize=plt.rcParams["legend.fontsize"] * 0.85, color="#666",
                    va="bottom", ha="left", transform=ax.get_xaxis_transform())


def e3(ex: Eksperyment, out: Wyjscie) -> None:
    """W4: S(N), E(N) i prawo Amdahla."""
    a = agreguj(ex.res)
    obr = list(dict.fromkeys(a.sort_values("mpix")["image"]))
    nmax = int(a["threads"].max())
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(SZER, WYS))
    NN = np.linspace(1, nmax, 200)
    ax1.plot(NN, NN, color="#999", linewidth=0.8, linestyle="--", label="idealne S = N")
    wiersze = []
    for i, o in enumerate(obr):
        d = a[a["image"] == o].sort_values("threads")
        t1 = d[d["threads"] == 1]["mediana"].iloc[0]
        d = d.assign(S=t1 / d["mediana"], E=t1 / d["mediana"] / d["threads"])
        fit = d[d["threads"] <= 24]
        s = dopasuj_amdahla(fit["threads"], fit["S"])
        st = styl_inny(i)
        ax1.plot(d["threads"], d["S"], label=f"{etykieta_obrazu(o)}, T(1) = {liczba(t1)} ms", **st)
        ax1.plot(NN, amdahl(NN, s), color=st["color"], linewidth=0.7, linestyle=":", label=f"Amdahl, s = {liczba(s * 100, 1)}%")
        ax2.plot(d["threads"], d["E"], **st)
        ko = klucz_obrazu(o)
        best = d.loc[d["S"].idxmax()]
        out.m(f"e3/{ko}/t1", t1)
        out.m(f"e3/{ko}/s", s * 100, 1)
        out.m(f"e3/{ko}/Smax", best["S"])
        out.m(f"e3/{ko}/Nmax", int(best["threads"]), 0)
        for n in (2, 4, 6, 8, 12, 16, 24, 32, 48):
            dn = d[d["threads"] == n]
            if len(dn):
                out.m(f"e3/{ko}/S{n}", dn["S"].iloc[0])
                out.m(f"e3/{ko}/E{n}", dn["E"].iloc[0] * 100, 0)
                out.m(f"e3/{ko}/t{n}", dn["mediana"].iloc[0])
        r12 = d[d["threads"] == 12]["S"]
        r24 = d[d["threads"] == 24]["S"]
        wiersze.append([tex_escape(etykieta_obrazu(o)), liczba_tex(t1), liczba_tex(r12.iloc[0]) if len(r12) else "–",
                        liczba_tex(r24.iloc[0]) if len(r24) else "–", f"{liczba_tex(best['S'])} ({int(best['threads'])})",
                        liczba_tex(s * 100, 1)])
    ax1.set_xlabel("liczba wątków N")
    ax1.set_ylabel("przyspieszenie S(N) = T(1)/T(N)")
    ax1.set_ylim(0, 25)
    ax2.set_xlabel("liczba wątków N")
    ax2.set_ylabel("efektywność E(N) = S(N)/N")
    ax2.set_ylim(0, 1.1)
    for ax in (ax1, ax2):
        _linie_topologii(ax, nmax)
        os_przecinek(ax)
    fig.tight_layout()
    legenda_pod(fig, ax1, ncol=3)
    out.zapisz_rys(fig, "e3_skalowanie", "E3")
    out.zapisz_tab("e3_watki", tabela(["Obraz", "T(1) [ms]", "S(12)", "S(24)", "maks. S (N)", "s [\\%]"], wiersze))


def e4(ex: Eksperyment, out: Wyjscie) -> None:
    """W4: harmonogram × rozmiar fragmentu."""
    a = agreguj(ex.res)
    imb = agreguj(ex.res, "imbalance_ms")
    harm = ["static", "cyclic", "dynamic", "guided"]
    nn = sorted(set(a["threads"]))
    fig, osie = plt.subplots(1, len(nn), figsize=(SZER, WYS), squeeze=False, sharey=True)
    for ax, n in zip(osie[0], nn):
        for i, h in enumerate(harm):
            d = a[(a["schedule"] == h) & (a["threads"] == n)].sort_values("chunk")
            if d.empty:
                continue
            if h == "static":
                ax.axhline(d["mediana"].iloc[0], color=INNE[i], linestyle="--", linewidth=1, label="static (N równych bloków)")
                continue
            ax.errorbar(d["chunk"], d["mediana"], yerr=[d["mediana"] - d["min"], d["max"] - d["mediana"]], capsize=1.5, label=h, **styl_inny(i))
        ax.set_xscale("log", base=2)
        ax.set_xlabel("rozmiar fragmentu [wiersze]")
        ax.set_title(f"{n} wątków", fontsize=plt.rcParams["axes.labelsize"])
        os_przecinek(ax)
    osie[0][0].set_ylabel("czas [ms]")
    fig.tight_layout()
    legenda_pod(fig, osie[0][0], ncol=4)
    out.zapisz_rys(fig, "e4_harmonogramy", "E4")
    for n in nn:
        d = a[a["threads"] == n]
        best, worst = d.loc[d["mediana"].idxmin()], d.loc[d["mediana"].idxmax()]
        opis = lambda x: x["schedule"] + ("" if x["schedule"] == "static" else f", {int(x['chunk'])} w.")
        out.m(f"e4/t{n}/najlepszy", opis(best))
        out.m(f"e4/t{n}/najlepszy-czas", best["mediana"])
        out.m(f"e4/t{n}/najgorszy", opis(worst))
        out.m(f"e4/t{n}/najgorszy-czas", worst["mediana"])
        for h in harm:
            for _, x in d[d["schedule"] == h].iterrows():
                out.m(f"e4/t{n}/{h}/c{int(x['chunk'])}", x["mediana"])
        i24 = imb[imb["threads"] == n]
        for h in harm:
            for _, x in i24[i24["schedule"] == h].iterrows():
                out.m(f"e4/t{n}/{h}/c{int(x['chunk'])}/nierownowaga", x["mediana"])


def e5(ex: Eksperyment, out: Wyjscie) -> None:
    """W5: przesył vs obliczenia; czas w funkcji rozmiaru wobec W1 i W4."""
    r = ex.res.copy()
    r["opis"] = np.where(r["variant"] == "W5", "W5 GPU (" + r["gpu_prec"] + ")", r["variant"].map(OPIS_W))
    etapy = ["upload_ms", "kernel_ms", "download_ms"]
    med = r.groupby(["opis", "variant", "mask", "image", "width", "height", "gpu_prec"], dropna=False)[["wall_ms"] + etapy].median().reset_index()
    med["mpix"] = med["width"] * med["height"] / 1e6
    o4 = obraz_wybierz(med, "3840x2160")

    # (a) etapy W5 dla 4K – słupki skumulowane na skali LINIOWEJ (na log proporcje odcinków byłyby fałszywe);
    #     odniesienie: W4 (najszybsza wersja CPU); W1 jest na rysunku E1
    maski = [m for m in ["sobel3", "sobel7"] if m in set(med["mask"])]
    fig, osie = plt.subplots(1, len(maski), figsize=(SZER, WYS * 0.85), squeeze=False, sharey=True)
    for ax, mk in zip(osie[0], maski):
        d = med[(med["image"] == o4) & (med["mask"] == mk)]
        d = pd.concat([d[d["variant"] == "W4"], d[d["variant"] == "W5"].sort_values("gpu_prec")])
        y = np.arange(len(d))[::-1]
        for yi, (_, x) in zip(y, d.iterrows()):
            if x["variant"] == "W5":
                lewo = 0.0
                for i, (e, nazwa) in enumerate(zip(etapy, ["przesył do GPU", "obliczenia na GPU", "przesył z GPU"])):
                    ax.barh(yi, x[e], 0.62, left=lewo, color=["#E8C6DC", STYL_W["W5"]["color"], "#E8C6DC"][i],
                            hatch=["//", "", "\\\\"][i], edgecolor="white", linewidth=0.4,
                            label=nazwa if (yi == y[-1] and mk == maski[0]) else None)
                    lewo += x[e]
            else:
                st = STYL_W[x["variant"]]
                ax.barh(yi, x["wall_ms"], 0.62, color=st["color"], hatch=st["hatch"], edgecolor="white", linewidth=0.4)
            ax.text(x["wall_ms"] * 1.02, yi, f" {liczba(x['wall_ms'])} ms", va="center", fontsize=plt.rcParams["legend.fontsize"])
        ax.set_xlim(0, d["wall_ms"].max() * 1.3)
        ax.set_yticks(y, list(d["opis"]))
        ax.set_xlabel("czas [ms]")
        ax.set_title(f"{nazwa_maski(mk)}, obraz 4K", fontsize=plt.rcParams["axes.labelsize"])
        ax.grid(axis="y", visible=False)
        os_przecinek(ax, os_y=False)
    fig.tight_layout()
    legenda_pod(fig, osie[0][0], ncol=3)
    out.zapisz_rys(fig, "e5_gpu_etapy", "E5")

    # (b) czas w funkcji rozmiaru
    fig, osie = plt.subplots(1, len(maski), figsize=(SZER, WYS * 1.05), squeeze=False)
    for ax, mk in zip(osie[0], maski):
        for v in ["W1", "W4", "W5"]:
            d = med[(med["mask"] == mk) & (med["variant"] == v) & (med["gpu_prec"].isin(["", "auto"]))].sort_values("mpix")
            ax.plot(d["mpix"], d["wall_ms"], **linia_w(v))
            if v == "W5":
                ax.plot(d["mpix"], d["kernel_ms"], color=STYL_W["W5"]["color"], linestyle=":", marker="",
                        label="W5: same obliczenia (bez przesyłu)")
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel("rozmiar obrazu [Mpix]")
        ax.set_ylabel("czas [ms]")
        ax.set_title(nazwa_maski(mk), fontsize=plt.rcParams["axes.labelsize"])
        os_przecinek(ax)
    fig.tight_layout()
    legenda_pod(fig, osie[0][0], ncol=4)
    out.zapisz_rys(fig, "e5_gpu_rozmiar", "E5")

    for _, x in med.iterrows():
        k = f"e5/{x['variant']}{'-' + x['gpu_prec'] if x['gpu_prec'] else ''}/{x['mask']}/{klucz_obrazu(x['image'])}"
        out.m(k + "/czas", x["wall_ms"])
        for e in etapy:
            if not math.isnan(x[e]):
                out.m(k + "/" + e.replace("_ms", ""), x[e])
    # od jakiego rozmiaru W5 (z przesyłem) szybsze niż W4 / W1
    for mk in maski:
        for ref in ["W1", "W4"]:
            g = med[(med["mask"] == mk) & (med["variant"] == "W5") & (med["gpu_prec"] == "auto")].set_index("image")["wall_ms"]
            b = med[(med["mask"] == mk) & (med["variant"] == ref)].set_index("image")["wall_ms"]
            mp = med.drop_duplicates("image").set_index("image")["mpix"]
            wsp = sorted((mp[i], b[i] / g[i]) for i in g.index if i in b.index)
            prog = next((m for m, s in wsp if s > 1), float("nan"))
            out.m(f"e5/{mk}/W5-szybsze-od-{ref}-od-mpix", prog, 2)
    if len(ex.cor):
        c = ex.cor[ex.cor["variant"] == "W5"]
        for p in ["auto", "fp32"]:
            cc = c[c["gpu_prec"] == p]
            if len(cc):
                out.m(f"e5/{p}/maks-roznica", int(pd.to_numeric(cc["max_abs_diff"]).max()), 0)


def e5b(ex: Eksperyment, out: Wyjscie) -> None:
    """W5: liczba wątków GPU (work-items) i rozmiar grupy roboczej."""
    r = ex.res
    a = r.groupby(["image", "gpu_wg", "gpu_ppw", "width"]).agg(items=("gpu_items", "first"), kernel=("kernel_ms", "median"),
                                                                 kmin=("kernel_ms", "min"), kmax=("kernel_ms", "max")).reset_index()
    obr = list(dict.fromkeys(a.sort_values("width")["image"]))
    wgs = sorted(set(a["gpu_wg"]), key=lambda s: (int(s.split("x")[0]) * int(s.split("x")[1]), s))
    fig, osie = plt.subplots(1, len(obr), figsize=(SZER, WYS), squeeze=False)
    for ax, o in zip(osie[0], obr):
        for i, wg in enumerate(wgs):
            d = a[(a["image"] == o) & (a["gpu_wg"] == wg)].sort_values("items")
            ax.errorbar(d["items"] / 1e6, d["kernel"], yerr=[d["kernel"] - d["kmin"], d["kmax"] - d["kernel"]], capsize=1.5,
                        label=f"grupa robocza {wg.replace('x', '×')}", **styl_inny(i))
        ax.set_xscale("log")
        ax.set_xlabel("liczba wątków GPU (work-items) [mln] (skala log.)")
        ax.set_ylabel("czas obliczeń na GPU [ms]")
        ax.set_title(etykieta_obrazu(o), fontsize=plt.rcParams["axes.labelsize"])
        os_przecinek(ax)
    fig.tight_layout()
    legenda_pod(fig, osie[0][0], ncol=4)
    out.zapisz_rys(fig, "e5b_gpu_watki", "E5b")
    for _, x in a.iterrows():
        out.m(f"e5b/{klucz_obrazu(x['image'])}/{x['gpu_wg']}/ppw{int(x['gpu_ppw'])}/kernel", x["kernel"])
        out.m(f"e5b/{klucz_obrazu(x['image'])}/{x['gpu_wg']}/ppw{int(x['gpu_ppw'])}/watki", int(x["items"]), 0)
    for o in obr:
        d = a[a["image"] == o]
        best, worst = d.loc[d["kernel"].idxmin()], d.loc[d["kernel"].idxmax()]
        ko = klucz_obrazu(o)
        out.m(f"e5b/{ko}/najlepszy", f"{best['gpu_wg'].replace('x', '$\\times$')}, {int(best['gpu_ppw'])} px/wątek")
        out.m(f"e5b/{ko}/najlepszy-czas", best["kernel"])
        out.m(f"e5b/{ko}/najlepszy-watki", int(best["items"]), 0)
        out.m(f"e5b/{ko}/najgorszy", f"{worst['gpu_wg'].replace('x', '$\\times$')}, {int(worst['gpu_ppw'])} px/wątek")
        out.m(f"e5b/{ko}/najgorszy-czas", worst["kernel"])


def e6(ex: Eksperyment, out: Wyjscie) -> None:
    """W4: pula wątków vs nowe wątki w każdym etapie."""
    a = agreguj(ex.res)
    obr = list(dict.fromkeys(a.sort_values("mpix")["image"]))
    fig, osie = plt.subplots(1, len(obr), figsize=(SZER, WYS), squeeze=False)
    for ax, o in zip(osie[0], obr):
        i = 0
        for m, mo in [("pool", "pula"), ("spawn", "nowe wątki w każdym etapie")]:
            for h in ["static", "dynamic"]:
                d = a[(a["image"] == o) & (a["mech"] == m) & (a["schedule"] == h)].sort_values("threads")
                if not d.empty:
                    ax.plot(d["threads"], d["mediana"], label=f"{mo}, {h}", **styl_inny(i))
                    for _, x in d.iterrows():
                        out.m(f"e6/{m}/{h}/{klucz_obrazu(o)}/t{x['threads']}", x["mediana"])
                i += 1
        ax.set_yscale("log")
        ax.set_xlabel("liczba wątków N")
        ax.set_ylabel("czas [ms] (skala log.)")
        ax.set_title(etykieta_obrazu(o), fontsize=plt.rcParams["axes.labelsize"])
        os_przecinek(ax)
    fig.tight_layout()
    legenda_pod(fig, osie[0][0], ncol=4)
    out.zapisz_rys(fig, "e6_mechanizmy", "E6")


def e8(ex: Eksperyment, out: Wyjscie) -> None:
    """W4: koszt bariery wynikającej ze skalowania do globalnego maksimum."""
    a = agreguj(ex.res)
    nazwy = {"global": "globalne maksimum (2 etapy + bariera)", "fixed": "stała skala (1 etap)", "clamp": "obcięcie (1 etap)"}
    fig, ax = plt.subplots(figsize=(SZER * 0.75, WYS))
    for i, sc in enumerate(["global", "fixed", "clamp"]):
        d = a[a["scaling"] == sc].sort_values("threads")
        if d.empty:
            continue
        ax.plot(d["threads"], d["mediana"], label=nazwy[sc], **styl_inny(i))
        for _, x in d.iterrows():
            out.m(f"e8/{sc}/t{x['threads']}", x["mediana"])
    ax.set_yscale("log")
    ax.set_xlabel("liczba wątków N")
    ax.set_ylabel("czas [ms] (skala log.)")
    os_przecinek(ax)
    ax.legend(loc="upper right")
    out.zapisz_rys(fig, "e8_normalizacja", "E8")


def e8b(ex: Eksperyment, out: Wyjscie) -> None:
    """W4: sposób wyznaczania globalnego maksimum (synchronizacja, false sharing)."""
    a = agreguj(ex.res)
    nazwy = {"local": "lokalne maksimum + redukcja", "atomic": "atomic CAS na piksel", "mutex": "mutex na piksel",
             "px_pad": "zapis na piksel, osobne linie cache", "px_nopad": "zapis na piksel, jedna linia cache (false sharing)"}
    n = int(a["threads"].max())
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(SZER, WYS), gridspec_kw={"width_ratios": [1.1, 1]})
    d = a[a["threads"] == n].set_index("reduce").reindex(["local", "atomic", "px_pad", "px_nopad", "mutex"]).dropna(how="all")
    y = np.arange(len(d))[::-1]
    for i, (yi, (red, x)) in enumerate(zip(y, d.iterrows())):
        ax1.barh(yi, x["mediana"], 0.62, color=INNE[i], edgecolor="white")
        ax1.errorbar(x["mediana"], yi, xerr=[[x["mediana"] - x["min"]], [x["max"] - x["mediana"]]], color="#222", capsize=2, linewidth=0.7)
        ax1.text(x["max"] * 1.15, yi, f"{liczba(x['mediana'])} ms", va="center", fontsize=plt.rcParams["legend.fontsize"])
    ax1.set_yticks(y, [nazwy[r] for r in d.index])
    ax1.set_xscale("log")
    ax1.set_xlim(d["min"].min() / 2, d["max"].max() * 8)
    ax1.set_xlabel(f"czas [ms] (skala log.), {n} wątków")
    ax1.grid(axis="y", visible=False)
    os_przecinek(ax1, os_y=False)
    for i, red in enumerate(d.index):
        dd = a[a["reduce"] == red].sort_values("threads")
        ax2.plot(dd["threads"], dd["mediana"], color=INNE[i], marker=INNE_M[i], linestyle=INNE_L[i])
        for _, x in dd.iterrows():
            out.m(f"e8b/{red}/t{x['threads']}", x["mediana"])
    ax2.set_yscale("log")
    ax2.set_xlabel("liczba wątków N")
    ax2.set_ylabel("czas [ms] (skala log.)")
    os_przecinek(ax2)
    fig.tight_layout()
    out.zapisz_rys(fig, "e8b_redukcja", "E8b")
    for red in d.index:
        out.m(f"e8b/{red}/wzgl-local-t{n}", d.loc[red, "mediana"] / d.loc["local", "mediana"])


def e9(ex: Eksperyment, out: Wyjscie) -> None:
    """W4, cały program z JPG: prawo Amdahla."""
    a = ex.res.groupby(["image", "threads", "width"])[["wall_ms", "process_ms", "load_ms", "save_ms"]].median().reset_index()
    obr = list(dict.fromkeys(a.sort_values("width")["image"]))
    fig, osie = plt.subplots(1, len(obr), figsize=(SZER, WYS), squeeze=False)
    for ax, o in zip(osie[0], obr):
        d = a[a["image"] == o].sort_values("threads")
        t1 = d[d["threads"] == 1].iloc[0]
        S_cal, S_prz = t1["wall_ms"] / d["wall_ms"], t1["process_ms"] / d["process_ms"]
        s_zm = (t1["load_ms"] + t1["save_ms"]) / t1["wall_ms"]
        NN = np.linspace(1, d["threads"].max(), 200)
        ax.plot(NN, NN, color="#999", linestyle="--", linewidth=0.8, label="idealne S = N")
        ax.plot(d["threads"], S_prz, label="sam algorytm", **styl_inny(0))
        ax.plot(d["threads"], S_cal, label="cały program (wczytanie + algorytm + zapis JPG)", **styl_inny(1))
        ax.plot(NN, amdahl(NN, s_zm), color=INNE[1], linestyle=":", linewidth=0.8, label="prawo Amdahla dla zmierzonego s")
        ax.set_xlabel("liczba wątków N")
        ax.set_ylabel("przyspieszenie S(N)")
        ax.set_title(f"{etykieta_obrazu(o)}: T(1) = {liczba(t1['wall_ms'])} ms, s = {liczba(s_zm * 100, 1)}%",
                     fontsize=plt.rcParams["axes.labelsize"])
        ax.set_ylim(0, max(S_prz.max() * 1.15, 2))
        os_przecinek(ax)
        ko = klucz_obrazu(o)
        tn = d.iloc[-1]
        n = int(tn["threads"])
        for k, v, m in [("s", s_zm * 100, 1), ("limit", 1 / s_zm, None), ("wczytanie", t1["load_ms"], None),
                        ("zapis", t1["save_ms"], None), ("przetw-t1", t1["process_ms"], None),
                        (f"przetw-t{n}", tn["process_ms"], None), ("calosc-t1", t1["wall_ms"], None),
                        (f"calosc-t{n}", tn["wall_ms"], None), (f"S-calosc-t{n}", S_cal.iloc[-1], None),
                        (f"S-przetw-t{n}", S_prz.iloc[-1], None)]:
            out.m(f"e9/{ko}/{k}", v, m)
    fig.tight_layout()
    legenda_pod(fig, osie[0][0], ncol=2)
    out.zapisz_rys(fig, "e9_amdahl", "E9")


def e10(ex: Eksperyment, out: Wyjscie) -> None:
    """W4: przypinanie wątków do rdzeni."""
    a = agreguj(ex.res)
    nazwy = {"none": "bez przypinania (system)", "compact": "compact: najpierw 1 CCD", "spread": "spread: naprzemiennie CCD",
             "nosmt": "nosmt: 1 wątek na rdzeń"}
    nn = sorted(set(a["threads"]))
    fig, ax = plt.subplots(figsize=(SZER, WYS))
    x = np.arange(len(nn))
    sz = 0.8 / 4
    for i, af in enumerate(["none", "compact", "spread", "nosmt"]):
        d = a[a["affinity"] == af].set_index("threads").reindex(nn)
        ax.bar(x + (i - 1.5) * sz, d["mediana"], sz, color=INNE[i], edgecolor="white", linewidth=0.4,
               hatch=["", "//", "..", "xx"][i], label=nazwy[af],
               yerr=[d["mediana"] - d["min"], d["max"] - d["mediana"]], capsize=1.5, ecolor="#333")
        for n_, v in zip(nn, d["mediana"]):
            if not math.isnan(v):
                out.m(f"e10/{af}/t{n_}", v)
    ax.set_xticks(x, [str(n) for n in nn])
    ax.set_xlabel("liczba wątków N")
    ax.set_ylabel("czas [ms]")
    ax.grid(axis="x", visible=False)
    os_przecinek(ax, os_x=False)
    legenda_pod(fig, ax, ncol=4)
    out.zapisz_rys(fig, "e10_topologia", "E10")


def e13(ex: Eksperyment, out: Wyjscie) -> None:
    """Rozmiar obrazu: czas, przepustowość i przyspieszenie względem W1 dla W1–W5."""
    a = agreguj(ex.res)
    seria = a[~a["image"].str.contains("2048x2048|16384x256")]
    maski = [m for m in ["sobel3", "sobel7"] if m in set(a["mask"])]
    wersje = [v for v in WERSJE if v in set(a["variant"])]

    fig, osie = plt.subplots(1, len(maski), figsize=(SZER, WYS * 1.05), squeeze=False)
    for ax, mk in zip(osie[0], maski):
        for v in wersje:
            d = seria[(seria["mask"] == mk) & (seria["variant"] == v)].sort_values("mpix")
            ax.plot(d["mpix"], d["mediana"], **linia_w(v))
        d0 = seria[(seria["mask"] == mk) & (seria["variant"] == "W1")].sort_values("mpix")
        xs = np.array([d0["mpix"].min(), d0["mpix"].max()])
        ax.plot(xs, d0["mediana"].iloc[-1] * xs / xs[-1], color="#999", linewidth=0.7, linestyle="--", label="O(n)")
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel("rozmiar obrazu n [Mpix]")
        ax.set_ylabel("czas [ms]")
        ax.set_title(nazwa_maski(mk), fontsize=plt.rcParams["axes.labelsize"])
        os_przecinek(ax)
    fig.tight_layout()
    legenda_pod(fig, osie[0][0], ncol=6)
    out.zapisz_rys(fig, "e13_czas", "E13")

    fig, osie = plt.subplots(1, len(maski), figsize=(SZER, WYS * 1.05), squeeze=False, sharey=True)
    for ax, mk in zip(osie[0], maski):
        baza = seria[(seria["mask"] == mk) & (seria["variant"] == "W1")].set_index("image")["mediana"]
        for v in wersje[1:]:
            d = seria[(seria["mask"] == mk) & (seria["variant"] == v)].sort_values("mpix")
            ax.plot(d["mpix"], baza.reindex(d["image"]).to_numpy() / d["mediana"].to_numpy(), **linia_w(v))
        ax.axhline(1, color="#595959", linewidth=0.8, label="W1 (= 1)")
        ax.set_xscale("log")
        ax.set_xlabel("rozmiar obrazu n [Mpix]")
        ax.set_ylabel("przyspieszenie względem W1")
        ax.set_title(nazwa_maski(mk), fontsize=plt.rcParams["axes.labelsize"])
        os_przecinek(ax)
    fig.tight_layout()
    legenda_pod(fig, osie[0][0], ncol=5)
    out.zapisz_rys(fig, "e13_przyspieszenie", "E13")

    fig, osie = plt.subplots(1, len(maski), figsize=(SZER, WYS * 1.05), squeeze=False)
    for ax, mk in zip(osie[0], maski):
        for v in wersje:
            d = seria[(seria["mask"] == mk) & (seria["variant"] == v)].sort_values("mpix")
            ax.plot(d["mpix"], d["mpix"] / (d["mediana"] / 1000.0), **linia_w(v))
        for b, opis in [(1 << 20, "L2 1 MB"), (32 << 20, "L3 jednego CCD"), (64 << 20, "L3 obu CCD")]:
            n6 = b / 6 / 1e6
            ax.axvline(n6, color="#888", linewidth=0.6, linestyle=":")
            ax.text(n6 * 1.08, 0.03, f"{opis} (6 B/piksel)", rotation=90, fontsize=plt.rcParams["legend.fontsize"] * 0.8,
                    color="#666", va="bottom", ha="left", transform=ax.get_xaxis_transform())
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel("rozmiar obrazu n [Mpix]")
        ax.set_ylabel("przepustowość [Mpix/s]")
        ax.set_title(nazwa_maski(mk), fontsize=plt.rcParams["axes.labelsize"])
        os_przecinek(ax)
    fig.tight_layout()
    legenda_pod(fig, osie[0][0], ncol=5)
    out.zapisz_rys(fig, "e13_przepustowosc", "E13")

    for mk in maski:
        baza = seria[(seria["mask"] == mk) & (seria["variant"] == "W1")].set_index("image")["mediana"]
        for v in wersje:
            d = seria[(seria["mask"] == mk) & (seria["variant"] == v)].sort_values("mpix")
            S = baza.reindex(d["image"]).to_numpy() / d["mediana"].to_numpy()
            for (_, x), s_ in zip(d.iterrows(), S):
                out.m(f"e13/{v}/{mk}/{x['width']}x{x['height']}/czas", x["mediana"])
                out.m(f"e13/{v}/{mk}/{x['width']}x{x['height']}/przysp", s_)
            if v != "W1":
                prog = d["mpix"].to_numpy()[np.argmax(S > 1)] if np.any(S > 1) else float("nan")
                out.m(f"e13/{v}/{mk}/oplaca-sie-od-mpix", prog, 2)
                out.m(f"e13/{v}/{mk}/przysp-max", float(np.nanmax(S)))
    ks = a[a["image"].str.contains("2731x1536|2048x2048|16384x256") & (a["mask"] == "sobel3")]
    wiersze = []
    for v in wersje:
        r = [OPIS_W[v]]
        for im in ["2731x1536", "2048x2048", "16384x256"]:
            d = ks[(ks["variant"] == v) & ks["image"].str.contains(im)]
            r.append(liczba_tex(d["mediana"].iloc[0]) if len(d) else "–")
            if len(d):
                out.m(f"e13/proporcje/{v}/{im}", d["mediana"].iloc[0])
        wiersze.append(r)
    out.zapisz_tab("e13_proporcje", tabela(["Wersja", r"2731$\times$1536 [ms]", r"2048$\times$2048 [ms]", r"16384$\times$256 [ms]"], wiersze))


def e13b(ex: Eksperyment, out: Wyjscie) -> None:
    """Treść obrazu: syntetyczny vs zdjęcie tego samego rozmiaru."""
    r = ex.res.copy()
    r["zrodlo"] = np.where(r["image"].str.startswith("synthetic:"), "syntetyczny", "zdjęcie")
    a = r.groupby(["variant", "zrodlo", "width", "height"])["wall_ms"].median().unstack("zrodlo").reset_index()
    if "syntetyczny" not in a or "zdjęcie" not in a:
        return
    a["roznica"] = (a["zdjęcie"] / a["syntetyczny"] - 1) * 100
    wiersze = [[OPIS_W[x["variant"]], f"{x['width']}$\\times${x['height']}", liczba_tex(x["zdjęcie"]), liczba_tex(x["syntetyczny"]),
                liczba_tex(x["roznica"], 1)] for _, x in a.sort_values(["variant", "width"]).iterrows()]
    out.zapisz_tab("e13b_tresc", tabela(["Wersja", "Rozmiar", "Zdjęcie [ms]", "Syntetyczny [ms]", "Różnica [\\%]"], wiersze, "llrrr"))
    for _, x in a.iterrows():
        out.m(f"e13b/{x['variant']}/{x['width']}x{x['height']}/roznica", x["roznica"], 1)
    out.m("e13b/maks-roznica-proc", float(a["roznica"].abs().max()), 1)


def _odleglosc_od_krawedzi(gt: np.ndarray, R: int = 12) -> np.ndarray:
    """Odległość (euklidesowa, obcięta do R) każdego piksela od najbliższej prawdziwej krawędzi."""
    H, Wd = gt.shape
    dist = np.full(gt.shape, float(R))
    for dy in range(-R, R + 1):
        for dx in range(-R, R + 1):
            d = math.hypot(dx, dy)
            if d > R:
                continue
            sh = np.zeros_like(gt)
            y0, y1, x0, x1 = max(0, dy), H + min(0, dy), max(0, dx), Wd + min(0, dx)
            sh[y0:y1, x0:x1] = gt[y0 - dy:y1 - dy, x0 - dx:x1 - dx]
            dist[sh] = np.minimum(dist[sh], d)
    return dist


def e14(ex: Eksperyment, out: Wyjscie, katalog: Path) -> None:
    """Jakość wykrywania krawędzi a rozmiar maski: miara Pratta (FOM) w funkcji szumu."""
    from PIL import Image
    gt_p = w.REPO / "obrazy" / "jakosc" / "gt.png"
    if not gt_p.exists():
        out.uwagi.append("E14: brak obrazy/jakosc/gt.png – uruchom analiza/przygotuj_obrazy.py")
        return
    gt = np.asarray(Image.open(gt_p)) > 0
    dist = _odleglosc_od_krawedzi(gt)
    NI, alfa = int(gt.sum()), 1.0 / 9.0

    def fom(kraw: np.ndarray) -> float:
        nd = int(kraw.sum())
        return float((1.0 / (1.0 + alfa * dist[kraw] ** 2)).sum() / max(NI, nd)) if nd else 0.0

    wyniki = []
    for f in sorted((katalog / "images").glob("W1_*_l2_global_szum_*.png")):
        m = re.match(r"W1_(\w+?)_l2_global_szum_(\d+)", f.stem)
        if not m:
            continue
        g = np.asarray(Image.open(f))
        najl = max((fom(g >= t), t) for t in range(2, 255, 2))
        wyniki.append(dict(mask=m.group(1), sigma=int(m.group(2)), fom=najl[0], prog=najl[1]))
    if not wyniki:
        out.uwagi.append("E14: brak zapisanych obrazów wynikowych (save_images)")
        return
    df = pd.DataFrame(wyniki)
    maski = [mk for mk in ["roberts", "prewitt", "sobel3", "scharr", "sobel5", "sobel7"] if mk in set(df["mask"])]
    fig, ax = plt.subplots(figsize=(SZER * 0.75, WYS))
    for i, mk in enumerate(maski):
        d = df[df["mask"] == mk].sort_values("sigma")
        ax.plot(d["sigma"], d["fom"], label=nazwa_maski(mk), **styl_inny(i))
    ax.set_xlabel(r"odchylenie standardowe szumu $\sigma$ [poziomy szarości]")
    ax.set_ylabel("miara Pratta FOM (1 = idealnie)")
    ax.set_ylim(0, 1.02)
    os_przecinek(ax)
    ax.legend(loc="lower left", ncol=2)
    out.zapisz_rys(fig, "e14_jakosc", "E14")

    sig = sorted(set(df["sigma"]))
    wiersze = [[tex_escape(nazwa_maski(mk))] + [liczba_tex(df[(df["mask"] == mk) & (df["sigma"] == s)]["fom"].iloc[0], 3) for s in sig]
               for mk in maski]
    out.zapisz_tab("e14_jakosc", tabela(["Maska"] + [f"$\\sigma = {s}$" for s in sig], wiersze))
    for _, x in df.iterrows():
        out.m(f"e14/{x['mask']}/s{x['sigma']}/fom", x["fom"], 3)
        out.m(f"e14/{x['mask']}/s{x['sigma']}/prog", int(x["prog"]), 0)
    for s in sig:
        d = df[df["sigma"] == s]
        out.m(f"e14/s{s}/najlepsza", tex_escape(nazwa_maski(d.loc[d["fom"].idxmax(), "mask"])))

    # Rysunek przykładowy: szum σ = 30, wynik 3×3 i 7×7 (progowany najlepszym progiem)
    s_ex = 30 if 30 in sig else sig[-1]
    try:
        wej = np.asarray(Image.open(w.REPO / "obrazy" / "jakosc" / f"szum_{s_ex:02d}.png"))
        panele = [(wej, f"wejście, szum $\\sigma$ = {s_ex}"), (gt * 255, "prawdziwe krawędzie")]
        for mk in ["sobel3", "sobel7"]:
            g = np.asarray(Image.open(katalog / "images" / f"W1_{mk}_l2_global_szum_{s_ex:02d}.png"))
            pr = int(df[(df["mask"] == mk) & (df["sigma"] == s_ex)]["prog"].iloc[0])
            f_ = df[(df["mask"] == mk) & (df["sigma"] == s_ex)]["fom"].iloc[0]
            panele.append(((g >= pr) * 255, f"{nazwa_maski(mk)}: FOM = {liczba(f_, 3)}"))
        fig, osie = plt.subplots(2, 2, figsize=(SZER, SZER * 0.68))
        for ax, (im, t) in zip(osie.ravel(), panele):
            ax.imshow(im, cmap="gray", vmin=0, vmax=255, interpolation="nearest")
            ax.set_title(t, fontsize=plt.rcParams["axes.labelsize"])
            ax.axis("off")
        fig.tight_layout()
        out.zapisz_rys(fig, "e14_przyklad", "E14")
    except FileNotFoundError as e:
        out.uwagi.append(f"E14: rysunek przykładowy – {e}")


PROCESORY = [
    ("E1_wersje", e1), ("E1b_calosc", e1b), ("E2_maski", e2), ("E3_watki", e3), ("E4_harmonogramy", e4),
    ("E5_gpu", e5), ("E5b_gpu_watki", e5b), ("E6_mechanizmy", e6), ("E8_normalizacja", e8), ("E8b_redukcja", e8b),
    ("E9_amdahl", e9), ("E10_topologia", e10), ("E13_rozmiar", e13), ("E13b_tresc", e13b), ("E14_jakosc", e14),
]
Z_KATALOGIEM = {e14}

# =============================================================================
#  Meta, makra, kontrola odwołań
# =============================================================================

def makra_meta(eksp: dict[str, Eksperyment], out: Wyjscie) -> None:
    if not eksp:
        return
    meta = next(iter(eksp.values())).meta
    s = meta.get("system", {})
    out.m("meta/cpu", tex_escape(s.get("cpu_model", "?")))
    out.m("meta/watki", s.get("logical_cpus", 0), 0)
    out.m("meta/topologia", tex_escape(s.get("topology", "?")))
    out.m("meta/kompilator", tex_escape(s.get("compiler", "?")))
    out.m("meta/flagi", r"\texttt{" + tex_escape(s.get("cxx_flags", "?")) + "}")
    out.m("meta/governor", tex_escape(s.get("governor", "?")))
    out.m("meta/system", tex_escape(s.get("os", "?")))
    out.m("meta/jadro", tex_escape(s.get("kernel", "?")))
    out.m("meta/qt", tex_escape(s.get("qt_version", "?")))
    out.m("meta/git", tex_escape(meta.get("git", "?")))
    daty = sorted(m.meta.get("started", "")[:10] for m in eksp.values() if m.meta.get("started"))
    if daty:
        out.m("meta/data", daty[0] if daty[0] == daty[-1] else f"{daty[0]} -- {daty[-1]}")
    for nazwa, e in eksp.items():
        sp = e.meta.get("spec", {})
        kod = nazwa.split("_")[0].lower()
        out.m(f"meta/{kod}/powtorzenia", sp.get("repeats", 0), 0)
        out.m(f"meta/{kod}/rozgrzewka", sp.get("warmup", 0), 0)
        out.m(f"meta/{kod}/wiersze", e.meta.get("rows", 0), 0)


def zapisz_makra(out: Wyjscie, plik: Path) -> None:
    lin = ["% Wygenerowane przez analiza/przetworz.py – nie edytować ręcznie.",
           "% Użycie w tekście: \\wynik{klucz}, np. \\wynik{e1/V6/4k/przysp}.",
           "% Brakujący klucz drukuje ??klucz?? i ostrzeżenie w logu LaTeX-a.",
           r"\makeatletter",
           r"\providecommand{\wynik}[1]{\@ifundefined{wynik@#1}{\textbf{??#1??}\PackageWarning{wyniki}{Brak wyniku #1}}{\@nameuse{wynik@#1}}}"]
    for k in sorted(out.makra):
        lin.append(rf"\@namedef{{wynik@{k}}}{{{out.makra[k]}}}")
    lin.append(r"\makeatother")
    plik.parent.mkdir(parents=True, exist_ok=True)
    plik.write_text("\n".join(lin) + "\n", encoding="utf-8")
    w.info(f"  makra   {plik.name}: {len(out.makra)} wartości")


def kontrola_odwolan(out: Wyjscie) -> None:
    for tex in [w.SPRAWKO / "sprawozdanie.tex", w.SPRAWKO / "prezentacja.tex"]:
        if not tex.exists():
            continue
        tresc = tex.read_text(encoding="utf-8")
        tresc = "\n".join(l.split("%")[0] for l in tresc.splitlines())
        # rysunki: \includegraphics{…} albo makro \rys[szer]{…}; tabele: \tab{…}
        uzyte = {Path(m).stem for m in re.findall(r"\\(?:includegraphics|rys)(?:\[[^\]]*\])?\{([^}]+)\}", tresc)}
        uzyte = {u for u in uzyte if u.startswith("e")}
        wygen = {Path(r).stem for r in out.rysunki}
        for b in sorted(uzyte - wygen):
            out.uwagi.append(f"{tex.name}: rysunek {b} – takiego rysunku nie wygenerowano")
        tabele = set(re.findall(r"\\tab\{([^}]+)\}", tresc))
        for b in sorted(tabele - {Path(r).stem for r in out.pliki_tabel}):
            out.uwagi.append(f"{tex.name}: tabela {b} – takiej tabeli nie wygenerowano")
        klucze = set(re.findall(r"\\wynik\{([^}]+)\}", tresc))
        for b in sorted(klucze - set(out.makra)):
            out.uwagi.append(f"{tex.name}: \\wynik{{{b}}} – brak takiej wartości")
        niewyk = sorted(wygen - uzyte)
        if niewyk and tex.name == "sprawozdanie.tex":
            out.uwagi.append(f"{tex.name}: nieużyte rysunki: {', '.join(niewyk)}")


def podsumowanie_csv(eksp: dict[str, Eksperyment], plik: Path) -> None:
    czesci = []
    for nazwa, e in eksp.items():
        if e.res.empty:
            continue
        a = agreguj(e.res)
        a.insert(0, "experiment", nazwa)
        czesci.append(a)
    if czesci:
        pd.concat(czesci).to_csv(plik, index=False)
        w.info(f"  zbiorczo {plik.relative_to(w.REPO) if plik.is_relative_to(w.REPO) else plik}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--wyniki", type=Path, default=w.WYNIKI, help="katalog z wynikami eksperymentów")
    ap.add_argument("--zrzuty", type=Path, default=None, help="katalog rysunków (domyślnie sprawko/zrzuty)")
    ap.add_argument("--tabele", type=Path, default=None, help="katalog tabel (domyślnie sprawko/tabele)")
    ap.add_argument("--makra", type=Path, default=None, help="plik makr (domyślnie sprawko/wyniki.tex)")
    ap.add_argument("--slajdy", action="store_true",
                    help="wersja rysunków na slajdy: węższe, większa czcionka, do <zrzuty>/slajdy/ (bez tabel i makr)")
    args = ap.parse_args(argv)

    domyslne = args.wyniki.resolve() == w.WYNIKI.resolve()
    zrzuty = args.zrzuty or (w.ZRZUTY if domyslne else args.wyniki / "zrzuty")
    tabele = args.tabele or (w.TABELE if domyslne else args.wyniki / "tabele")
    makra = args.makra or (w.SPRAWKO / "wyniki.tex" if domyslne else args.wyniki / "wyniki.tex")

    ustaw_styl(args.slajdy)
    if args.slajdy:
        global SZER, SZER_POL, WYS
        SZER, SZER_POL, WYS = 13.0 * CM, 6.4 * CM, 6.0 * CM
        zrzuty = zrzuty / "slajdy"
    out = Wyjscie(zrzuty, tabele)
    eksp: dict[str, Eksperyment] = {}
    w.info(f"=== przetwarzanie: {args.wyniki} -> {zrzuty} ===")
    for nazwa, fn in PROCESORY:
        e = wczytaj(args.wyniki, nazwa)
        if e is None or e.res.empty:
            out.uwagi.append(f"{nazwa}: brak wyników – pominięto")
            continue
        eksp[nazwa] = e
        w.info(f"[{nazwa}] {len(e.res)} wierszy" + ("" if e.meta.get("complete", True) else " (NIEKOMPLETNY)"))
        try:
            if fn in Z_KATALOGIEM:
                fn(e, out, args.wyniki / nazwa)
            else:
                fn(e, out)
        except Exception as ex:  # noqa: BLE001 – jeden eksperyment nie blokuje reszty
            out.uwagi.append(f"{nazwa}: błąd przetwarzania – {ex}")
            traceback.print_exc()

    if args.slajdy:
        w.info(f"rysunki na slajdy: {len(out.rysunki)} w {zrzuty}")
        return 0
    makra_meta(eksp, out)
    zapisz_makra(out, makra)
    podsumowanie_csv(eksp, args.wyniki / "podsumowanie.csv")
    if domyslne:
        kontrola_odwolan(out)

    w.info("")
    w.info(f"rysunki: {len(out.rysunki)}, tabele: {len(out.pliki_tabel)}, wartości: {len(out.makra)}")
    for u in out.uwagi:
        w.info(f"UWAGA: {u}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
