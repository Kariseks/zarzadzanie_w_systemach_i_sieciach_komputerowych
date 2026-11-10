/**
 * @file oknoglowne.cpp
 * @brief Implementacja okna głównego i okna pomocy (patrz oknoglowne.h).
 */
#include "oknoglowne.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStyle>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWhatsThis>

#include "engine.h"
#include "presets.h"
#include "strona_porownanie.h"
#include "strona_przetwarzanie.h"
#include "strona_skalowanie.h"
#include "strona_system.h"
#include "historia.h"
#include <QTimer>

// ============================================================================
//  OknoPomocy
// ============================================================================

OknoPomocy::OknoPomocy(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Pomoc – Wykrywanie krawędzi"));
    resize(860, 720);
    tekst_ = new QTextBrowser;
    tekst_->setOpenExternalLinks(true);
    tekst_->setSource(QUrl(QStringLiteral("qrc:/pomoc/pomoc.html")));
    auto* przyciski = new QDialogButtonBox(QDialogButtonBox::Close);
    auto* wstecz = przyciski->addButton(QStringLiteral("← Wstecz"), QDialogButtonBox::ActionRole);
    auto* spis = przyciski->addButton(QStringLiteral("Spis treści"), QDialogButtonBox::ActionRole);
    connect(wstecz, &QPushButton::clicked, tekst_, &QTextBrowser::backward);
    connect(spis, &QPushButton::clicked, this, [this] { tekst_->scrollToAnchor("spis"); });
    connect(przyciski, &QDialogButtonBox::rejected, this, &QDialog::close);
    auto* l = new QVBoxLayout(this);
    l->addWidget(tekst_);
    l->addWidget(przyciski);
}

void OknoPomocy::pokaz(const QString& kotwica)
{
    show();
    raise();
    activateWindow();
    tekst_->scrollToAnchor(kotwica);
}

// ============================================================================
//  OknoGlowne
// ============================================================================

OknoGlowne::OknoGlowne(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Wykrywanie krawędzi – mechanizmy zarządzania wątkami"));
    setWindowIcon(QIcon(QStringLiteral(":/ikony/ikona.svg")));

    zakladki_ = new QTabWidget;
    zakladki_->setDocumentMode(true);
    przetw_ = new StronaPrzetwarzanie;
    porown_ = new StronaPorownanie;
    skal_ = new StronaSkalowanie;
    system_ = new StronaSystem;
    historia_ = new StronaHistoria;
    zakladki_->addTab(przetw_, QStringLiteral("Przetwarzanie"));
    zakladki_->addTab(porown_, QStringLiteral("Porównanie wersji"));
    zakladki_->addTab(skal_, QStringLiteral("Skalowanie wątków"));
    zakladki_->addTab(historia_, QStringLiteral("Historia pomiarów"));
    zakladki_->addTab(system_, QStringLiteral("System i poprawność"));
    zakladki_->setTabToolTip(0, QStringLiteral("Jedna wersja: obraz wejściowy, wynik i czasy etapów"));
    zakladki_->setTabToolTip(1, QStringLiteral("W1–W5 na tym samym obrazie: sam algorytm i cały program"));
    zakladki_->setTabToolTip(2, QStringLiteral("W4 dla 1…%1 wątków: przyspieszenie S(N), efektywność E(N), prawo Amdahla").arg(topologia().logiczne));
    zakladki_->setTabToolTip(3, QStringLiteral("Wszystkie pomiary tej sesji z parametrami, rozrzutem i warunkami; eksport do CSV"));
    zakladki_->setTabToolTip(4, QStringLiteral("Sprzęt, warunki pomiaru i weryfikacja poprawności"));
    connect(&Koordynator::instancja(), &Koordynator::nowyPomiar, historia_, &StronaHistoria::dodaj);
    connect(historia_, &StronaHistoria::komunikat, this, &OknoGlowne::komunikat);
    setCentralWidget(zakladki_);

    connect(przetw_, &StronaPrzetwarzanie::komunikat, this, &OknoGlowne::komunikat);
    connect(porown_, &StronaPorownanie::komunikat, this, &OknoGlowne::komunikat);
    connect(skal_, &StronaSkalowanie::komunikat, this, &OknoGlowne::komunikat);
    connect(system_, &StronaSystem::komunikat, this, &OknoGlowne::komunikat);

    // --- pasek stanu
    stanRegulatora_ = new QLabel;
    const QString g = regulatorCzestotliwosci();
    stanRegulatora_->setText(g.isEmpty() ? QString() : QStringLiteral("regulator CPU: %1").arg(g));
    stanRegulatora_->setToolTip(QStringLiteral("Regulator częstotliwości procesora. Do pomiarów zalecany „performance”."));
    if (!g.isEmpty() && g != "performance") stanRegulatora_->setStyleSheet("color:#b26a00;");
    stanPomiaru_ = new QLabel(QStringLiteral("● gotowy"));
    stanPomiaru_->setObjectName("stanPomiaru");
    stanPomiaru_->setToolTip(QStringLiteral("Stan pomiaru. W danej chwili trwa najwyżej jeden pomiar – dwa jednocześnie zabierałyby sobie rdzenie."));
    stanWarunkow_ = new QLabel;
    stanWarunkow_->setObjectName("stanWarunkow");
    statusBar()->addPermanentWidget(stanWarunkow_);
    statusBar()->addPermanentWidget(stanRegulatora_);
    odswiezWarunki();
    auto* zegar = new QTimer(this);   // laptop: zasilacz można odłączyć w trakcie pokazu
    connect(zegar, &QTimer::timeout, this, &OknoGlowne::odswiezWarunki);
    zegar->start(15000);
    statusBar()->addPermanentWidget(stanPomiaru_);
    connect(&Koordynator::instancja(), &Koordynator::zmiana, this, [this](bool z, const QString& opis) {
        stanPomiaru_->setText(z ? QStringLiteral("● pomiar: %1").arg(opis) : QStringLiteral("● gotowy"));
        stanPomiaru_->setProperty("zajety", z);
        stanPomiaru_->style()->unpolish(stanPomiaru_);
        stanPomiaru_->style()->polish(stanPomiaru_);
        for (int i = 0; i < zakladki_->count(); ++i) zakladki_->widget(i)->setProperty("zajety", z);
    });

    zbudujMenu();
    pomoc_ = new OknoPomocy(this);

    QSettings s;
    restoreGeometry(s.value("okno/geometria").toByteArray());
    zakladki_->setCurrentIndex(s.value("okno/zakladka", 0).toInt());
    if (!s.contains("okno/geometria")) resize(1500, 920);
    const QString w = ostrzezenieOWarunkach();
    komunikat(w.isEmpty() ? QStringLiteral("Gotowy. F1 – pomoc, Shift+F1 – „Co to jest?” dla dowolnego elementu okna.")
                          : QStringLiteral("Uwaga: %1").arg(w), 15000);
}

