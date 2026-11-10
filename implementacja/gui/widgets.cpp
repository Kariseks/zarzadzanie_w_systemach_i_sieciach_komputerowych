/**
 * @file widgets.cpp
 * @brief Implementacja wspólnych kontrolek edge_gui (patrz widgets.h).
 */
#include "widgets.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsPixmapItem>
#include <QHelpEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "masks.h"
#include "topology.h"

// ============================================================================
//  WyborObrazu
// ============================================================================

WyborObrazu::WyborObrazu(QWidget* parent) : QComboBox(parent)
{
    for (const auto& o : obrazyTestowe()) {
        addItem(o.etykieta, o.sciezka);
        setItemData(count() - 1, o.syntetyczny() ? QStringLiteral("Obraz generowany w pamięci – etap „wczytanie” to czas generowania.")
                                                 : o.sciezka, Qt::ToolTipRole);
    }
    insertSeparator(count());
    addItem(QStringLiteral("Inny plik…"), QString());
    setToolTip(QStringLiteral("Obraz wejściowy. Kolorowe obrazy są przed pomiarem zamieniane na skalę szarości."));
    setWhatsThis(QStringLiteral("<b>Obraz wejściowy</b><br>Obrazy testowe z projektu: zdjęcia Kodak, zdjęcie NASA "
                                "przeskalowane do HD, 4K i 8K oraz obrazy syntetyczne. „Inny plik…” pozwala wybrać "
                                "dowolny plik JPG, PNG, GIF lub BMP."));
    connect(this, qOverload<int>(&QComboBox::activated), this, &WyborObrazu::wybranoIndeks);
}

ZrodloObrazu WyborObrazu::zrodlo() const { return { currentData().toString(), currentText() }; }

void WyborObrazu::ustaw(const QString& sciezka)
{
    const int i = findData(sciezka);
    if (i >= 0) { setCurrentIndex(i); poprzedni_ = i; }
    else if (sciezka.startsWith(QLatin1String("synthetic:"))) {
        // obraz zastępczy (brak pliku na tym komputerze) – dopisany do listy jako generowany
        const int j = count() - 2;
        insertItem(j, QStringLiteral("Syntetyczny %1 (generowany)").arg(sciezka.mid(10)), sciezka);
        setItemData(j, QStringLiteral("Obraz generowany w pamięci – zastępuje plik, którego nie ma na tym komputerze."), Qt::ToolTipRole);
        setCurrentIndex(j);
        poprzedni_ = j;
    } else if (QFileInfo::exists(sciezka)) dodajPlik(sciezka);
}

void WyborObrazu::dodajPlik(const QString& sciezka)
{
    int i = findData(sciezka);
    if (i < 0) {
        i = count() - 2;   // przed separatorem i „Inny plik…”
        insertItem(i, QFileInfo(sciezka).fileName(), sciezka);
        setItemData(i, sciezka, Qt::ToolTipRole);
    }
    setCurrentIndex(i);
    poprzedni_ = i;
}

void WyborObrazu::wybranoIndeks(int i)
{
    if (!itemData(i).toString().isEmpty()) { poprzedni_ = i; return; }
    const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Wybierz obraz"), katalogRepo() + "/obrazy",
                                                   QStringLiteral("Obrazy (*.png *.jpg *.jpeg *.gif *.bmp)"));
    if (f.isEmpty()) setCurrentIndex(poprzedni_);
    else dodajPlik(f);
}

// ============================================================================
//  WyborMaski
// ============================================================================

