// ============================================================================
//  static_masks.h  –  maski jako stale czasu kompilacji (warianty V2+)
//
//  Wspolczynniki sa constexpr, a rozmiar K parametrem szablonu, wiec
//  kompilator rozwija petle po masce, pomija mnozenia przez 0 i zamienia
//  mnozenia przez potegi 2 na przesuniecia.  Wartosci musza byc identyczne
//  z masks.cpp – pilnuje tego weryfikacja (wynik == V0).
// ============================================================================
#pragma once

#include <string>

#include "../masks.h"

template <int K>
struct CoefArr { int v[K * K]; };

template <int K>
constexpr CoefArr<K> outer(const int (&rowCoef)[K], const int (&colCoef)[K])
{
    CoefArr<K> a{};
    for (int i = 0; i < K; ++i)
        for (int j = 0; j < K; ++j) a.v[i * K + j] = rowCoef[i] * colCoef[j];
    return a;
}

struct MSobel3 {
    static constexpr int  K = 3;
    static constexpr bool separable = true;
    static constexpr int  smooth[K] = { 1, 2, 1 };
    static constexpr int  diff[K]   = { -1, 0, 1 };
    static constexpr CoefArr<K> gx = outer<K>(smooth, diff);
    static constexpr CoefArr<K> gy = outer<K>(diff, smooth);
};
struct MSobel5 {
    static constexpr int  K = 5;
    static constexpr bool separable = true;
    static constexpr int  smooth[K] = { 1, 4, 6, 4, 1 };
    static constexpr int  diff[K]   = { -1, -2, 0, 2, 1 };
    static constexpr CoefArr<K> gx = outer<K>(smooth, diff);
    static constexpr CoefArr<K> gy = outer<K>(diff, smooth);
};
struct MSobel7 {
    static constexpr int  K = 7;
    static constexpr bool separable = true;
    static constexpr int  smooth[K] = { 1, 6, 15, 20, 15, 6, 1 };
    static constexpr int  diff[K]   = { -1, -4, -5, 0, 5, 4, 1 };
    static constexpr CoefArr<K> gx = outer<K>(smooth, diff);
    static constexpr CoefArr<K> gy = outer<K>(diff, smooth);
};
struct MPrewitt {
    static constexpr int  K = 3;
    static constexpr bool separable = true;
    static constexpr int  smooth[K] = { 1, 1, 1 };
    static constexpr int  diff[K]   = { -1, 0, 1 };
    static constexpr CoefArr<K> gx = outer<K>(smooth, diff);
    static constexpr CoefArr<K> gy = outer<K>(diff, smooth);
};
struct MScharr {
    static constexpr int  K = 3;
    static constexpr bool separable = true;
    static constexpr int  smooth[K] = { 3, 10, 3 };
    static constexpr int  diff[K]   = { -1, 0, 1 };
    static constexpr CoefArr<K> gx = outer<K>(smooth, diff);
    static constexpr CoefArr<K> gy = outer<K>(diff, smooth);
};
struct MRoberts {
    static constexpr int  K = 3;
    static constexpr bool separable = false;
    static constexpr CoefArr<K> gx = { { 0, 0, 0,  0, 1, 0,  0, 0, -1 } };
    static constexpr CoefArr<K> gy = { { 0, 0, 0,  0, 0, 1,  0, -1, 0 } };
};

// Wywoluje f(M{}) dla maski o danym id; false gdy maska nieznana
template <class F>
bool dispatchStaticMask(const Mask& m, F&& f)
{
    if (m.id == "sobel3")  { f(MSobel3{});  return true; }
    if (m.id == "sobel5")  { f(MSobel5{});  return true; }
    if (m.id == "sobel7")  { f(MSobel7{});  return true; }
    if (m.id == "prewitt") { f(MPrewitt{}); return true; }
    if (m.id == "scharr")  { f(MScharr{});  return true; }
    if (m.id == "roberts") { f(MRoberts{}); return true; }
    return false;
}
