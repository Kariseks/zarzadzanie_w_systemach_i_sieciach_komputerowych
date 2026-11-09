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

def ustaw_styl() -> None:
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
#  Eksperymenty
# =============================================================================

KOLEJNOSC_1W = ["V0", "V1", "V2", "V3", "V4", "V5", "V6", "V8", "AI"]


def e1(ex: Eksperyment, out: Wyjscie) -> None:
    """Warianty 1-wątkowe: czas i przyspieszenie względem V0."""
    a = agreguj(ex.res)
    a = a[a["mask"] == "sobel3"]
    warianty = [v for v in KOLEJNOSC_1W if v in set(a["variant"])]
    obrazy = list(dict.fromkeys(a.sort_values("mpix", ascending=False)["image"]))

    # --- czas (słupki, skala log) dla HD/4K/8K
    duze = [o for o in obrazy if Path(o).stem in ZNANE] or obrazy[:3]
    duze = sorted(duze, key=lambda o: a[a["image"] == o]["mpix"].iloc[0])
    fig, ax = plt.subplots(figsize=(SZER, WYS))
    szer = 0.8 / max(1, len(duze))
    x = np.arange(len(warianty))
    for i, o in enumerate(duze):
        d = a[a["image"] == o].set_index("variant").reindex(warianty)
        ax.bar(x + (i - (len(duze) - 1) / 2) * szer, d["mediana"], szer, color=KOLORY[i], alpha=0.85,
               yerr=[d["mediana"] - d["min"], d["max"] - d["mediana"]], capsize=1.5, ecolor="#333",
               label=etykieta_obrazu(o), hatch=["", "//", ".."][i % 3], edgecolor="white", linewidth=0.3)
    ax.set_yscale("log")
    lo, hi = ax.get_ylim()
    ax.set_ylim(lo, hi * 4)
    ax.set_xticks(x, warianty)
    ax.set_ylabel("czas przetwarzania [ms]")
    ax.set_xlabel("wariant (1 wątek, Sobel 3×3)")
    os_przecinek(ax, os_x=False)
    ax.legend(ncol=len(duze), loc="upper right")
    out.zapisz_rys(fig, "e1_warianty_czas", "E1")

    # --- przyspieszenie względem V0 dla wszystkich obrazów
    fig, ax = plt.subplots(figsize=(SZER, WYS))
    for i, o in enumerate(obrazy):
        d = a[a["image"] == o].set_index("variant").reindex(warianty)
        sp = d.loc["V0", "mediana"] / d["mediana"]
        ax.plot(warianty, sp, label=etykieta_obrazu(o, int(d["width"].dropna().iloc[0]), int(d["height"].dropna().iloc[0])), **styl(i))
    ax.set_yscale("log")
    ax.set_ylabel("przyspieszenie względem V0 [×]")
    ax.set_xlabel("wariant (1 wątek, Sobel 3×3)")
    os_przecinek(ax, os_x=False)
    ax.legend(ncol=2)
    out.zapisz_rys(fig, "e1_przyspieszenie", "E1")

    # --- tabela i makra
    wiersze = []
    ref = "3840x2160" if any(Path(o).stem == "3840x2160" for o in obrazy) else Path(duze[-1]).stem
    for v in warianty:
        r = [v]
        for o in duze:
            d = a[(a["image"] == o) & (a["variant"] == v)]
            r.append(liczba_tex(d["mediana"].iloc[0]) if len(d) else "–")
        d4 = a[(a["variant"] == v) & (a["image"].str.contains(ref))]
        d0 = a[(a["variant"] == "V0") & (a["image"].str.contains(ref))]
        sp = d0["mediana"].iloc[0] / d4["mediana"].iloc[0] if len(d4) and len(d0) else float("nan")
        r.append(liczba_tex(sp))
        wiersze.append(r)
    nagl = ["Wariant"] + [tex_escape(etykieta_obrazu(o)) + " [ms]" for o in duze] + ["Przysp. 4K [$\\times$]"]
    out.zapisz_tab("e1_warianty", tabela(nagl, wiersze))

    # czasy etapów (splot / normalizacja) – mediany
    etapy = ex.res[ex.res["mask"] == "sobel3"].groupby(["variant", "image"])[["conv_ms", "normalize_ms"]].median()
    for (v, o), x in etapy.iterrows():
        out.m(f"e1/{v}/{klucz_obrazu(o)}/splot", x["conv_ms"])
        if not math.isnan(x["normalize_ms"]):
            out.m(f"e1/{v}/{klucz_obrazu(o)}/normalizacja", x["normalize_ms"])

    for o in obrazy:
        ko = klucz_obrazu(o)
        d = a[a["image"] == o].set_index("variant")
        t0 = d.loc["V0", "mediana"] if "V0" in d.index else float("nan")
        poprz = None
        for v in warianty:
            if v not in d.index:
                continue
            t = d.loc[v, "mediana"]
            out.m(f"e1/{v}/{ko}/czas", t)
            out.m(f"e1/{v}/{ko}/przysp", t0 / t)
            if poprz is not None:
                out.m(f"e1/{v}/{ko}/krok", d.loc[poprz, "mediana"] / t)
            poprz = v