WyborMaski::WyborMaski(QWidget* parent) : QComboBox(parent)
{
    const struct { const char* id; const char* opis; } lista[] = {
        { "sobel3", "Sobel 3×3 – podstawowa maska; 9 pól, rozdzielna" },
        { "sobel5", "Sobel 5×5 – silniejsze wygładzanie; 25 pól, rozdzielna" },
        { "sobel7", "Sobel 7×7 – najsilniejsze wygładzanie; 49 pól, rozdzielna (W2 zyskuje najwięcej)" },
        { "prewitt", "Prewitt 3×3 – wygładzanie bez wag; rozdzielna" },
        { "scharr", "Scharr 3×3 – lepsza symetria kierunków; rozdzielna" },
        { "roberts", "Roberts 2×2 – najmniejsza maska, nierozdzielna" },
    };
    for (const auto& m : lista) {
        const Mask* mk = findMask(m.id);
        addItem(mk ? QString::fromStdString(mk->name) : QString::fromLatin1(m.id), QString::fromLatin1(m.id));
        setItemData(count() - 1, QString::fromUtf8(m.opis), Qt::ToolTipRole);
    }
    setToolTip(QStringLiteral("Maska (operator gradientowy). Większa maska mocniej wygładza obraz: lepiej znosi szum, "
                              "ale wymaga więcej obliczeń."));
}

void WyborMaski::ustaw(const QString& id)
{
    const int i = findData(id);
    if (i >= 0) setCurrentIndex(i);
}

// ============================================================================
//  ParametryWersji
// ============================================================================

namespace {
QComboBox* combo(const QList<QPair<QString, QString>>& pozycje, const QString& tip)
{
    auto* c = new QComboBox;
    for (const auto& [tekst, opis] : pozycje) {
        c->addItem(tekst);
        c->setItemData(c->count() - 1, opis, Qt::ToolTipRole);
    }
    c->setToolTip(tip);
    return c;
}
} // namespace

