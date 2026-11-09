// ============================================================================
//  V1 – brzegi obrazu obsluzone poza glowna petla
//
//  Zmiana wzgledem V0: wskazniki do k wierszy zrodlowych liczone raz na
//  wiersz wyniku (clamp tylko po y, raz na wiersz).  Wnetrze obrazu
//  [r, w-r) liczone bez std::clamp i bez przeliczania indeksow; clamp
//  zostaje tylko w waskich pasach brzegowych szerokosci r.
//  Maska nadal czytana z std::vector (rozmiar znany w czasie wykonania).
// ============================================================================
#include <algorithm>
#include <array>

#include "../kernels.h"

namespace {

template <bool Global>
float rows(const KernelArgs& a, int y0, int y1)
{
    const GrayImage& img = a.img;
    const Mask& m = a.mask;
    const int w = img.width, h = img.height;
    const int k = m.size, r = k / 2;
    const int* gx = m.gx.data();
    const int* gy = m.gy.data();
    const std::uint8_t* src = img.data.data();

    const int xa = std::min(r, w);           // [0, xa)   – lewy brzeg
    const int xb = std::max(xa, w - r);      // [xa, xb)  – wnetrze, [xb, w) – prawy brzeg

    std::array<const std::uint8_t*, 16> rp{};
    float localMax = 0.0f;

    for (int y = y0; y < y1; ++y) {
        for (int ky = 0; ky < k; ++ky)
            rp[ky] = src + static_cast<size_t>(std::clamp(y + ky - r, 0, h - 1)) * w;
        const size_t rowBase = static_cast<size_t>(y) * w;

        auto border = [&](int x) {
            int sx = 0, sy = 0;
            for (int ky = 0; ky < k; ++ky)
                for (int kx = 0; kx < k; ++kx) {
                    const int p = rp[ky][std::clamp(x + kx - r, 0, w - 1)];
                    sx += gx[ky * k + kx] * p;
                    sy += gy[ky * k + kx] * p;
                }
            storePixel<Global>(a, rowBase + x, sx, sy, localMax);
        };

        for (int x = 0; x < xa; ++x) border(x);
        for (int x = xa; x < xb; ++x) {
            int sx = 0, sy = 0;
            for (int ky = 0; ky < k; ++ky) {
                const std::uint8_t* p = rp[ky] + (x - r);
                const int* cx = gx + ky * k;
                const int* cy = gy + ky * k;
                for (int kx = 0; kx < k; ++kx) {
                    sx += cx[kx] * p[kx];
                    sy += cy[kx] * p[kx];
                }
            }
            storePixel<Global>(a, rowBase + x, sx, sy, localMax);
        }
        for (int x = xb; x < w; ++x) border(x);
    }
    return localMax;
}

} // namespace

float rowsV1(const KernelArgs& a, int y0, int y1, Scratch&)
{
    return a.mag ? rows<true>(a, y0, y1) : rows<false>(a, y0, y1);
}
