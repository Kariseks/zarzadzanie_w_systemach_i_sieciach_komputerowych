#include "image.h"

#include <iostream>

#include "stb_image.h"
#include "stb_image_write.h"

bool loadGray(const std::string& path, GrayImage& img)
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

bool saveGrayJpg(const std::string& path, const std::vector<std::uint8_t>& data,
                 int w, int h, int quality)
{
    return stbi_write_jpg(path.c_str(), w, h, 1, data.data(), quality) != 0;
}

bool saveGrayPng(const std::string& path, const std::vector<std::uint8_t>& data,
                 int w, int h)
{
    return stbi_write_png(path.c_str(), w, h, 1, data.data(), w) != 0;
}