def e2(ex: Eksperyment, out: Wyjscie) -> None:
    """Maski i rozmiar k."""
    a = agreguj(ex.res)
    a = a[a["image"] == a.sort_values("mpix")["image"].iloc[-1]]     # jeden obraz (największy)
    warianty = [v for v in ["V0", "V2", "V3", "V6"] if v in set(a["variant"])]
    sob = a[a["mask"].isin(["sobel3", "sobel5", "sobel7"])].copy()
    sob["k"] = sob["mask"].str[-1].astype(int)
    fig, ax = plt.subplots(figsize=(SZER_POL * 1.25, WYS))
    for i, v in enumerate(warianty):
        d = sob[sob["variant"] == v].sort_values("k")
        ax.errorbar(d["k"], d["mediana"], yerr=[d["mediana"] - d["min"], d["max"] - d["mediana"]],
                    capsize=2, label=v, **styl(i))
    ax.set_yscale("log")
    ax.set_xticks([3, 5, 7], ["3×3", "5×5", "7×7"])
    ax.set_xlabel("rozmiar maski Sobela")
    ax.set_ylabel("czas [ms]")
    os_przecinek(ax, os_x=False)
    ax.legend()
    out.zapisz_rys(fig, "e2_rozmiar_maski", "E2")

    maski = ["sobel3", "sobel5", "sobel7", "prewitt", "scharr", "roberts"]
    nazwy = {"sobel3": "Sobel 3×3", "sobel5": "Sobel 5×5", "sobel7": "Sobel 7×7", "prewitt": "Prewitt 3×3",
             "scharr": "Scharr 3×3", "roberts": "Roberts 2×2"}
    wiersze = []
    for mk in [m for m in maski if m in set(a["mask"])]:
        r = [tex_escape(nazwy[mk])]
        d = a[a["mask"] == mk].set_index("variant")
        for v in warianty:
            r.append(liczba_tex(d.loc[v, "mediana"]) if v in d.index else "–")
            if v in d.index:
                out.m(f"e2/{v}/{mk}/czas", d.loc[v, "mediana"])
        if "V0" in d.index and warianty[-1] in d.index:
            sp = d.loc["V0", "mediana"] / d.loc[warianty[-1], "mediana"]
            r.append(liczba_tex(sp))
            out.m(f"e2/{mk}/przysp", sp)
        wiersze.append(r)
    out.zapisz_tab("e2_maski", tabela(["Maska"] + [f"{v} [ms]" for v in warianty] + [f"V0/{warianty[-1]} [$\\times$]"], wiersze))


def _skalowanie(a: pd.DataFrame, grupa: dict) -> pd.DataFrame:
    """Dodaje S i E względem 1 wątku w obrębie grupy (te same kolumny poza threads)."""
    d = a.copy()
    klucze = [c for c in ID_COLS if c != "threads"] + ["width", "height"]
    t1 = d[d["threads"] == 1].set_index(klucze)["mediana"]
    d = d.join(t1.rename("t1"), on=klucze)
    d["S"] = d["t1"] / d["mediana"]
    d["E"] = d["S"] / d["threads"]
    return d


def _linie_topologii(ax, maks: int = 48) -> None:
    for n, opis in ((6, "6 = 1 CCD"), (12, "12 = rdzenie fizyczne"), (24, "24 = wątki SMT")):
        if n <= maks:
            ax.axvline(n, color="#999", linewidth=0.6, linestyle=":")
            ax.text(n + 0.4, 0.03, opis, rotation=90, fontsize=6, color="#666", va="bottom", ha="left",
                    transform=ax.get_xaxis_transform())


def e3(ex: Eksperyment, out: Wyjscie) -> None:
    """Skalowanie względem liczby wątków."""
    a = _skalowanie(agreguj(ex.res), {})
    obrazy = list(dict.fromkeys(a.sort_values("mpix")["image"]))
    jadra = [k for k in KOLEJNOSC_1W if k in set(a["kernel"])]
    nmax = int(a["threads"].max())
    wiersze = []
    for o in obrazy:
        ko = klucz_obrazu(o)
        fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(SZER, WYS))
        NN = np.linspace(1, nmax, 200)
        ax1.plot(NN, NN, color="#999", linewidth=0.8, linestyle="--", label="idealne S = N")
        for i, k in enumerate(jadra):
            d = a[(a["image"] == o) & (a["kernel"] == k)].sort_values("threads")
            if d.empty:
                continue
            fit = d[d["threads"] <= 24]
            s = dopasuj_amdahla(fit["threads"], fit["S"])
            st = styl(i)
            ax1.plot(d["threads"], d["S"], label=f"T/{k}", **st)
            ax1.plot(NN, amdahl(NN, s), color=st["color"], linewidth=0.7, linestyle=":",
                     label=f"Amdahl, s = {liczba(s * 100, 1)}%")
            ax2.plot(d["threads"], d["E"], label=f"T/{k}", **st)
            best = d.loc[d["S"].idxmax()]
            r12 = d[d["threads"] == 12]["S"]
            r24 = d[d["threads"] == 24]["S"]
            wiersze.append([f"T/{k}", tex_escape(etykieta_obrazu(o)), liczba_tex(d[d['threads'] == 1]['mediana'].iloc[0]),
                            liczba_tex(r12.iloc[0]) if len(r12) else "–", liczba_tex(r24.iloc[0]) if len(r24) else "–",
                            f"{liczba_tex(best['S'])} ({int(best['threads'])})", liczba_tex(s * 100, 1)])
            out.m(f"e3/{k}/{ko}/s", s * 100, 1)
            out.m(f"e3/{k}/{ko}/t1", d[d["threads"] == 1]["mediana"].iloc[0])
            for n in (2, 4, 6, 8, 12, 16, 24, 32, 48):
                dn = d[d["threads"] == n]
                if len(dn):
                    out.m(f"e3/{k}/{ko}/S{n}", dn["S"].iloc[0])
                    out.m(f"e3/{k}/{ko}/E{n}", dn["E"].iloc[0] * 100, 0)
                    out.m(f"e3/{k}/{ko}/t{n}", dn["mediana"].iloc[0])
            out.m(f"e3/{k}/{ko}/Smax", best["S"])
            out.m(f"e3/{k}/{ko}/Nmax", int(best["threads"]))
        ax1.set_xlabel("liczba wątków N")
        ax1.set_ylabel("przyspieszenie S(N)")
        ax1.set_ylim(0, max(25, a[a["image"] == o]["S"].max() * 1.1))
        _linie_topologii(ax1, nmax)
        os_przecinek(ax1)
        ax2.set_xlabel("liczba wątków N")
        ax2.set_ylabel("efektywność E(N) = S(N)/N")
        ax2.set_ylim(0, 1.15)
        _linie_topologii(ax2, nmax)
        os_przecinek(ax2)
        fig.tight_layout()
        legenda_pod(fig, ax1, ncol=4)
        out.zapisz_rys(fig, f"e3_skalowanie_{ko}", "E3")
    out.zapisz_tab("e3_watki", tabela(["Jądro", "Obraz", "T(1) [ms]", "S(12)", "S(24)", "maks. S (N)", "s [\\%]"], wiersze, "llrrrrr"))


