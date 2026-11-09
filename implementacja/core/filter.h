// ============================================================================
//  filter.h  –  parametry filtru wspolne dla wszystkich wariantow
//
//  Norma gradientu:
//    L2   = sqrt(Gx^2 + Gy^2)        (dokladna, referencja)
//    L1   = |Gx| + |Gy|              (przyblizenie bez pierwiastka)
//    Linf = max(|Gx|, |Gy|)          (przyblizenie bez pierwiastka)
//
//  Skalowanie wyniku do 0..255:
//    GlobalMax – wg najwiekszego modulu w obrazie: wymaga 2 przebiegow,
//                redukcji maksimum i (wielowatkowo) bariery miedzy etapami
//    Fixed     – wg teoretycznego maksimum maski: 1 przebieg, bez bariery
//    Clamp     – bez skalowania, obciecie do 255: 1 przebieg
//
//  Wszystkie warianty musza liczyc piksel DOKLADNIE tymi funkcjami, zeby wynik
//  byl identyczny co do bajtu.  L2 liczone w double: iloczyny sa dokladne
//  (|Gx| <= 255*64*10 dla Sobela 7x7), pierwiastek poprawnie zaokraglony,
//  wiec wynik nie zalezy od kolejnosci dzialan ani od FMA.
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "masks.h"

enum class Norm    { L2, L1, Linf };
enum class Scaling { GlobalMax, Fixed, Clamp };

const char* toString(Norm n);
const char* toString(Scaling s);
bool parseNorm(const std::string& s, Norm& out);        // l2 | l1 | linf
bool parseScaling(const std::string& s, Scaling& out);  // global | fixed | clamp

inline float gradMagnitude(int sx, int sy, Norm n)
{
    switch (n) {
    case Norm::L1:   return static_cast<float>(std::abs(sx) + std::abs(sy));
    case Norm::Linf: return static_cast<float>(std::max(std::abs(sx), std::abs(sy)));
    case Norm::L2:
    default: {
        const double dx = sx, dy = sy;
        return static_cast<float>(std::sqrt(dx * dx + dy * dy));
    }
    }
}

// Modul -> bajt: zaokraglenie do najblizszej, nasycenie na 255
inline std::uint8_t toByte(float v, float scale)
{
    const float t = v * scale;
    return static_cast<std::uint8_t>(t > 255.0f ? 255.0f : t + 0.5f);
}

// Skala dla GlobalMax (po wyznaczeniu maksimum)
inline float globalScale(float maxV) { return maxV > 0.0f ? 255.0f / maxV : 0.0f; }

// Teoretyczne maksimum modulu dla maski i normy (obraz 0..255)
float theoreticalMax(const Mask& m, Norm n);

// Skala dla Fixed / Clamp (dla GlobalMax nie uzywac)
float directScale(const Mask& m, Norm n, Scaling s);
