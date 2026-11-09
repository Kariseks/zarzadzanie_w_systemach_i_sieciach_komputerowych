#include "filter.h"

#include <cctype>

namespace {
std::string lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

const char* toString(Norm n)
{
    switch (n) {
    case Norm::L2:   return "l2";
    case Norm::L1:   return "l1";
    case Norm::Linf: return "linf";
    }
    return "?";
}

const char* toString(Scaling s)
{
    switch (s) {
    case Scaling::GlobalMax: return "global";
    case Scaling::Fixed:     return "fixed";
    case Scaling::Clamp:     return "clamp";
    }
    return "?";
}

bool parseNorm(const std::string& s, Norm& out)
{
    const std::string v = lower(s);
    if (v == "l2")   { out = Norm::L2;   return true; }
    if (v == "l1")   { out = Norm::L1;   return true; }
    if (v == "linf") { out = Norm::Linf; return true; }
    return false;
}

bool parseScaling(const std::string& s, Scaling& out)
{
    const std::string v = lower(s);
    if (v == "global") { out = Scaling::GlobalMax; return true; }
    if (v == "fixed")  { out = Scaling::Fixed;     return true; }
    if (v == "clamp")  { out = Scaling::Clamp;     return true; }
    return false;
}

float theoreticalMax(const Mask& m, Norm n)
{
    // Najwieksze |Gx| = 255 * suma dodatnich wspolczynnikow (piksele 255 pod
    // dodatnimi, 0 pod ujemnymi).  Dla L2 / L1 to gorne ograniczenie.
    long px = 0, py = 0;
    for (int c : m.gx) if (c > 0) px += c;
    for (int c : m.gy) if (c > 0) py += c;
    const double gx = 255.0 * px, gy = 255.0 * py;
    switch (n) {
    case Norm::L1:   return static_cast<float>(gx + gy);
    case Norm::Linf: return static_cast<float>(std::max(gx, gy));
    case Norm::L2:
    default:         return static_cast<float>(std::sqrt(gx * gx + gy * gy));
    }
}

float directScale(const Mask& m, Norm n, Scaling s)
{
    if (s == Scaling::Clamp) return 1.0f;
    const float t = theoreticalMax(m, n);
    return t > 0.0f ? 255.0f / t : 0.0f;
}
