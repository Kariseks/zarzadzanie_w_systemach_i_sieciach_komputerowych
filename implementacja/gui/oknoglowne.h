/**
 * @file oknoglowne.h
 * @brief Okno główne edge_gui: zakładki, menu, pasek narzędzi z gotowymi ustawieniami, pasek stanu i pomoc.
 */
#pragma once

#include <QDialog>
#include <QMainWindow>

class QComboBox;
class QLabel;
class QTabWidget;
class QTextBrowser;
class StronaPorownanie;
class StronaPrzetwarzanie;
class StronaSkalowanie;
class StronaSystem;
class StronaHistoria;
struct GotoweUstawienie;

/**
 * @brief Okno pomocy: przeglądarka dokumentu HTML z zasobów (:/pomoc/pomoc.html) ze spisem treści.
 *
 * Otwierane klawiszem F1 – od razu na rozdziale dotyczącym bieżącej zakładki.
 */
class OknoPomocy : public QDialog {
    Q_OBJECT
public:
    explicit OknoPomocy(QWidget* parent = nullptr);
    /** @brief Przewija do kotwicy (np. „przetwarzanie”) i pokazuje okno. */
    void pokaz(const QString& kotwica);

private:
    QTextBrowser* tekst_ = nullptr;
};

/**
 * @brief Okno główne aplikacji.
 *
 * Zakładki: Przetwarzanie, Porównanie wersji, Skalowanie wątków, System i poprawność.
 * Gotowe ustawienia (menu i lista na pasku narzędzi) przełączają zakładkę i ustawiają parametry.
 * Ustawienia okna są zapamiętywane między uruchomieniami (QSettings).
 */
class OknoGlowne : public QMainWindow {
    Q_OBJECT
public:
    explicit OknoGlowne(QWidget* parent = nullptr);

    /** @brief Przełącza na zakładkę o podanym numerze (0–4). */
    void pokazZakladke(int i);
    /** @brief Stosuje gotowe ustawienie nr i (od 0) i od razu uruchamia pomiar. */
    void uruchomGotowe(int i);
    /** @brief Przechodzi na zakładkę System i uruchamia weryfikację poprawności. */
    void uruchomWeryfikacje();
    /** @brief Otwiera okno pomocy na rozdziale bieżącej zakładki (F1). */
    void pokazPomoc();

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void zbudujMenu();
    void zastosujUstawienie(int indeks, bool uruchomOdRazu);
    void oProgramie();
    void komunikat(const QString& t, int ms = 6000);

    QTabWidget* zakladki_ = nullptr;
    StronaPrzetwarzanie* przetw_ = nullptr;
    StronaPorownanie* porown_ = nullptr;
    StronaSkalowanie* skal_ = nullptr;
    StronaSystem* system_ = nullptr;
    StronaHistoria* historia_ = nullptr;
    QLabel* stanWarunkow_ = nullptr;
    void odswiezWarunki();
    QComboBox* gotowe_ = nullptr;
    QLabel* stanPomiaru_ = nullptr;
    QLabel* stanRegulatora_ = nullptr;
    OknoPomocy* pomoc_ = nullptr;
};
