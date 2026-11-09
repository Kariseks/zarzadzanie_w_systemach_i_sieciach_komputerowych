cmake_minimum_required(VERSION 3.16)
project(EdgeDetection LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Do pomiarow czasu zawsze kompiluj w Release (Debug jest kilka razy wolniejszy)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Wszystkie programy laduja w glownym katalogu budowania
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_package(Threads REQUIRED)

add_subdirectory(core)   # edge_core – biblioteka: warianty, scheduler, pula, pomiary
add_subdirectory(cli)    # edge_cli  – program terminalowy do recznych prob

# edge_bench wymaga Qt 6 (tylko Core).  Domyslnie szukamy w ~/Qt/6.12.0/gcc_64;
# inna instalacja: -DCMAKE_PREFIX_PATH=/sciezka/do/qt
if(EXISTS "$ENV{HOME}/Qt/6.12.0/gcc_64")
    list(APPEND CMAKE_PREFIX_PATH "$ENV{HOME}/Qt/6.12.0/gcc_64")
endif()
find_package(Qt6 COMPONENTS Core QUIET)
if(Qt6_FOUND)
    add_subdirectory(bench)  # edge_bench – pomiary sterowane plikami eksperymentow
else()
    message(WARNING "Nie znaleziono Qt6 Core – edge_bench nie zostanie zbudowany")
endif()

# Testy poprawnosci: ctest --test-dir build   (szybki zestaw domyslny)
enable_testing()
# Pelna weryfikacja (wszystkie jadra, ok. 90 s): edge_cli --verify
add_test(NAME weryfikacja_wariantow COMMAND edge_cli --verify -k V0,V6)

# Skopiuj input.jpg do katalogu budowania (tam Qt Creator uruchamia program)
if(EXISTS "${CMAKE_SOURCE_DIR}/input.jpg")
    configure_file("${CMAKE_SOURCE_DIR}/input.jpg"
                   "${CMAKE_BINARY_DIR}/input.jpg" COPYONLY)
endif()
