// ============================================================================
//  V0 – wersja naiwna (punkt odniesienia)
//
//  Ten sam kod co w kernels_ref.h:
//    * std::clamp przy kazdym odczycie piksela (takze we wnetrzu obrazu),
//    * wspolczynniki maski czytane z std::vector, rozmiar znany dopiero
//      w czasie wykonania -> kompilator nie rozwinie petli po masce,
//    * GlobalMax: modul w float (4 B/piksel) i osobny przebieg normalizacji,
//    * bufory alokowane przy kazdym wywolaniu.
// ============================================================================
#include "../kernels.h"
#include "../kernels_ref.h"

float rowsV0(const KernelArgs& a, int y0, int y1, Scratch&)
{
    const int w = a.img.width;
    float localMax = 0.0f;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < w; ++x) {
            int sx, sy;
            gradientAtRef(a.img, a.mask, x, y, sx, sy);
            const size_t idx = static_cast<size_t>(y) * w + x;
            if (a.mag) storePixel<true>(a, idx, sx, sy, localMax);
            else       storePixel<false>(a, idx, sx, sy, localMax);
        }
    return localMax;
}