ParametryWersji::ParametryWersji(QWidget* parent) : QWidget(parent)
{
    stos_ = new QStackedWidget;
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(stos_);

    // --- W1–W3: brak parametrów
    auto* brak = new QLabel(QStringLiteral("Wersja jednowątkowa – brak parametrów.\n"
                                           "Wynik zależy tylko od obrazu i maski."));
    brak->setWordWrap(true);
    brak->setObjectName("podpowiedz");
    brak->setToolTip(QStringLiteral("W1, W2 i W3 działają w jednym wątku, a ich wynik zależy tylko od obrazu i maski."));
    stos_->addWidget(brak);

    // --- W4: wątki
    auto* w4 = new QWidget;
    auto* f4 = new QFormLayout(w4);
    f4->setContentsMargins(0, 0, 0, 0);
    watki_ = new QSpinBox;
    watki_->setRange(1, 64);
    const int logiczne = std::max(1, static_cast<int>(cpuTopology().size()));
    watki_->setValue(logiczne);
    watki_->setToolTip(QStringLiteral("Liczba wątków roboczych. Ten procesor ma %1 wątków sprzętowych.").arg(logiczne));
    watki_->setWhatsThis(QStringLiteral("<b>Liczba wątków N</b><br>Obraz jest dzielony na fragmenty wierszy pomiędzy N wątków. "
                                        "Powyżej liczby rdzeni fizycznych (12) drugi wątek sprzętowy (SMT) współdzieli rdzeń, "
                                        "więc zysk jest mały. Przyspieszenie ogranicza część sekwencyjna (prawo Amdahla)."));
    harmonogram_ = combo({ { "static", "N równych bloków, po jednym na wątek – bez narzutu, ale wolny wątek opóźnia cały etap" },
                           { "cyclic", "fragmenty przydzielane wątkom po kolei (fragment i → wątek i mod N)" },
                           { "dynamic", "wolny wątek pobiera następny fragment ze wspólnego licznika atomowego" },
                           { "guided", "jak dynamic, ale fragmenty maleją wraz z ilością pozostałej pracy" } },
                         QStringLiteral("Strategia przydziału fragmentów wierszy do wątków (jak klauzula schedule w OpenMP)."));
    harmonogram_->setCurrentIndex(2);
    fragment_ = new QSpinBox;
    fragment_->setRange(1, 4096);
    fragment_->setValue(16);
    fragment_->setSuffix(QStringLiteral(" wierszy"));
    fragment_->setToolTip(QStringLiteral("Rozmiar fragmentu przydzielanego naraz. Mały – dobre równoważenie, więcej operacji "
                                         "atomowych; duży – mniej porcji pracy niż wątków (nierównowaga). static go nie używa."));
    redukcja_ = combo({ { "lokalne maksimum", "każdy wątek liczy własne maksimum, redukcja po barierze (zalecane)" },
                        { "zmienna atomowa", "porównanie każdego piksela ze wspólną zmienną std::atomic (CAS)" },
                        { "mutex na piksel", "aktualizacja wspólnej zmiennej pod blokadą przy każdym pikselu – bardzo wolne" },
                        { "komórki w osobnych liniach", "zapis do własnej komórki wątku, komórki wyrównane do 64 B" },
                        { "komórki w jednej linii", "zapis do własnej komórki wątku, wszystkie w jednej linii cache – false sharing" } },
                      QStringLiteral("Sposób wyznaczania maksimum globalnego (potrzebnego do skalowania wyniku)."));
    mechanizm_ = combo({ { "pula wątków", "wątki tworzone raz i usypiane między etapami" },
                         { "nowe wątki", "std::thread tworzone i kończone w każdym etapie" } },
                       QStringLiteral("Pula wątków czy tworzenie nowych wątków przy każdym etapie."));
    przypisanie_ = combo({ { "system decyduje", "bez przypinania – planista systemu wybiera rdzenie" },
                           { "najpierw jeden blok (compact)", "najpierw wszystkie wątki jednego CCD, z SMT" },
                           { "naprzemiennie bloki (spread)", "rdzenie fizyczne z obu CCD na zmianę, SMT na końcu" },
                           { "jeden wątek na rdzeń (nosmt)", "tylko pierwszy wątek sprzętowy każdego rdzenia" } },
                         QStringLiteral("Przypisanie wątków do rdzeni logicznych (pthread_setaffinity_np)."));
    f4->addRow(QStringLiteral("Wątki:"), watki_);
    f4->addRow(QStringLiteral("Harmonogram:"), harmonogram_);
    f4->addRow(QStringLiteral("Fragment:"), fragment_);
    f4->addRow(QStringLiteral("Maksimum:"), redukcja_);
    f4->addRow(QStringLiteral("Mechanizm:"), mechanizm_);
    f4->addRow(QStringLiteral("Rdzenie:"), przypisanie_);
    stos_->addWidget(w4);

    // --- W5: GPU
    auto* w5 = new QWidget;
    auto* f5 = new QFormLayout(w5);
    f5->setContentsMargins(0, 0, 0, 0);
    grupa_ = combo({ { "8 × 8", "64 wątki w grupie" }, { "16 × 16", "256 wątków w grupie (domyślnie)" },
                     { "32 × 8", "256 wątków, szersze odczyty wierszy" }, { "32 × 32", "1024 wątki – największa grupa" } },
                   QStringLiteral("Rozmiar grupy roboczej (work-group): wątki współdzielące pamięć lokalną jednego multiprocesora."));
    grupa_->setCurrentIndex(1);
    ppw_ = new QSpinBox;
    ppw_->setRange(1, 64);
    ppw_->setValue(1);
    ppw_->setSuffix(QStringLiteral(" px / wątek"));
    ppw_->setToolTip(QStringLiteral("Ile pikseli liczy jeden wątek GPU. Więcej pikseli = mniej wątków i mniejszy narzut "
                                    "(w pomiarach najlepiej ok. 16)."));
    precyzja_ = combo({ { "dokładna (auto)", "pojedyncza precyzja tylko tam, gdzie daje wynik dokładny, inaczej podwójna" },
                        { "zawsze pojedyncza (fp32)", "szybciej, możliwa różnica o 1 poziom szarości" } },
                      QStringLiteral("Precyzja obliczania modułu gradientu na karcie (podwójna jest ok. 32× wolniejsza)."));
    f5->addRow(QStringLiteral("Grupa robocza:"), grupa_);
    f5->addRow(QStringLiteral("Piksele:"), ppw_);
    f5->addRow(QStringLiteral("Precyzja:"), precyzja_);
    stos_->addWidget(w5);

    for (QSpinBox* s : { watki_, fragment_, ppw_ }) connect(s, &QSpinBox::valueChanged, this, &ParametryWersji::zmieniono);
    for (QComboBox* c : { harmonogram_, redukcja_, mechanizm_, przypisanie_, grupa_, precyzja_ })
        connect(c, &QComboBox::currentIndexChanged, this, &ParametryWersji::zmieniono);
}

