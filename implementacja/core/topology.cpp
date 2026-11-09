#include "topology.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <tuple>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace {

std::string readFile(const std::string& path)
{
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}

// "0-5,12-17" -> pierwszy numer (identyfikator grupy)
int firstOfList(const std::string& list)
{
    if (list.empty()) return -1;
    return std::atoi(list.c_str());
}

std::vector<LogicalCpu> readTopology()
{
    std::vector<LogicalCpu> cpus;
#if defined(__linux__)
    for (int id = 0; id < 1024; ++id) {
        const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(id) + "/";
        const std::string core = readFile(base + "topology/core_id");
        if (core.empty()) {
            if (!std::ifstream(base + "uevent")) break;   // nie ma takiego CPU – koniec listy
            continue;                                      // CPU offline
        }
        const std::string pkg  = readFile(base + "topology/physical_package_id");
        const std::string sibs = readFile(base + "topology/thread_siblings_list");
        const std::string l3   = readFile(base + "cache/index3/shared_cpu_list");
        LogicalCpu c;
        c.id      = id;
        c.core    = std::atoi(pkg.c_str()) * 10000 + std::atoi(core.c_str());
        c.sibling = firstOfList(sibs) == id ? 0 : 1;
        c.l3      = firstOfList(l3);
        cpus.push_back(c);
    }
#endif
    return cpus;
}

} // namespace

const char* toString(Affinity a)
{
    switch (a) {
    case Affinity::None:    return "none";
    case Affinity::Compact: return "compact";
    case Affinity::Spread:  return "spread";
    case Affinity::NoSmt:   return "nosmt";
    }
    return "?";
}

bool parseAffinity(const std::string& s, Affinity& out)
{
    std::string v = s;
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "none" || v == "os") { out = Affinity::None;    return true; }
    if (v == "compact")           { out = Affinity::Compact; return true; }
    if (v == "spread")            { out = Affinity::Spread;  return true; }
    if (v == "nosmt")             { out = Affinity::NoSmt;   return true; }
    return false;
}

const std::vector<LogicalCpu>& cpuTopology()
{
    static const std::vector<LogicalCpu> t = readTopology();
    return t;
}

std::vector<int> cpuOrder(Affinity a)
{
    std::vector<LogicalCpu> c = cpuTopology();
    std::vector<int> order;
    if (a == Affinity::None || c.empty()) return order;

    if (a == Affinity::Compact) {
        std::sort(c.begin(), c.end(), [](const LogicalCpu& x, const LogicalCpu& y) {
            return std::tie(x.l3, x.core, x.sibling) < std::tie(y.l3, y.core, y.sibling);
        });
        for (const auto& x : c) order.push_back(x.id);
        return order;
    }

    // grupy L3 -> lista rdzeni (posortowana), osobno dla sibling 0 i 1
    std::map<int, std::vector<LogicalCpu>> byL3[2];
    for (const auto& x : c) byL3[std::min(x.sibling, 1)][x.l3].push_back(x);
    for (auto& level : byL3)
        for (auto& [l3, v] : level)
            std::sort(v.begin(), v.end(), [](const LogicalCpu& x, const LogicalCpu& y) { return x.core < y.core; });

    auto interleave = [&](int sib) {
        std::vector<std::vector<LogicalCpu>*> groups;
        for (auto& [l3, v] : byL3[sib]) groups.push_back(&v);
        for (size_t i = 0;; ++i) {
            bool any = false;
            for (auto* g : groups)
                if (i < g->size()) { order.push_back((*g)[i].id); any = true; }
            if (!any) break;
        }
    };
    auto sequential = [&](int sib) {
        for (auto& [l3, v] : byL3[sib])
            for (const auto& x : v) order.push_back(x.id);
    };

    if (a == Affinity::Spread) { interleave(0); interleave(1); }
    else                       { sequential(0); }               // NoSmt
    return order;
}

bool pinCurrentThread(int cpu)
{
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)cpu;
    return false;
#endif
}

std::string topologySummary()
{
    const auto& c = cpuTopology();
    std::set<int> cores, l3;
    for (const auto& x : c) { cores.insert(x.core); l3.insert(x.l3); }
    std::ostringstream os;
    os << c.size() << " CPU, " << cores.size() << " rdzeni, " << l3.size() << " grupy L3";
    return os.str();
}
