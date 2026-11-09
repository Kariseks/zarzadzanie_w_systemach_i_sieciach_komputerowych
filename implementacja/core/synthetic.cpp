#include "synthetic.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {

// xorshift32 – prosty, w pelni deterministyczny generator
struct Rng {
    std::uint32_t s;
    explicit Rng(std::uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    std::uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int range(int lo, int hi)   // [lo, hi]
    {
        if (hi <= lo) return lo;
        return lo + static_cast<int>(next() % static_cast<std::uint32_t>(hi - lo + 1));
    }
};

} // namespace

GrayImage makeSynthetic(int width, int height, std::uint32_t seed)
{
    GrayImage img;
    img.width  = std::max(1, width);
    img.height = std::max(1, height);
    const int w = img.width, h = img.height;
    img.data.assign(static_cast<size_t>(w) * h, 0);

    // 1. Tlo: gradient po przekatnej 40..120
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const long long num = 80LL * (static_cast<long long>(x) * h + static_cast<long long>(y) * w);
            const long long den = 2LL * w * h;
            img.data[static_cast<size_t>(y) * w + x] = static_cast<std::uint8_t>(40 + num / den);
        }

    Rng rng(seed);

    // 2. Prostokaty (ostre krawedzie poziome i pionowe)
    const int shapes = 4 + static_cast<int>((static_cast<long long>(w) * h) / 200000);
    for (int i = 0; i < std::min(shapes, 60); ++i) {
        const int x0 = rng.range(0, w - 1), y0 = rng.range(0, h - 1);
        const int x1 = std::min(w, x0 + rng.range(1, std::max(1, w / 4)));
        const int y1 = std::min(h, y0 + rng.range(1, std::max(1, h / 4)));
        const auto v = static_cast<std::uint8_t>(rng.range(0, 255));
        for (int y = y0; y < y1; ++y)
            std::fill(img.data.begin() + static_cast<long>(y) * w + x0,
                      img.data.begin() + static_cast<long>(y) * w + x1, v);
    }

    // 3. Kola (krawedzie we wszystkich kierunkach)
    for (int i = 0; i < std::min(shapes, 60); ++i) {
        const int cx = rng.range(0, w - 1), cy = rng.range(0, h - 1);
        const int r  = rng.range(1, std::max(1, std::min(w, h) / 6));
        const auto v = static_cast<std::uint8_t>(rng.range(0, 255));
        const int ya = std::max(0, cy - r), yb = std::min(h - 1, cy + r);
        const int xa = std::max(0, cx - r), xb = std::min(w - 1, cx + r);
        for (int y = ya; y <= yb; ++y)
            for (int x = xa; x <= xb; ++x) {
                const long long dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy <= static_cast<long long>(r) * r)
                    img.data[static_cast<size_t>(y) * w + x] = v;
            }
    }

    // 4. Szum +-8 poziomow
    for (auto& p : img.data) {
        const int v = p + static_cast<int>(rng.next() % 17) - 8;
        p = static_cast<std::uint8_t>(std::clamp(v, 0, 255));
    }
    return img;
}

bool parseSyntheticSpec(const std::string& spec, int& width, int& height, std::uint32_t& seed)
{
    char* end = nullptr;
    const long w = std::strtol(spec.c_str(), &end, 10);
    if (!end || (*end != 'x' && *end != 'X')) return false;
    const char* p = end + 1;
    const long h = std::strtol(p, &end, 10);
    if (end == p) return false;
    long s = 1;
    if (*end == ':') {
        p = end + 1;
        s = std::strtol(p, &end, 10);
        if (end == p) return false;
    }
    if (*end != '\0' || w < 1 || h < 1 || w > 100000 || h > 100000) return false;
    width  = static_cast<int>(w);
    height = static_cast<int>(h);
    seed   = static_cast<std::uint32_t>(s);
    return true;
}

bool loadOrGenerate(const std::string& source, GrayImage& img)
{
    const std::string prefix = "synthetic:";
    if (source.rfind(prefix, 0) == 0) {
        int w = 0, h = 0;
        std::uint32_t seed = 1;
        if (!parseSyntheticSpec(source.substr(prefix.size()), w, h, seed)) {
            std::cerr << "Blad: zly opis obrazu syntetycznego '" << source
                      << "' (oczekiwano synthetic:WxH albo synthetic:WxH:ziarno)\n";
            return false;
        }
        img = makeSynthetic(w, h, seed);
        return true;
    }
    return loadGray(source, img);
}