void ParametryWersji::ustawWersje(Wersja w)
{
    stos_->setCurrentIndex(w == Wersja::W4 ? 1 : w == Wersja::W5 ? 2 : 0);
}

RunConfig ParametryWersji::konfiguracja() const
{
    RunConfig c;
    c.threads = watki_->value();
    c.schedule = static_cast<Schedule>(harmonogram_->currentIndex());
    c.chunkRows = fragment_->value();
    c.reduce = static_cast<Reduce>(redukcja_->currentIndex());
    c.mech = static_cast<ThreadMech>(mechanizm_->currentIndex());
    c.affinity = static_cast<Affinity>(przypisanie_->currentIndex());
    static const int wg[][2] = { { 8, 8 }, { 16, 16 }, { 32, 8 }, { 32, 32 } };
    c.gpuWgX = wg[grupa_->currentIndex()][0];
    c.gpuWgY = wg[grupa_->currentIndex()][1];
    c.gpuPpw = ppw_->value();
    c.gpuFp32 = precyzja_->currentIndex() == 1;
    return c;
}

void ParametryWersji::ustaw(const RunConfig& c)
{
    if (c.threads > 0) watki_->setValue(c.threads);
    harmonogram_->setCurrentIndex(static_cast<int>(c.schedule));
    fragment_->setValue(c.chunkRows);
    redukcja_->setCurrentIndex(static_cast<int>(c.reduce));
    mechanizm_->setCurrentIndex(static_cast<int>(c.mech));
    przypisanie_->setCurrentIndex(static_cast<int>(c.affinity));
    const int wg = c.gpuWgX == 8 ? 0 : c.gpuWgY == 8 ? 2 : c.gpuWgX == 32 ? 3 : 1;
    grupa_->setCurrentIndex(wg);
    ppw_->setValue(c.gpuPpw);
    precyzja_->setCurrentIndex(c.gpuFp32 ? 1 : 0);
}

// ============================================================================
//  PasekCzasow
// ============================================================================

