/**
 * @file engine.h
 * @brief Warstwa pomiarowa aplikacji edge_gui – jedyne miejsce, które woła bibliotekę edge_core.
 *
 * Pomiar przebiega tak samo jak w edge_bench (tryb `include_io`):
 * w każdym powtórzeniu osobno mierzone jest **wczytanie** obrazu z pliku,
 * **sam algorytm** (`VariantRunner::run`, od obrazu w pamięci do gotowego obrazu 0–255)
 * i **zapis** wyniku do pliku JPG. Wynikiem jest mediana każdego etapu z wszystkich powtórzeń.
 *
 * Pomiary wykonuje wątek roboczy (QtConcurrent), a interfejs w tym czasie nie odświeża
 * wykresów – rysowanie zabierałoby rdzenie mierzonej wersji i zaburzało czasy.
 */
#pragma once

#include <QDateTime>
#include <QImage>
#include <QWidget>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <map>
#include <string>

#include "variants.h"

/** @brief Pięć wersji programu opisanych w sprawozdaniu (W1–W5). */
enum class Wersja { W1, W2, W3, W4, W5 };

/** @brief Wszystkie wersje w kolejności W1…W5. */
QVector<Wersja> wszystkieWersje();
/** @brief Identyfikator wariantu w edge_core odpowiadający wersji. */
VariantId idWariantu(Wersja w);
/** @brief Krótka nazwa, np. „W4”. */
QString nazwaWersji(Wersja w);
/** @brief Opis rodzaju przyspieszenia, np. „wątki CPU”. */
QString opisWersji(Wersja w);
/** @brief Dłuższy opis do podpowiedzi (tooltip / „Co to jest?”). */
QString pomocWersji(Wersja w);
/** @brief Stały kolor wersji (paleta Okabe–Ito, ta sama co na rysunkach w sprawozdaniu). */
QColor kolorWersji(Wersja w);
/** @brief Czy wersja jest dostępna na tym komputerze (W5 wymaga OpenCL). */
bool wersjaDostepna(Wersja w, QString* powod = nullptr);

/**
 * @brief Źródło obrazu: plik na dysku albo obraz syntetyczny generowany w pamięci.
 *
 * Dla obrazu syntetycznego etap „wczytanie” to czas wygenerowania obrazu,
 * a nie odczytu z dysku – interfejs podpisuje go wtedy jako „generowanie”.
 */
struct ZrodloObrazu {
    QString sciezka;      ///< ścieżka do pliku albo opis `synthetic:SZERxWYS[:ziarno]`
    QString etykieta;     ///< nazwa wyświetlana w interfejsie
    /** @brief Czy obraz jest generowany, a nie wczytywany z pliku. */
    bool syntetyczny() const { return sciezka.startsWith("synthetic:"); }
};

/** @brief Ustawienia jednego pomiaru. */
struct Zlecenie {
    ZrodloObrazu zrodlo;
    Wersja       wersja = Wersja::W1;
    QString      maska  = "sobel3";   ///< identyfikator maski z masks.h
    RunConfig    cfg;                 ///< parametry W4 (wątki, harmonogram…) i W5 (grupa robocza…)
    int          rozgrzewka   = 1;    ///< wywołania bez pomiaru przed powtórzeniami
    int          powtorzenia  = 5;    ///< liczba mierzonych powtórzeń
    bool         zWczytaniemIZapisem = true; ///< mierzyć też wczytanie i zapis (cały program)
    bool         porownajZW1 = true;  ///< sprawdzić zgodność wyniku z W1
};

/** @brief Mediana, minimum, maksimum i współczynnik zmienności serii czasów [ms]. */
struct Statystyka {
    double mediana = 0, min = 0, max = 0;
    double cv = 0;   ///< współczynnik zmienności = odchylenie standardowe / średnia (0,05 = 5 %)
    /** @brief Czy pomiar jest niestabilny (CV > 5 % – próg ze skilla prezentacja-wynikow). */
    bool niestabilny() const { return cv > 0.05; }
    /** @brief Liczy statystykę z serii (pustej – same zera). */
    static Statystyka z(std::vector<double> v);
};

/**
 * @brief Warunki pomiaru odczytane z systemu (Linux: /sys, /proc).
 *
 * Na laptopie zasilanie z baterii albo profil oszczędzania energii potrafią wydłużyć czasy
 * nawet dwukrotnie, dlatego warunki są zapisywane razem z każdym wynikiem (historia pomiarów).
 */
struct WarunkiPomiaru {
    QString regulator;      ///< scaling_governor, np. „performance”, „powersave”
    QString preferencja;    ///< energy_performance_preference (amd-pstate-epp / intel_pstate), np. „performance”
    QString profil;         ///< /sys/firmware/acpi/platform_profile (laptopy), np. „performance”, „low-power”
    int     zasilacz = -1;  ///< 1 = zasilacz podłączony, 0 = bateria, −1 = komputer stacjonarny / nieznane
    double  zajetosc = -1;  ///< zajętość CPU przez inne programy [%] tuż przed pomiarem (−1 = nie mierzono)
    /** @brief Krótki opis do tabeli, np. „zasilacz, performance, CPU 1 %”. */
    QString opis() const;
    /** @brief Lista ostrzeżeń (pusta, gdy warunki dobre). */
    QStringList ostrzezenia() const;
};

/** @brief Odczytuje warunki pomiaru; z `mierzZajetosc` dodatkowo mierzy zajętość CPU (ok. 0,2 s). */
WarunkiPomiaru odczytajWarunki(bool mierzZajetosc);

