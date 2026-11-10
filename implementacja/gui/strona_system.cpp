/**
 * @file strona_system.cpp
 * @brief Implementacja zakładki „System i poprawność” (patrz strona_system.h).
 */
#include "strona_system.h"

#include <QApplication>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMap>
#include <QPainter>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QSysInfo>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include "engine.h"
#include "gpu/gpu_opencl.h"
#include "presets.h"
#include "topology.h"
#include "verify.h"

namespace {
/** Nazwa modelu procesora z /proc/cpuinfo. */
QString modelProcesora()
{
    QFile f("/proc/cpuinfo");
    if (!f.open(QIODevice::ReadOnly)) return QSysInfo::currentCpuArchitecture();
    for (const QByteArray& l : f.readAll().split('\n'))
        if (l.startsWith("model name")) return QString::fromUtf8(l.mid(l.indexOf(':') + 1)).trimmed();
    return QSysInfo::currentCpuArchitecture();
}
} // namespace

// ============================================================================
//  MapaRdzeni
// ============================================================================

MapaRdzeni::MapaRdzeni(QWidget* parent) : QWidget(parent)
{
    setToolTip(QStringLiteral("Bloki rdzeni (CCD) z własną pamięcią L3; w każdym rdzeniu fizycznym dwa wątki sprzętowe (SMT). "
                              "Numery to identyfikatory logicznych CPU używane przy przypinaniu wątków."));
}

QSize MapaRdzeni::sizeHint() const { return { znaki(this, 70), wiersze(this, 8) }; }

void MapaRdzeni::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto& topo = cpuTopology();
    if (topo.empty()) { p.drawText(rect(), Qt::AlignCenter, QStringLiteral("brak danych o topologii")); return; }
    QMap<int, QMap<int, QVector<int>>> ccd;   // l3 → rdzeń → lista CPU
    for (const auto& c : topo) ccd[c.l3][c.core].push_back(c.id);
    const int nccd = ccd.size();
    const double szer = (width() - 10.0 * (nccd + 1)) / nccd;
    int i = 0;
    for (auto it = ccd.cbegin(); it != ccd.cend(); ++it, ++i) {
        const QRectF blok(10 + i * (szer + 10), 4, szer, height() - 8);
        p.setPen(QPen(QColor("#90a4ae"), 1.5));
        p.setBrush(QColor("#eceff1"));
        p.drawRoundedRect(blok, 8, 8);
        p.setPen(QColor("#37474f"));
        QFont f = font();
        f.setBold(true);
        p.setFont(f);
        p.drawText(blok.adjusted(8, 4, -8, 0), Qt::AlignTop | Qt::AlignLeft,
                   QStringLiteral("CCD %1 · L3 wspólne dla %2 rdzeni").arg(i).arg(it->size()));
        p.setFont(font());
        const int nr = it->size();
        const int kol = std::min(nr, 6);
        const int wier = (nr + kol - 1) / kol;
        const double cw = (blok.width() - 16 - (kol - 1) * 6.0) / kol;
        const double ch = (blok.height() - 30 - (wier - 1) * 6.0) / wier;
        int k = 0;
        for (auto r = it->cbegin(); r != it->cend(); ++r, ++k) {
            const QRectF rr(blok.left() + 8 + (k % kol) * (cw + 6), blok.top() + 24 + (k / kol) * (ch + 6), cw, ch);
            p.setPen(QPen(QColor("#546e7a"), 1));
            p.setBrush(Qt::white);
            p.drawRoundedRect(rr, 5, 5);
            const QVector<int>& cpus = r.value();
            for (int j = 0; j < cpus.size(); ++j) {
                const QRectF w(rr.left() + 4, rr.top() + 4 + j * (rr.height() - 8) / cpus.size(), rr.width() - 8,
                               (rr.height() - 8) / cpus.size() - 2);
                p.setPen(Qt::NoPen);
                p.setBrush(j == 0 ? QColor("#D55E00") : QColor("#F0B48C"));
                p.drawRoundedRect(w, 3, 3);
                p.setPen(j == 0 ? Qt::white : Qt::black);
                p.drawText(w, Qt::AlignCenter, QString::number(cpus[j]));
            }
        }
    }
}

// ============================================================================
//  StronaSystem
// ============================================================================

