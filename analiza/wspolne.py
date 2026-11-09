"""Wspólne funkcje skryptów w analiza/: ścieżki, blokada pomiarów, kontrole
systemu, uruchamianie edge_bench i wczytywanie wyników.

Moduł korzysta tylko z biblioteki standardowej, więc testy.py działa bez
instalowania pakietów.  Rysowanie (matplotlib, pandas) jest w przetworz.py.
"""

from __future__ import annotations

import contextlib
import csv
import fcntl
import json
import math
import os
import signal
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterator

# --- ścieżki -----------------------------------------------------------------

REPO = Path(__file__).resolve().parent.parent
ANALIZA = REPO / "analiza"
EKSPERYMENTY = ANALIZA / "eksperymenty"
WYNIKI = ANALIZA / "wyniki"
SPRAWKO = REPO / "sprawko"
ZRZUTY = SPRAWKO / "zrzuty"
TABELE = SPRAWKO / "tabele"
LOCK_FILE = ANALIZA / ".testy.lock"
BUILD_DIR = Path(os.environ.get("EDGE_BUILD_DIR", REPO / "implementacja" / "build"))

# Kody wyjścia edge_bench (zob. implementacja/bench/main.cpp)
EXIT_OK, EXIT_ARGS, EXIT_UNAVAILABLE, EXIT_CORRECTNESS, EXIT_IO, EXIT_INTERRUPTED = 0, 1, 2, 3, 4, 130
EXIT_NAMES = {
    EXIT_OK: "OK",
    EXIT_ARGS: "złe argumenty",
    EXIT_UNAVAILABLE: "wariant niedostępny (--strict)",
    EXIT_CORRECTNESS: "błąd poprawności",
    EXIT_IO: "błąd wejścia-wyjścia",
    EXIT_INTERRUPTED: "przerwano",
}

# Procesy, które zakłócają pomiar (kompilacja, inny pomiar)
PRZESZKADZAJACE = {"edge_bench", "edge_cli", "cc1plus", "cc1", "ninja", "make", "cmake", "ld", "lto1"}


def edge_bench_path(build_dir: Path | None = None) -> Path:
    return (build_dir or BUILD_DIR) / "edge_bench"


# --- wypisywanie ---------------------------------------------------------------

