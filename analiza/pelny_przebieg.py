#!/usr/bin/env python3
"""(3) Pełny przebieg jednym poleceniem:

  1. build Release (cmake + ninja) – PRZED pomiarami, nigdy w trakcie,
  2. obrazy testowe (przygotuj_obrazy.py), jeśli ich brakuje,
  3. weryfikacja poprawności wszystkich wariantów (edge_bench --verify) – zły wynik = stop
     przed długimi pomiarami,
  4. testy.py --all (te same blokady i kontrole: po kolei, cooldown, obciążenie, governor),
  5. przetworz.py (rysunki -> sprawko/zrzuty, tabele, sprawko/wyniki.tex),
  6. pdflatex sprawozdania (i prezentacji z --prezentacja).

Blokada pomiarów jest trzymana przez cały przebieg (drugi testy.py / pelny_przebieg.py
nie wystartuje).  Log: analiza/wyniki/przebieg.log (przy --quick w katalogu tymczasowym).

Przykłady (z katalogu głównego repo):
  analiza/.venv/bin/python analiza/pelny_przebieg.py --quick     # próba całego łańcucha (kilka minut)
  analiza/.venv/bin/python analiza/pelny_przebieg.py             # pełne pomiary do sprawozdania
  analiza/.venv/bin/python analiza/pelny_przebieg.py --force     # powtórz także gotowe eksperymenty
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import wspolne as w

QT_PREFIX = Path.home() / "Qt" / "6.12.0" / "gcc_64"
QT_TOOLS = Path.home() / "Qt" / "Tools"


def env_z_narzedziami() -> dict:
    env = dict(os.environ)
    extra = [str(QT_TOOLS / "CMake" / "bin"), str(QT_TOOLS / "Ninja")]
    env["PATH"] = os.pathsep.join(p for p in extra if Path(p).exists()) + os.pathsep + env.get("PATH", "")
    return env


def krok(log: w.Log, nazwa: str, cmd: list[str], **kw) -> tuple[bool, float]:
    log(f"--- {nazwa}: {' '.join(str(c) for c in cmd)}")
    t = time.monotonic()
    wynik = subprocess.run([str(c) for c in cmd], cwd=w.REPO, **kw)
    sek = time.monotonic() - t
    ok = wynik.returncode == 0
    log(f"    {'OK' if ok else f'BŁĄD (kod {wynik.returncode})'}, {w.czas_hms(sek)}")
    return ok, sek


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quick", action="store_true", help="próba łańcucha: testy --quick, wyniki i rysunki do katalogu tymczasowego")
    ap.add_argument("--force", action="store_true", help="powtórz eksperymenty, które mają już kompletne wyniki")
    ap.add_argument("--cooldown", type=float, default=5.0, help="przerwa między eksperymentami [s]")
    ap.add_argument("--eksperymenty", nargs="*", default=None, help="tylko wybrane (domyślnie wszystkie poza E0)")
    ap.add_argument("--bez-weryfikacji", action="store_true", help="pomiń pełną weryfikację (ok. 1,5 min)")
    ap.add_argument("--bez-pdf", action="store_true", help="nie kompiluj LaTeX-a")
    ap.add_argument("--prezentacja", action="store_true", help="skompiluj też prezentacja.tex")
    ap.add_argument("--ignore-load", action="store_true", help="przekazywane do testy.py")
    ap.add_argument("--yes", action="store_true", help="przekazywane do testy.py (bez pytań)")
    args = ap.parse_args()

    out_root = Path(tempfile.gettempdir()) / "zssk_quick" if args.quick else w.WYNIKI
    if args.quick:
        shutil.rmtree(out_root, ignore_errors=True)
    log = w.Log(out_root / "przebieg.log")
    czasy: list[tuple[str, float, str]] = []
    start = time.monotonic()
    build = w.BUILD_DIR
    env = env_z_narzedziami()

    try:
        with w.blokada_pomiarow():
            log(f"=== pełny przebieg {'(--quick) ' if args.quick else ''}– {time.strftime('%Y-%m-%d %H:%M')} ===")

            # 1. build
            ok, s = krok(log, "konfiguracja CMake", ["cmake", "-S", "implementacja", "-B", build, "-G", "Ninja",
                                                     "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_PREFIX_PATH={QT_PREFIX}"],
                         env=env, stdout=subprocess.DEVNULL)
            czasy.append(("konfiguracja", s, "OK" if ok else "BŁĄD"))
            if not ok:
                return 1
            ok, s = krok(log, "kompilacja", ["cmake", "--build", build], env=env)
            czasy.append(("kompilacja", s, "OK" if ok else "BŁĄD"))
            if not ok:
                return 1

            # 2. obrazy
            if not (w.REPO / "obrazy" / "skalowane" / "manifest.json").exists():
                ok, s = krok(log, "obrazy testowe", [sys.executable, w.ANALIZA / "przygotuj_obrazy.py"])
                czasy.append(("obrazy", s, "OK" if ok else "BŁĄD"))
                if not ok:
                    return 1

            # 3. weryfikacja
            if not args.bez_weryfikacji:
                cmd = [build / "edge_bench", "--verify"]
                if args.quick:
                    cmd += ["--kernels", "V0,V6", "--masks", "sobel3,sobel7"]
                ok, s = krok(log, "weryfikacja poprawności", cmd, stdout=subprocess.DEVNULL)
                czasy.append(("weryfikacja", s, "OK" if ok else "BŁĄD"))
                if not ok:
                    log("STOP: co najmniej jeden wariant daje zły wynik – szczegóły: implementacja/build/edge_cli --verify")
                    return 3

            # 4. testy (ta sama blokada – testy.py jej nie bierze ponownie)
            sys.path.insert(0, str(w.ANALIZA))
            import testy
            # weryfikacja przed chwilą obciążyła wszystkie rdzenie – czekamy, aż load average opadnie
            targs = ["--cooldown", str(args.cooldown), "--wait-load", "600"]
            if args.quick:
                targs.append("--quick")
            elif args.eksperymenty:
                targs += args.eksperymenty
            else:
                targs.append("--all")
            if args.force:
                targs.append("--force")
            if args.ignore_load:
                targs.append("--ignore-load")
            if args.yes:
                targs.append("--yes")
            log(f"--- testy.py {' '.join(targs)}")
            t = time.monotonic()
            kod = testy.uruchom_testy(testy.parsuj_argumenty(targs), blokada_juz_jest=True)
            czasy.append(("pomiary", time.monotonic() - t, "OK" if kod == 0 else f"kod {kod}"))
            if kod != 0:
                log(f"STOP: testy zakończone kodem {kod}")
                return kod

            # 5. przetwarzanie
            import przetworz
            log("--- przetworz.py")
            t = time.monotonic()
            pargs = ["--wyniki", str(out_root)] if args.quick else []
            kod = przetworz.main(pargs)
            if kod == 0:
                kod = przetworz.main(pargs + ["--slajdy"])     # osobny eksport rysunków na slajdy
            czasy.append(("przetwarzanie", time.monotonic() - t, "OK" if kod == 0 else f"kod {kod}"))

            # 6. PDF
            if not args.bez_pdf and not args.quick and shutil.which("pdflatex"):
                for tex in ["sprawozdanie"] + (["prezentacja"] if args.prezentacja else []):
                    t = time.monotonic()
                    ok = True
                    for _ in range(2):
                        r = subprocess.run(["pdflatex", "-interaction=nonstopmode", "-halt-on-error", f"{tex}.tex"],
                                           cwd=w.SPRAWKO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                        ok = ok and r.returncode == 0
                    czasy.append((f"{tex}.pdf", time.monotonic() - t, "OK" if ok else f"BŁĄD (zob. sprawko/{tex}.log)"))
    except w.PomiaryTrwaja as e:
        log(f"BŁĄD: {e}")
        return 1
    except KeyboardInterrupt:
        log("PRZERWANO (Ctrl+C)")
        return w.EXIT_INTERRUPTED
    finally:
        log("")
        log(f"{'Etap':<22}{'Czas':>10}  Wynik")
        for nazwa, s, st in czasy:
            log(f"{nazwa:<22}{w.czas_hms(s):>10}  {st}")
        log(f"Razem: {w.czas_hms(time.monotonic() - start)}")
        if not args.quick:
            log(f"Rysunki: {w.ZRZUTY.relative_to(w.REPO)}/, tabele: {w.TABELE.relative_to(w.REPO)}/, "
                f"liczby: sprawko/wyniki.tex, PDF: sprawko/sprawozdanie.pdf")
    return 0


if __name__ == "__main__":
    sys.exit(main())
