#include "measure.h"

#include <iomanip>
#include <sstream>

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

#if defined(_WIN32)
static double filetimeToSec(const FILETIME& ft)
{
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<double>(u.QuadPart) * 1e-7;   // jednostka: 100 ns
}
#endif

double cpuTimeSec()
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

double threadCpuTimeSec()
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

size_t currentRamBytes()
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

size_t peakRamBytes()
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

std::string mb(size_t bytes)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << bytes / (1024.0 * 1024.0) << " MB";
    return os.str();
}