void OknoGlowne::zbudujMenu()
{
    // --- Plik
    QMenu* plik = menuBar()->addMenu(QStringLiteral("&Plik"));
    QAction* otworz = plik->addAction(QStringLiteral("&Otwórz obraz…"), this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Otwórz obraz"), katalogRepo() + "/obrazy",
                                                       QStringLiteral("Obrazy (*.png *.jpg *.jpeg *.gif *.bmp)"));
        if (f.isEmpty()) return;
        przetw_->otworzObraz(f);
        zakladki_->setCurrentWidget(przetw_);
    });
    otworz->setShortcut(QKeySequence::Open);
    otworz->setStatusTip(QStringLiteral("Wybierz własny obraz do przetworzenia na zakładce Przetwarzanie"));
    QAction* zapisz = plik->addAction(QStringLiteral("&Zapisz wynik…"), przetw_, &StronaPrzetwarzanie::zapiszWynik);
    zapisz->setShortcut(QKeySequence::Save);
    zapisz->setStatusTip(QStringLiteral("Zapisz obraz krawędzi z ostatniego pomiaru na zakładce Przetwarzanie"));
    QAction* csv = plik->addAction(QStringLiteral("&Eksportuj porównanie do CSV…"), porown_, &StronaPorownanie::eksportujCsv);
    csv->setStatusTip(QStringLiteral("Zapisz tabelę z zakładki Porównanie wersji do pliku CSV"));
    QAction* hist = plik->addAction(QStringLiteral("Eksportuj &historię pomiarów do CSV…"), historia_, &StronaHistoria::eksportujCsv);
    hist->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    hist->setStatusTip(QStringLiteral("Zapisz wszystkie pomiary tej sesji z parametrami i warunkami do pliku CSV"));
    plik->addSeparator();
    QAction* koniec = plik->addAction(QStringLiteral("Za&kończ"), this, &QWidget::close);
    koniec->setShortcut(QKeySequence::Quit);

    // --- Gotowe ustawienia
    QMenu* gotoweMenu = menuBar()->addMenu(QStringLiteral("&Gotowe ustawienia"));
    gotoweMenu->setToolTipsVisible(true);
    const auto& lista = gotoweUstawienia();
    for (int i = 0; i < lista.size(); ++i) {
        QAction* a = gotoweMenu->addAction(lista[i].nazwa, this, [this, i] { zastosujUstawienie(i, false); });
        a->setStatusTip(lista[i].opis);
        a->setToolTip(lista[i].opis);
    }

    // --- Widok
    QMenu* widok = menuBar()->addMenu(QStringLiteral("&Widok"));
    for (int i = 0; i < zakladki_->count(); ++i) {
        QAction* a = widok->addAction(zakladki_->tabText(i), this, [this, i] { zakladki_->setCurrentIndex(i); });
        a->setShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + i)));
    }
    widok->addSeparator();
    QAction* pelny = widok->addAction(QStringLiteral("&Pełny ekran"), this, [this] {
        isFullScreen() ? showNormal() : showFullScreen();
    });
    pelny->setShortcut(QKeySequence::FullScreen);
    pelny->setStatusTip(QStringLiteral("Pełny ekran – wygodne przy rzutniku (F11)"));

    // --- Pomoc
    QMenu* pomoc = menuBar()->addMenu(QStringLiteral("Po&moc"));
    QAction* hp = pomoc->addAction(QStringLiteral("&Pomoc programu"), this, &OknoGlowne::pokazPomoc);
    hp->setShortcut(QKeySequence::HelpContents);
    hp->setStatusTip(QStringLiteral("Opis programu i bieżącej zakładki (F1)"));
    QAction* cto = QWhatsThis::createAction(this);
    cto->setText(QStringLiteral("&Co to jest?"));
    cto->setStatusTip(QStringLiteral("Kliknij potem dowolny element okna, żeby zobaczyć jego opis (Shift+F1)"));
    pomoc->addAction(cto);
    pomoc->addSeparator();
    pomoc->addAction(QStringLiteral("&O programie"), this, &OknoGlowne::oProgramie);
    pomoc->addAction(QStringLiteral("O &Qt"), qApp, &QApplication::aboutQt);

    // --- Pasek narzędzi
    QToolBar* tb = addToolBar(QStringLiteral("Gotowe ustawienia"));
    tb->setObjectName("pasekGotowych");
    tb->setMovable(false);
    auto* etyk = new QLabel(QStringLiteral("  Gotowe ustawienie: "));
    etyk->setToolTip(QStringLiteral("Ustawienia przygotowane do pokazu – każde ilustruje jedno zjawisko z wyników projektu."));
    gotowe_ = new QComboBox;
    gotowe_->setMinimumWidth(znaki(gotowe_, 48));
    gotowe_->setToolTip(QStringLiteral("Wybierz gotowe ustawienie do pokazu – przełącza zakładkę i ustawia obraz, maskę, wersję i parametry."));
    gotowe_->addItem(QStringLiteral("— wybierz —"));
    for (const auto& u : lista) {
        gotowe_->addItem(u.nazwa);
        gotowe_->setItemData(gotowe_->count() - 1, u.opis, Qt::ToolTipRole);
    }
    auto* uruchom = new QToolButton;
    uruchom->setText(QStringLiteral("Zastosuj i uruchom"));
    uruchom->setObjectName("uruchomGotowe");
    uruchom->setToolTip(QStringLiteral("Zastosuj wybrane ustawienie i od razu uruchom pomiar"));
    auto* opis = new QLabel;
    opis->setObjectName("podpowiedz");
    opis->setToolTip(QStringLiteral("Co pokazuje wybrane ustawienie i czego się spodziewać."));
    tb->addWidget(etyk);
    tb->addWidget(gotowe_);
    tb->addWidget(uruchom);
    tb->addSeparator();
    tb->addWidget(opis);
    tb->addSeparator();
    tb->addAction(cto);
    tb->addAction(hp);
    connect(gotowe_, &QComboBox::activated, this, [this, opis](int i) {
        if (i <= 0) { opis->clear(); return; }
        zastosujUstawienie(i - 1, false);
        opis->setText(gotoweUstawienia()[i - 1].opis);
    });
    connect(uruchom, &QToolButton::clicked, this, [this] {
        if (gotowe_->currentIndex() > 0) zastosujUstawienie(gotowe_->currentIndex() - 1, true);
    });
}

