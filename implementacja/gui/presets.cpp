/**
 * @file presets.cpp
 * @brief Gotowe ustawienia pokazu i koordynator pomiarów (patrz presets.h).
 */
#include "presets.h"

#include "topology.h"

#include <QFileInfo>
#include <QRegularExpression>

namespace {
/** Konfiguracja W4 z najważniejszymi polami. */
RunConfig w4(int watki, Schedule h = Schedule::Dynamic, int fragment = 16, Reduce r = Reduce::Local,
             Affinity a = Affinity::None)
{
    RunConfig c;
    c.threads = watki;
    c.schedule = h;
    c.chunkRows = fragment;
    c.reduce = r;
    c.affinity = a;
    return c;
}
RunConfig w5(int wgx, int wgy, int ppw)
{
    RunConfig c;
    c.gpuWgX = wgx;
    c.gpuWgY = wgy;
    c.gpuPpw = ppw;
    return c;
}
} // namespace

const QVector<GotoweUstawienie>& gotoweUstawienia()
{
    const int n = std::max(1, static_cast<int>(cpuTopology().size()));
    static const QVector<GotoweUstawienie> lista = {
        { "1. Pierwsze uruchomienie: Kodak, W4",
          "Zdjęcie 768×512 przetworzone przez wszystkie wątki sprzętowe – obraz wejściowy i krawędzie obok siebie, "
          "czasy wczytania, algorytmu i zapisu.",
          Zakladka::Przetwarzanie, "obrazy/kodim08.png", "sobel3", Wersja::W4, w4(n), 7, true },
        { "2. Wersja bazowa W1 na 4K",
          "Punkt odniesienia (w pomiarach do sprawozdania ok. 76 ms samego algorytmu). Zapis JPG trwa dłużej niż algorytm.",
          Zakladka::Przetwarzanie, "obrazy/skalowane/3840x2160.jpg", "sobel3", Wersja::W1, {}, 5, true },
        { "3. Porównanie W1–W5 (4K, Sobel 3×3)",
          "Wszystkie wersje na tym samym obrazie (w pomiarach do sprawozdania: ok. 5× W2, 9× W3 i W4, 11× W5 względem W1).",
          Zakladka::Porownanie, "obrazy/skalowane/3840x2160.jpg", "sobel3", Wersja::W1, w4(n), 5, true },
        { "4. Duża maska: Sobel 7×7",
          "Splot rozdzielny (W2) wygrywa z wektoryzacją i wątkami – algorytm ważniejszy niż sprzęt.",
          Zakladka::Porownanie, "obrazy/skalowane/3840x2160.jpg", "sobel7", Wersja::W1, w4(n), 3, false },
        { "5. Skalowanie wątków (4K)",
          "Przyspieszenie S(N) daleko od liniowego: część sekwencyjna (prawo Amdahla) i mały zysk z SMT powyżej liczby rdzeni fizycznych.",
          Zakladka::Skalowanie, "obrazy/skalowane/3840x2160.jpg", "sobel3", Wersja::W4, w4(n), 5, false, n },
        { "6. False sharing",
          "Każdy wątek zapisuje własną komórkę, ale wszystkie leżą w jednej linii cache – kilkukrotnie wolniej.",
          Zakladka::Przetwarzanie, "obrazy/skalowane/3840x2160.jpg", "sobel3", Wersja::W4,
          w4(n, Schedule::Dynamic, 16, Reduce::PixelUnpadded), 5, false },
        { "7. Mutex przy każdym pikselu",
          "Sekcja krytyczna szereguje wątki – kilkadziesiąt razy wolniej niż lokalne maksimum.",
          Zakladka::Przetwarzanie, "obrazy/skalowane/3840x2160.jpg", "sobel3", Wersja::W4,
          w4(n, Schedule::Dynamic, 16, Reduce::Mutex), 3, false },
        { "8. Zły rozmiar fragmentu (256 wierszy)",
          "Fragmenty po 256 wierszy dają za mało porcji pracy dla wszystkich wątków – część wątków czeka (nierównowaga obciążenia).",
          Zakladka::Przetwarzanie, "obrazy/skalowane/3840x2160.jpg", "sobel3", Wersja::W4,
          w4(n, Schedule::Dynamic, 256), 5, false },
        { "9. GPU na obrazie 8K",
          "Karta graficzna z przesyłem przez PCIe; 16 pikseli na wątek GPU.",
          Zakladka::Przetwarzanie, "obrazy/skalowane/7680x4320.jpg", "sobel3", Wersja::W5, w5(16, 16, 16), 5, true },
        { "10. Szum i maska 7×7",
          "Obraz z szumem σ = 30: porównaj wynik masek Sobel 3×3 i 7×7 (zmień maskę i uruchom ponownie).",
          Zakladka::Przetwarzanie, "obrazy/jakosc/szum_30.png", "sobel7", Wersja::W2, {}, 3, false },
    };
    return lista;
}

QString pelnaSciezka(const QString& obraz)
{
    if (obraz.startsWith("synthetic:") || obraz.startsWith('/')) return obraz;
    const QString p = katalogRepo() + '/' + obraz;
    if (QFileInfo::exists(p)) return p;
    // Brak pliku (np. obrazy NASA nie są w repozytorium – inny komputer): obraz syntetyczny tego samego rozmiaru
    const auto m = QRegularExpression(QStringLiteral("(\\d+)x(\\d+)")).match(QFileInfo(obraz).fileName());
    if (m.hasMatch()) return QStringLiteral("synthetic:%1x%2").arg(m.captured(1), m.captured(2));
    if (obraz.contains(QLatin1String("jakosc"))) return QStringLiteral("synthetic:768x512");
    return p;
}

Koordynator& Koordynator::instancja()
{
    static Koordynator k;
    return k;
}

void Koordynator::ustawZajety(bool z, const QString& opis)
{
    zajety_ = z;
    emit zmiana(z, opis);
}