def e4(ex: Eksperyment, out: Wyjscie) -> None:
    """Harmonogramy x rozmiar fragmentu."""
    a = agreguj(ex.res)
    imb = agreguj(ex.res, "imbalance_ms")
    duzy = a.sort_values("mpix")["image"].iloc[-1]                  # jeden obraz (największy)
    a, imb = a[a["image"] == duzy], imb[imb["image"] == duzy]
    jadra = [k for k in KOLEJNOSC_1W if k in set(a["kernel"])]
    harm = ["static", "cyclic", "dynamic", "guided"]
    for n in sorted(set(a["threads"])):
        fig, osie = plt.subplots(1, len(jadra), figsize=(SZER, WYS), squeeze=False)
        for j, k in enumerate(jadra):
            ax = osie[0][j]
            for i, h in enumerate(harm):
                d = a[(a["kernel"] == k) & (a["schedule"] == h) & (a["threads"] == n)].sort_values("chunk")
                if d.empty:
                    continue
                if h == "static":
                    ax.axhline(d["mediana"].iloc[0], color=KOLORY[i], linestyle="--", linewidth=1, label="static (bez fragmentów)")
                    continue
                ax.errorbar(d["chunk"], d["mediana"], yerr=[d["mediana"] - d["min"], d["max"] - d["mediana"]],
                            capsize=1.5, label=h, **styl(i))
            ax.set_xscale("log", base=2)
            ax.set_xlabel("rozmiar fragmentu [wiersze]")
            ax.set_ylabel("czas [ms]")
            ax.set_title(f"T/{k}, {n} wątków")
            os_przecinek(ax)
        fig.tight_layout()
        legenda_pod(fig, osie[0][0], ncol=4)
        out.zapisz_rys(fig, f"e4_harmonogramy_t{n}", "E4")
        for k in jadra:
            d = a[(a["kernel"] == k) & (a["threads"] == n)]
            best = d.loc[d["mediana"].idxmin()]
            worst = d.loc[d["mediana"].idxmax()]
            out.m(f"e4/{k}/t{n}/najlepszy", f"{best['schedule']}" + (f", {int(best['chunk'])}" if best["schedule"] != "static" else ""))
            out.m(f"e4/{k}/t{n}/najlepszy-czas", best["mediana"])
            out.m(f"e4/{k}/t{n}/najgorszy", f"{worst['schedule']}" + (f", {int(worst['chunk'])}" if worst["schedule"] != "static" else ""))
            out.m(f"e4/{k}/t{n}/najgorszy-czas", worst["mediana"])
            for h in harm:
                dh = d[d["schedule"] == h]
                for c in sorted(set(dh["chunk"])):
                    out.m(f"e4/{k}/t{n}/{h}/c{c}", dh[dh["chunk"] == c]["mediana"].iloc[0])

    # nierównowaga obciążenia
    nmax = max(set(imb["threads"]))
    k = jadra[-1]
    fig, ax = plt.subplots(figsize=(SZER_POL * 1.25, WYS))
    for i, h in enumerate(harm):
        d = imb[(imb["kernel"] == k) & (imb["schedule"] == h) & (imb["threads"] == nmax)].sort_values("chunk")
        if d.empty:
            continue
        if h == "static":
            ax.axhline(d["mediana"].iloc[0], color=KOLORY[i], linestyle="--", linewidth=1, label="static")
            continue
        ax.plot(d["chunk"], d["mediana"], label=h, **styl(i))
    ax.set_xscale("log", base=2)
    ax.set_xlabel("rozmiar fragmentu [wiersze]")
    ax.set_ylabel("nierównowaga [ms]")
    ax.set_title(f"T/{k}, {nmax} wątków", fontsize=8)
    os_przecinek(ax)
    ax.legend(fontsize=6.5)
    out.zapisz_rys(fig, "e4_nierownowaga", "E4")


