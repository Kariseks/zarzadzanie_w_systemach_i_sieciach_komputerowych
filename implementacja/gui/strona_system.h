/**
 * @file strona_system.h
 * @brief Zakładka „System i poprawność”: sprzęt, warunki pomiaru i weryfikacja wszystkich wersji względem W1.
 */
#pragma once

#include <QFuture>
#include <QWidget>

#include <atomic>
#include <memory>

class QLabel;
class QProgressBar;
class QPushButton;
class QTreeWidget;

/**
 * @brief Graficzna mapa rdzeni procesora: bloki CCD, rdzenie fizyczne i ich wątki sprzętowe (SMT).
 *
 * Rysowana na podstawie topologii odczytanej z /sys (topology.h).
 */
class MapaRdzeni : public QWidget {
    Q_OBJECT
public:
    explicit MapaRdzeni(QWidget* parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;
};

/**
 * @brief Zakładka z informacjami o sprzęcie, warunkach pomiaru i przyciskiem weryfikacji poprawności.
 *
 * Weryfikacja porównuje wynik W2–W5 z W1 piksel po pikselu na zestawie obrazów z przypadkami
 * brzegowymi (1×1, jedna kolumna, obraz mniejszy od maski…), dla wielu konfiguracji wątków i GPU.
 */
class StronaSystem : public QWidget {
    Q_OBJECT
public:
    explicit StronaSystem(QWidget* parent = nullptr);
    ~StronaSystem() override;
    /** @brief Uruchamia weryfikację poprawności (W1–W5, maski Sobel 3×3 i 7×7). */
    void weryfikuj();
    /** @brief Odświeża informacje o warunkach pomiaru (regulator, zajętość CPU). */
    void sprawdzWarunki();

signals:
    /** @brief Komunikat do paska stanu. */
    void komunikat(const QString& tekst, int ms = 6000);

private:
    QLabel* warunki_ = nullptr;
    QPushButton* weryfikuj_ = nullptr;
    QProgressBar* postep_ = nullptr;
    QTreeWidget* wyniki_ = nullptr;
    QLabel* werdykt_ = nullptr;
    QFuture<void> fut_;
    std::shared_ptr<std::atomic<bool>> koniec_;
};