StronaSystem::StronaSystem(QWidget* parent) : QWidget(parent), koniec_(std::make_shared<std::atomic<bool>>(false))
{
    setWhatsThis(QStringLiteral("<b>System i poprawność</b><br>Sprzęt, na którym działa program, warunki pomiaru "
                                "i automatyczna weryfikacja, że wszystkie wersje dają ten sam wynik co W1."));

    // --- sprzęt
    auto* gSprzet = new QGroupBox(QStringLiteral("Sprzęt"));
    gSprzet->setToolTip(QStringLiteral("Komputer, na którym działa aplikacja. Wyniki w sprawozdaniu zmierzono na tym samym sprzęcie."));
    auto* fs = new QFormLayout(gSprzet);
    fs->addRow(QStringLiteral("Procesor:"), new QLabel(modelProcesora()));
    fs->addRow(QStringLiteral("Topologia:"), new QLabel(QString::fromStdString(topologySummary())));
    const auto gpu = gpuDevices();
    QString g = gpu.empty() ? QStringLiteral("brak urządzenia OpenCL – W5 niedostępna")
                            : QStringLiteral("%1 (%2 jednostek obliczeniowych, %3 MB, %4)")
                                  .arg(QString::fromStdString(gpu[0].device)).arg(gpu[0].computeUnits)
                                  .arg(gpu[0].globalMemMB).arg(QString::fromStdString(gpu[0].version));
    auto* lg = new QLabel(g);
    lg->setToolTip(QStringLiteral("Urządzenie OpenCL używane przez W5. Jednostki obliczeniowe = multiprocesory karty."));
    lg->setWordWrap(true);
    fs->addRow(QStringLiteral("Karta graficzna:"), lg);
    fs->addRow(QStringLiteral("System:"), new QLabel(QSysInfo::prettyProductName() + ", jądro " + QSysInfo::kernelVersion()));
    fs->addRow(QStringLiteral("Kompilacja:"), new QLabel(QStringLiteral("GCC %1, Qt %2").arg(__VERSION__, qVersion())));
    auto* mapa = new MapaRdzeni;
    auto* lSprzet = new QVBoxLayout;
    lSprzet->addWidget(gSprzet);
    auto* gMapa = new QGroupBox(QStringLiteral("Rdzenie procesora"));
    gMapa->setToolTip(QStringLiteral("Topologia odczytana z /sys: bloki CCD, rdzenie fizyczne i ich wątki sprzętowe."));
    auto* lm = new QVBoxLayout(gMapa);
    lm->addWidget(mapa);
    auto* legenda = new QLabel(QStringLiteral("■ pierwszy wątek rdzenia   ■ drugi wątek sprzętowy (SMT)"));
    legenda->setObjectName("podpowiedz");
    legenda->setTextFormat(Qt::RichText);
    legenda->setToolTip(QStringLiteral("Drugi wątek sprzętowy (SMT) współdzieli jednostki wykonawcze rdzenia – zwykle daje mały dodatkowy zysk."));
    legenda->setText(QStringLiteral("<span style='color:#D55E00'>■</span> pierwszy wątek rdzenia &nbsp;&nbsp; "
                                    "<span style='color:#F0B48C'>■</span> drugi wątek sprzętowy (SMT)"));
    lm->addWidget(legenda);
    lSprzet->addWidget(gMapa);

    // --- warunki pomiaru
    auto* gWar = new QGroupBox(QStringLiteral("Warunki pomiaru"));
    gWar->setToolTip(QStringLiteral("Czy komputer jest gotowy do rzetelnego pomiaru: stała częstotliwość procesora i brak innych obciążeń."));
    auto* lw = new QVBoxLayout(gWar);
    warunki_ = new QLabel;
    warunki_->setWordWrap(true);
    warunki_->setTextFormat(Qt::RichText);
    warunki_->setToolTip(QStringLiteral("Regulator „performance” utrzymuje najwyższą częstotliwość; zajętość powyżej 5 % zawyża i rozrzuca czasy."));
    auto* sprawdz = new QPushButton(QStringLiteral("Sprawdź warunki"));
    sprawdz->setToolTip(QStringLiteral("Odczytuje regulator częstotliwości i mierzy przez 0,2 s zajętość procesora przez inne programy."));
    lw->addWidget(warunki_);
    lw->addWidget(sprawdz, 0, Qt::AlignLeft);
    lSprzet->addWidget(gWar);
    lSprzet->addStretch();

    // --- poprawność
    auto* gPop = new QGroupBox(QStringLiteral("Poprawność wszystkich wersji"));
    gPop->setToolTip(QStringLiteral("Automatyczne sprawdzenie, że każda wersja daje ten sam obraz co W1."));
    auto* lp = new QVBoxLayout(gPop);
    auto* opis = new QLabel(QStringLiteral("Weryfikacja porównuje wynik W2–W5 z W1 piksel po pikselu: obrazy od 1×1 do zdjęć, "
                                           "obraz mniejszy od maski, mniej wierszy niż wątków, różne liczby wątków, harmonogramy "
                                           "i ustawienia GPU, maski Sobel 3×3 i 7×7, trzy sposoby skalowania."));
    opis->setWordWrap(true);
    opis->setObjectName("podpowiedz");
    opis->setToolTip(QStringLiteral("Przypadki brzegowe wykrywają błędy indeksowania przy brzegach i w podziale wierszy pomiędzy wątki."));
    weryfikuj_ = new QPushButton(QStringLiteral("Sprawdź poprawność"));
    weryfikuj_->setObjectName("glowny");
    weryfikuj_->setToolTip(QStringLiteral("Uruchamia weryfikację (kilkanaście sekund)."));
    postep_ = new QProgressBar;
    postep_->setValue(0);
    postep_->setFormat(QStringLiteral("nie uruchomiono"));
    postep_->setToolTip(QStringLiteral("Liczba sprawdzonych przypadków testowych."));
    wyniki_ = new QTreeWidget;
    wyniki_->setHeaderLabels({ "Wersja", "Przypadki", "Zgodne", "Maks. różnica", "Wynik" });
    wyniki_->setRootIsDecorated(false);
    wyniki_->setToolTip(QStringLiteral("Podsumowanie weryfikacji dla każdej wersji."));
    {
        const QStringList tipy = { QStringLiteral("Wersja programu"), QStringLiteral("Liczba sprawdzonych przypadków (obraz × maska × skalowanie × konfiguracja)"),
                                   QStringLiteral("Przypadki z wynikiem zgodnym z W1"), QStringLiteral("Największa różnica poziomu szarości względem W1 (0 = identyczny)"),
                                   QStringLiteral("Werdykt dla wersji") };
        for (int i = 0; i < tipy.size(); ++i) wyniki_->headerItem()->setToolTip(i, tipy[i]);
    }
    wyniki_->setAlternatingRowColors(true);
    wyniki_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    werdykt_ = new QLabel;
    werdykt_->setObjectName("odznaka");
    werdykt_->setToolTip(QStringLiteral("Wynik całej weryfikacji."));
    lp->addWidget(opis);
    lp->addWidget(weryfikuj_, 0, Qt::AlignLeft);
    lp->addWidget(postep_);
    lp->addWidget(wyniki_, 1);
    lp->addWidget(werdykt_);

    auto* glowny = new QHBoxLayout(this);
    glowny->addLayout(lSprzet, 1);
    glowny->addWidget(gPop, 1);

    connect(sprawdz, &QPushButton::clicked, this, &StronaSystem::sprawdzWarunki);
    connect(weryfikuj_, &QPushButton::clicked, this, &StronaSystem::weryfikuj);
    connect(&Koordynator::instancja(), &Koordynator::zmiana, this,
            [this](bool z) { weryfikuj_->setEnabled(!z); });
    sprawdzWarunki();
}