def e5(ex: Eksperyment, out: Wyjscie) -> None:
    """GPU: etapy i porównanie z CPU w funkcji rozmiaru."""
    r = ex.res.copy()
    r["opis"] = r.apply(opis_wariantu, axis=1)
    etapy = ["upload_ms", "kernel_ms", "download_ms"]
    med = r.groupby(["opis", "variant", "mask", "image", "width", "height"])[["wall_ms"] + etapy].median().reset_index()
    med["mpix"] = med["width"] * med["height"] / 1e6
    ref = med.loc[(med["mpix"] - 8.29).abs().idxmin(), "image"] if len(med) else None

    # --- etapy dla obrazu ~4K
    maski = [m for m in ["sobel3", "sobel7"] if m in set(med["mask"])]
    fig, osie = plt.subplots(1, len(maski), figsize=(SZER, WYS), squeeze=False)
    for j, mk in enumerate(maski):
        ax = osie[0][j]
        d = med[(med["image"] == ref) & (med["mask"] == mk)].sort_values("wall_ms", ascending=False)
        y = np.arange(len(d))
        lewo = np.zeros(len(d))
        for i, (e, nazwa) in enumerate(zip(etapy, ["przesył do GPU", "obliczenia GPU", "przesył z GPU"])):
            v = d[e].fillna(0).to_numpy()
            ax.barh(y, v, left=lewo, color=KOLORY[i], label=nazwa if j == 0 else None, hatch=["", "//", ".."][i], edgecolor="white", linewidth=0.3)
            lewo += v
        cpu = d[d["upload_ms"].isna()]
        ax.barh(y[d["upload_ms"].isna().to_numpy()], cpu["wall_ms"], color=KOLORY[4], label="CPU (całość)" if j == 0 else None)
        ax.scatter(d["wall_ms"], y, marker="|", color="black", s=40, label="czas całkowity" if j == 0 else None, zorder=3)
        ax.set_yticks(y, d["opis"])
        ax.set_xlabel("czas [ms]")
        ax.set_title(f"{etykieta_obrazu(ref)}, {mk.replace('sobel', 'Sobel ')}×{mk[-1]}", fontsize=8)
        os_przecinek(ax, os_y=False)
    fig.legend(loc="lower center", ncol=5, bbox_to_anchor=(0.5, -0.06))
    fig.tight_layout()
    out.zapisz_rys(fig, "e5_gpu_etapy", "E5")

    # --- czas w funkcji rozmiaru
    for mk in maski:
        fig, ax = plt.subplots(figsize=(SZER, WYS))
        opisy = sorted(set(med[med["mask"] == mk]["opis"]))
        for i, o in enumerate(opisy):
            d = med[(med["mask"] == mk) & (med["opis"] == o)].sort_values("mpix")
            ax.plot(d["mpix"], d["wall_ms"], label=o, **styl(i))
            if o.startswith("GPUC") and "auto" in o:
                ax.plot(d["mpix"], d["kernel_ms"], label=f"{o}: same obliczenia", color=styl(i)["color"], linestyle=":", marker="")
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel("rozmiar obrazu [Mpix]")
        ax.set_ylabel("czas [ms]")
        os_przecinek(ax)
        ax.legend(ncol=2, fontsize=6.5)
        out.zapisz_rys(fig, f"e5_gpu_rozmiar_{mk}", "E5")

    for _, x in med.iterrows():
        k = f"e5/{x['opis'].replace(' (', '-').replace(')', '').replace('/', '-')}/{x['mask']}/{klucz_obrazu(x['image'])}"
        out.m(k + "/czas", x["wall_ms"])
        for e in etapy:
            if not math.isnan(x[e]):
                out.m(k + "/" + e.replace("_ms", ""), x[e])
    # dokładność fp32
    if len(ex.cor):
        c = ex.cor[ex.cor["variant"].str.startswith("GPU")]
        if len(c):
            out.m("e5/fp32/maks-roznica", int(c[c["gpu_prec"] == "fp32"]["max_abs_diff"].max()), 0)
            out.m("e5/auto/maks-roznica", int(c[c["gpu_prec"] == "auto"]["max_abs_diff"].max()), 0)


def e6(ex: Eksperyment, out: Wyjscie) -> None:
    """Pula vs nowe wątki vs OpenMP."""
    r = ex.res.copy()
    r["mech_opis"] = np.where(r["variant"] == "OMP", "OpenMP", np.where(r["mech"] == "spawn", "nowe wątki (spawn)", "własna pula"))
    a = r.groupby(["mech_opis", "image", "schedule", "threads", "width", "height"])["wall_ms"].agg(mediana="median", min="min", max="max").reset_index()
    obrazy = list(dict.fromkeys(a.sort_values("width")["image"]))
    harm = sorted(set(a["schedule"]))
    fig, osie = plt.subplots(1, len(obrazy), figsize=(SZER, WYS), squeeze=False)
    for j, o in enumerate(obrazy):
        ax = osie[0][j]
        i = 0
        for m in ["własna pula", "nowe wątki (spawn)", "OpenMP"]:
            for h in harm:
                d = a[(a["image"] == o) & (a["mech_opis"] == m) & (a["schedule"] == h)].sort_values("threads")
                if d.empty:
                    continue
                ax.plot(d["threads"], d["mediana"], label=f"{m}, {h}", **styl(i))
                i += 1
        ax.set_yscale("log")
        ax.set_xlabel("liczba wątków N")
        ax.set_ylabel("czas [ms]")
        ax.set_title(etykieta_obrazu(o), fontsize=8)
        os_przecinek(ax)
    fig.tight_layout()
    legenda_pod(fig, osie[0][0], ncol=3)
    out.zapisz_rys(fig, "e6_mechanizmy", "E6")
    for _, x in a.iterrows():
        m = {"własna pula": "pool", "nowe wątki (spawn)": "spawn", "OpenMP": "omp"}[x["mech_opis"]]
        out.m(f"e6/{m}/{x['schedule']}/{klucz_obrazu(x['image'])}/t{x['threads']}", x["mediana"])


