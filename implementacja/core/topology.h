// ============================================================================
//  topology.h  –  topologia procesora i przypinanie watkow do rdzeni
//
//  Ryzen 9 9900X: 12 rdzeni / 24 watki logiczne, 2 CCD po 6 rdzeni, kazdy
//  CCD z wlasnym L3 32 MB.  Kolejnosc przydzialu watkow:
//    none    – system operacyjny decyduje (bez przypinania)
//    compact – najpierw wszystkie watki logiczne jednego CCD (rdzen + jego
//              brat SMT obok siebie), potem drugi CCD: wspolny L3, ale SMT
//              od 2. watku
//    spread  – rdzenie fizyczne naprzemiennie z obu CCD, bracia SMT dopiero
//              po wyczerpaniu rdzeni: maks. L3 i przepustowosc, brak SMT do 12
//    nosmt   – tylko pierwszy watek logiczny kazdego rdzenia (CCD po kolei);
//              wiecej watkow niz rdzeni -> zawijanie (2 watki na rdzen)
//  Odczyt z /sys/devices/system/cpu (Linux).  Na innych systemach lista
//  jest pusta i przypinanie nie dziala (affinity = none).
// ============================================================================
#pragma once

#include <string>
#include <vector>

enum class Affinity { None, Compact, Spread, NoSmt };

const char* toString(Affinity a);
bool parseAffinity(const std::string& s, Affinity& out);

struct LogicalCpu {
    int id;
    int core;      // identyfikator rdzenia fizycznego (unikalny w systemie)
    int sibling;   // 0 = pierwszy watek rdzenia, 1 = brat SMT
    int l3;        // grupa L3 (CCD)
};

const std::vector<LogicalCpu>& cpuTopology();

// Kolejnosc logicznych CPU dla danej strategii (pusta dla None / braku danych)
std::vector<int> cpuOrder(Affinity a);

// Przypina biezacy watek do jednego logicznego CPU; false gdy sie nie udalo
bool pinCurrentThread(int cpu);

// Opis topologii do meta.json, np. "24 CPU, 12 rdzeni, 2 grupy L3"
std::string topologySummary();
