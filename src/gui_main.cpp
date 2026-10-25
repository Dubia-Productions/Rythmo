#include "rythmo/main_window.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QIcon>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
void isolatePortableQtPlugins() {
#ifdef Q_OS_WIN
    // A developer machine may export QT_PLUGIN_PATH or have Qt compiled with
    // an absolute plugin path. Resolve this executable's directory before
    // QApplication loads qwindows.dll, then restrict plugin discovery to the
    // portable bundle. This prevents loading an unbundled qopensslbackend.dll
    // from another Qt installation (and its missing libcrypto dependency).
    wchar_t executablePath[32768]{};
    constexpr DWORD pathCapacity = static_cast<DWORD>(sizeof(executablePath) / sizeof(executablePath[0]));
    const DWORD length = GetModuleFileNameW(nullptr, executablePath,
                                           pathCapacity);
    if (length > 0 && length < pathCapacity) {
        const QString appDir = QFileInfo(QString::fromWCharArray(executablePath,
                                                                static_cast<qsizetype>(length)))
                                   .absolutePath();
        const QByteArray pluginRoot = QDir::toNativeSeparators(appDir).toUtf8();
        const QByteArray platformPlugins = QDir::toNativeSeparators(appDir + QStringLiteral("/platforms")).toUtf8();
        qputenv("QT_PLUGIN_PATH", pluginRoot);
        qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", platformPlugins);
    }
#endif
}
} // namespace

int main(int argc, char* argv[]) {
    isolatePortableQtPlugins();
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("Rythmo by Dubia Productions");
    app.setWindowIcon(QIcon(QStringLiteral(":/app/rythmo.png")));
    QCoreApplication::setApplicationVersion(QStringLiteral(RYTHMO_VERSION));
    QCommandLineParser parser;
    parser.setApplicationDescription("Rythmo by Dubia Productions: studio de synchronisation et de production vocale.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("document", "Fichier DetX à ouvrir.", "[document.detx]");
    parser.process(app);

    const auto initial = parser.positionalArguments().isEmpty() ? QString{} : parser.positionalArguments().front();
    rythmo::MainWindow window(initial);
    window.show();
    return app.exec();
}
