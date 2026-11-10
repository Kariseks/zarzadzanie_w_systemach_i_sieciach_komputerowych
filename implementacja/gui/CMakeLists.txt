# edge_gui – aplikacja Qt Widgets do prezentacji działania i wyników projektu.
# Wymaga modułów Qt: Widgets, Charts, Concurrent, Svg (ikona).
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_executable(edge_gui
    main.cpp
    engine.h                 engine.cpp
    presets.h                presets.cpp
    wykonawca.h              wykonawca.cpp
    widgets.h                widgets.cpp
    wykresy.h                wykresy.cpp
    strona_przetwarzanie.h   strona_przetwarzanie.cpp
    strona_porownanie.h      strona_porownanie.cpp
    strona_skalowanie.h      strona_skalowanie.cpp
    strona_system.h          strona_system.cpp
    historia.h               historia.cpp
    oknoglowne.h             oknoglowne.cpp
    zasoby/zasoby.qrc
)
target_link_libraries(edge_gui PRIVATE edge_core Qt6::Widgets Qt6::Charts Qt6::Concurrent Qt6::Svg)

# Ścieżek nie wkompilowujemy: obrazy testowe aplikacja znajduje w czasie działania,
# szukając katalogu z obrazy/ i implementacja/ w górę od własnego pliku (folder przenośny).
