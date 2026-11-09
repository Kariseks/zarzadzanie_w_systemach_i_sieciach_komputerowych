// ============================================================================
//  gpu_opencl.h  –  wariant GPU (OpenCL 1.2)
//
//  Trzy kernele splotu (drabinka jak na CPU, kazdy dodaje jedna zmiane):
//    GPU   – naiwny: kazdy work-item (1 piksel) czyta K x K pikseli
//            z pamieci globalnej, maska z bufora __constant, K w czasie wykonania,
//    GPUC  – maska wkompilowana w program (-D), petle rozwijane (jak V2),
//    GPUL  – GPUC + kafel 16x16 z obrzezem R wczytywany raz do pamieci
//            lokalnej (__local), splot z pamieci lokalnej.
//  Maksimum: redukcja w grupie roboczej w pamieci lokalnej + jeden
//  atomic_max na grupe (bity dodatniego float porownuja sie jak uint).
//  Normalizacja na GPU, skala liczona na hoscie po odczycie maksimum.
//
//  Czasy etapow (eventy OpenCL z profilowaniem): upload, kernel (splot +
//  normalizacja), download.  Inicjalizacja (platforma, kontekst, kompilacja
//  programu) i alokacja buforow odbywaja sie poza pomiarem.
//
//  Dokladnosc: L2 liczone w double, gdy urzadzenie ma cl_khr_fp64 (GTX 1050 Ti
//  ma) – wtedy wynik identyczny z V0.  Bez fp64 liczone w float z
//  -cl-fp32-correctly-rounded-divide-sqrt (dokladne dla malych masek,
//  dla wiekszych roznica do 1 poziomu szarosci).
//
//  Budowane tylko, gdy CMake znajdzie OpenCL (EDGE_HAVE_OPENCL).
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "../variants.h"

bool gpuCompiledIn();

// Opis urzadzen OpenCL (do --list-devices); pusty, gdy brak
struct GpuDeviceInfo {
    std::string platform, device, version, driver;
    bool        fp64 = false;
    long long   globalMemMB = 0;
    int         computeUnits = 0;
};
std::vector<GpuDeviceInfo> gpuDevices();

enum class GpuKernel { Naive, Static, Local };

// Przygotowanie (kontekst, kompilacja programow, bufory) – wolane przed pomiarem
void gpuPrepare(const GrayImage& img, const Mask& mask);

// Wykonanie; rzuca std::runtime_error przy bledzie OpenCL / braku urzadzenia
void runGpu(GpuKernel kind, const GrayImage& img, const Mask& mask, const RunConfig& cfg, RunResult& r);
