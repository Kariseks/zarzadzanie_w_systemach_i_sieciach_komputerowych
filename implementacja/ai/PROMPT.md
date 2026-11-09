# Wariant AI – procedura

Cel: sprawdzić, jakie optymalizacje zaproponuje ogólnodostępny model AI, gdy dostanie kod V0
i proste polecenie, oraz porównać wynik z ręcznie przygotowanymi wariantami V1–V8.
Prowadzący dopuszcza takie porównanie (`materialy/projekt_zssk_2024.pdf`).

Żeby eksperyment był uczciwy:

1. Użyć **jednego** narzędzia (np. ChatGPT, Gemini albo Claude w zwykłym czacie), w nowej rozmowie, bez dodatkowego kontekstu.
2. Wkleić **dokładnie** poniższy prompt z kodem (bez poprawek).
3. Zapisać całą odpowiedź bez zmian do `implementacja/ai/odpowiedz.md` i dopisać na górze: narzędzie, wersję modelu, datę.
4. Kod z odpowiedzi przenieść do `implementacja/core/variants/ai_variant.cpp` z **minimalnym** dopasowaniem do sygnatury `rowsAI` (opisać w komentarzu, co zmieniono – np. tylko nazwa funkcji i argumenty).
5. W `ai_variant.cpp` ustawić `aiVariantImplemented()` na `true`, przebudować, uruchomić `edge_cli --verify -v AI`.
6. Jeśli wynik się różni albo kod się nie kompiluje – **nie poprawiać**. Zapisać to jako wynik eksperymentu (to też jest wniosek). Ewentualna druga runda („popraw błąd: …”) jako osobny wariant, opisany osobno.

## Prompt (do wklejenia)

```text
Zoptymalizuj poniższy kod C++17 pod kątem szybkości działania na jednym rdzeniu procesora
x86-64 (AMD Ryzen 9 9900X, kompilator GCC 13, flagi -O3). Funkcja liczy moduł gradientu
(filtr Sobela i pokrewne) dla wierszy [y0, y1) obrazu w skali szarości. Wynik musi być
identyczny co do bitu z wynikiem obecnej wersji dla każdego obrazu i każdej maski.
Nie zmieniaj sygnatury funkcji. Podaj gotowy kod i krótko opisz zastosowane optymalizacje.

struct GrayImage { int width, height; std::vector<std::uint8_t> data; };
struct Mask { std::string id; int size; std::vector<int> gx, gy; };
enum class Norm { L2, L1, Linf };

inline float gradMagnitude(int sx, int sy, Norm n) {
    switch (n) {
    case Norm::L1:   return static_cast<float>(std::abs(sx) + std::abs(sy));
    case Norm::Linf: return static_cast<float>(std::max(std::abs(sx), std::abs(sy)));
    default: { const double dx = sx, dy = sy; return static_cast<float>(std::sqrt(dx*dx + dy*dy)); }
    }
}

// Zapisuje moduł gradientu do mag (rozmiar width*height), zwraca maksimum w zakresie.
// Brzegi obrazu: powielenie skrajnych pikseli.
float edgeRows(const GrayImage& in, float* mag, const Mask& m, Norm norm, int y0, int y1)
{
    const int w = in.width, h = in.height, r = m.size / 2;
    const std::uint8_t* src = in.data.data();
    float localMax = 0.0f;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < w; ++x) {
            int sx = 0, sy = 0;
            for (int ky = -r; ky <= r; ++ky) {
                const int yy = std::clamp(y + ky, 0, h - 1);
                const std::uint8_t* row = src + static_cast<size_t>(yy) * w;
                const int kRow = (ky + r) * m.size;
                for (int kx = -r; kx <= r; ++kx) {
                    const int xx = std::clamp(x + kx, 0, w - 1);
                    const int p = row[xx];
                    const int idx = kRow + (kx + r);
                    sx += m.gx[idx] * p;
                    sy += m.gy[idx] * p;
                }
            }
            const float v = gradMagnitude(sx, sy, norm);
            mag[static_cast<size_t>(y) * w + x] = v;
            if (v > localMax) localMax = v;
        }
    return localMax;
}
```

## W sprawozdaniu

Podać: narzędzie i model, datę, prompt (dosłownie, np. w załączniku), listę zaproponowanych optymalizacji,
wynik weryfikacji (identyczny / różnice / nie kompiluje się), czas względem V0 i względem najlepszego
wariantu 1-wątkowego, oraz które optymalizacje z V1–V8 model pominął albo dodał.
