// ============================================================================
//  masks.h  –  maski (jadra splotu) do wykrywania krawedzi
//  gx – gradient poziomy, gy – gradient pionowy
//
//  Maski rozdzielne (Sobel 3/5/7, Prewitt, Scharr) sa budowane z dwoch
//  wektorow 1D:  gx[i][j] = smooth[i] * diff[j],  gy[i][j] = diff[i] * smooth[j]
//  (i – wiersz, j – kolumna).  Splot 2D k x k da sie wtedy policzyc jako dwa
//  sploty 1D: 2k zamiast k^2 mnozen na piksel (wariant V3).
//  Sobel k x k: smooth = wiersz trojkata Pascala dlugosci k,
//               diff   = wiersz Pascala dlugosci k-2 splecony z [-1 0 1].
// ============================================================================
#pragma once

#include <string>
#include <vector>

struct Mask {
    std::string      id;     // krotka nazwa do linii polecen / CSV, np. "sobel3"
    std::string      name;   // nazwa do wyswietlenia
    int              size;   // maska size x size (nieparzyste)
    std::vector<int> gx;
    std::vector<int> gy;
    bool             separable = false;
    std::vector<int> smooth; // tylko dla rozdzielnych (dlugosc size)
    std::vector<int> diff;
};

// Kolejnosc = numery w menu CLI: 1 sobel3, 2 sobel5, 3 prewitt, 4 scharr,
// 5 roberts, 6 sobel7
const std::vector<Mask>& availableMasks();

// Wyszukuje maske po id ("sobel3") albo po numerze z menu ("1"); nullptr gdy brak
const Mask* findMask(const std::string& idOrNumber);