def e7(ex: Eksperyment, out: Wyjscie, katalog: Path) -> None:
    """Normy: czas i dokładność względem L2 + rysunek przykładowy."""
    a = agreguj(ex.res)
    c = ex.cor.copy()
    a["opis"] = a.apply(opis_wariantu, axis=1)
    c["opis"] = c.apply(opis_wariantu, axis=1)
    wiersze = []
    ref = [o for o in set(a["image"]) if "3840x2160" in o]
    ref = ref[0] if ref else a.sort_values("mpix")["image"].iloc[-1]
    for mk in sorted(set(a["mask"])):
        for v in [x for x in ["V0", "V6", "T/V6"] if x in set(a["opis"])]:
            for nm in ["l2", "l1", "linf"]:
                d = a[(a["opis"] == v) & (a["norm"] == nm) & (a["mask"] == mk) & (a["image"] == ref)]
                cc = c[(c["opis"] == v) & (c["norm"] == nm) & (c["mask"] == mk)]
                if d.empty:
                    continue
                t = d["mediana"].iloc[0]
                t2 = a[(a["opis"] == v) & (a["norm"] == "l2") & (a["mask"] == mk) & (a["image"] == ref)]["mediana"].iloc[0]
                maxd = int(pd.to_numeric(cc["l2_max_abs_diff"]).max()) if len(cc) else 0
                meand = pd.to_numeric(cc["l2_mean_abs_diff"]).mean() if len(cc) else 0.0
                psnr = pd.to_numeric(cc["l2_psnr_db"].replace("inf", np.inf)).min() if len(cc) else np.inf
                nazwa = {"l2": r"$L_2$", "l1": r"$L_1$", "linf": r"$L_\infty$"}[nm]
                wiersze.append([mk.replace("sobel", "Sobel ") + r"$\times$" + mk[-1], v, nazwa, liczba_tex(t),
                                liczba_tex(t2 / t), str(maxd), liczba_tex(meand, 2), liczba_tex(psnr, 1)])
                k = f"e7/{v.replace('/', '-')}/{mk}/{nm}"
                out.m(k + "/czas", t)
                out.m(k + "/przysp", t2 / t)
                out.m(k + "/maks-roznica", maxd, 0)
                out.m(k + "/sr-roznica", meand, 2)
                out.m(k + "/psnr", psnr, 1)
    out.zapisz_tab("e7_normy", tabela(["Maska", "Wariant", "Norma", "Czas 4K [ms]", "Przysp. wzgl. $L_2$",
                                       "Maks. błąd", "Śr. błąd", "PSNR [dB]"], wiersze, "lllrrrrr"))

    # --- rysunek przykładowy z zapisanych obrazów (V0, Kodak 08)
    img_dir = katalog / "images"
    oryg = w.REPO / "obrazy" / "kodim08.png"
    try:
        from PIL import Image
        l2 = img_dir / "V0_sobel3_l2_global_kodim08.png"
        l1 = img_dir / "V0_sobel3_l1_global_kodim08.png"
        s7 = img_dir / "V0_sobel7_l2_global_kodim08.png"
        if l2.exists() and l1.exists() and oryg.exists():
            A = np.asarray(Image.open(oryg).convert("L"), dtype=float)
            B = np.asarray(Image.open(l2), dtype=float)
            C = np.asarray(Image.open(l1), dtype=float)
            panele = [(A, "obraz wejściowy (skala szarości)"), (B, "Sobel 3×3, norma $L_2$"),
                      (np.abs(C - B) * 8, "|$L_1$ − $L_2$| × 8")]
            if s7.exists():
                panele.insert(2, (np.asarray(Image.open(s7), dtype=float), "Sobel 7×7, norma $L_2$"))
            kol = 2
            wier = math.ceil(len(panele) / kol)
            fig, osie = plt.subplots(wier, kol, figsize=(SZER, SZER / kol * 0.72 * wier))
            osie = np.atleast_1d(osie).ravel()
            for ax in osie[len(panele):]:
                ax.axis("off")
            for ax, (im, t) in zip(osie, panele):
                ax.imshow(np.clip(im, 0, 255), cmap="gray", vmin=0, vmax=255, interpolation="nearest")
                ax.set_title(t, fontsize=8.5)
                ax.axis("off")
            fig.tight_layout()
            out.zapisz_rys(fig, "e7_przyklad", "E7")
        else:
            out.uwagi.append("E7: brak zapisanych obrazów Kodak 08 – rysunek e7_przyklad pominięty")
    except Exception as e:  # noqa: BLE001
        out.uwagi.append(f"E7: rysunek przykładowy – {e}")


def e8(ex: Eksperyment, out: Wyjscie) -> None:
    """Skalowanie wyniku: global / fixed / clamp."""
    a = agreguj(ex.res)
    obrazy = list(dict.fromkeys(a.sort_values("mpix")["image"]))
    nazwy = {"global": "globalne maksimum", "fixed": "stała skala", "clamp": "obcięcie"}
    fig, osie = plt.subplots(1, len(obrazy), figsize=(SZER, WYS), squeeze=False)
    for j, o in enumerate(obrazy):
        ax = osie[0][j]
        i = 0
        for k in sorted(set(a["kernel"])):
            for sc in ["global", "fixed", "clamp"]:
                d = a[(a["image"] == o) & (a["kernel"] == k) & (a["scaling"] == sc)].sort_values("threads")
                if d.empty:
                    continue
                ax.plot(d["threads"], d["mediana"], label=f"T/{k}, {nazwy[sc]}", **styl(i))
                i += 1
                for _, x in d.iterrows():
                    out.m(f"e8/{k}/{sc}/{klucz_obrazu(o)}/t{x['threads']}", x["mediana"])
        ax.set_yscale("log")
        ax.set_xlabel("liczba wątków N")
        ax.set_ylabel("czas [ms]")
        ax.set_title(etykieta_obrazu(o), fontsize=8)
        os_przecinek(ax)
    fig.tight_layout()
    legenda_pod(fig, osie[0][0], ncol=3)
    out.zapisz_rys(fig, "e8_normalizacja", "E8")