void OknoGlowne::zastosujUstawienie(int indeks, bool uruchomOdRazu)
{
    const auto& u = gotoweUstawienia().at(indeks);
    switch (u.zakladka) {
    case Zakladka::Przetwarzanie: przetw_->zastosuj(u); zakladki_->setCurrentWidget(przetw_); if (uruchomOdRazu) przetw_->uruchom(); break;
    case Zakladka::Porownanie:    porown_->zastosuj(u); zakladki_->setCurrentWidget(porown_); if (uruchomOdRazu) porown_->uruchom(); break;
    case Zakladka::Skalowanie:    skal_->zastosuj(u);   zakladki_->setCurrentWidget(skal_);   if (uruchomOdRazu) skal_->uruchom(); break;
    case Zakladka::Historia:      zakladki_->setCurrentWidget(historia_); break;
    case Zakladka::System:        zakladki_->setCurrentWidget(system_); break;
    }
    if (gotowe_->currentIndex() != indeks + 1) gotowe_->setCurrentIndex(indeks + 1);
    komunikat(QStringLiteral("Ustawienie: %1").arg(u.nazwa));
}

void OknoGlowne::pokazPomoc()
{
    static const char* kotwice[] = { "przetwarzanie", "porownanie", "skalowanie", "historia", "system" };
    pomoc_->pokaz(QString::fromLatin1(kotwice[std::clamp(zakladki_->currentIndex(), 0, 4)]));
}

