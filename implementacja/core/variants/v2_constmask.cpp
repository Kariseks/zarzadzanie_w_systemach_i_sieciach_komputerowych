// ============================================================================
//  V2 – maska jako stala czasu kompilacji
//
//  Zmiana wzgledem V1: rozmiar K i wspolczynniki sa constexpr (szablon po
//  typie maski z static_masks.h).  Kompilator rozwija obie petle po masce,
//  usuwa mnozenia przez 0 (np. srodkowa kolumna Gx Sobela) i nie czyta
//  wspolczynnikow z pamieci.  Brzegi jak w V1.
// ============================================================================
#include <algorithm>

#include "../kernels.h"
#include "static_masks.h"

namespace {

template <class M, bool Global>
float rows(const KernelArgs& a, int y0, int y1)
{
    constexpr int K = M::K, R = K / 2;
    const GrayImage& img = a.img;
    const int w = img.width, h = img.height;
    const std::uint8_t* src = img.data.data();
    const int xa = std::min(R, w);
    const int xb = std::max(xa, w - R);

    const std::uint8_t* rp[K];
    float localMax = 0.0f;

    for (int y = y0; y < y1; ++y) {
        for (int ky = 0; ky < K; ++ky)
            rp[ky] = src + static_cast<size_t>(std::clamp(y + ky - R, 0, h - 1)) * w;
        const size_t rowBase = static_cast<size_t>(y) * w;

        auto border = [&](int x) {
            int sx = 0, sy = 0;
            for (int ky = 0; ky < K; ++ky)
                for (int kx = 0; kx < K; ++kx) {
                    const int p = rp[ky][std::clamp(x + kx - R, 0, w - 1)];
                    sx += M::gx.v[ky * K + kx] * p;
                    sy += M::gy.v[ky * K + kx] * p;
                }
            storePixel<Global>(a, rowBase + x, sx, sy, localMax);
        };

        for (int x = 0; x < xa; ++x) border(x);
        for (int x = xa; x < xb; ++x) {
            int sx = 0, sy = 0;
            for (int ky = 0; ky < K; ++ky) {
                const std::uint8_t* p = rp[ky] + (x - R);
                for (int kx = 0; kx < K; ++kx) {
                    sx += M::gx.v[ky * K + kx] * p[kx];
                    sy += M::gy.v[ky * K + kx] * p[kx];
                }
            }
            storePixel<Global>(a, rowBase + x, sx, sy, localMax);
        }
        for (int x = xb; x < w; ++x) border(x);
    }
    return localMax;
}

} // namespace

float rowsV2(const KernelArgs& a, int y0, int y1, Scratch& s)
{
    float result = 0.0f;
    const bool ok = dispatchStaticMask(a.mask, [&](auto m) {
        using M = decltype(m);
        result = a.mag ? rows<M, true>(a, y0, y1) : rows<M, false>(a, y0, y1);
    });
    return ok ? result : rowsV1(a, y0, y1, s);   // maska spoza listy – wersja ogolna
}
