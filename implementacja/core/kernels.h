// ============================================================================
//  kernels.h  –  wspolny interfejs jader obliczen (wariantow V0..V8)
//
//  Kazdy wariant jednowatkowy to funkcja liczaca zakres wierszy [y0, y1):
//    * args.mag != nullptr  (skalowanie GlobalMax): zapisuje modul do mag,
//      zwraca maksimum w zakresie,
//    * args.mag == nullptr, args.out != nullptr  (Fixed / Clamp, albo drugi
//      przebieg V8): zapisuje od razu bajty do out ze skala args.scale,
//    * args.mag == args.out == nullptr  (pierwszy przebieg V8): tylko zwraca
//      maksimum modulu, niczego nie zapisuje.
//  Dzieki temu ta sama funkcja sluzy wersji 1-watkowej (caly obraz) i
//  wielowatkowej (wariant T rozdaje fragmenty obrazu watkom).
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

#include "filter.h"
#include "image.h"
#include "masks.h"

// --- alokator wyrownany do linii cache (64 B) ---------------------------------
template <class T, std::size_t Align = 64>
struct AlignedAllocator {
    using value_type = T;
    template <class U> struct rebind { using other = AlignedAllocator<U, Align>; };
    AlignedAllocator() noexcept = default;
    template <class U> AlignedAllocator(const AlignedAllocator<U, Align>&) noexcept {}
    T* allocate(std::size_t n)
    {
        return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t(Align)));
    }
    void deallocate(T* p, std::size_t) noexcept { ::operator delete(p, std::align_val_t(Align)); }
    template <class U> bool operator==(const AlignedAllocator<U, Align>&) const noexcept { return true; }
    template <class U> bool operator!=(const AlignedAllocator<U, Align>&) const noexcept { return false; }
};
template <class T> using AlignedVec = std::vector<T, AlignedAllocator<T>>;

// --- pamiec robocza jednego watku (bufory pierscieniowe wariantow V3+) ---------
struct Scratch {
    AlignedVec<std::int32_t> i32;
    AlignedVec<std::int16_t> i16;
    std::vector<int>         owner;     // ktory wiersz zrodlowy jest w danym slocie bufora
    AlignedVec<float>        f32;
};

struct KernelArgs {
    const GrayImage& img;
    const Mask&      mask;
    Norm             norm;
    float*           mag;     // GlobalMax: bufor modulu (w*h), inaczej nullptr
    std::uint8_t*    out;     // Fixed/Clamp: wynik
    float            scale;   // Fixed/Clamp: skala
};

using RowsFn = float (*)(const KernelArgs& a, int y0, int y1, Scratch& s);

// Skalowanie bufora modulu do bajtow (drugi etap GlobalMax); nullptr = petla skalarna toByte()
using NormalizeFn = void (*)(const float* mag, std::uint8_t* out, std::size_t n, float scale);

struct KernelInfo {
    RowsFn fn            = nullptr;
    bool   reuseBuffers  = false;  // V4+: bufory (mag, scratch) trzymane miedzy wywolaniami
    bool   recompute     = false;  // V8: GlobalMax bez bufora modulu – przebieg 1 liczy tylko
                                   // maksimum (mag == out == nullptr), przebieg 2 liczy od nowa
                                   // i zapisuje bajty
    NormalizeFn normalize = nullptr;  // W3: wektorowe skalowanie
};

// Petla skalowania wspolna dla wszystkich wariantow (skalarna albo KernelInfo::normalize)
inline void normalizeRange(const KernelInfo& k, const float* mag, std::uint8_t* out, std::size_t n, float scale)
{
    if (k.normalize) { k.normalize(mag, out, n, scale); return; }
    for (std::size_t i = 0; i < n; ++i) out[i] = toByte(mag[i], scale);
}

// Zapis jednego piksela – identyczny we wszystkich wariantach
template <bool Global>
inline void storePixel(const KernelArgs& a, std::size_t idx, int sx, int sy, float& localMax)
{
    const float v = gradMagnitude(sx, sy, a.norm);
    if constexpr (Global) {
        a.mag[idx] = v;
        if (v > localMax) localMax = v;
    } else {
        a.out[idx] = toByte(v, a.scale);
    }
}

// Funkcje wierszowe poszczegolnych wariantow (core/variants/*.cpp)
float rowsV0(const KernelArgs& a, int y0, int y1, Scratch& s);
float rowsV1(const KernelArgs& a, int y0, int y1, Scratch& s);
float rowsV2(const KernelArgs& a, int y0, int y1, Scratch& s);
float rowsV3(const KernelArgs& a, int y0, int y1, Scratch& s);
float rowsV4(const KernelArgs& a, int y0, int y1, Scratch& s);
float rowsV5(const KernelArgs& a, int y0, int y1, Scratch& s);   // wymaga AVX2
float rowsV6(const KernelArgs& a, int y0, int y1, Scratch& s);   // wymaga AVX2; obsluguje tryb "tylko maksimum"

bool cpuHasAvx2();

// W3: algorytm W1 (V0) + wektoryzacja AVX2 (core/variants/w3_simd.cpp)
float rowsW3(const KernelArgs& a, int y0, int y1, Scratch& s);
void  normalizeW3(const float* mag, std::uint8_t* out, std::size_t n, float scale);

// Wariant AI (core/variants/ai_variant.cpp, procedura w implementacja/ai/PROMPT.md)
float rowsAI(const KernelArgs& a, int y0, int y1, Scratch& s);
bool  aiVariantImplemented();