def e8b(ex: Eksperyment, out: Wyjscie) -> None:
    """Sposoby redukcji maksimum."""
    a = agreguj(ex.res)
    a = a[a["image"] == a.sort_values("mpix")["image"].iloc[-1]]     # jeden obraz (największy)
    nazwy = {"local": "lokalne + redukcja", "atomic": "atomic CAS / piksel", "mutex": "mutex / piksel",
             "px_pad": "zapis / piksel, wyrównane", "px_nopad": "zapis / piksel, bez wyrównania"}
    fig, ax = plt.subplots(figsize=(SZER, WYS))
    for i, red in enumerate(["local", "atomic", "mutex", "px_pad", "px_nopad"]):
        d = a[a["reduce"] == red].sort_values("threads")
        if d.empty:
            continue
        ax.errorbar(d["threads"], d["mediana"], yerr=[d["mediana"] - d["min"], d["max"] - d["mediana"]],
                    capsize=1.5, label=nazwy[red], **styl(i))
        for _, x in d.iterrows():
            out.m(f"e8b/{red}/t{x['threads']}", x["mediana"])
    ax.set_yscale("log")
    ax.set_xlabel("liczba wątków N")
    ax.set_ylabel("czas [ms]")
    os_przecinek(ax)
    ax.legend(fontsize=6.5)
    out.zapisz_rys(fig, "e8b_redukcja", "E8b")
    n = a["threads"].max()
    loc = a[(a["reduce"] == "local") & (a["threads"] == n)]["mediana"]
    if len(loc):
        for red in set(a["reduce"]):
            d = a[(a["reduce"] == red) & (a["threads"] == n)]["mediana"]
            if len(d):
                out.m(f"e8b/{red}/wzgl-local-t{n}", d.iloc[0] / loc.iloc[0])


def e9(ex: Eksperyment, out: Wyjscie) -> None:
    """Cały program z I/O – prawo Amdahla."""
    r = ex.res
    a = r.groupby(["image", "threads", "width", "height"])[["wall_ms", "process_ms", "load_ms", "save_ms"]].median().reset_index()
    obrazy = list(dict.fromkeys(a.sort_values("width")["image"]))
    fig, osie = plt.subplots(1, len(obrazy), figsize=(SZER, WYS), squeeze=False)
    wiersze = []
    for j, o in enumerate(obrazy):
        ax = osie[0][j]
        d = a[a["image"] == o].sort_values("threads")
        t1 = d[d["threads"] == 1].iloc[0]
        S_cal = t1["wall_ms"] / d["wall_ms"]
        S_prz = t1["process_ms"] / d["process_ms"]
        s_zm = (t1["load_ms"] + t1["save_ms"]) / t1["wall_ms"]
        NN = np.linspace(1, d["threads"].max(), 200)
        ax.plot(NN, NN, color="#999", linestyle="--", linewidth=0.8, label="idealne S = N")
        ax.plot(d["threads"], S_prz, label="samo przetwarzanie", **styl(0))
        ax.plot(d["threads"], S_cal, label="cały program (z wczytaniem i zapisem JPG)", **styl(1))
        ax.plot(NN, amdahl(NN, s_zm), color=KOLORY[1], linestyle=":", linewidth=0.8,
                label=f"Amdahl, zmierzone s = {liczba(s_zm * 100, 1)}%")
        ax.axhline(1 / s_zm, color=KOLORY[1], linewidth=0.5, linestyle="-.")
        ax.set_xlabel("liczba wątków N")
        ax.set_ylabel("przyspieszenie S(N)")
        ax.set_title(etykieta_obrazu(o) + " (JPG)", fontsize=8)
        ax.set_ylim(0, max(S_prz.max(), 1 / s_zm) * 1.1)
        os_przecinek(ax)
        ko = klucz_obrazu(o)
        tn = d.iloc[-1]
        out.m(f"e9/{ko}/s", s_zm * 100, 1)
        out.m(f"e9/{ko}/limit", 1 / s_zm)
        out.m(f"e9/{ko}/wczytanie", t1["load_ms"])
        out.m(f"e9/{ko}/zapis", t1["save_ms"])
        out.m(f"e9/{ko}/przetw-t1", t1["process_ms"])
        out.m(f"e9/{ko}/przetw-t{int(tn['threads'])}", tn["process_ms"])
        out.m(f"e9/{ko}/calosc-t1", t1["wall_ms"])
        out.m(f"e9/{ko}/calosc-t{int(tn['threads'])}", tn["wall_ms"])
        out.m(f"e9/{ko}/S-calosc-t{int(tn['threads'])}", S_cal.iloc[-1])
        out.m(f"e9/{ko}/S-przetw-t{int(tn['threads'])}", S_prz.iloc[-1])
        wiersze.append([tex_escape(etykieta_obrazu(o)), liczba_tex(t1["load_ms"]), liczba_tex(t1["save_ms"]),
                        liczba_tex(t1["process_ms"]), liczba_tex(tn["process_ms"]), liczba_tex(s_zm * 100, 1),
                        liczba_tex(S_cal.iloc[-1]), liczba_tex(1 / s_zm)])
    fig.tight_layout()
    legenda_pod(fig, osie[0][0], ncol=2)
    out.zapisz_rys(fig, "e9_amdahl", "E9")
    n = int(a["threads"].max())
    out.zapisz_tab("e9_amdahl", tabela(["Obraz", "Wczytanie [ms]", "Zapis [ms]", "Przetw. 1 w. [ms]",
                                        f"Przetw. {n} w. [ms]", "s [\\%]", f"S({n}) całość", "1/s"], wiersze))


