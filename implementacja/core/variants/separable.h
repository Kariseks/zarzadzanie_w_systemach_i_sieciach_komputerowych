// ============================================================================
//  separable.h  –  splot rozdzielny z buforem pierscieniowym (warianty V3, V4)
//
//  Gx = suma_i smooth[i] * Hd(y+i-R),   Hd(s)[x] = suma_j diff[j]   * p(s, x+j-R)
//  Gy = suma_i diff[i]   * Hs(y+i-R),   Hs(s)[x] = suma_j smooth[j] * p(s, x+j-R)
//
//  Hd i Hs (przebieg poziomy) liczone raz na wiersz zrodlowy i trzymane w
//  buforze K wierszy (slot = wiersz % K), wiec kazdy wiersz obrazu jest
//  przetwarzany poziomo tylko raz: 2K zamiast K^2 mnozen na piksel i maly
//  zbior roboczy (K wierszy) mieszczacy sie w L1/L2.
//  Uwaga: kazde wywolanie (fragment obrazu) zaczyna z pustym buforem, wiec
//  przy malych fragmentach (chunk) dochodzi narzut 2R dodatkowych wierszy.
//
//  Acc – typ bufora: int32_t (V3) albo int16_t (V4: |Hs| <= 255*64 = 16320,
//  |Hd| <= 255*10 dla Sobela 7x7 – miesci sie w int16).
// ============================================================================
#pragma once

#include <algorithm>
#include <cstdint>

#include "../kernels.h"

template <class M, class Acc>
inline void horizontalPass(const std::uint8_t* row, int w, Acc* hs, Acc* hd)
{
    constexpr int K = M::K, R = K / 2;
    const int xa = std::min(R, w);
    const int xb = std::max(xa, w - R);
    auto edge = [&](int x) {
        int s = 0, d = 0;
        for (int j = 0; j < K; ++j) {
            const int p = row[std::clamp(x + j - R, 0, w - 1)];
            s += M::smooth[j] * p;
            d += M::diff[j] * p;
        }
        hs[x] = static_cast<Acc>(s);
        hd[x] = static_cast<Acc>(d);
    };
    for (int x = 0; x < xa; ++x) edge(x);
    for (int x = xa; x < xb; ++x) {
        const std::uint8_t* p = row + (x - R);
        int s = 0, d = 0;
        for (int j = 0; j < K; ++j) {
            s += M::smooth[j] * p[j];
            d += M::diff[j] * p[j];
        }
        hs[x] = static_cast<Acc>(s);
        hd[x] = static_cast<Acc>(d);
    }
    for (int x = xb; x < w; ++x) edge(x);
}

template <class M, class Acc, bool Global>
float separableRows(const KernelArgs& a, int y0, int y1, Acc* ring, std::vector<int>& owner)
{
    constexpr int K = M::K, R = K / 2;
    const int w = a.img.width, h = a.img.height;
    const std::uint8_t* src = a.img.data.data();
    const size_t stride = static_cast<size_t>(w);
    // ring: [2*K wierszy]: sloty 0..K-1 = Hs, K..2K-1 = Hd
    std::fill(owner.begin(), owner.end(), -1);

    auto rowPtrs = [&](int s, const Acc*& hs, const Acc*& hd) {
        const int slot = s % K;
        Acc* phs = ring + static_cast<size_t>(slot) * stride;
        Acc* phd = ring + static_cast<size_t>(slot + K) * stride;
        if (owner[slot] != s) {
            horizontalPass<M, Acc>(src + static_cast<size_t>(s) * stride, w, phs, phd);
            owner[slot] = s;
        }
        hs = phs;
        hd = phd;
    };

    float localMax = 0.0f;
    const Acc* hs[K];
    const Acc* hd[K];
    for (int y = y0; y < y1; ++y) {
        for (int i = 0; i < K; ++i) rowPtrs(std::clamp(y + i - R, 0, h - 1), hs[i], hd[i]);
        const size_t rowBase = static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            int sx = 0, sy = 0;
            for (int i = 0; i < K; ++i) {
                sx += M::smooth[i] * static_cast<int>(hd[i][x]);
                sy += M::diff[i]   * static_cast<int>(hs[i][x]);
            }
            storePixel<Global>(a, rowBase + x, sx, sy, localMax);
        }
    }
    return localMax;
}
