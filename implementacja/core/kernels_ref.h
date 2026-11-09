// ============================================================================
//  kernels_ref.h  –  jadra obliczen wersji referencyjnej (V0 i T)
//
//  Splot i normalizacja licza dowolny zakres wierszy [y0, y1), dzieki czemu
//  ten sam kod sluzy wersji 1-watkowej (caly obraz) i wielowatkowej
//  (scheduler rozdaje fragmenty).  Tego pliku NIE optymalizujemy – to punkt
//  odniesienia dla wszystkich pozostalych wariantow:
//    * std::clamp przy kazdym odczycie piksela (takze we wnetrzu obrazu),
//    * wspolczynniki z std::vector, rozmiar maski znany dopiero w czasie
//      wykonania -> kompilator nie rozwinie petli po masce.
// ============================================================================
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "filter.h"
#include "image.h"
#include "masks.h"

// Gx, Gy w punkcie (x, y); brzegi: powielenie skrajnych pikseli (clamp)
inline void gradientAtRef(const GrayImage& in, const Mask& m, int x, int y, int& sx, int& sy)
{
    const int w = in.width;
    const int h = in.height;
    const int r = m.size / 2;
    const std::uint8_t* src = in.data.data();
    sx = 0;
    sy = 0;
    for (int ky = -r; ky <= r; ++ky) {
        const int yy = std::clamp(y + ky, 0, h - 1);
        const std::uint8_t* row = src + static_cast<size_t>(yy) * w;
        const int kRow = (ky + r) * m.size;
        for (int kx = -r; kx <= r; ++kx) {
            const int xx  = std::clamp(x + kx, 0, w - 1);
            const int p   = row[xx];
            const int idx = kRow + (kx + r);
            sx += m.gx[idx] * p;
            sy += m.gy[idx] * p;
        }
    }
}

// ----------------------------------------------------------------------------
//  Skalowanie GlobalMax, krok 1: modul gradientu wierszy [y0, y1) do 'mag'.
//  Zwraca maksimum w tym fragmencie.  Funkcja tylko CZYTA 'in' i PISZE
//  wylacznie wiersze [y0,y1) w 'mag', wiec watki moga ja wolac bez blokad.
// ----------------------------------------------------------------------------
inline float edgeRows(const GrayImage& in, std::vector<float>& mag,
                      const Mask& m, Norm norm, int y0, int y1)
{
    const int w = in.width;
    float localMax = 0.0f;
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < w; ++x) {
            int sx, sy;
            gradientAtRef(in, m, x, y, sx, sy);
            const float v = gradMagnitude(sx, sy, norm);
            mag[static_cast<size_t>(y) * w + x] = v;
            if (v > localMax) localMax = v;
        }
    }
    return localMax;
}

// Skalowanie GlobalMax, krok 2: normalizacja wierszy [y0, y1) skala 255/maks
inline void normalizeRows(const std::vector<float>& mag, std::vector<std::uint8_t>& out,
                          int w, float scale, int y0, int y1)
{
    const size_t begin = static_cast<size_t>(y0) * w;
    const size_t end   = static_cast<size_t>(y1) * w;
    for (size_t i = begin; i < end; ++i) out[i] = toByte(mag[i], scale);
}

// ----------------------------------------------------------------------------
//  Skalowanie Fixed / Clamp: jeden przebieg, wynik od razu do 'out'
// ----------------------------------------------------------------------------
inline void edgeRowsDirect(const GrayImage& in, std::vector<std::uint8_t>& out,
                           const Mask& m, Norm norm, float scale, int y0, int y1)
{
    const int w = in.width;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < w; ++x) {
            int sx, sy;
            gradientAtRef(in, m, x, y, sx, sy);
            out[static_cast<size_t>(y) * w + x] = toByte(gradMagnitude(sx, sy, norm), scale);
        }
}
