// ============================================================================
//  row_scheduler.h  –  scheduler dzielacy wiersze obrazu miedzy watki
//
//  Kazdy watek w petli pyta:  while (sched.next(id, y0, y1)) { licz [y0,y1) }
//  Scheduler decyduje, ktory fragment dostanie.  Dostepne strategie:
//
//  STATIC   – obraz dzielony na T rownych blokow, kazdy watek dostaje 1 blok.
//             Zero narzutu, ale jesli jeden watek zwolni (np. system zabierze
//             mu rdzen), reszta na niego czeka.
//  CYCLIC   – fragmenty po 'chunk' wierszy rozdawane po kolei (round-robin):
//             fragment k -> watek k % T.  Podzial staly, ale "przemieszany".
//  DYNAMIC  – wspolna kolejka fragmentow po 'chunk' wierszy; wolny watek
//             bierze nastepny (atomowy licznik).  Najlepsze rownowazenie
//             obciazenia, niewielki narzut na kazdy fragment.
//  GUIDED   – jak DYNAMIC, ale fragmenty maleja: pozostale / (2*T),
//             nie mniej niz 'chunk'.  Duze kawalki na poczatku (maly narzut),
//             male na koncu (dobre domkniecie).
//
//  Nazwy i zachowanie sa wzorowane na klauzuli schedule(...) z OpenMP.
// ============================================================================
#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <string>
#include <vector>

enum class Schedule { Static, Cyclic, Dynamic, Guided };

inline const char* toString(Schedule s)
{
    switch (s) {
    case Schedule::Static:  return "static";
    case Schedule::Cyclic:  return "cyclic";
    case Schedule::Dynamic: return "dynamic";
    case Schedule::Guided:  return "guided";
    }
    return "?";
}

inline bool parseSchedule(std::string s, Schedule& out)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "static"  || s == "1") { out = Schedule::Static;  return true; }
    if (s == "cyclic"  || s == "2") { out = Schedule::Cyclic;  return true; }
    if (s == "dynamic" || s == "3") { out = Schedule::Dynamic; return true; }
    if (s == "guided"  || s == "4") { out = Schedule::Guided;  return true; }
    return false;
}

class RowScheduler {
public:
    RowScheduler(int totalRows, int threads, Schedule mode, int chunkRows)
        : total_(totalRows),
          threads_(std::max(1, threads)),
          chunk_(std::max(1, chunkRows)),
          mode_(mode),
          local_(static_cast<size_t>(threads_))
    {}

    // Wywolywane rownolegle przez watki.  Zwraca false, gdy pracy brak.
    bool next(int tid, int& y0, int& y1)
    {
        switch (mode_) {
        case Schedule::Static:  return nextStatic(tid, y0, y1);
        case Schedule::Cyclic:  return nextCyclic(tid, y0, y1);
        case Schedule::Dynamic: return nextDynamic(y0, y1);
        case Schedule::Guided:  return nextGuided(y0, y1);
        }
        return false;
    }

private:
    // Stan prywatny kazdego watku (tylko on go dotyka -> brak blokad).
    // alignas(64) = osobna linia cache, zeby watki sobie nie przeszkadzaly
    // (false sharing).
    struct alignas(64) PerThread {
        int  round = 0;
        bool done  = false;
    };

    bool nextStatic(int tid, int& y0, int& y1)
    {
        PerThread& me = local_[tid];
        if (me.done) return false;
        me.done = true;
        const int base  = total_ / threads_;
        const int extra = total_ % threads_;     // pierwsze 'extra' watkow +1 wiersz
        y0 = tid * base + std::min(tid, extra);
        y1 = y0 + base + (tid < extra ? 1 : 0);
        return y1 > y0;
    }

    bool nextCyclic(int tid, int& y0, int& y1)
    {
        PerThread& me = local_[tid];
        const long long k = static_cast<long long>(me.round) * threads_ + tid;
        const long long start = k * chunk_;
        if (start >= total_) return false;
        ++me.round;
        y0 = static_cast<int>(start);
        y1 = std::min(total_, y0 + chunk_);
        return true;
    }

    bool nextDynamic(int& y0, int& y1)
    {
        const int start = next_.fetch_add(chunk_, std::memory_order_relaxed);
        if (start >= total_) return false;
        y0 = start;
        y1 = std::min(total_, start + chunk_);
        return true;
    }

    bool nextGuided(int& y0, int& y1)
    {
        int start = next_.load(std::memory_order_relaxed);
        for (;;) {
            if (start >= total_) return false;
            const int remaining = total_ - start;
            int size = (remaining + 2 * threads_ - 1) / (2 * threads_);
            size = std::min(remaining, std::max(chunk_, size));
            // probujemy "zarezerwowac" [start, start+size); jesli inny watek
            // byl szybszy, compare_exchange wpisze nowe 'start' i probujemy dalej
            if (next_.compare_exchange_weak(start, start + size,
                                            std::memory_order_relaxed)) {
                y0 = start;
                y1 = start + size;
                return true;
            }
        }
    }

    const int              total_;
    const int              threads_;
    const int              chunk_;
    const Schedule         mode_;
    std::atomic<int>       next_{0};
    std::vector<PerThread> local_;
};
