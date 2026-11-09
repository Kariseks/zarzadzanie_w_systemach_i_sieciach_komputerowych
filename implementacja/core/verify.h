// ============================================================================
//  verify.h  –  automatyczne sprawdzanie poprawnosci wariantow wzgledem V0
//
//  Dla kazdego obrazu, maski, normy i skalowania liczony jest wynik V0,
//  a nastepnie kazdy wybrany wariant (wariant T: z kazdym jadrem i dla
//  zestawu konfiguracji watkow / harmonogramow / fragmentow) i porownywany
//  z V0 z tolerancja z VariantInfo::maxAllowedDiff.
//
//  Domyslny zestaw obejmuje przypadki brzegowe: obraz 1x1, jedna kolumna,
//  jeden wiersz, obraz mniejszy od maski, mniej wierszy niz watkow,
//  wymiary nieparzyste, wiecej watkow niz rdzeni, fragment 1 wiersza.
// ============================================================================
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "compare.h"
#include "variants.h"

struct VerifyOptions {
    std::vector<std::string> variants;   // nazwy; puste = wszystkie dostepne
    std::vector<std::string> images;     // zrodla dla loadOrGenerate; puste = zestaw domyslny
    std::vector<std::string> masks;      // id masek; puste = wszystkie
    std::vector<Norm>        norms    = { Norm::L2, Norm::L1, Norm::Linf };
    std::vector<Scaling>     scalings = { Scaling::GlobalMax, Scaling::Fixed, Scaling::Clamp };
    std::vector<std::string> kernels;    // jadra dla T; puste = wszystkie jadra
    std::vector<RunConfig>   mtConfigs = {
        { 1,  Schedule::Static,  16 },  { 1,  Schedule::Dynamic, 1 },
        { 7,  Schedule::Static,  16 },  { 7,  Schedule::Cyclic,  16 },
        { 7,  Schedule::Dynamic, 1 },   { 7,  Schedule::Guided,  1000 },
        { 32, Schedule::Static,  16 },  { 32, Schedule::Cyclic,  1 },
        { 32, Schedule::Dynamic, 16 },  { 32, Schedule::Guided,  1 },
    };
};

struct VerifyCase {
    std::string variant;
    std::string kernel;                  // tylko T
    std::string image;
    int         width = 0, height = 0;
    std::string mask;
    RunConfig   cfg;
    bool        multithreaded = false;
    Comparison  cmp;
    bool        pass = false;
    std::string error;                   // wyjatek z wariantu, jesli byl
};

std::vector<std::string> defaultVerifyImages();

// progress(zrobione, wszystkie) – wolane po kazdym przypadku (moze byc puste)
std::vector<VerifyCase> runVerification(
    const VerifyOptions& opt,
    const std::function<void(size_t, size_t)>& progress = {});