def e10(ex: Eksperyment, out: Wyjscie) -> None:
    """Przypinanie wątków do rdzeni."""
    a = agreguj(ex.res)
    a = a[a["image"] == a.sort_values("mpix")["image"].iloc[-1]]     # jeden obraz (największy)
    nazwy = {"none": "system (bez przypinania)", "compact": "compact (najpierw 1 CCD)",
             "spread": "spread (naprzemiennie CCD)", "nosmt": "nosmt (1 wątek/rdzeń)"}
    jadra = sorted(set(a["kernel"]))
    fig, osie = plt.subplots(1, len(jadra), figsize=(SZER, WYS), squeeze=False)
    for j, k in enumerate(jadra):
        ax = osie[0][j]
        nn = sorted(set(a["threads"]))
        x = np.arange(len(nn))
        szer = 0.8 / 4
        for i, af in enumerate(["none", "compact", "spread", "nosmt"]):
            d = a[(a["kernel"] == k) & (a["affinity"] == af) & (a["reduce"].isin(["local", ""]))].set_index("threads").reindex(nn)
            ax.bar(x + (i - 1.5) * szer, d["mediana"], szer, color=KOLORY[i], label=nazwy[af] if j == 0 else None,
                   hatch=["", "//", "..", "xx"][i], edgecolor="white", linewidth=0.3)
            for n_, v in zip(nn, d["mediana"]):
                if not math.isnan(v):
                    out.m(f"e10/{k}/{af}/t{n_}", v)
        ax.set_xticks(x, [str(n) for n in nn])
        ax.set_xlabel("liczba wątków N")
        ax.set_ylabel("czas [ms]")
        ax.set_title(f"T/{k}", fontsize=8)
        os_przecinek(ax, os_x=False)
    fig.legend(loc="lower center", ncol=4, bbox_to_anchor=(0.5, -0.07), fontsize=6.5)
    fig.tight_layout()
    out.zapisz_rys(fig, "e10_topologia", "E10")


def e13(ex: Eksperyment, out: Wyjscie) -> None:
    """Rozmiar danych wejściowych."""
    r = ex.res.copy()
    r["opis"] = r.apply(lambda x: f"T/{x['kernel']}, {x['threads']} w." if x["variant"] == "T" else x["variant"], axis=1)
    a = r.groupby(["opis", "mask", "image", "width", "height"])["wall_ms"].agg(mediana="median", min="min", max="max").reset_index()
    a["mpix"] = a["width"] * a["height"] / 1e6
    seria = a[a["image"].str.contains("x") & ~a["image"].str.contains("2048x2048|16384x256")]
    maski = [m for m in ["sobel3", "sobel7"] if m in set(a["mask"])]
    opisy = [o for o in ["V0", "V6", "V8"] if o in set(a["opis"])] + sorted(o for o in set(a["opis"]) if o.startswith("T/"))

    # (a) czas(n), log-log
    fig, osie = plt.subplots(1, len(maski), figsize=(SZER, WYS * 1.1), squeeze=False)
    for j, mk in enumerate(maski):
        ax = osie[0][j]
        for i, o in enumerate(opisy):
            d = seria[(seria["opis"] == o) & (seria["mask"] == mk)].sort_values("mpix")
            ax.plot(d["mpix"], d["mediana"], label=o, **styl(i))
        d0 = seria[(seria["opis"] == opisy[0]) & (seria["mask"] == mk)].sort_values("mpix")
        if len(d0):
            xs = np.array([d0["mpix"].min(), d0["mpix"].max()])
            ax.plot(xs, d0["mediana"].iloc[-1] * xs / xs[-1], color="#999", linewidth=0.7, linestyle="--", label="O(n)")
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel("rozmiar obrazu n [Mpix]")
        ax.set_ylabel("czas [ms]")
        ax.set_title(mk.replace("sobel", "Sobel ") + "×" + mk[-1], fontsize=8)
        os_przecinek(ax)
    fig.tight_layout()
    legenda_pod(fig, osie[0][0], ncol=6)
    out.zapisz_rys(fig, "e13_czas", "E13")

    # (b) przepustowość z progami cache
    fig, osie = plt.subplots(1, len(maski), figsize=(SZER, WYS * 1.1), squeeze=False)
    progi = [(1 << 20, "L2 1 MB"), (32 << 20, "L3 1 CCD"), (64 << 20, "L3 2 CCD")]
    for j, mk in enumerate(maski):
        ax = osie[0][j]
        for i, o in enumerate(opisy):
            d = seria[(seria["opis"] == o) & (seria["mask"] == mk)].sort_values("mpix")
            ax.plot(d["mpix"], d["mpix"] / (d["mediana"] / 1000.0), label=o, **styl(i))
        for b, opis in progi:
            n6 = b / 6 / 1e6     # 6 B/piksel: wejście + float + wyjście (V0, V6)
            ax.axvline(n6, color="#888", linewidth=0.6, linestyle=":")
            ax.text(n6 * 1.08, 0.03, f"{opis} przy 6 B/px", rotation=90, fontsize=5.8, color="#666",
                    va="bottom", ha="left", transform=ax.get_xaxis_transform())
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel("rozmiar obrazu n [Mpix]")
        ax.set_ylabel("przepustowość [Mpix/s]")
        ax.set_title(mk.replace("sobel", "Sobel ") + "×" + mk[-1], fontsize=8)
        os_przecinek(ax)
    fig.tight_layout()
    legenda_pod(fig, osie[0][0], ncol=6)
    out.zapisz_rys(fig, "e13_przepustowosc", "E13")

    # (c) opłacalność wątków: S = T(V8, 1 w.) / T(T/V8, N w.)
    fig, ax = plt.subplots(figsize=(SZER, WYS))
    i = 0
    for mk in maski:
        baza = seria[(seria["opis"] == "V8") & (seria["mask"] == mk)].set_index("image")["mediana"]
        for o in [x for x in opisy if x.startswith("T/")]:
            d = seria[(seria["opis"] == o) & (seria["mask"] == mk)].sort_values("mpix")
            S = baza.reindex(d["image"]).to_numpy() / d["mediana"].to_numpy()
            ax.plot(d["mpix"], S, label=f"{o}, {mk.replace('sobel', 'Sobel ')}×{mk[-1]}", **styl(i))
            i += 1
            prog = d["mpix"].to_numpy()[np.argmax(S > 1)] if np.any(S > 1) else float("nan")
            prog_e = d["mpix"].to_numpy()[np.argmax(S / int(o.split(",")[1].split()[0]) > 0.5)] \
                if np.any(S / int(o.split(",")[1].split()[0]) > 0.5) else float("nan")
            kk = o.replace("/", "-").replace(", ", "-").replace(" w.", "w")
            out.m(f"e13/{kk}/{mk}/prog-S1", prog, 2)
            out.m(f"e13/{kk}/{mk}/prog-E05", prog_e, 2)
            out.m(f"e13/{kk}/{mk}/Smax", float(np.nanmax(S)))
    ax.axhline(1, color="black", linewidth=0.6)
    ax.set_xscale("log")
    ax.set_xlabel("rozmiar obrazu n [Mpix]")
    ax.set_ylabel("przyspieszenie względem V8 (1 wątek)")
    os_przecinek(ax)
    ax.legend(fontsize=6.5)
    out.zapisz_rys(fig, "e13_oplacalnosc", "E13")

    # (d) proporcje: te same ~4,2 Mpix
    ks = a[a["image"].str.contains("2731x1536|2048x2048|16384x256")]
    wiersze = []
    for o in opisy:
        r_ = [tex_escape(o)]
        for im in ["2731x1536", "2048x2048", "16384x256"]:
            d = ks[(ks["opis"] == o) & ks["image"].str.contains(im) & (ks["mask"] == "sobel3")]
            r_.append(liczba_tex(d["mediana"].iloc[0]) if len(d) else "–")
            if len(d):
                out.m(f"e13/proporcje/{o.replace('/', '-').replace(', ', '-').replace(' w.', 'w')}/{im}", d["mediana"].iloc[0])
        wiersze.append(r_)
    out.zapisz_tab("e13_proporcje", tabela(["Wariant", r"2731$\times$1536 [ms]", r"2048$\times$2048 [ms]", r"16384$\times$256 [ms]"], wiersze))
    for _, x in seria.iterrows():
        out.m(f"e13/{x['opis'].replace('/', '-').replace(', ', '-').replace(' w.', 'w')}/{x['mask']}/{x['width']}x{x['height']}", x["mediana"])


