// ============================================================================
//  V5 – splot rozdzielny przepisany pod auto-wektoryzacje kompilatora
//
//  Zmiana wzgledem V4: kazdy etap to prosta petla po x bez rozgalezien
//  zaleznych od danych, z ciaglym dostepem do pamieci i wskaznikami
//  __restrict (brak aliasingu), kompilowana dla AVX2 (atrybut target –
//  tylko te funkcje, reszta programu zostaje dla zwyklego x86-64):
//    1. przebieg poziomy wnetrza wiersza: uint8 -> int16 (16 pikseli/instr.),
//    2. przebieg pionowy + modul: int16 -> int32 -> float/double,
//    3. maksimum liczone osobna, prosta petla (redukcja).
//  Brzegi (R pikseli z kazdej strony) skalarnie.
//
//  Plik kompilowany z -fno-math-errno (sqrt bez ustawiania errno – inaczej
//  kompilator nie zwektoryzuje pierwiastka) i -ffp-contract=off (bez FMA,
//  zeby wynik byl identyczny z V0).  Raport wektoryzacji:
//      cmake ... -DEDGE_VEC_REPORT=ON   (dodaje -fopt-info-vec-optimized)
// ============================================================================
#include <algorithm>
#include <cmath>

#include "../kernels.h"
#include "static_masks.h"

#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#define EDGE_TARGET_AVX2 __attribute__((target("avx2")))
#else
#define EDGE_TARGET_AVX2
#endif