void OknoGlowne::oProgramie()
{
    QMessageBox::about(this, QStringLiteral("O programie"),
        QStringLiteral("<h3>Wykrywanie krawędzi – mechanizmy zarządzania wątkami</h3>"
                       "<p>Aplikacja do prezentacji wyników projektu z przedmiotu <i>Zarządzanie w systemach "
                       "i sieciach komputerowych</i> (Politechnika Wrocławska, W4N, 2026).</p>"
                       "<p>Pięć wersji programu wykrywającego krawędzie operatorami gradientowymi: sekwencyjna (W1), "
                       "z optymalizacją algorytmu (W2), wektorowa AVX2 (W3), wielowątkowa (W4) i na karcie graficznej (W5).</p>"
                       "<p><b>Autorzy:</b> Karol Gąsior, Mateusz Kunicki<br>"
                       "<b>Prowadzący:</b> dr inż. Robert Wójcik</p>"
                       "<p>C++17, Qt %1 (Widgets, Charts, Concurrent), OpenCL, biblioteka stb.</p>").arg(qVersion()));
}

void OknoGlowne::odswiezWarunki()
{
    const WarunkiPomiaru w = odczytajWarunki(false);
    const QStringList o = w.ostrzezenia();
    QString t;
    if (w.zasilacz == 1) t = QStringLiteral("🔌 zasilacz");
    else if (w.zasilacz == 0) t = QStringLiteral("🔋 bateria");
    if (!w.profil.isEmpty()) t += (t.isEmpty() ? QString() : QStringLiteral(" · ")) + QStringLiteral("profil %1").arg(w.profil);
    if (!o.isEmpty()) t = QStringLiteral("⚠ ") + t;
    stanWarunkow_->setText(t);
    stanWarunkow_->setVisible(!t.isEmpty());
    stanWarunkow_->setProperty("zle", !o.isEmpty());
    stanWarunkow_->style()->unpolish(stanWarunkow_);
    stanWarunkow_->style()->polish(stanWarunkow_);
    stanWarunkow_->setToolTip(o.isEmpty() ? QStringLiteral("Warunki pomiaru dobre: %1").arg(w.opis())
                                          : QStringLiteral("<b>Warunki pomiaru:</b><br>⚠ ") + o.join(QStringLiteral("<br>⚠ ")));
}

void OknoGlowne::pokazZakladke(int i) { if (i >= 0 && i < zakladki_->count()) zakladki_->setCurrentIndex(i); }

void OknoGlowne::uruchomGotowe(int i)
{
    if (i >= 0 && i < gotoweUstawienia().size()) zastosujUstawienie(i, true);
}

void OknoGlowne::uruchomWeryfikacje()
{
    zakladki_->setCurrentWidget(system_);
    system_->weryfikuj();
}

void OknoGlowne::komunikat(const QString& t, int ms) { statusBar()->showMessage(t, ms); }

void OknoGlowne::closeEvent(QCloseEvent* e)
{
    QSettings s;
    s.setValue("okno/geometria", saveGeometry());
    s.setValue("okno/zakladka", zakladki_->currentIndex());
    QMainWindow::closeEvent(e);
}