def e13b(ex: Eksperyment, out: Wyjscie) -> None:
    """Treść obrazu: syntetyczny vs zdjęcie tego samego rozmiaru."""
    r = ex.res.copy()
    r["opis"] = r.apply(lambda x: f"T/{x['kernel']}, {x['threads']} w." if x["variant"] == "T" else x["variant"], axis=1)
    r["zrodlo"] = np.where(r["image"].str.startswith("synthetic:"), "syntetyczny", "zdjęcie")
    a = r.groupby(["opis", "zrodlo", "width", "height"])["wall_ms"].median().unstack("zrodlo").reset_index()
    if "syntetyczny" not in a or "zdjęcie" not in a:
        return
    a["roznica"] = (a["zdjęcie"] / a["syntetyczny"] - 1) * 100
    wiersze = [[tex_escape(x["opis"]), f"{x['width']}$\\times${x['height']}", liczba_tex(x["zdjęcie"]), liczba_tex(x["syntetyczny"]),
                liczba_tex(x["roznica"], 1)] for _, x in a.sort_values(["opis", "width"]).iterrows()]
    out.zapisz_tab("e13b_tresc", tabela(["Wariant", "Rozmiar", "Zdjęcie [ms]", "Syntetyczny [ms]", "Różnica [\\%]"], wiersze, "llrrr"))
    out.m("e13b/maks-roznica-proc", float(a["roznica"].abs().max()), 1)
    for _, x in a.iterrows():
        k = f"e13b/{x['opis'].replace('/', '-').replace(', ', '-').replace(' w.', 'w')}/{x['width']}x{x['height']}"
        out.m(k + "/zdjecie", x["zdjęcie"])
        out.m(k + "/syntetyczny", x["syntetyczny"])
        out.m(k + "/roznica", x["roznica"], 1)


PROCESORY = [
    ("E1_warianty", e1), ("E2_maski", e2), ("E3_watki", e3), ("E4_harmonogramy", e4), ("E5_gpu", e5),
    ("E6_mechanizmy", e6), ("E7_normy", e7), ("E8_normalizacja", e8), ("E8b_redukcja", e8b),
    ("E9_amdahl", e9), ("E10_topologia", e10), ("E13_rozmiar", e13), ("E13b_syntetyczne", e13b),
]


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
        uzyte = {Path(m).stem for m in re.findall(r"\\includegraphics(?:\[[^\]]*\])?\{([^}]+)\}", tresc)}
        uzyte = {u for u in uzyte if u.startswith("e")}
        wygen = {Path(r).stem for r in out.rysunki}
        for b in sorted(uzyte - wygen):
            out.uwagi.append(f"{tex.name}: \\includegraphics{{{b}}} – takiego rysunku nie wygenerowano")
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
    args = ap.parse_args(argv)

    domyslne = args.wyniki.resolve() == w.WYNIKI.resolve()
    zrzuty = args.zrzuty or (w.ZRZUTY if domyslne else args.wyniki / "zrzuty")
    tabele = args.tabele or (w.TABELE if domyslne else args.wyniki / "tabele")
    makra = args.makra or (w.SPRAWKO / "wyniki.tex" if domyslne else args.wyniki / "wyniki.tex")

    ustaw_styl()
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
            if fn is e7:
                fn(e, out, args.wyniki / nazwa)
            else:
                fn(e, out)
        except Exception as ex:  # noqa: BLE001 – jeden eksperyment nie blokuje reszty
            out.uwagi.append(f"{nazwa}: błąd przetwarzania – {ex}")
            traceback.print_exc()

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