PasekCzasow::PasekCzasow(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void PasekCzasow::ustaw(const QString& tytul, const QVector<Segment>& segmenty)
{
    tytul_ = tytul;
    seg_.clear();
    for (const auto& s : segmenty) if (s.ms > 0) seg_.push_back(s);
    update();
}

QRectF PasekCzasow::prostokatSegmentu(int i) const
{
    double suma = 0;
    for (const auto& s : seg_) suma += s.ms;
    const double h = fontMetrics().height();
    const QRectF pas(0, h * 1.3, width() - 1, h * 1.75);
    double x = pas.left();
    for (int k = 0; k < seg_.size(); ++k) {
        const double w = suma > 0 ? pas.width() * seg_[k].ms / suma : 0;
        if (k == i) return QRectF(x, pas.top(), w, pas.height());
        x += w;
    }
    return {};
}

void PasekCzasow::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    double suma = 0;
    for (const auto& s : seg_) suma += s.ms;

    QFont f = font();
    f.setBold(true);
    p.setFont(f);
    p.setPen(pal.color(QPalette::WindowText));
    const double h = fontMetrics().height();
    p.drawText(QRectF(0, 0, width(), h * 1.2), Qt::AlignLeft | Qt::AlignVCenter, tytul_);
    if (suma > 0) {
        p.drawText(QRectF(0, 0, width(), h * 1.2), Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("razem %1").arg(tekstCzasu(suma)));
    }
    p.setFont(font());

    const QRectF pas(0, h * 1.3, width() - 1, h * 1.75);
    if (seg_.isEmpty()) {
        p.setPen(QPen(pal.color(QPalette::Mid), 1, Qt::DashLine));
        p.drawRoundedRect(pas, 4, 4);
        p.drawText(pas, Qt::AlignCenter, QStringLiteral("brak pomiaru – kliknij „Uruchom”"));
        return;
    }
    QPainterPath zaokr;
    zaokr.addRoundedRect(pas, 4, 4);
    p.save();
    p.setClipPath(zaokr);
    for (int i = 0; i < seg_.size(); ++i) {
        const QRectF r = prostokatSegmentu(i);
        p.fillRect(r, seg_[i].kolor);
        if (seg_[i].kreskowanie) p.fillRect(r, QBrush(QColor(255, 255, 255, 90), Qt::BDiagPattern));
        if (i > 0) { p.setPen(QPen(Qt::white, 2)); p.drawLine(r.topLeft(), r.bottomLeft()); }
        const QString t = QStringLiteral("%1 · %2%").arg(tekstCzasu(seg_[i].ms), liczba(100.0 * seg_[i].ms / suma, 0));
        const QFontMetrics fm(font());
        if (fm.horizontalAdvance(t) + 8 < r.width()) {
            const QColor tlo = seg_[i].kolor;
            p.setPen(tlo.lightness() < 140 ? Qt::white : Qt::black);
            p.drawText(r, Qt::AlignCenter, t);
        }
    }
    p.restore();

    // legenda
    double x = 0;
    const double y = pas.bottom() + h * 0.35;
    const double kw = h * 0.7;   // kwadrat legendy
    for (const auto& s : seg_) {
        p.fillRect(QRectF(x, y + h * 0.15, kw, kw), s.kolor);
        if (s.kreskowanie) p.fillRect(QRectF(x, y + h * 0.15, kw, kw), QBrush(QColor(255, 255, 255, 90), Qt::BDiagPattern));
        p.setPen(pal.color(QPalette::WindowText));
        const QString t = QStringLiteral("%1 %2").arg(s.nazwa, tekstCzasu(s.ms));
        p.drawText(QPointF(x + kw + h * 0.3, y + h * 0.8), t);
        x += kw + h * 0.3 + fontMetrics().horizontalAdvance(t) + h;
    }
}

bool PasekCzasow::event(QEvent* e)
{
    if (e->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(e);
        double suma = 0;
        for (const auto& s : seg_) suma += s.ms;
        for (int i = 0; i < seg_.size(); ++i) {
            if (prostokatSegmentu(i).contains(he->pos())) {
                QString t = QStringLiteral("<b>%1</b>: %2 (%3% całości)").arg(seg_[i].nazwa, tekstCzasu(seg_[i].ms),
                                                                            liczba(100.0 * seg_[i].ms / suma, 1));
                if (!seg_[i].opis.isEmpty()) t += "<br>" + seg_[i].opis;
                QToolTip::showText(he->globalPos(), t, this);
                return true;
            }
        }
        QToolTip::hideText();
        e->ignore();
        return true;
    }
    return QWidget::event(e);
}

// ============================================================================
//  PodgladObrazu
// ============================================================================

