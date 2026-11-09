#!/usr/bin/env python3
"""(1) Pełne testy: uruchamia eksperymenty z analiza/eksperymenty/ PO KOLEI.

Pomiary wielowątkowe nie mogą się nakładać, dlatego:
  * w danej chwili działa jeden proces edge_bench (wywołanie blokujące),
  * blokada pliku analiza/.testy.lock nie wpuści drugiego testy.py / pelny_przebieg.py,
  * start jest odrzucany, gdy działa kompilacja albo inny pomiar, lub gdy system jest obciążony,
  * między eksperymentami jest przerwa (--cooldown), żeby wróciło taktowanie i temperatura.

Przykłady (z katalogu głównego repo):
  analiza/.venv/bin/python analiza/testy.py --quick          # test dymny, wyniki do /tmp
  analiza/.venv/bin/python analiza/testy.py E1 E3            # wybrane eksperymenty
  analiza/.venv/bin/python analiza/testy.py --all            # wszystkie poza E0 (dymnym)
Skrypt działa też zwykłym python3 – nie potrzebuje pakietów spoza biblioteki standardowej.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

import wspolne as w

QUICK_IMAGES = "synthetic:320x240:1,synthetic:64x33:2"


@dataclass
class Podsumowanie:
    nazwa: str
    status: str
    sekundy: float = 0.0
    wiersze: int = 0
    grupy: int = 0
    max_cv: float = 0.0
    uwagi: int = 0


def parsuj_argumenty(argv: list[str] | None = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("eksperymenty", nargs="*", help="np. E1 E3 albo pełne nazwy plików bez .json")
    ap.add_argument("--all", action="store_true", help="wszystkie eksperymenty poza E0 (dymnym)")
    ap.add_argument("--quick", action="store_true",
                    help="test dymny: małe obrazy syntetyczne, 1 powtórzenie, bez rozgrzewki, wyniki do katalogu tymczasowego")
    ap.add_argument("--force", action="store_true", help="powtórz eksperymenty, które mają już wyniki")
    ap.add_argument("--repeats", type=int, default=None, help="nadpisz liczbę powtórzeń (tylko próby – nie do sprawozdania)")
    ap.add_argument("--warmup", type=int, default=None, help="nadpisz liczbę przebiegów rozgrzewki")
    ap.add_argument("--cooldown", type=float, default=5.0, help="przerwa między eksperymentami [s] (domyślnie 5)")
    ap.add_argument("--shuffle", type=int, default=None, help="ziarno tasowania kolejności konfiguracji")
    ap.add_argument("--ignore-load", action="store_true", help="nie sprawdzaj obciążenia ani innych procesów")
    ap.add_argument("--yes", action="store_true", help="nie pytaj (np. o governor) – kontynuuj z ostrzeżeniem")
    ap.add_argument("--max-cpu", type=float, default=5.0,
                    help="maks. zajętość CPU przez inne procesy przy starcie [%%] (domyślnie 5 = ok. 1,2 z 24 wątków)")
    ap.add_argument("--wait-load", type=float, default=0.0,
                    help="ile sekund czekać, aż zajętość CPU spadnie poniżej --max-cpu (domyślnie 0 = od razu odmowa)")
    ap.add_argument("--build-dir", type=Path, default=None, help="katalog budowania z edge_bench")
    ap.add_argument("--out-root", type=Path, default=None,
                    help="katalog nadrzędny wyników (domyślnie analiza/wyniki, przy --quick katalog tymczasowy)")
    args = ap.parse_args(argv)
    if not args.eksperymenty and not args.all and not args.quick:
        ap.error("podaj eksperymenty, --all albo --quick")
    return args


def wybierz_eksperymenty(args: argparse.Namespace) -> list[Path]:
    if args.eksperymenty:
        return [w.znajdz_eksperyment(n) for n in args.eksperymenty]
    wszystkie = w.lista_eksperymentow()
    if args.all:
        return [e for e in wszystkie if not e.stem.startswith("E0_")]
    return wszystkie                        # --quick bez listy: wszystko, także E0


def kontrole_przed_startem(args: argparse.Namespace, exe: Path, log: w.Log) -> bool:
    if not exe.exists():
        log(f"BŁĄD: nie ma {exe} – zbuduj projekt (pelny_przebieg.py robi to sam)")
        return False
    try:
        dane = w.info_o_binarce(exe)
    except Exception as e:                  # noqa: BLE001 – komunikat dla użytkownika
        log(f"BŁĄD: {e}")
        return False
    sysinfo = dane.get("system", {})
    if str(sysinfo.get("build_type", "")).lower() != "release":
        log(f"BŁĄD: edge_bench zbudowany jako '{sysinfo.get('build_type')}' – pomiary tylko w Release")
        return False
    log(f"edge_bench: {sysinfo.get('cpu_model')}, {sysinfo.get('logical_cpus')} wątków, "
        f"{sysinfo.get('compiler')} {sysinfo.get('cxx_flags')}")

    if not args.ignore_load:
        # W --quick wyniki nie trafiają do sprawozdania, więc tylko ostrzegamy
        problem = w.ostrzezenie if args.quick else (lambda m: log(f"BŁĄD: {m}"))
        inne = w.przeszkadzajace_procesy()
        if inne:
            problem("działają procesy, które zakłócą pomiar: " + ", ".join(inne)
                    + " – poczekaj, aż się skończą (albo --ignore-load)")
            if not args.quick:
                return False
        cpu = w.zajetosc_cpu()
        if cpu > args.max_cpu and args.wait_load > 0 and not args.quick:
            log(f"zajętość CPU {cpu:.1f}% > {args.max_cpu:.1f}% – czekam do {args.wait_load:.0f} s, aż spadnie ...")
            koniec = time.monotonic() + args.wait_load
            while cpu > args.max_cpu and time.monotonic() < koniec:
                cpu = w.zajetosc_cpu(5.0)
        log(f"zajętość CPU przed startem: {cpu:.1f}% (load average {w.obciazenie():.2f})")
        if cpu > args.max_cpu:
            top = ", ".join(w.najbardziej_obciazajace(3))
            problem(f"inne procesy zajmują {cpu:.1f}% CPU > {args.max_cpu:.1f}% (najwięcej: {top}); "
                    "zamknij inne programy, odczekaj chwilę (albo --ignore-load)")
            if not args.quick:
                return False

    gov = w.governory()
    if gov and set(gov) != {"performance"}:
        opis = ", ".join(f"{g}: {n} rdzeni" for g, n in gov.items())
        w.ostrzezenie(f"governor CPU nie jest 'performance' ({opis}); taktowanie będzie się zmieniać")
        if args.quick or args.yes:
            pass
        elif sys.stdin.isatty():
            if input("Kontynuować mimo to? [t/N] ").strip().lower() not in ("t", "tak", "y", "yes"):
                return False
        else:
            log("BŁĄD: brak terminala do pytania – uruchom z --yes, aby kontynuować")
            return False
    return True


def argumenty_edge_bench(plik: Path, katalog: Path, args: argparse.Namespace) -> list[str]:
    a = ["--experiment", str(plik), "--out", str(katalog)]
    if args.quick:
        a += ["--repeats", "1", "--warmup", "0", "--images", QUICK_IMAGES, "--force"]
    elif args.force:
        a += ["--force"]
    if args.shuffle is not None:
        a += ["--shuffle", str(args.shuffle)]
    if args.repeats is not None and not args.quick:
        a += ["--repeats", str(args.repeats)]
    if args.warmup is not None and not args.quick:
        a += ["--warmup", str(args.warmup)]
    return a


def pasek_postepu(nazwa: str) -> callable:
    start = time.monotonic()

    def pokaz(zd: dict) -> None:
        done, total = zd.get("done", 0), max(1, zd.get("total", 1))
        uplynelo = time.monotonic() - start
        eta = uplynelo / done * (total - done) if done else 0.0
        sys.stderr.write(f"\r  {nazwa}: {done}/{total} ({done / total:5.1%})  {zd.get('variant', '')} "
                         f"t={zd.get('threads', '')} {zd.get('median_ms', 0):.2f} ms   "
                         f"pozostało ~{w.czas_hms(eta)}   ")
        sys.stderr.flush()
    return pokaz


def uruchom_testy(args: argparse.Namespace, blokada_juz_jest: bool = False) -> int:
    """Główna funkcja – wołana też z pelny_przebieg.py (który trzyma już blokadę)."""
    exe = w.edge_bench_path(args.build_dir)
    out_root = args.out_root or (Path(tempfile.gettempdir()) / "zssk_quick" if args.quick else w.WYNIKI)
    log = w.Log(out_root / "testy.log")

    try:
        eksperymenty = wybierz_eksperymenty(args)
    except FileNotFoundError as e:
        log(f"BŁĄD: {e}")
        return 1
    if not eksperymenty:
        log("BŁĄD: brak eksperymentów do uruchomienia")
        return 1

    blokada = w.blokada_pomiarow() if not blokada_juz_jest else _bez_blokady()
    try:
        with blokada:
            if not kontrole_przed_startem(args, exe, log):
                return 1
            return _petla(eksperymenty, args, exe, out_root, log)
    except w.PomiaryTrwaja as e:
        log(f"BŁĄD: {e}")
        return 1


class _bez_blokady:
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def _petla(eksperymenty: list[Path], args: argparse.Namespace, exe: Path, out_root: Path, log: w.Log) -> int:
    log(f"=== testy: {len(eksperymenty)} eksperymentów, wyniki w {out_root}"
        + (" (tryb --quick)" if args.quick else "") + " ===")
    podsum: list[Podsumowanie] = []
    kod_koncowy = 0
    start_calosci = time.monotonic()

    for i, plik in enumerate(eksperymenty):
        nazwa = w.wczytaj_eksperyment(plik).get("name", plik.stem)
        katalog = out_root / plik.stem
        nadpisz = args.force or args.quick
        if katalog.exists() and any(katalog.iterdir()) and not nadpisz:
            if _kompletny(katalog):
                log(f"[{nazwa}] pominięto – kompletne wyniki już są w {katalog} (--force, aby powtórzyć)")
                podsum.append(Podsumowanie(nazwa, "pominięty (istnieje)"))
                continue
            log(f"[{nazwa}] poprzednie wyniki niekompletne (przerwane lub z błędem) – mierzę od nowa")
            nadpisz = True

        if i > 0 and args.cooldown > 0 and podsum and podsum[-1].sekundy > 0:
            w.info(f"  przerwa {args.cooldown:.0f} s ...")
            time.sleep(args.cooldown)

        log(f"[{nazwa}] start")
        try:
            argumenty = argumenty_edge_bench(plik, katalog, args)
            if nadpisz and "--force" not in argumenty:
                argumenty.append("--force")
            wynik = w.uruchom_edge_bench(argumenty, katalog, exe,
                                         postep=pasek_postepu(nazwa))
        except KeyboardInterrupt:
            sys.stderr.write("\n")
            log(f"[{nazwa}] PRZERWANO (Ctrl+C) – częściowe wyniki zostały w {katalog}")
            podsum.append(Podsumowanie(nazwa, "przerwany"))
            kod_koncowy = w.EXIT_INTERRUPTED
            break
        sys.stderr.write("\n")

        if wynik.kod not in (w.EXIT_OK, w.EXIT_CORRECTNESS):
            log(f"[{nazwa}] BŁĄD: edge_bench zakończył się kodem {wynik.kod} ({w.EXIT_NAMES.get(wynik.kod, '?')})")
            podsum.append(Podsumowanie(nazwa, f"błąd {wynik.kod}", wynik.sekundy))
            kod_koncowy = wynik.kod
            break

        k = w.kontrola_wynikow(katalog)
        for o in k.ostrzezenia:
            log(f"[{nazwa}] UWAGA: {o}")
        for b in k.bledy[:20]:
            log(f"[{nazwa}] BŁĄD: {b}")
        status = "OK" if not k.bledy else "BŁĘDY"
        if args.quick and not k.bledy:
            status = "OK (quick)"
        podsum.append(Podsumowanie(nazwa, status, wynik.sekundy, k.wiersze, k.grupy, k.max_cv,
                                   len(k.ostrzezenia)))
        log(f"[{nazwa}] koniec: {status}, {k.wiersze} wierszy, {k.grupy} konfiguracji, "
            f"czas {w.czas_hms(wynik.sekundy)}")
        if k.bledy:
            kod_koncowy = w.EXIT_CORRECTNESS
            break

    _drukuj_podsumowanie(podsum, time.monotonic() - start_calosci, log)
    return kod_koncowy


def _kompletny(katalog: Path) -> bool:
    try:
        meta = json.loads((katalog / "meta.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False
    return bool(meta.get("complete")) and meta.get("correctness_failures", 1) == 0


def _drukuj_podsumowanie(podsum: list[Podsumowanie], sekundy: float, log: w.Log) -> None:
    log("")
    log(f"{'Eksperyment':<22}{'Status':<22}{'Czas':>9}{'Wiersze':>9}{'Konfig.':>9}{'Maks. CV':>10}{'Uwagi':>7}")
    log("-" * 88)
    for p in podsum:
        cv = f"{p.max_cv:.1%}" if p.grupy else "-"
        czas = w.czas_hms(p.sekundy) if p.sekundy else "-"
        log(f"{p.nazwa:<22}{p.status:<22}{czas:>9}{p.wiersze:>9}{p.grupy:>9}{cv:>10}{p.uwagi:>7}")
    log(f"Razem: {w.czas_hms(sekundy)}")


def main() -> int:
    args = parsuj_argumenty()
    if args.quick and args.out_root is None:
        tmp = Path(tempfile.gettempdir()) / "zssk_quick"
        shutil.rmtree(tmp, ignore_errors=True)
    return uruchom_testy(args)


if __name__ == "__main__":
    sys.exit(main())
