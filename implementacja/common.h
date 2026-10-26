// ============================================================================
//  common.h  –  kod wspolny
//
//  * wczytywanie / zapis JPG (biblioteka stb – same naglowki, zero instalacji)
//  * maski do wykrywania krawedzi (Sobel 3x3, Sobel 5x5, Prewitt, Scharr, Roberts)
//  * splot liczony dla zakresu wierszy [y0, y1) – dzieki temu scheduler moze
//    rozdawac watkom dowolne fragmenty obrazu
//  * pomiary: czas zegarowy (scienny), czas CPU procesu i pojedynczego watku,
//    zuzycie RAM (Windows, Linux, macOS)
// ============================================================================
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <psapi.h>
#elif defined(__APPLE__)
    #include <mach/mach.h>
    #include <sys/resource.h>
    #include <time.h>
#else
    #include <fstream>
    #include <sys/resource.h>
    #include <time.h>
#endif

#include "stb_image.h"
#include "stb_image_write.h"

// ----------------------------------------------------------------------------
//  Obraz w skali szarosci (1 bajt na piksel)
// ----------------------------------------------------------------------------
struct GrayImage {
    int width  = 0;
    int height = 0;
    std::vector<std::uint8_t> data;   // width * height
};

inline bool loadGray(const std::string& path, GrayImage& img)
{
    int w = 0, h = 0, channels = 0;
    // ostatni argument = 1 -> stb od razu konwertuje do skali szarosci
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 1);
    if (!pixels) {
        std::cerr << "Blad: nie moge wczytac '" << path << "': "
                  << stbi_failure_reason() << "\n";
        return false;
    }
    img.width  = w;
    img.height = h;
    img.data.assign(pixels, pixels + static_cast<size_t>(w) * h);
    stbi_image_free(pixels);
    return true;
}

inline bool saveGrayJpg(const std::string& path, const std::vector<std::uint8_t>& data,
                        int w, int h, int quality = 95)
{
    return stbi_write_jpg(path.c_str(), w, h, 1, data.data(), quality) != 0;
}

// ----------------------------------------------------------------------------
//  Maski (jadra splotu).  gx – gradient poziomy, gy – gradient pionowy.
// ----------------------------------------------------------------------------
struct Mask {
    std::string      name;
    int              size;   // maska size x size (nieparzyste)
    std::vector<int> gx;
    std::vector<int> gy;
};

inline std::vector<Mask> availableMasks()
{
    return {
        { "Sobel 3x3", 3,
          { -1, 0, 1,
            -2, 0, 2,
            -1, 0, 1 },
          { -1,-2,-1,
             0, 0, 0,
             1, 2, 1 } },

        { "Sobel 5x5", 5,
          { -1, -2,  0,  2, 1,
            -4, -8,  0,  8, 4,
            -6,-12,  0, 12, 6,
            -4, -8,  0,  8, 4,
            -1, -2,  0,  2, 1 },
          { -1,-4, -6,-4,-1,
            -2,-8,-12,-8,-2,
             0, 0,  0, 0, 0,
             2, 8, 12, 8, 2,
             1, 4,  6, 4, 1 } },

        { "Prewitt 3x3", 3,
          { -1, 0, 1,
            -1, 0, 1,
            -1, 0, 1 },
          { -1,-1,-1,
             0, 0, 0,
             1, 1, 1 } },

        { "Scharr 3x3", 3,
          {  -3, 0,  3,
            -10, 0, 10,
             -3, 0,  3 },
          { -3,-10,-3,
             0,  0, 0,
             3, 10, 3 } },

        // Roberts to maska 2x2 – wpisana w 3x3 z zerami
        { "Roberts (krzyz)", 3,
          { 0, 0, 0,
            0, 1, 0,
            0, 0,-1 },
          { 0, 0, 0,
            0, 0, 1,
            0,-1, 0 } },
    };
}

// ----------------------------------------------------------------------------
//  Krok 1: splot dla wierszy [y0, y1).  Wynik: modul gradientu sqrt(Gx^2+Gy^2)
//  zapisany do 'mag'.  Zwraca maksymalna wartosc w tym fragmencie
//  (potrzebna do normalizacji do zakresu 0..255).
//  Brzegi obrazu: powielenie skrajnych pikseli (clamp).
//  Funkcja tylko CZYTA 'in' i PISZE wylacznie wiersze [y0,y1) w 'mag',
//  wiec rozne watki moga ja wolac rownolegle bez blokad.
// ----------------------------------------------------------------------------
inline float edgeRows(const GrayImage& in, std::vector<float>& mag,
                      const Mask& m, int y0, int y1)
{
    const int w = in.width;
    const int h = in.height;
    const int r = m.size / 2;
    const std::uint8_t* src = in.data.data();
    float localMax = 0.0f;

    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < w; ++x) {
            int sx = 0, sy = 0;
            for (int ky = -r; ky <= r; ++ky) {
                const int yy = std::clamp(y + ky, 0, h - 1);
                const std::uint8_t* row = src + static_cast<size_t>(yy) * w;
                const int kRow = (ky + r) * m.size;
                for (int kx = -r; kx <= r; ++kx) {
                    const int xx  = std::clamp(x + kx, 0, w - 1);
                    const int p   = row[xx];
                    const int idx = kRow + (kx + r);
                    sx += m.gx[idx] * p;
                    sy += m.gy[idx] * p;
                }
            }
            const float v = std::sqrt(static_cast<float>(sx) * sx +
                                      static_cast<float>(sy) * sy);
            mag[static_cast<size_t>(y) * w + x] = v;
            if (v > localMax) localMax = v;
        }
    }
    return localMax;
}

