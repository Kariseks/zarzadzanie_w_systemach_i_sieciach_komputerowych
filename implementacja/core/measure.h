// ============================================================================
//  measure.h  –  pomiary: czas zegarowy (scienny), czas CPU procesu
//  i pojedynczego watku, zuzycie RAM (Windows, Linux, macOS)
// ============================================================================
#pragma once

#include <chrono>
#include <cstddef>
#include <string>

// Czas CPU calego procesu = suma czasow WSZYSTKICH watkow [s]
double cpuTimeSec();

// Czas CPU zuzyty przez BIEZACY watek [s]
double threadCpuTimeSec();

// Aktualnie zajeta pamiec fizyczna procesu (RSS / Working Set) w bajtach
size_t currentRamBytes();

// Szczytowe zuzycie pamieci fizycznej od startu procesu
size_t peakRamBytes();

// "12.34 MB"
std::string mb(size_t bytes);

// ----------------------------------------------------------------------------
//  Stoper mierzacy jednoczesnie czas zegarowy (scienny) i czas CPU procesu
// ----------------------------------------------------------------------------
class Stopwatch {
public:
    Stopwatch() { restart(); }
    void restart()
    {
        wall0_ = std::chrono::steady_clock::now();
        cpu0_  = cpuTimeSec();
    }
    double wallMs() const
    {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - wall0_).count();
    }
    double cpuMs() const { return (cpuTimeSec() - cpu0_) * 1000.0; }

private:
    std::chrono::steady_clock::time_point wall0_;
    double cpu0_ = 0.0;
};
