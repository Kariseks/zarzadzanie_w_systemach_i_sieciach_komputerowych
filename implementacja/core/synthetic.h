// ============================================================================
//  synthetic.h  –  deterministyczne obrazy testowe
//
//  Ten sam (szerokosc, wysokosc, ziarno) daje zawsze ten sam obraz, na kazdym
//  systemie i kompilatorze (wlasny generator liczb pseudolosowych, bez
//  std::uniform_*_distribution, ktorych wynik zalezy od implementacji STL).
//  Dzieki temu duze obrazy do pomiarow nie musza lezec w repo.
//
//  Zawartosc: lagodny gradient tla + prostokaty i kola o ostrych krawedziach
//  (znane polozenie krawedzi) + szum.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

#include "image.h"

GrayImage makeSynthetic(int width, int height, std::uint32_t seed = 1);

// Parsuje "WxH" albo "WxH:seed" (np. "3840x2160:7").  false gdy format zly.
bool parseSyntheticSpec(const std::string& spec, int& width, int& height, std::uint32_t& seed);

// Wczytuje obraz z pliku albo, gdy 'source' zaczyna sie od "synthetic:",
// generuje go (np. "synthetic:1920x1080:3").
bool loadOrGenerate(const std::string& source, GrayImage& img);