PodgladObrazu::PodgladObrazu(const QString& tytul, QWidget* parent) : QGraphicsView(parent), tytul_(tytul)
{
    setScene(new QGraphicsScene(this));
    pix_ = scene()->addPixmap(QPixmap());
    pix_->setTransformationMode(Qt::SmoothTransformation);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setRenderHints(QPainter::SmoothPixmapTransform | QPainter::Antialiasing);
    setBackgroundBrush(QColor("#2b2b2b"));
    setFrameShape(QFrame::NoFrame);
    setMinimumSize(znaki(this, 30), wiersze(this, 9));
    setToolTip(QStringLiteral("Kółko myszy – powiększenie, przeciąganie – przesuwanie, dwuklik – dopasowanie do okna."));
    viewport()->setMouseTracking(true);
    auto zmiana = [this] { if (!synchronizuje_) emit widokZmieniony(); };
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, zmiana);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, zmiana);
}

void PodgladObrazu::ustawObraz(const QImage& img)
{
    rozmiarOryginalu_ = img.size();
    oryginal_ = img;
    QImage pokaz = img;
    constexpr int maks = 4096;   // podgląd ogranicza pamięć karty i czas rysowania
    if (img.width() > maks || img.height() > maks) pokaz = img.scaled(maks, maks, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    pix_->setPixmap(QPixmap::fromImage(pokaz));
    pix_->setScale(img.isNull() ? 1.0 : static_cast<double>(img.width()) / std::max(1, pokaz.width()));
    scene()->setSceneRect(pix_->sceneBoundingRect());
    if (dopasowany_) dopasuj();
    viewport()->update();
}

void PodgladObrazu::dopasuj()
{
    if (pix_->pixmap().isNull()) return;
    fitInView(pix_, Qt::KeepAspectRatio);
    dopasowany_ = true;
    emit widokZmieniony();
}

void PodgladObrazu::sprzez(PodgladObrazu* a, PodgladObrazu* b)
{
    a->para_ = b;
    b->para_ = a;
    auto kopiuj = [](PodgladObrazu* z, PodgladObrazu* d) {
        if (d->synchronizuje_) return;
        d->synchronizuje_ = true;
        d->setTransform(z->transform());
        d->horizontalScrollBar()->setValue(z->horizontalScrollBar()->value());
        d->verticalScrollBar()->setValue(z->verticalScrollBar()->value());
        d->dopasowany_ = z->dopasowany_;
        d->synchronizuje_ = false;
    };
    connect(a, &PodgladObrazu::widokZmieniony, b, [a, b, kopiuj] { kopiuj(a, b); });
    connect(b, &PodgladObrazu::widokZmieniony, a, [a, b, kopiuj] { kopiuj(b, a); });
}

void PodgladObrazu::wheelEvent(QWheelEvent* e)
{
    if (pix_->pixmap().isNull()) return;
    const double k = e->angleDelta().y() > 0 ? 1.25 : 0.8;
    scale(k, k);
    dopasowany_ = false;
    emit widokZmieniony();
}

void PodgladObrazu::mouseDoubleClickEvent(QMouseEvent*) { dopasuj(); }

void PodgladObrazu::mouseMoveEvent(QMouseEvent* e)
{
    QGraphicsView::mouseMoveEvent(e);
    if (e->buttons() != Qt::NoButton || oryginal_.isNull()) return;
    const QPointF s = mapToScene(e->position().toPoint());
    const int x = static_cast<int>(s.x()), y = static_cast<int>(s.y());
    if (x < 0 || y < 0 || x >= oryginal_.width() || y >= oryginal_.height()) { QToolTip::hideText(); return; }
    QToolTip::showText(e->globalPosition().toPoint(),
                       QStringLiteral("x = %1, y = %2<br>%3: <b>%4</b> / 255<br><i>kółko – powiększ, przeciągnij – przesuń, dwuklik – dopasuj</i>")
                           .arg(x).arg(y).arg(opisWartosci_).arg(qGray(oryginal_.pixel(x, y))), this);
}

void PodgladObrazu::resizeEvent(QResizeEvent* e)
{
    QGraphicsView::resizeEvent(e);
    if (dopasowany_) dopasuj();
}

void PodgladObrazu::drawForeground(QPainter* p, const QRectF&)
{
    p->save();
    p->resetTransform();
    QString t = tytul_;
    if (rozmiarOryginalu_.isValid() && !rozmiarOryginalu_.isEmpty())
        t += QStringLiteral("  ·  %1 × %2").arg(rozmiarOryginalu_.width()).arg(rozmiarOryginalu_.height());
    const QFontMetrics fm(font());
    const QRectF r(8, 8, fm.horizontalAdvance(t) + 16, fm.height() + 8);
    p->setRenderHint(QPainter::Antialiasing);
    p->setBrush(QColor(0, 0, 0, 150));
    p->setPen(Qt::NoPen);
    p->drawRoundedRect(r, 4, 4);
    p->setPen(Qt::white);
    p->drawText(r, Qt::AlignCenter, t);
    if (pix_->pixmap().isNull()) {
        p->setPen(QColor("#aaaaaa"));
        p->drawText(viewport()->rect(), Qt::AlignCenter, QStringLiteral("brak obrazu"));
    }
    p->restore();
}

// ============================================================================
//  Segmenty pasków czasu
// ============================================================================

QVector<PasekCzasow::Segment> segmentyProgramu(const Pomiar& p)
{
    const QColor kw = kolorWersji(p.zlecenie.wersja);
    const bool gen = p.zlecenie.zrodlo.syntetyczny();
    return {
        { gen ? QStringLiteral("generowanie") : QStringLiteral("wczytanie"), p.wczytanie.mediana, QColor("#9e9e9e"), false,
          gen ? QStringLiteral("utworzenie obrazu syntetycznego w pamięci")
              : QStringLiteral("odczyt pliku i dekodowanie (stb_image), zamiana na skalę szarości – jeden wątek") },
        { QStringLiteral("algorytm"), p.algorytm.mediana, kw, false,
          QStringLiteral("sam algorytm %1: od obrazu w pamięci do obrazu 0–255").arg(nazwaWersji(p.zlecenie.wersja)) },
        { QStringLiteral("zapis"), p.zapis.mediana, QColor("#616161"), true,
          QStringLiteral("kodowanie i zapis JPG (stb_image_write) – jeden wątek") },
    };
}

QVector<PasekCzasow::Segment> segmentyAlgorytmu(const Pomiar& p)
{
    const QColor kw = kolorWersji(p.zlecenie.wersja);
    const QColor jasny = kw.lighter(150);
    auto e = [&](const char* k) { auto it = p.etapy.find(k); return it == p.etapy.end() ? 0.0 : it->second; };
    if (p.zlecenie.wersja == Wersja::W5) {
        return {
            { QStringLiteral("przesył na GPU"), e("upload"), jasny, true, QStringLiteral("obraz przez magistralę PCIe do pamięci karty") },
            { QStringLiteral("obliczenia na GPU"), e("kernel"), kw, false, QStringLiteral("kernele splotu i skalowania") },
            { QStringLiteral("przesył z GPU"), e("download"), jasny, true, QStringLiteral("wynik z powrotem do pamięci komputera") },
            { QStringLiteral("sterowanie"), p.pozostaleMs, QColor("#bdbdbd"), false, QStringLiteral("uruchamianie kerneli i synchronizacja z CPU") },
        };
    }
    return {
        { QStringLiteral("splot i moduł"), e("conv"), kw, false, QStringLiteral("splot z maską i moduł gradientu") },
        { QStringLiteral("skalowanie 0–255"), e("norm"), jasny, true, QStringLiteral("drugi przebieg: przeskalowanie względem maksimum") },
        { QStringLiteral("pozostałe"), p.pozostaleMs, QColor("#bdbdbd"), false,
          QStringLiteral("przydział i zerowanie buforów, synchronizacja wątków – część sekwencyjna") },
    };
}
