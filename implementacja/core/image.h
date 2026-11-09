// ============================================================================
//  image.h  –  obraz w skali szarosci + wczytywanie / zapis (biblioteka stb)
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Obraz w skali szarosci (1 bajt na piksel), wiersze ulozone jeden po drugim
struct GrayImage {
    int width  = 0;
    int height = 0;
    std::vector<std::uint8_t> data;   // width * height
};

// Wczytuje dowolny format obslugiwany przez stb (JPG, PNG, GIF, BMP, ...)
// i od razu konwertuje do skali szarosci.  Blad -> komunikat na stderr, false.
bool loadGray(const std::string& path, GrayImage& img);

// Zapis JPG (quality 1..100) i PNG (bezstratnie)
bool saveGrayJpg(const std::string& path, const std::vector<std::uint8_t>& data,
                 int w, int h, int quality = 95);
bool saveGrayPng(const std::string& path, const std::vector<std::uint8_t>& data,
                 int w, int h);