def info(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def ostrzezenie(msg: str) -> None:
    print(f"UWAGA: {msg}", file=sys.stderr, flush=True)


class Log:
    """Kopia komunikatów do pliku logu (dopisywanie)."""

    def __init__(self, path: Path | None):
        self.path = path
        if path:
            path.parent.mkdir(parents=True, exist_ok=True)

    def __call__(self, msg: str) -> None:
        info(msg)
        if self.path:
            with self.path.open("a", encoding="utf-8") as f:
                f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')}  {msg}\n")


# --- blokada -------------------------------------------------------------------

class PomiaryTrwaja(RuntimeError):
    pass


@contextlib.contextmanager
def blokada_pomiarow(path: Path = LOCK_FILE) -> Iterator[None]:
    """Wyłączna blokada: w danej chwili tylko jeden skrypt może mierzyć."""
    path.parent.mkdir(parents=True, exist_ok=True)
    f = path.open("a+")
    try:
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            f.seek(0)
            kto = f.read().strip() or "nieznany proces"
            raise PomiaryTrwaja(f"pomiary już trwają ({kto}); poczekaj, aż się skończą") from None
        f.seek(0)
        f.truncate()
        f.write(f"PID {os.getpid()}, start {time.strftime('%H:%M:%S')}, {' '.join(sys.argv)}\n")
        f.flush()
        yield
    finally:
        with contextlib.suppress(Exception):
            f.seek(0)
            f.truncate()
        fcntl.flock(f, fcntl.LOCK_UN)
        f.close()


# --- kontrole systemu -------------------------------------------------------------

def przeszkadzajace_procesy() -> list[str]:
    """Nazwy i PID-y procesów kompilacji / innych pomiarów działających teraz."""
    wlasne = {os.getpid(), os.getppid()}
    znalezione = []
    for d in Path("/proc").iterdir():
        if not d.name.isdigit() or int(d.name) in wlasne:
            continue
        try:
            comm = (d / "comm").read_text().strip()
        except OSError:
            continue
        if comm in PRZESZKADZAJACE:
            znalezione.append(f"{comm} (PID {d.name})")
    return sorted(znalezione)


def obciazenie() -> float:
    return os.getloadavg()[0]


def zajetosc_cpu(sekundy: float = 3.0) -> float:
    """Średnie zużycie CPU całego systemu w % (0–100) z /proc/stat w podanym oknie.

    Lepsze niż load average: na pulpicie load ~1 bywa samym tłem (wątki w stanie D,
    krótkie wybudzenia), choć procesor jest praktycznie wolny."""
    def odczyt():
        pola = Path("/proc/stat").read_text().splitlines()[0].split()[1:]
        v = [int(x) for x in pola]
        bezczynne = v[3] + (v[4] if len(v) > 4 else 0)        # idle + iowait
        return sum(v), bezczynne
    c0, i0 = odczyt()
    time.sleep(sekundy)
    c1, i1 = odczyt()
    return 100.0 * (1.0 - (i1 - i0) / max(1, c1 - c0))


def najbardziej_obciazajace(n: int = 3) -> list[str]:
    """Procesy zużywające najwięcej CPU (ps), np. ['vlc 55%', 'firefox 5%']."""
    try:
        out = subprocess.run(["ps", "-eo", "pcpu=,comm=", "--sort=-pcpu"],
                             capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.TimeoutExpired):
        return []
    wynik = []
    for linia in out.splitlines():
        czesci = linia.split(None, 1)
        if len(czesci) == 2 and czesci[1] not in ("ps", "python3", "python"):
            wynik.append(f"{czesci[1]} {float(czesci[0]):.0f}%")
        if len(wynik) >= n:
            break
    return wynik


def governory() -> dict[str, int]:
    """Licznik governorów na wszystkich rdzeniach, np. {'performance': 24}."""
    licznik: dict[str, int] = {}
    for p in Path("/sys/devices/system/cpu").glob("cpu[0-9]*/cpufreq/scaling_governor"):
        with contextlib.suppress(OSError):
            g = p.read_text().strip()
            licznik[g] = licznik.get(g, 0) + 1
    return licznik


def info_o_binarce(exe: Path) -> dict:
    """Wynik `edge_bench --list-devices` (system, warianty)."""
    out = subprocess.run([str(exe), "--list-devices"], capture_output=True, text=True, timeout=30)
    if out.returncode != 0:
        raise RuntimeError(f"{exe} --list-devices zakończył się kodem {out.returncode}: {out.stderr.strip()}")
    return json.loads(out.stdout.strip().splitlines()[-1])


# --- eksperymenty ------------------------------------------------------------------

def lista_eksperymentow() -> list[Path]:
    return sorted(EKSPERYMENTY.glob("E*.json"), key=lambda p: _klucz_eksperymentu(p.stem))


def _klucz_eksperymentu(stem: str) -> tuple[int, str]:
    # E3_watki -> (3, 'E3_watki'), E13_... po E12
    num = "".join(ch for ch in stem.split("_")[0][1:] if ch.isdigit())
    return (int(num) if num else 9999, stem)


def znajdz_eksperyment(nazwa: str) -> Path:
    """'E3', 'E3_watki' albo ścieżka do pliku."""
    p = Path(nazwa)
    if p.suffix == ".json" and p.exists():
        return p.resolve()
    for e in lista_eksperymentow():
        if e.stem == nazwa or e.stem.split("_")[0] == nazwa:
            return e
    dostepne = ", ".join(e.stem for e in lista_eksperymentow())
    raise FileNotFoundError(f"nie ma eksperymentu '{nazwa}' (dostępne: {dostepne})")


def wczytaj_eksperyment(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


# --- uruchamianie edge_bench --------------------------------------------------------

@dataclass
class WynikUruchomienia:
    kod: int
    sekundy: float
    katalog: Path
    ostrzezenia: list[str] = field(default_factory=list)
    ostatnie_zdarzenie: dict | None = None


def uruchom_edge_bench(args: list[str], katalog: Path, exe: Path,
                       postep: Callable[[dict], None] | None = None) -> WynikUruchomienia:
    """Uruchamia jeden proces edge_bench i czeka na jego koniec.

    Tylko jeden proces naraz: wywołanie jest blokujące, Python w tym czasie
    jedynie czyta zdarzenia JSON ze stdout.  Ctrl+C trafia do edge_bench
    (ta sama grupa procesów) – czekamy, aż zapisze wyniki i sam się zakończy.
    """
    start = time.monotonic()
    wynik = WynikUruchomienia(kod=-1, sekundy=0.0, katalog=katalog)
    proc = subprocess.Popen([str(exe), *args], cwd=REPO, stdout=subprocess.PIPE, text=True, bufsize=1)
    try:
        assert proc.stdout is not None
        for linia in proc.stdout:
            linia = linia.strip()
            if not linia:
                continue
            try:
                zd = json.loads(linia)
            except json.JSONDecodeError:
                continue
            wynik.ostatnie_zdarzenie = zd
            if zd.get("event") == "warning":
                wynik.ostrzezenia.append(zd.get("message", ""))
            elif zd.get("event") == "progress" and postep:
                postep(zd)
        wynik.kod = proc.wait()
    except KeyboardInterrupt:
        # Z terminala edge_bench dostaje ten sam SIGINT; gdy przerwano tylko Pythona
        # (np. kill -INT), przekazujemy sygnał dalej.  Potem czekamy na zapis wyników.
        with contextlib.suppress(ProcessLookupError):
            proc.send_signal(signal.SIGINT)
        try:
            wynik.kod = proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
            wynik.kod = proc.wait()
        wynik.sekundy = time.monotonic() - start
        raise
    wynik.sekundy = time.monotonic() - start
    return wynik


# --- wczytywanie wyników i kontrola jakości --------------------------------------------

# Kolumny identyfikujące konfigurację (bez experiment i rep) – jak EDGE_ID_COLS w harness.cpp
KLUCZ_GRUPY = ("variant", "kernel", "image", "mask", "norm", "scaling", "threads", "schedule", "chunk",
               "mech", "reduce", "affinity", "gpu_prec", "gpu_wg", "gpu_ppw")


def wczytaj_csv(path: Path) -> list[dict]:
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


@dataclass
class Kontrola:
    bledy: list[str] = field(default_factory=list)
    ostrzezenia: list[str] = field(default_factory=list)
    grupy: int = 0
    wiersze: int = 0
    max_cv: float = 0.0


def kontrola_wynikow(katalog: Path, prog_cv: float = 0.05) -> Kontrola:
    """Sprawdza kompletność, poprawność i rozrzut powtórzeń jednego eksperymentu."""
    k = Kontrola()
    meta_p, res_p, cor_p = katalog / "meta.json", katalog / "results.csv", katalog / "correctness.csv"
    for p in (meta_p, res_p, cor_p):
        if not p.exists():
            k.bledy.append(f"brak pliku {p.name}")
    if k.bledy:
        return k

    meta = json.loads(meta_p.read_text(encoding="utf-8"))
    if not meta.get("complete"):
        k.bledy.append(f"pomiar niekompletny ({meta.get('cases_done')}/{meta.get('cases_planned')} przypadków)")
    if meta.get("variants_skipped"):
        k.ostrzezenia.append("pominięte warianty: " + ", ".join(meta["variants_skipped"]))
    sysinfo = meta.get("system", {})
    if sysinfo.get("governor") not in ("performance", None):
        k.ostrzezenia.append(f"governor w czasie pomiaru: {sysinfo.get('governor')}")

    for r in wczytaj_csv(cor_p):
        if r.get("pass") != "1":
            opis = f"{r['variant']} {r['image']} {r['mask']} wątki {r['threads']}"
            k.bledy.append(f"zły wynik: {opis} (maks. różnica {r.get('max_abs_diff')}) {r.get('error', '')}".strip())

    wiersze = wczytaj_csv(res_p)
    k.wiersze = len(wiersze)
    grupy: dict[tuple, list[float]] = {}
    for r in wiersze:
        grupy.setdefault(tuple(r.get(c, "") for c in KLUCZ_GRUPY), []).append(float(r["wall_ms"]))
    k.grupy = len(grupy)

    zaszumione = []
    for klucz, czasy in grupy.items():
        if len(czasy) < 3:
            continue
        sr = statistics.fmean(czasy)
        cv = statistics.stdev(czasy) / sr if sr > 0 else 0.0
        k.max_cv = max(k.max_cv, cv)
        if cv > prog_cv:
            zaszumione.append((cv, klucz, statistics.median(czasy)))
    if zaszumione:
        zaszumione.sort(reverse=True)
        k.ostrzezenia.append(
            f"pomiar zaszumiony: {len(zaszumione)} z {len(grupy)} konfiguracji ma współczynnik zmienności "
            f"> {prog_cv:.0%}; najgorsze:")
        for cv, klucz, med in zaszumione[:5]:
            opis = ", ".join(f"{n}={v}" for n, v in zip(KLUCZ_GRUPY, klucz) if v)
            k.ostrzezenia.append(f"    CV {cv:5.1%}  mediana {med:9.3f} ms  ({opis})")
    return k


def czas_hms(sekundy: float) -> str:
    sekundy = int(math.ceil(sekundy))
    h, r = divmod(sekundy, 3600)
    m, s = divmod(r, 60)
    return f"{h}:{m:02d}:{s:02d}" if h else f"{m}:{s:02d}"
