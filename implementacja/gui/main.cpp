/**
 * @file main.cpp
 * @brief Punkt wejścia edge_gui – aplikacji Qt do prezentacji działania i wyników projektu.
 *
 * Aplikacja korzysta z tej samej biblioteki edge_core co edge_bench i edge_cli,
 * więc mierzy dokładnie te same wersje W1–W5 w tych samych granicach pomiaru.
 */
#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <QFile>
#include <QLocale>
#include <QStyleFactory>

#include "oknoglowne.h"
#include "presets.h"

/**
 * @brief Ustawia styl Fusion z jasną paletą i arkuszem stylów z zasobów (:/styl/styl.qss).
 *
 * Fusion wygląda tak samo na każdym systemie, co ułatwia pokaz na obcym komputerze lub rzutniku.
 */
static void ustawWyglad(QApplication& app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette p;
    p.setColor(QPalette::Window, QColor("#f5f6f8"));
    p.setColor(QPalette::Base, Qt::white);
    p.setColor(QPalette::AlternateBase, QColor("#f0f2f5"));
    p.setColor(QPalette::Button, QColor("#ffffff"));
    p.setColor(QPalette::Highlight, QColor("#0072B2"));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, QColor("#fffdf2"));
    p.setColor(QPalette::ToolTipText, QColor("#222222"));
    p.setColor(QPalette::WindowText, QColor("#1f2933"));
    p.setColor(QPalette::Text, QColor("#1f2933"));
    app.setPalette(p);
    QFile qss(QStringLiteral(":/styl/styl.qss"));
    if (qss.open(QIODevice::ReadOnly)) app.setStyleSheet(QString::fromUtf8(qss.readAll()));
}

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("edge_gui"));
    QApplication::setApplicationDisplayName(QStringLiteral("Wykrywanie krawędzi"));
    QApplication::setOrganizationName(QStringLiteral("PWr-ZSiSK"));
    QApplication::setApplicationVersion(QStringLiteral("1.0"));
    QLocale::setDefault(QLocale(QLocale::Polish, QLocale::Poland));
    ustawWyglad(app);

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("Prezentacja działania i wyników wykrywania krawędzi (W1–W5)."));
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption zrzut(QStringLiteral("zrzut"), QStringLiteral("Zapisz zrzut okna do pliku PNG i zakończ."), QStringLiteral("plik"));
    QCommandLineOption gotowe(QStringLiteral("gotowe"), QStringLiteral("Zastosuj gotowe ustawienie nr N (1…) i uruchom pomiar."), QStringLiteral("N"));
    QCommandLineOption zakladka(QStringLiteral("zakladka"), QStringLiteral("Otwórz zakładkę nr N (1–4)."), QStringLiteral("N"));
    QCommandLineOption rozmiar(QStringLiteral("rozmiar"), QStringLiteral("Rozmiar okna SZERxWYS, np. 1280x800 (test małego ekranu)."), QStringLiteral("SZERxWYS"));
    QCommandLineOption weryfikuj(QStringLiteral("weryfikuj"), QStringLiteral("Uruchom weryfikację poprawności W1–W5."));
    QCommandLineOption pomoc(QStringLiteral("pomoc"), QStringLiteral("Otwórz okno pomocy."));
    cli.addOptions({ zrzut, gotowe, zakladka, weryfikuj, pomoc, rozmiar });
    cli.process(app);

    OknoGlowne okno;
    okno.show();
    if (cli.isSet(zrzut)) okno.resize(1600, 1000);
    if (cli.isSet(rozmiar)) {
        const QStringList r = cli.value(rozmiar).split('x');
        if (r.size() == 2) okno.resize(r[0].toInt(), r[1].toInt());
    }
    if (cli.isSet(gotowe)) okno.uruchomGotowe(cli.value(gotowe).toInt() - 1);
    // --zakladka po --gotowe: przełączenie po zakończeniu pomiaru (np. zrzut historii)
    const bool zakladkaPoPomiarze = cli.isSet(zakladka) && cli.isSet(gotowe);
    if (cli.isSet(zakladka) && !zakladkaPoPomiarze) okno.pokazZakladke(cli.value(zakladka).toInt() - 1);
    if (cli.isSet(weryfikuj)) okno.uruchomWeryfikacje();
    if (cli.isSet(pomoc)) okno.pokazPomoc();
    if (cli.isSet(zrzut)) {
        // zrzut po zakończeniu pomiaru (jeśli trwa) – używane do dokumentacji i testu wyglądu
        const QString plik = cli.value(zrzut);
        auto* t = new QTimer(&okno);
        const int poZakladka = zakladkaPoPomiarze ? cli.value(zakladka).toInt() - 1 : -1;
        QObject::connect(t, &QTimer::timeout, &okno, [&okno, plik, t, poZakladka] {
            if (Koordynator::instancja().zajety()) return;
            t->stop();
            if (poZakladka >= 0) okno.pokazZakladke(poZakladka);
            QTimer::singleShot(400, &okno, [&okno, plik] { okno.grab().save(plik); QApplication::quit(); });
        });
        t->start(300);
    }
    return app.exec();
}