StronaSystem::~StronaSystem()
{
    koniec_->store(true);
    fut_.waitForFinished();
}

void StronaSystem::sprawdzWarunki()
{
    const WarunkiPomiaru w = odczytajWarunki(true);
    auto wiersz = [](bool ok, const QString& tekst) { return (ok ? QStringLiteral("✔ ") : QStringLiteral("⚠ ")) + tekst; };
    QStringList t;
    if (w.zasilacz >= 0)
        t << wiersz(w.zasilacz == 1, w.zasilacz == 1 ? QStringLiteral("zasilanie: <b>zasilacz</b>")
                                                     : QStringLiteral("zasilanie: <b>bateria</b> – podłącz zasilacz, na baterii procesor ma niższy limit mocy"));
    if (!w.profil.isEmpty())
        t << wiersz(w.profil == QLatin1String("performance"),
                    QStringLiteral("profil zasilania: <b>%1</b>%2").arg(w.profil,
                        w.profil == QLatin1String("performance") ? QString() : QStringLiteral(" – ustaw „performance” (Ustawienia → Zasilanie)")));
    const bool regOk = w.regulator == QLatin1String("performance") || w.preferencja == QLatin1String("performance");
    t << wiersz(regOk, QStringLiteral("regulator częstotliwości: <b>%1</b>%2").arg(w.regulator.isEmpty() ? QStringLiteral("nieznany") : w.regulator,
                    w.preferencja.isEmpty() ? QString() : QStringLiteral(", preferencja energii (EPP): <b>%1</b>").arg(w.preferencja)));
    if (w.zajetosc >= 0)
        t << wiersz(w.zajetosc < 5, QStringLiteral("zajętość procesora przez inne programy: <b>%1%</b>%2").arg(liczba(w.zajetosc, 1),
                    w.zajetosc < 5 ? QString() : QStringLiteral(" – zamknij inne programy, czasy będą zawyżone")));
    const Topologia tp = topologia();
    t << QStringLiteral("Procesor: <b>%1</b> wątków sprzętowych, <b>%2</b> rdzeni fizycznych, <b>%3</b> blok(i) L3")
             .arg(tp.logiczne).arg(tp.fizyczne).arg(tp.bloki);
    t << QStringLiteral("<i>Na laptopie przed pomiarem: zasilacz, profil „performance”, kilka rozgrzewających uruchomień.</i>");
    warunki_->setText(t.join("<br>"));
}

