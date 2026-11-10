#!/usr/bin/env bash
# Uruchamia aplikację edge_gui (prezentacja wyników projektu).
# Działa z dowolnego miejsca i po przeniesieniu folderu na inny komputer:
# wszystkie ścieżki są względne do tego skryptu. Jeśli programu jeszcze nie ma
# albo kod się zmienił, najpierw go kompiluje (Release, katalog implementacja/build).
set -euo pipefail

REPO="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
BUILD="$REPO/implementacja/build"

# Qt i narzędzia: najpierw zmienna QT_DIR, potem typowa instalacja w ~/Qt
QT_DIR="${QT_DIR:-$(ls -d "$HOME"/Qt/6.*/gcc_64 2>/dev/null | sort -V | tail -n1 || true)}"
export PATH="$HOME/Qt/Tools/CMake/bin:$HOME/Qt/Tools/Ninja:$PATH"

if [[ ! -f "$BUILD/build.ninja" ]]; then
    echo "Konfiguracja projektu (pierwsze uruchomienie na tym komputerze)…"
    cmake -S "$REPO/implementacja" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
          ${QT_DIR:+-DCMAKE_PREFIX_PATH="$QT_DIR"}
fi
echo "Kompilacja (tylko zmienione pliki)…"
cmake --build "$BUILD" --target edge_gui

exec "$BUILD/edge_gui" "$@"
