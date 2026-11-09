// ============================================================================
//  compare.h  –  porownanie wyniku wariantu z wynikiem referencyjnym (V0)
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Comparison {
    bool      sameSize       = false;
    bool      identical      = false;
    int       maxAbsDiff     = 0;      // najwieksza roznica poziomu szarosci
    double    meanAbsDiff    = 0.0;    // srednia roznica
    long long differing      = 0;      // liczba roznych pikseli
    double    psnrDb         = 0.0;    // +inf gdy identyczne
};

Comparison compareImages(const std::vector<std::uint8_t>& reference,
                         const std::vector<std::uint8_t>& tested);

// "inf" albo liczba z 2 miejscami po przecinku
std::string formatPsnr(double psnrDb);