void StronaSystem::weryfikuj()
{
    if (Koordynator::instancja().zajety()) return;
    Koordynator::instancja().ustawZajety(true, QStringLiteral("Weryfikacja poprawności"));
    weryfikuj_->setEnabled(false);
    wyniki_->clear();
    werdykt_->clear();
    postep_->setValue(0);

    QPointer<StronaSystem> ja(this);
    auto flaga = koniec_;
    fut_ = QtConcurrent::run([ja, flaga] {
        VerifyOptions o;
        for (Wersja w : wszystkieWersje()) if (wersjaDostepna(w)) o.variants.push_back(nazwaWersji(w).toStdString());
        o.masks = { "sobel3", "sobel7" };
        o.norms = { Norm::L2 };
        const auto wyn = runVerification(o, [ja, flaga](size_t z, size_t w) {
            if (flaga->load()) return;
            const int proc = static_cast<int>(100.0 * z / std::max<size_t>(1, w));
            QMetaObject::invokeMethod(ja, [ja, proc, z, w] {
                if (!ja) return;
                ja->postep_->setValue(proc);
                ja->postep_->setFormat(QStringLiteral("%1 / %2 przypadków").arg(z).arg(w));
            }, Qt::QueuedConnection);
        });
        // podsumowanie na wersję
        struct Suma { int n = 0, ok = 0, maks = 0; };
        QMap<QString, Suma> sumy;
        for (const auto& c : wyn) {
            Suma& s = sumy[QString::fromStdString(c.variant)];
            ++s.n;
            if (c.pass) ++s.ok;
            s.maks = std::max(s.maks, c.cmp.maxAbsDiff);
        }
        QMetaObject::invokeMethod(ja, [ja, sumy, n = static_cast<int>(wyn.size())] {
            if (!ja) return;
            int bledy = 0;
            for (auto it = sumy.cbegin(); it != sumy.cend(); ++it) {
                const bool ok = it->ok == it->n;
                bledy += it->n - it->ok;
                auto* item = new QTreeWidgetItem({ it.key(), QString::number(it->n), QString::number(it->ok),
                                                   QString::number(it->maks), ok ? QStringLiteral("✔ zgodna") : QStringLiteral("✖ błędy") });
                item->setForeground(4, ok ? QColor("#1b7f3b") : QColor("#b71c1c"));
                for (int c = 1; c < 4; ++c) item->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
                for (int c = 0; c < 5; ++c) item->setToolTip(c, ja->wyniki_->headerItem()->toolTip(c));
                ja->wyniki_->addTopLevelItem(item);
            }
            ja->postep_->setValue(100);
            ja->postep_->setFormat(QStringLiteral("%1 przypadków").arg(n));
            ja->werdykt_->setText(bledy == 0 ? QStringLiteral("✔ Wszystkie wersje dają wynik identyczny z W1 (%1 przypadków)").arg(n)
                                             : QStringLiteral("✖ %1 przypadków niezgodnych").arg(bledy));
            ja->werdykt_->setProperty("stan", bledy == 0 ? "ok" : "blad");
            ja->werdykt_->style()->unpolish(ja->werdykt_);
            ja->werdykt_->style()->polish(ja->werdykt_);
            ja->weryfikuj_->setEnabled(true);
            Koordynator::instancja().ustawZajety(false);
            emit ja->komunikat(QStringLiteral("Weryfikacja zakończona: %1 przypadków, %2 niezgodnych").arg(n).arg(bledy));
        }, Qt::QueuedConnection);
    });
}