// ----------------------------------------------------------------------------
//  Krok 2: normalizacja wierszy [y0, y1) do 0..255 wg globalnego maksimum
// ----------------------------------------------------------------------------
inline void normalizeRows(const std::vector<float>& mag, std::vector<std::uint8_t>& out,
                          int w, float globalMax, int y0, int y1)
{
    const float scale = (globalMax > 0.0f) ? 255.0f / globalMax : 0.0f;
    const size_t begin = static_cast<size_t>(y0) * w;
    const size_t end   = static_cast<size_t>(y1) * w;
    for (size_t i = begin; i < end; ++i) {
        const float v = mag[i] * scale;
        out[i] = static_cast<std::uint8_t>(v > 255.0f ? 255.0f : v + 0.5f);
    }
}

// ----------------------------------------------------------------------------
//  Pomiary czasu CPU
// ----------------------------------------------------------------------------
#if defined(_WIN32)
inline double filetimeToSec(const FILETIME& ft)
{
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<double>(u.QuadPart) * 1e-7;   // jednostka: 100 ns
}
#endif

// Czas CPU calego procesu = suma czasow WSZYSTKICH watkow
inline double cpuTimeSec()
{
#if defined(_WIN32)
    FILETIME creation, exitT, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitT, &kernel, &user))
        return 0.0;
    return filetimeToSec(kernel) + filetimeToSec(user);
#else
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) + ts.tv_nsec * 1e-9;
#endif
}

// Czas CPU zuzyty przez BIEZACY watek
inline double threadCpuTimeSec()
{
#if defined(_WIN32)
    FILETIME creation, exitT, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exitT, &kernel, &user))
        return 0.0;
    return filetimeToSec(kernel) + filetimeToSec(user);
#else
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) + ts.tv_nsec * 1e-9;
#endif
}

// ----------------------------------------------------------------------------
//  Pomiary RAM
// ----------------------------------------------------------------------------
// Aktualnie zajeta pamiec fizyczna procesu (RSS / Working Set) w bajtach
inline size_t currentRamBytes()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return static_cast<size_t>(pmc.WorkingSetSize);
    return 0;
#elif defined(__APPLE__)
    mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return static_cast<size_t>(info.resident_size);
    return 0;
#else
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("VmRSS:", 0) == 0)
            return std::stoull(line.substr(6)) * 1024ULL;   // w pliku: kB
    return 0;
#endif
}

// Szczytowe zuzycie pamieci fizycznej od startu procesu
inline size_t peakRamBytes()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return static_cast<size_t>(pmc.PeakWorkingSetSize);
    return 0;
#elif defined(__APPLE__)
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<size_t>(ru.ru_maxrss);               // macOS: bajty
#else
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("VmHWM:", 0) == 0)
            return std::stoull(line.substr(6)) * 1024ULL;
    return 0;
#endif
}

inline std::string mb(size_t bytes)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << bytes / (1024.0 * 1024.0) << " MB";
    return os.str();
}

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

// ----------------------------------------------------------------------------
//  Wypisywanie tabeli etapow
// ----------------------------------------------------------------------------
inline void printHeader()
{
    std::cout << "\n"
              << std::left  << std::setw(24) << "Etap"
              << std::right << std::setw(14) << "Zegar [ms]"
              << std::setw(14) << "CPU [ms]"
              << std::setw(12) << "CPU/zegar"
              << std::setw(16) << "RAM (teraz)" << "\n"
              << std::string(80, '-') << "\n";
}

// CPU/zegar ~ ile rdzeni srednio pracowalo (1.0 = jeden rdzen, 4.0 = cztery)
inline void printRow(const std::string& name, double wallMs, double cpuMs)
{
    std::cout << std::left  << std::setw(24) << name
              << std::right << std::fixed << std::setprecision(3)
              << std::setw(14) << wallMs
              << std::setw(14) << cpuMs
              << std::setprecision(2)
              << std::setw(12) << (wallMs > 0.0 ? cpuMs / wallMs : 0.0)
              << std::setw(16) << mb(currentRamBytes()) << "\n";
}

inline void printStage(const std::string& name, const Stopwatch& sw)
{
    printRow(name, sw.wallMs(), sw.cpuMs());
}
