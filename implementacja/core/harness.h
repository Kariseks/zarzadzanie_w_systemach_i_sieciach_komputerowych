// ============================================================================
//  harness.h  –  wykonanie eksperymentu pomiarowego i zapis surowych wynikow
//
//  Kolejnosc pracy (wszystko PO KOLEI, w danej chwili liczy jeden przypadek):
//    dla kazdego obrazu (wczytany raz, poza pomiarem)
//      dla kazdej maski, normy i skalowania: wynik referencyjny V0 (poza pomiarem)
//        dla kazdego wariantu i konfiguracji (T: jadro x watki x harmonogram x chunk):
//          rozgrzewka (nie zapisywana), potem N powtorzen -> N wierszy CSV,
//          wynik pierwszego powtorzenia porownany z V0 -> correctness.csv
//
//  Pliki w katalogu wyjsciowym:
//    results.csv      – jeden wiersz na powtorzenie (agregacja w Pythonie)
//    correctness.csv  – jeden wiersz na przypadek
//    images/          – obrazy wynikowe (opcjonalnie)
//  meta.json zapisuje program wywolujacy (edge_bench).
//
//  Wiersze CSV sa zapisywane i oprozniane na biezaco, wiec przerwany pomiar
//  zostawia wszystko, co zdazyl zmierzyc.
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "edge_detector.h"
#include "filter.h"
#include "topology.h"
#include "row_scheduler.h"

struct ExperimentSpec {
    std::string              name = "test";
    std::vector<std::string> variants;           // nazwy wariantow ("V0", "T", ...); "all" = wszystkie dostepne
    std::vector<std::string> images;             // pliki albo synthetic:WxH[:ziarno]
    std::vector<std::string> masks = { "sobel3" };
    std::vector<std::string> kernels  = { "V0" };            // jadra dla wariantu T
    std::vector<Norm>        norms    = { Norm::L2 };
    std::vector<Scaling>     scalings = { Scaling::GlobalMax };
    std::vector<int>         threads   = { 0 };  // 0 = liczba rdzeni logicznych
    std::vector<Schedule>    schedules = { Schedule::Dynamic };
    std::vector<int>         chunks    = { 16 };
    std::vector<ThreadMech>  mechs      = { ThreadMech::Pool };    // tylko T
    std::vector<Reduce>      reduces    = { Reduce::Local };       // tylko T, GlobalMax
    std::vector<Affinity>    affinities = { Affinity::None };      // tylko T
    std::vector<bool>        gpuFp32    = { false };               // tylko GPU: false = auto (dokladnie), true = fp32
    std::vector<std::pair<int, int>> gpuWorkgroups = { { 16, 16 } }; // tylko GPU/W5 (kernel naiwny)
    std::vector<int>         gpuPpws    = { 1 };                   // tylko GPU/W5: pikseli na work-item
    int                      warmup    = 1;
    int                      repeats   = 7;
    std::uint32_t            shuffleSeed = 0;    // 0 = stala kolejnosc; inaczej tasowanie przypadkow w obrebie obrazu
    bool                     saveImages = false;
    bool                     includeIo  = false; // E9: czas obejmuje wczytanie pliku i zapis JPG wyniku
    bool                     strict     = false; // niedostepny wariant = blad zamiast ostrzezenia
};

struct HarnessProgress {
    size_t      done  = 0;      // zakonczone przypadki
    size_t      total = 0;
    std::string variant;
    std::string image;
    std::string mask;
    int         threads = 1;
    double      lastMedianMs = 0.0;
};

struct HarnessCallbacks {
    std::function<void(const HarnessProgress&)> progress;   // po kazdym przypadku
    std::function<bool()>                       stopRequested;
    std::function<void(const std::string&)>     warning;
};

struct HarnessImageInfo {
    std::string source;
    int         width = 0, height = 0;
};

struct HarnessSummary {
    size_t                        casesPlanned = 0;
    size_t                        casesDone    = 0;
    size_t                        rowsWritten  = 0;
    size_t                        correctnessFailures = 0;
    bool                          complete     = false;
    bool                          ioError      = false;
    std::string                   errorMessage;
    std::vector<std::string>      runVariants;      // faktycznie mierzone
    std::vector<std::string>      skippedVariants;  // niedostepne
    std::vector<HarnessImageInfo> images;
};

// Zamienia liste typu "1-4,8,12-16" / "max" na liczby (max -> hwThreads)
bool parseIntList(const std::string& text, std::vector<int>& out, int maxValue);

HarnessSummary runExperiment(const ExperimentSpec& spec, const std::string& outDir,
                             const HarnessCallbacks& cb = {});

// Kolumny results.csv (w tej kolejnosci) – do dokumentacji i testow
const std::vector<std::string>& resultsCsvColumns();