/**
 * @brief Topologia procesora potrzebna w interfejsie (linie odniesienia, domyślna liczba wątków).
 *
 * Wyliczana z /sys – na PC (2 bloki CCD po 6 rdzeni, SMT) i na laptopie (1 blok 8 rdzeni) wychodzą inne liczby.
 */
struct Topologia {
    int logiczne = 1;      ///< wątki sprzętowe (CPU logiczne)
    int fizyczne = 1;      ///< rdzenie fizyczne
    int bloki = 1;         ///< grupy rdzeni ze wspólnym L3 (CCD / CCX)
    int rdzeniWBloku = 1;  ///< rdzenie fizyczne w największym bloku
};
/** @brief Topologia bieżącego procesora. */
Topologia topologia();

/** @brief Wynik jednego pomiaru. */
struct Pomiar {
    bool        ok = false;
    QString     blad;                 ///< opis błędu, gdy ok == false
    Zlecenie    zlecenie;
    int         szer = 0, wys = 0;
    Statystyka  wczytanie;            ///< odczyt i dekodowanie pliku (albo generowanie obrazu)
    Statystyka  algorytm;             ///< sam algorytm (VariantRunner::run)
    Statystyka  zapis;                ///< zapis wyniku do JPG
    Statystyka  calosc;               ///< wczytanie + algorytm + zapis w jednym powtórzeniu
    std::map<std::string, double> etapy; ///< mediany etapów algorytmu: conv, norm, upload, kernel, download
    double      pozostaleMs = 0;      ///< czas algorytmu poza splotem i skalowaniem (przydział buforów, synchronizacja)
    int         watki = 1;            ///< liczba wątków użyta przez wersję
    long long   watkiGpu = 0;         ///< W5: liczba uruchomionych work-items
    double      nierownowagaMs = 0;   ///< W4: różnica czasu zakończenia pracy przez wątki (mediana)
    QVector<double> czasyAlgorytmu;   ///< wszystkie powtórzenia algorytmu [ms]
    QImage      wejscie;              ///< obraz wejściowy (skala szarości)
    QImage      wynik;                ///< obraz krawędzi 0–255
    bool        porownano = false;    ///< czy porównano z W1
    bool        identyczny = false;   ///< wynik identyczny co do bajtu z W1
    int         maksRoznica = 0;      ///< największa różnica poziomu szarości względem W1
    double      czasW1Ms = 0;         ///< mediana czasu algorytmu W1 dla tych samych danych (0 = nieznany)
    WarunkiPomiaru warunki;           ///< warunki w chwili pomiaru
    QString     opisSerii;            ///< skąd pochodzi pomiar (zakładka / seria), do historii
    QDateTime   kiedy;                ///< chwila zakończenia pomiaru
};

/** @brief Postęp pomiaru: (wykonane kroki, wszystkie kroki, opis). */
using Postep = std::function<void(int, int, const QString&)>;

/**
 * @brief Wykonuje pomiar w bieżącym wątku (wołane z QtConcurrent::run).
 * @param z        ustawienia pomiaru
 * @param przerwij flaga ustawiana przez interfejs („Przerwij”) – sprawdzana między powtórzeniami
 * @param postep   wywoływane po każdym kroku (może być puste)
 * @return wynik; przy błędzie lub przerwaniu `ok == false` i opis w `blad`
 */
Pomiar zmierz(const Zlecenie& z, const std::atomic<bool>& przerwij, const Postep& postep = {});

/** @brief Zamienia obraz z edge_core na QImage (kopia danych, format Grayscale8). */
QImage doQImage(const std::vector<std::uint8_t>& dane, int szer, int wys);

/** @brief Katalog główny repozytorium (do obrazów testowych): szukany w górę od katalogu programu, więc folder projektu jest przenośny. */
QString katalogRepo();

/** @brief Predefiniowane obrazy testowe (Kodak, HD, 4K, 8K, syntetyczne…), tylko istniejące pliki. */
QVector<ZrodloObrazu> obrazyTestowe();

/**
 * @brief Informacja o warunkach pomiaru: regulator częstotliwości i zajętość CPU.
 * @return pusty tekst, gdy warunki są dobre; inaczej ostrzeżenie do paska stanu
 */
QString ostrzezenieOWarunkach();
/** @brief Bieżący regulator częstotliwości procesora (np. „performance”), pusty gdy nieznany. */
QString regulatorCzestotliwosci();
/** @brief Zajętość CPU przez inne procesy [%] mierzona przez ok. 200 ms z /proc/stat (−1 gdy nieznana). */
double zajetoscCpu();

/**
 * @brief Szerokość w pikselach odpowiadająca n średnim znakom czcionki widżetu.
 *
 * Rozmiary paneli liczone ze znaków skalują się z czcionką i ekranem HiDPI (laptop 2560×1600 przy 150–200 %),
 * zamiast stałych pikseli, które na małym ekranie logicznym zabierają za dużo miejsca.
 */
int znaki(const QWidget* w, double n);
/** @brief Wysokość w pikselach odpowiadająca n wierszom tekstu. */
int wiersze(const QWidget* w, double n);

/** @brief Tekst czasu po polsku: „8,49 ms”, „1,23 s”. */
QString tekstCzasu(double ms);
/** @brief Liczba po polsku z przecinkiem dziesiętnym i automatyczną precyzją. */
QString liczba(double v, int miejsca = -1);
