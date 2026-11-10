/**
 * @file widgets.h
 * @brief Wspólne kontrolki edge_gui: wybór obrazu i maski, parametry wersji,
 *        pasek czasów z podziałem na etapy, podgląd obrazu z powiększaniem.
 */
#pragma once

#include <QComboBox>
#include <QGraphicsView>
#include <QWidget>

#include "engine.h"

class QCheckBox;
class QGraphicsPixmapItem;
class QLabel;
class QSpinBox;
class QStackedWidget;

// ============================================================================
/**
 * @brief Lista rozwijana z obrazami testowymi i pozycją „Inny plik…”.
 *
 * Wybranie „Inny plik…” otwiera okno wyboru pliku; wybrany plik jest dopisywany do listy.
 */
class WyborObrazu : public QComboBox {
    Q_OBJECT
public:
    explicit WyborObrazu(QWidget* parent = nullptr);
    /** @brief Aktualnie wybrane źródło obrazu. */
    ZrodloObrazu zrodlo() const;
    /** @brief Wybiera obraz o podanej ścieżce (dopisuje go, jeśli nie ma go na liście). */
    void ustaw(const QString& sciezka);
    /** @brief Dopisuje plik do listy i go wybiera (np. z menu Plik → Otwórz obraz). */
    void dodajPlik(const QString& sciezka);

private:
    void wybranoIndeks(int i);
    int poprzedni_ = 0;
};

// ============================================================================
/** @brief Lista rozwijana masek (Sobel 3×3/5×5/7×7, Prewitt, Scharr, Roberts) z opisami. */
class WyborMaski : public QComboBox {
    Q_OBJECT
public:
    explicit WyborMaski(QWidget* parent = nullptr);
    /** @brief Identyfikator wybranej maski (np. „sobel3”). */
    QString maska() const { return currentData().toString(); }
    /** @brief Wybiera maskę po identyfikatorze. */
    void ustaw(const QString& id);
};

// ============================================================================
/**
 * @brief Parametry zależne od wersji: dla W4 wątki i harmonogram, dla W5 grupa robocza i precyzja.
 *
 * Dla W1–W3 pokazuje informację, że wersja jest jednowątkowa i nie ma parametrów.
 */
class ParametryWersji : public QWidget {
    Q_OBJECT
public:
    explicit ParametryWersji(QWidget* parent = nullptr);
    /** @brief Pokazuje parametry właściwe dla wersji. */
    void ustawWersje(Wersja w);
    /** @brief Konfiguracja z bieżących ustawień (pola nieużywane przez wersję mają wartości domyślne). */
    RunConfig konfiguracja() const;
    /** @brief Ustawia pola zgodnie z konfiguracją (gotowe ustawienia). */
    void ustaw(const RunConfig& c);

signals:
    /** @brief Wysyłany po każdej zmianie parametru. */
    void zmieniono();

private:
    QStackedWidget* stos_ = nullptr;
    QSpinBox *watki_ = nullptr, *fragment_ = nullptr, *ppw_ = nullptr;
    QComboBox *harmonogram_ = nullptr, *redukcja_ = nullptr, *mechanizm_ = nullptr, *przypisanie_ = nullptr;
    QComboBox *grupa_ = nullptr, *precyzja_ = nullptr;
};

// ============================================================================
/**
 * @brief Poziomy pasek czasu podzielony na etapy (np. wczytanie / algorytm / zapis).
 *
 * Każdy segment ma szerokość proporcjonalną do czasu, a w środku – czas i udział w %
 * (gdy się mieści). Pod paskiem jest legenda, a po najechaniu myszą na segment –
 * podpowiedź z dokładną wartością.
 */
class PasekCzasow : public QWidget {
    Q_OBJECT
public:
    /** @brief Jeden odcinek paska. */
    struct Segment {
        QString nazwa;    ///< np. „wczytanie”
        double  ms = 0;   ///< czas odcinka
        QColor  kolor;
        bool    kreskowanie = false; ///< drugie kodowanie (czytelne w druku czarno-białym)
        QString opis;     ///< dodatkowe wyjaśnienie w podpowiedzi
    };
    explicit PasekCzasow(QWidget* parent = nullptr);
    /** @brief Ustawia odcinki i tytuł (np. „Cały program”); pusta lista czyści pasek. */
    void ustaw(const QString& tytul, const QVector<Segment>& segmenty);
    QSize sizeHint() const override { return { znaki(this, 70), wiersze(this, 5.3) }; }
    QSize minimumSizeHint() const override { return { znaki(this, 40), wiersze(this, 5.3) }; }

protected:
    void paintEvent(QPaintEvent*) override;
    bool event(QEvent* e) override;

private:
    QRectF prostokatSegmentu(int i) const;
    QString tytul_;
    QVector<Segment> seg_;
};

// ============================================================================
/**
 * @brief Podgląd obrazu: kółko myszy powiększa, przeciąganie przesuwa, dwuklik dopasowuje do okna.
 *
 * Dwa podglądy można zsynchronizować (sprzez()), żeby wejście i wynik pokazywały ten sam fragment.
 */
class PodgladObrazu : public QGraphicsView {
    Q_OBJECT
public:
    explicit PodgladObrazu(const QString& tytul, QWidget* parent = nullptr);
    /** @brief Nazwa wartości piksela w podpowiedzi (np. „jasność” albo „moduł gradientu”). */
    void ustawOpisWartosci(const QString& opis) { opisWartosci_ = opis; }
    /** @brief Ustawia obraz (duże obrazy są do podglądu zmniejszane – pomiar dotyczy pełnego rozmiaru). */
    void ustawObraz(const QImage& img);
    /** @brief Dopasowuje cały obraz do okna. */
    void dopasuj();
    /** @brief Łączy powiększenie i przesunięcie dwóch podglądów. */
    static void sprzez(PodgladObrazu* a, PodgladObrazu* b);

signals:
    /** @brief Zmiana powiększenia lub przesunięcia (do synchronizacji). */
    void widokZmieniony();

protected:
    void wheelEvent(QWheelEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    /** @brief Bez wciśniętego przycisku pokazuje współrzędne i wartość piksela pod kursorem. */
    void mouseMoveEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void drawForeground(QPainter* p, const QRectF& rect) override;

private:
    QString tytul_;
    QGraphicsPixmapItem* pix_ = nullptr;
    QSize rozmiarOryginalu_;
    QImage oryginal_;               ///< pełny obraz (do odczytu wartości piksela)
    QString opisWartosci_ = QStringLiteral("jasność");
    bool dopasowany_ = true;
    bool synchronizuje_ = false;
    PodgladObrazu* para_ = nullptr;
};

/** @brief Odcinki „wczytanie / algorytm / zapis” dla pomiaru. */
QVector<PasekCzasow::Segment> segmentyProgramu(const Pomiar& p);
/** @brief Odcinki etapów samego algorytmu (splot, skalowanie, przesył GPU…). */
QVector<PasekCzasow::Segment> segmentyAlgorytmu(const Pomiar& p);