namespace {

// Czy modul^2 miesci sie dokladnie w float (< 2^24) – wtedy sqrtf daje ten
// sam wynik co (float)sqrt(double) z referencji
template <class M>
constexpr bool floatSqrtExact()
{
    long px = 0, py = 0;
    for (int v : M::gx.v) if (v > 0) px += v;
    for (int v : M::gy.v) if (v > 0) py += v;
    const long gx = 255 * px, gy = 255 * py;
    return gx * gx + gy * gy < (1L << 24);
}

template <class M>
EDGE_TARGET_AVX2 void hpass(const std::uint8_t* __restrict row, int w,
                            std::int16_t* __restrict hs, std::int16_t* __restrict hd)
{
    constexpr int K = M::K, R = K / 2;
    const int xa = std::min(R, w), xb = std::max(xa, w - R);
    for (int x = 0; x < xa; ++x) {
        int s = 0, d = 0;
        for (int j = 0; j < K; ++j) {
            const int p = row[std::clamp(x + j - R, 0, w - 1)];
            s += M::smooth[j] * p;  d += M::diff[j] * p;
        }
        hs[x] = static_cast<std::int16_t>(s);  hd[x] = static_cast<std::int16_t>(d);
    }
    // Wnetrze – petla do wektoryzacji: kazdy x niezalezny, staly krok
    for (int x = xa; x < xb; ++x) {
        int s = 0, d = 0;
        for (int j = 0; j < K; ++j) {
            const int p = row[x + j - R];
            s += M::smooth[j] * p;  d += M::diff[j] * p;
        }
        hs[x] = static_cast<std::int16_t>(s);  hd[x] = static_cast<std::int16_t>(d);
    }
    for (int x = xb; x < w; ++x) {
        int s = 0, d = 0;
        for (int j = 0; j < K; ++j) {
            const int p = row[std::clamp(x + j - R, 0, w - 1)];
            s += M::smooth[j] * p;  d += M::diff[j] * p;
        }
        hs[x] = static_cast<std::int16_t>(s);  hd[x] = static_cast<std::int16_t>(d);
    }
}

// Przebieg pionowy + modul do bufora float (jeden wiersz wyniku)
template <class M, Norm N>
EDGE_TARGET_AVX2 void vpassMag(const std::int16_t* const* hs, const std::int16_t* const* hd,
                               int w, float* __restrict dst)
{
    constexpr int K = M::K;
    for (int x = 0; x < w; ++x) {
        int sx = 0, sy = 0;
        for (int i = 0; i < K; ++i) {
            sx += M::smooth[i] * hd[i][x];
            sy += M::diff[i]   * hs[i][x];
        }
        float v;
        if constexpr (N == Norm::L1) {
            v = static_cast<float>(std::abs(sx) + std::abs(sy));
        } else if constexpr (N == Norm::Linf) {
            v = static_cast<float>(std::max(std::abs(sx), std::abs(sy)));
        } else if constexpr (floatSqrtExact<M>()) {
            const float fx = static_cast<float>(sx), fy = static_cast<float>(sy);
            v = std::sqrt(fx * fx + fy * fy);
        } else {
            const double dx = sx, dy = sy;
            v = static_cast<float>(std::sqrt(dx * dx + dy * dy));
        }
        dst[x] = v;
    }
}

EDGE_TARGET_AVX2 float rowMax(const float* __restrict v, int w, float m)
{
    for (int x = 0; x < w; ++x) m = v[x] > m ? v[x] : m;
    return m;
}

EDGE_TARGET_AVX2 void rowToBytes(const float* __restrict v, int w, float scale, std::uint8_t* __restrict out)
{
    for (int x = 0; x < w; ++x) {
        const float t = v[x] * scale;
        out[x] = static_cast<std::uint8_t>(t > 255.0f ? 255.0f : t + 0.5f);
    }
}

template <class M, Norm N>
float rows(const KernelArgs& a, int y0, int y1, Scratch& s)
{
    constexpr int K = M::K, R = K / 2;
    const int w = a.img.width, h = a.img.height;
    const size_t stride = static_cast<size_t>(w);
    const std::uint8_t* src = a.img.data.data();

    const size_t need = static_cast<size_t>(2 * K) * stride;
    if (s.i16.size() < need) s.i16.resize(need);
    if (s.f32.size() < stride) s.f32.resize(stride);
    s.owner.assign(K, -1);
    std::int16_t* ring = s.i16.data();
    float* tmp = s.f32.data();

    const std::int16_t* hs[K];
    const std::int16_t* hd[K];
    float localMax = 0.0f;

    for (int y = y0; y < y1; ++y) {
        for (int i = 0; i < K; ++i) {
            const int sr = std::clamp(y + i - R, 0, h - 1);
            const int slot = sr % K;
            std::int16_t* phs = ring + static_cast<size_t>(slot) * stride;
            std::int16_t* phd = ring + static_cast<size_t>(slot + K) * stride;
            if (s.owner[slot] != sr) {
                hpass<M>(src + static_cast<size_t>(sr) * stride, w, phs, phd);
                s.owner[slot] = sr;
            }
            hs[i] = phs;  hd[i] = phd;
        }
        const size_t base = static_cast<size_t>(y) * stride;
        if (a.mag) {
            vpassMag<M, N>(hs, hd, w, a.mag + base);
            localMax = rowMax(a.mag + base, w, localMax);
        } else {
            vpassMag<M, N>(hs, hd, w, tmp);
            rowToBytes(tmp, w, a.scale, a.out + base);
        }
    }
    return localMax;
}

template <class M>
float rowsNorm(const KernelArgs& a, int y0, int y1, Scratch& s)
{
    switch (a.norm) {
    case Norm::L1:   return rows<M, Norm::L1>(a, y0, y1, s);
    case Norm::Linf: return rows<M, Norm::Linf>(a, y0, y1, s);
    default:         return rows<M, Norm::L2>(a, y0, y1, s);
    }
}

} // namespace

float rowsV5(const KernelArgs& a, int y0, int y1, Scratch& s)
{
    float result = 0.0f;
    bool handled = false;
    dispatchStaticMask(a.mask, [&](auto m) {
        using M = decltype(m);
        if constexpr (M::separable) {
            result = rowsNorm<M>(a, y0, y1, s);
            handled = true;
        }
    });
    return handled ? result : rowsV2(a, y0, y1, s);
}
